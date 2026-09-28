//! Read-only, direct-I/O FUSE view. Only metadata is written to disk.
//! Blocks are striped across decoder workers; replies leave the FUSE dispatcher
//! immediately, allowing independent PFP readers to decode concurrently.
use super::*;
use fuser::{FileAttr, FileType, Filesystem, KernelConfig, MountOption, ReplyAttr,
    ReplyData, ReplyDirectory, ReplyEntry, ReplyOpen, Request};
use std::ffi::OsStr;
use std::sync::{mpsc::{sync_channel, SyncSender}, Arc};
use std::time::{Duration, UNIX_EPOCH};
use std::collections::VecDeque;
const BLOCK: u64 = 4 << 20;
const CACHE_BLOCKS: usize = 4;
const RESET_BYTES: u64 = 128 << 20;
const TTL: Duration = Duration::from_secs(3600);
#[derive(Debug)]
struct Part { base: u64, len: u64, skip: usize, desc: ragc_common::SegmentDesc }
#[derive(Debug)]
struct Row { base: u64, len: u64, start: u64, contig: String, parts: Vec<Part> }
struct Layout { rows: Vec<Row>, total: u64 }
impl Layout {
    fn read(&self, dec: &mut Decompressor, mut offset: u64, length: u64) -> Result<Vec<u8>> {
        let end = offset.saturating_add(length).min(self.total);
        let mut out = Vec::with_capacity(end.saturating_sub(offset) as usize);
        while offset < end {
            let row = &self.rows[self.rows.partition_point(|r| r.base <= offset) - 1];
            let local = offset - row.base;
            if local == row.len { out.push(30); offset += 1; continue; }
            let take = (end - offset).min(row.len - local);
            let forward = row.start + row.len - local - take;
            let end_forward = forward + take;
            let mut numeric = Vec::with_capacity(take as usize);
            for part in row.parts.iter().skip(row.parts.partition_point(|p| p.base+p.len <= forward)) {
                if part.base >= end_forward { break; }
                let raw = dec.get_segment_data_by_desc(&part.desc)?;
                anyhow::ensure!(raw.len() as u64 == part.len + part.skip as u64, "archive changed since ghost indexing");
                let lo = (forward.saturating_sub(part.base)) as usize + part.skip;
                let hi = (end_forward-part.base).min(part.len) as usize + part.skip;
                if part.desc.is_rev_comp {
                    numeric.extend(raw[raw.len()-hi..raw.len()-lo].iter().rev()
                        .map(|&b| if b < 4 { 3-b } else { b }));
                } else { numeric.extend_from_slice(&raw[lo..hi]); }
            }
            anyhow::ensure!(numeric.len() as u64 == take, "short ghost archive range");
            let mut seq = ascii_of(&numeric, true, 30)?;
            seq.reverse(); out.extend(seq); offset += take;
        }
        Ok(out)
    }
}
struct Job { offset: u64, size: u32, reply: Option<ReplyData> }
struct Worker {
    archive: String, dec: Decompressor, layout: Arc<Layout>,
    cache: VecDeque<(u64, Vec<u8>)>, decoded: u64,
}
impl Worker {
    fn block(&mut self, base: u64) -> Result<&[u8]> {
        if let Some(i) = self.cache.iter().position(|(b, _)| *b == base) {
            let item = self.cache.remove(i).unwrap(); self.cache.push_back(item);
        } else {
            // ragc's reference cache has no eviction API. Reopen periodically
            // to bound its lifetime as well as our explicit byte cache.
            if self.decoded >= RESET_BYTES {
                self.dec = Decompressor::open(&self.archive, DecompressorConfig::default())?;
                self.decoded = 0;
            }
            let data = self.layout.read(&mut self.dec, base, BLOCK)?;
            self.decoded += data.len() as u64;
            if self.cache.len() == CACHE_BLOCKS { self.cache.pop_front(); }
            self.cache.push_back((base, data));
        }
        Ok(&self.cache.back().unwrap().1)
    }
    fn read(&mut self, mut offset: u64, size: u32) -> Result<Vec<u8>> {
        let end = offset.saturating_add(size as u64).min(self.layout.total);
        let mut data = Vec::with_capacity(end.saturating_sub(offset) as usize);
        while offset < end {
            let base = offset / BLOCK * BLOCK;
            let block = self.block(base)?;
            let start = (offset - base) as usize;
            let take = ((end-offset) as usize).min(block.len()-start);
            anyhow::ensure!(take > 0, "empty ghost block");
            data.extend_from_slice(&block[start..start+take]); offset += take as u64;
        }
        Ok(data)
    }
}
struct Ghost { layout: Arc<Layout>, workers: Vec<SyncSender<Job>>, next: u64 }
impl Ghost {
    fn attr(&self, ino: u64) -> FileAttr {
        FileAttr { ino, size: if ino == 2 { self.layout.total } else { 0 }, blocks: 0,
            atime: UNIX_EPOCH, mtime: UNIX_EPOCH, ctime: UNIX_EPOCH, crtime: UNIX_EPOCH,
            kind: if ino == 1 { FileType::Directory } else { FileType::RegularFile },
            perm: if ino == 1 { 0o500 } else { 0o400 }, nlink: 1,
            uid: unsafe { libc::getuid() }, gid: unsafe { libc::getgid() },
            rdev: 0, blksize: 1 << 20, flags: 0 }
    }
}
impl Filesystem for Ghost {
    fn init(&mut self, _: &Request, config: &mut KernelConfig) -> std::result::Result<(), i32> {
        let _ = config.set_max_background(256);
        let _ = config.set_max_readahead(0);
        Ok(())
    }
    fn lookup(&mut self, _: &Request, parent: u64, name: &OsStr, reply: ReplyEntry) {
        if parent == 1 && name == "collection" { reply.entry(&TTL, &self.attr(2), 0); }
        else { reply.error(libc::ENOENT); }
    }
    fn getattr(&mut self, _: &Request, ino: u64, _: Option<u64>, reply: ReplyAttr) {
        if ino == 1 || ino == 2 { reply.attr(&TTL, &self.attr(ino)); } else { reply.error(libc::ENOENT); }
    }
    fn readdir(&mut self, _: &Request, ino: u64, _: u64, offset: i64, mut reply: ReplyDirectory) {
        if ino != 1 { reply.error(libc::ENOTDIR); return; }
        for (i, (ino, kind, name)) in [(1,FileType::Directory,"."),(1,FileType::Directory,".."),
            (2,FileType::RegularFile,"collection")].into_iter().enumerate().skip(offset.max(0) as usize) {
            if reply.add(ino, (i+1) as i64, kind, name) { break; }
        }
        reply.ok();
    }
    fn open(&mut self, _: &Request, ino: u64, flags: i32, reply: ReplyOpen) {
        if ino != 2 { reply.error(libc::EISDIR); return; }
        if flags & libc::O_ACCMODE != libc::O_RDONLY { reply.error(libc::EROFS); return; }
        let handle = self.next; self.next = self.next.wrapping_add(1);
        // FOPEN_DIRECT_IO: never accumulate decompressed text in kernel cache.
        reply.opened(handle, 1);
    }
    fn read(&mut self, _: &Request, ino: u64, _fh: u64, offset: i64, size: u32,
        _: i32, _: Option<u64>, reply: ReplyData) {
        if ino != 2 || offset < 0 { reply.error(libc::EINVAL); return; }
        let base = offset as u64 / BLOCK;
        let job = Job { offset: offset as u64, size, reply: Some(reply) };
        if let Err(e) = self.workers[base as usize % self.workers.len()].send(job) {
            e.0.reply.unwrap().error(libc::EIO);
        }
        // A single sequential reader also benefits from independent archive
        // queries. Prefetch a bounded window across the decoder pool. These
        // hints never block dispatch or displace queued demand reads.
        if offset as u64 % BLOCK < size as u64 {
            for ahead in 1..=self.workers.len() {
                let block = base + ahead as u64;
                if block * BLOCK >= self.layout.total { break; }
                let _ = self.workers[block as usize % self.workers.len()].try_send(
                    Job { offset: block * BLOCK, size: 0, reply: None });
            }
        }
    }
}
pub(super) fn mount(dec: &mut Decompressor, samples: &[String], args: &Args, path: &str) -> Result<()> {
    // A hard address-space ceiling also bounds archive-owned caches and
    // metadata on unexpectedly large/corrupt inputs. Allocation failure closes
    // the service; the pipeline fails before publication.
    let mut old = libc::rlimit { rlim_cur: 0, rlim_max: 0 };
    unsafe {
        anyhow::ensure!(libc::getrlimit(libc::RLIMIT_AS, &mut old) == 0, "getrlimit ghost");
        let limit = 32_000_000_000u64.min(old.rlim_cur).min(old.rlim_max);
        let bounded = libc::rlimit { rlim_cur: limit, rlim_max: old.rlim_max };
        anyhow::ensure!(libc::setrlimit(libc::RLIMIT_AS, &bounded) == 0, "setrlimit ghost");
    }
    anyhow::ensure!(args.revlines && args.upper && args.sep == 30 && !args.reverse && !args.stdout
        && !args.groups && args.serve_ranges.is_none(), "ghost requires only --revlines --upper --sep 1e");
    anyhow::ensure!((1..=96).contains(&args.workers), "--workers must be 1..96");
    anyhow::ensure!(!args.out.is_empty(), "ghost requires -o for names sidecar");
    let (bs, be) = args.band.unwrap_or((0, u64::MAX));
    anyhow::ensure!(bs <= be, "invalid ghost band");
    // Segment raw_length can differ from canonical get_contig output. Query
    // each segment once for its actual length; retain descriptors, never text.
    // Independent sample jobs build metadata in parallel, then restore order.
    let cursor = std::sync::atomic::AtomicUsize::new(0);
    let indexed = std::sync::Mutex::new(Vec::new());
    std::thread::scope(|scope| -> Result<()> {
        let mut tasks = Vec::new();
        for _ in 0..args.workers {
            let mut reader = dec.clone_for_thread()?;
            let cursor = &cursor; let indexed = &indexed;
            tasks.push(scope.spawn(move || -> Result<()> {
                let mut decoded = 0u64;
                loop {
                    let i = cursor.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                    if i >= samples.len() { break; }
                    let sample = &samples[i]; let mut rows = Vec::new();
                    for contig in reader.list_contigs(sample)? {
                        if args.group.as_ref().is_some_and(|g| group_key(&contig) != g) { continue; }
                        let mut parts = Vec::new(); let mut len = 0u64;
                        for (j, desc) in reader.get_contig_segments_desc(sample, &contig)?.into_iter().enumerate() {
                            if decoded >= RESET_BYTES {
                                reader = Decompressor::open(&args.archive, DecompressorConfig::default())?;
                                decoded = 0;
                            }
                            let raw_len = reader.get_segment_data_by_desc(&desc)?.len();
                            decoded += raw_len as u64;
                            let skip = if j == 0 { 0 } else { reader.kmer_length as usize };
                            anyhow::ensure!(raw_len >= skip, "short archive segment");
                            let contribution = (raw_len-skip) as u64;
                            parts.push(Part { base: len, len: contribution, skip, desc });
                            len += contribution;
                        }
                        let start = bs.min(len); let len = be.min(len).saturating_sub(start);
                        rows.push(Row { base: 0, len, start, contig, parts });
                    }
                    indexed.lock().unwrap().push((i, rows));
                }
                Ok(())
            }));
        }
        for t in tasks { t.join().map_err(|_| anyhow::anyhow!("ghost index worker panicked"))??; }
        Ok(())
    })?;
    let mut indexed = indexed.into_inner().unwrap(); indexed.sort_by_key(|(i,_)| *i);
    let mut rows = Vec::new(); let mut total = 0u64;
    for (_, sample_rows) in indexed {
        for mut row in sample_rows {
            row.base = total;
            total = total.checked_add(row.len).and_then(|v| v.checked_add(1)).context("ghost size overflow")?;
            rows.push(row);
        }
    }
    anyhow::ensure!(total > 0, "empty ghost collection");
    let mut names = BufWriter::new(File::create(format!("{}.names.tsv", args.out))?);
    for r in &rows { writeln!(names, "{}\t{}\t{}", r.contig, total-1-r.base-r.len, r.len)?; }
    names.flush()?;
    let layout = Arc::new(Layout { rows, total }); let mut workers = Vec::new();
    let mut joins = Vec::new();
    for _ in 0..args.workers {
        let (tx, rx) = sync_channel::<Job>(8); workers.push(tx);
        let mut worker = Worker { archive: args.archive.clone(), dec: dec.clone_for_thread()?,
            layout: layout.clone(), cache: VecDeque::new(), decoded: 0 };
        joins.push(std::thread::spawn(move || {
            while let Ok(job) = rx.recv() {
                if let Some(reply) = job.reply {
                    match worker.read(job.offset, job.size) {
                        Ok(data) => reply.data(&data),
                        Err(e) => { eprintln!("ghost read: {e:#}"); reply.error(libc::EIO); }
                    }
                } else { let _ = worker.block(job.offset); }
            }
        }));
    }
    eprintln!("ghost bytes={total} workers={} cache_limit_bytes={} direct_io=true decoder_reset_bytes={RESET_BYTES}",
        args.workers, args.workers as u64 * CACHE_BLOCKS as u64 * BLOCK);
    let result = fuser::mount2(Ghost { layout, workers, next: 0 }, path,
        &[MountOption::RO, MountOption::NoExec, MountOption::NoSuid, MountOption::NoDev,
          MountOption::FSName("agc-ghost".into())]);
    for join in joins { let _ = join.join(); }
    result.context("mount ghost filesystem")
}
