/// Private anonymous scratch: one decoded block, no LRU, locks or shared writes.
/// The retained block is just the current decode; retiring invalidates it.
pub struct WorkerArena {
    memory: memmap2::MmapMut,
    br: usize,
    lens_off: usize,
    current: Option<(usize, u64)>,
    pub decodes: u64,
    pub decode_ns: u64,
}
impl WorkerArena {
    pub fn new(index: &Index) -> Result<Self, String> {
        let br = index.block_runs as usize;
        let lens_off = (br + 3) & !3;
        let bytes = (lens_off + br * 4 + 4095) & !4095;
        Ok(Self { memory: memmap2::MmapMut::map_anon(bytes).map_err(|e| e.to_string())?,
            br, lens_off, current: None, decodes: 0, decode_ns: 0 })
    }
    pub fn bytes(&self) -> usize { self.memory.len() }
    pub fn retire(&mut self) -> Result<(), String> {
        self.current = None;
        // SAFETY: anonymous private mapping, exclusively borrowed; all decoded
        // slices have ended. DONTNEED zeroes pages and leaves the mapping valid.
        let rc = unsafe { madvise(self.memory.as_mut_ptr().cast(), self.memory.len(), 4) };
        if rc == 0 { Ok(()) } else { Err(std::io::Error::last_os_error().to_string()) }
    }
    fn with_block<R>(&mut self, idx: &Index, b: u64, f: impl FnOnce(&[u8], &[u32]) -> R) -> R {
        assert_eq!(self.br, idx.block_runs as usize, "arena block width mismatch");
        assert!(b < idx.nblocks);
        let want = (idx.reader.r - b * idx.block_runs).min(idx.block_runs) as usize;
        let (head_area, len_area) = self.memory.split_at_mut(self.lens_off);
        let heads = &mut head_area[..want];
        // mmap is page aligned, lens_off is aligned, bytes fit and u32 accepts
        // all bit patterns. The disjoint slices never escape the closure.
        let lens = unsafe { std::slice::from_raw_parts_mut(len_area.as_mut_ptr().cast::<u32>(), want) };
        let key = (idx.reader.map.as_ptr() as usize, b);
        if self.current != Some(key) {
            let (off, hb, lb) = idx.dir(b);
            let hn = (hb as u64 + 7) / 8;
            let ln = (lb as u64 + 7) / 8;
            let start = idx.payload_off + off;
            assert!(start + hn + ln <= idx.payload_end);
            let payload = idx.reader.slice(start, hn + ln);
            let decode_start = Instant::now();
            decode_block(&idx.huff, &idx.gamma, &payload[..hn as usize], hb as u64,
                &payload[hn as usize..], lb as u64, want, heads, lens, b);
            self.decode_ns += decode_start.elapsed().as_nanos() as u64;
            self.current = Some(key);
            self.decodes += 1;
        }
        f(heads, lens)
    }
}
impl Drop for WorkerArena {
    fn drop(&mut self) { let _ = self.retire(); }
}
impl Index {
    /// No shared decoded cache is allocated. Queries only read this index.
    pub fn open_stateless(reader: Reader) -> Result<Self, String> {
        Self::open_with_cache(reader, 1, 0)
    }
    pub fn worker_ctx<'a>(&'a self, arena: &'a std::cell::RefCell<WorkerArena>) -> Ctx<'a> {
        // A new navigation context cannot inherit a decode from another index,
        // including a mapping subsequently allocated at the same virtual address.
        arena.borrow_mut().current = None;
        let mut cx = self.ctx();
        cx.arena = Some(arena);
        cx
    }
}
/// Stateless count entry point; arena ownership belongs to the calling worker.
pub fn stateless_count(index: &Index, arena: &std::cell::RefCell<WorkerArena>, query: &[u8]) -> u64 {
    if query.iter().any(|&b| index.reader.sigma[b as usize] == 0) { return 0; }
    let work: Vec<u8> = query.iter().rev().map(|&b| index.reader.sigma[b as usize]).collect();
    let (lo, hi) = index.worker_ctx(arena).search(&work);
    hi - lo
}

/// Scaling harness: fixed total positions, disjoint corpus windows, one full
/// next-byte histogram per position, raw rolling context (default 8 bytes).
pub fn stateless_bench_command(args: &[std::ffi::OsString]) {
    if args.len() < 4 { die("stateless-bench CONTAINER CORPUS THREADS POSITIONS [LEFT]"); }
    let path = args[0].to_str().unwrap();
    let corpus_file = File::open(&args[1]).unwrap();
    let corpus = unsafe { memmap2::MmapOptions::new().map(&corpus_file).unwrap() };
    let jobs: usize = args[2].to_str().unwrap().parse().unwrap();
    let positions: usize = args[3].to_str().unwrap().parse().unwrap();
    let left: usize = args.get(4).map(|x| x.to_str().unwrap().parse().unwrap()).unwrap_or(8);
    assert!(jobs > 0 && positions >= jobs && left > 0 && left <= 1024);
    let index = Index::open_stateless(Reader::open(path).unwrap()).unwrap();
    let candidates = Candidates::of(&index);
    let tail = &corpus[(index.n() - 1) as usize - 1024..(index.n() - 1) as usize];
    for query in [b"the ".as_slice(), b"Harper Reed", b" function "] {
        let arena = std::cell::RefCell::new(WorkerArena::new(&index).unwrap());
        eprintln!("STATELESS_COUNT query={:?} count={}", String::from_utf8_lossy(query), stateless_count(&index, &arena, query));
    }
    let anonymous = || -> u64 {
        std::fs::read_to_string("/proc/self/smaps_rollup").unwrap().lines()
            .find(|s| s.starts_with("Anonymous:")).unwrap().split_whitespace().nth(1).unwrap().parse::<u64>().unwrap() * 1024
    };
    let anonymous_before = anonymous();
    let rendezvous = std::sync::Barrier::new(jobs + 1);
    let release = std::sync::Barrier::new(jobs + 1);
    let start = Instant::now();
    let (results, seconds) = std::thread::scope(|scope| {
        let mut handles = Vec::new();
        for job in 0..jobs {
            let (idx, cand, text) = (&index, &candidates, &corpus);
            let (done, release) = (&rendezvous, &release);
            handles.push(scope.spawn(move || {
                let arena = std::cell::RefCell::new(WorkerArena::new(idx).unwrap());
                let mut cx = idx.worker_ctx(&arena);
                let mut pairs = Vec::with_capacity(256);
                let mut checksum = 0u64;
                // Logical positions independent of thread count, spread over 100GB.
                for i in (job..positions).step_by(jobs) {
                    let off = 4096 + (i / 256) * 1000003 + i % 256;
                    assert!(off < idx.n() as usize - 1);
                    let answer = consult_answer(&mut cx, cand, &text[off-left..off], tail, &mut pairs);
                    checksum = checksum.wrapping_add(answer.0);
                }
                let a = arena.borrow();
                let result = (checksum, a.decodes, a.bytes(), a.decode_ns);
                done.wait();
                release.wait();
                (result.0, result.1, result.2, result.3, stateless_worker_stack_rss())
            }));
        }
        rendezvous.wait();
        let elapsed = start.elapsed().as_secs_f64();
        let anon = anonymous();
        eprintln!("STATELESS_MEMORY workers={jobs} anonymous_before_bytes={anonymous_before} anonymous_live_bytes={anon} incremental_anonymous_per_worker_bytes={} query_wall_seconds={elapsed:.6}", anon.saturating_sub(anonymous_before) / jobs as u64);
        release.wait();
        (handles.into_iter().map(|h| h.join().unwrap()).collect::<Vec<_>>(), elapsed)
    });
    let checksum = results.iter().fold(0u64, |sum, r| sum.wrapping_add(r.0));
    let decodes: u64 = results.iter().map(|r| r.1).sum();
    let decode_ns: u64 = results.iter().map(|r| r.3).sum();
    let max_stack = results.iter().map(|r| r.4).max().unwrap();
    eprintln!("STATELESS_WORKER_RSS arena_peak_bytes={} stack_peak_max_bytes={max_stack} stack_plus_arena_peak_max_bytes={} heap_pair_capacity_bytes=4096 context_heap_bytes={left}", results[0].2, max_stack + results[0].2 as u64);
    let status = std::fs::read_to_string("/proc/self/status").unwrap();
    let hwm = status.lines().find(|s| s.starts_with("VmHWM:")).unwrap();
    println!("STATELESS_BENCH threads={jobs} positions={positions} left={left} seconds={seconds:.6} positions_per_s={:.3} checksum={checksum} decodes={decodes} decode_seconds_sum={:.6} worker_arena_bytes={} {hwm}", positions as f64 / seconds, decode_ns as f64 / 1e9, results[0].2);
}

/// Next-byte counts in byte order; the structural END is reported separately.
/// Caller supplies the corpus tail (up to 1024 bytes), an immutable slice.
pub fn stateless_histogram(index: &Index, arena: &std::cell::RefCell<WorkerArena>,
    query: &[u8], corpus_tail: &[u8]) -> ([u64; 256], u64) {
    assert!(query.len() <= 1024);
    if query.is_empty() { return ([0; 256], 0); }
    let cand = Candidates::of(index);
    let mut pairs = Vec::with_capacity(256);
    let (_, eof) = consult_answer(&mut index.worker_ctx(arena), &cand, query, corpus_tail, &mut pairs);
    let mut counts = [0; 256];
    for (b, n) in pairs { counts[b as usize] = n; }
    (counts, eof)
}

#[cfg(test)]
mod stateless_tests {
    use super::*;
    #[test]
    #[ignore = "requires the 100GB pile container; run explicitly"]
    fn pile_counts_histograms_and_retirement() {
        let path = "/mnt/nvme2n1/erikg/P100c/pile-head100.sxi2";
        let idx = Index::open_stateless(Reader::open(path).unwrap()).unwrap();
        assert_eq!(idx.cache_bytes(), 0);
        let arena = std::cell::RefCell::new(WorkerArena::new(&idx).unwrap());
        for (q, n) in [(b"the ".as_slice(), 704942163), (b"Harper Reed", 55), (b" function ", 4785152)] {
            assert_eq!(stateless_count(&idx, &arena, q), n);
            arena.borrow_mut().retire().unwrap();
            assert!(arena.borrow().memory.iter().all(|&b| b == 0));
            assert_eq!(stateless_count(&idx, &arena, q), n);
        }
        assert_eq!(stateless_histogram(&idx, &arena, b"", b""), ([0;256], 0));
        let compat = Index::open(Reader::open(path).unwrap(), 1).unwrap();
        let cand = Candidates::of(&idx);
        let before = compat.cache_stats();
        for q in [b"the ".as_slice(), b"Harper Reed", b" function ", b"unlikely-absent-xyzzy-1234"] {
            let mut expected = Vec::new();
            let answer = consult_answer(&mut compat.ctx(), &cand, q, b"", &mut expected);
            let (counts, eof) = stateless_histogram(&idx, &arena, q, b"");
            assert_eq!(eof, answer.1);
            assert_eq!(counts.iter().sum::<u64>() + eof, answer.0);
            for (b, n) in expected { assert_eq!(counts[b as usize], n); }
        }
        assert_ne!(compat.cache_stats(), before);
        assert_eq!(idx.cache_stats(), (0,0,0,0));
    }
}

/// Benchmark-only attribution: each fresh pthread stack has its own guarded VMA.
/// No stack pages have been retired yet, so its resident set retains the peak
/// touched by this worker. Runs after the timed query, before thread exit.
fn stateless_worker_stack_rss() -> u64 {
    use std::io::BufRead;
    let marker = 0u8;
    let address = &marker as *const u8 as usize;
    let file = File::open("/proc/self/smaps").unwrap();
    let mut found = false;
    for line in std::io::BufReader::new(file).lines() {
        let line = line.unwrap();
        if !found {
            if let Some(range) = line.split_whitespace().next() {
                if let Some((lo, hi)) = range.split_once('-') {
                    if let (Ok(lo), Ok(hi)) = (usize::from_str_radix(lo, 16), usize::from_str_radix(hi, 16)) {
                        found = lo <= address && address < hi;
                    }
                }
            }
        } else if line.starts_with("Rss:") {
            return line.split_whitespace().nth(1).unwrap().parse::<u64>().unwrap() * 1024;
        }
    }
    panic!("worker stack mapping absent from smaps");
}
