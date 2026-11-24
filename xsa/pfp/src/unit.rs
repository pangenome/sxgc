// =============================================================
// AMENDMENT R: THE UNIT - the per-level induction pipeline (the
// pairsbins + the emit, fused as in the C++ B machine).
//
// Per bin: the windowed distribute writes the 8 B (P,o) entries;
// the bucket is read + expanded + sorted (the slice order, the
// (P,o) tie-break - unique keys) + merged into the bin's sorted
// stream + its run segment (the equal-slice groups); the windows
// (mass-cut group ranges within the bin) run the L1 landing walks +
// the B3 cross-bin exchange + the L2 CSR + the L3 resolved merge;
// the assembly emits. DELETE the bin's inputs (B2's deletion law).
// Bin order IS the key order (A3 - no merge).
//
// THE SINKS: level 0 writes the product (the BPS1 piece stream +
// the SXLS lcp slices + the TRIE_TAP band stream); the deeper
// levels write the induced SA column.
// =============================================================
use crate::col::{die, ColMap};
use crate::laws::Laws;
use std::fs;
use std::io::{Read, Write};

pub const SENTINEL_SYMBOL: u8 = 0;

#[derive(Clone, Copy)]
pub struct BandRow {
    pub occ_o: u64,   // (occ << 32) | o
    pub nrank: u32,   // the continuation's aligned rank + 1; 0 = the END
    pub cont4: u32,   // the continuation's first 4 bytes BE (0-pad)
}

#[derive(Clone, Copy)]
pub struct FactRow { pub v: u32, pub o: u32, pub n: u32 }

#[derive(Clone, Copy)]
struct PEnt { p: u32, o: u32, pos: u64, end: u64 }

// THE SINKS
pub enum Sink {
    Product {
        out_prefix: String,
        bands_dir: String,
        remap: Option<Vec<u8>>,
        trie_tap: bool,
    },
    SaColumn {
        out: ColMap,   // the induced SA (u32 positions)
    },
}

pub struct LevelCtx<'a> {
    pub t: &'a [u8],
    pub stride: u32,          // 1 (bytes) | 4 (BE ids)
    pub rows_n: u64,          // the row-coverage count (N at level 0)
    pub file_n: u64,          // the END position (the text length)
    pub pa: &'a [u32],
    pub m: u64,
    pub sa: &'a [u32],
    pub rank: &'a [u32],
    pub dref: &'a [u64],
    pub dlen: &'a [u32],
    pub dict_n: u64,
    pub dict_bytes_total: u64,
    pub pstart: &'a [u64],
    pub ostart: &'a [u32],
    pub olist: &'a [u32],
    pub lcp: &'a [u64],
    pub lcp_tree: &'a [u64],
    pub lcp_seg_n: u64,
    pub modeb_dir: String,
    pub laws: &'a Laws,
}

#[path="v8_piece_pool.rs"]
mod v8_piece_pool;
// ---- the piece cutter (BPS1/PRC1/BPG1, transliterated)
pub struct PieceCutter {
    pool: Option<v8_piece_pool::Pool>,
    pub prefix: String,
    pub dir: String,
    pub n: u64,
    pub window: u64,
    pub width: u32,
    band_size: u64,
    next: u64,
    index: u64,
    band_start: u64,
    // the pending run
    pend_c: u8,
    pend_len: u64,
    pend_head: u64,
    pend_tail: u64,
    pend_head_rank: u64,
    have_pending: bool,
    band: Vec<(u8, u32, u64, u64)>,   // (c, len, head, tail)
    band_base_rank: u64,
    band_cum_len: u64,
    run_exc: Vec<[u64; 3]>,
    counts: [u64; 256],
    frequencies: [u64; 256],
}

impl PieceCutter {
    pub fn new(prefix: &str, n: u64, window: u64) -> PieceCutter {
        Self::with_piece_target(prefix, n, window, None)
    }

    // Gate only: redirect a freshly created pieces directory, not the finish
    // spools. No overwrite, no recursive deletion, and never resume it.
    pub fn with_piece_target(prefix: &str, n: u64, window: u64,
                             volatile_target: Option<&std::path::Path>) -> PieceCutter {
        Self::create(prefix, n, window, volatile_target, false)
    }

    pub fn fixed_v8_pool(prefix: &str, n: u64, window: u64) -> PieceCutter {
        assert_eq!((n, window), (v8_piece_pool::ROWS, 6), "fixed v8 piece pool only");
        Self::create(prefix, n, window, None, true)
    }

    fn create(prefix: &str, n: u64, window: u64, volatile_target: Option<&std::path::Path>, pooled: bool) -> PieceCutter {
        let dir = format!("{}.pieces", prefix);
        let mut pool = None;
        if pooled {
            fs::create_dir(&dir).expect("fenced PRC1 directory must be fresh");
            pool = Some(v8_piece_pool::Pool::fixed(std::path::Path::new(&dir)).expect("fresh PRC1 pool"));
        } else if let Some(target) = volatile_target {
            assert!(target.is_absolute() && target.starts_with("/dev/shm") && target.file_name()==Some("pieces".as_ref()));
            fs::create_dir(target).unwrap_or_else(|_| die("volatile piece target must be fresh"));
            std::os::unix::fs::symlink(target, &dir).unwrap_or_else(|_| die("refusing to replace piece path"));
        } else {
            let _ = fs::remove_dir_all(&dir);
            fs::create_dir_all(&dir).unwrap_or_else(|_| die("create the pieces dir"));
        }
        let width = 64 - (n.leading_zeros()) as u32;
        let mut pc = PieceCutter {
            prefix: prefix.to_string(), dir, n, window, width, pool,
            band_size: 65536, next: 0, index: 0, band_start: 0,
            pend_c: 0, pend_len: 0, pend_head: 0, pend_tail: 0,
            pend_head_rank: 0, have_pending: false,
            band: Vec::new(), band_base_rank: 0, band_cum_len: 0, run_exc: Vec::new(),
            counts: [0; 256], frequencies: [0; 256],
        };
        // the start marker
        let mut b: Vec<u8> = Vec::new();
        b.extend_from_slice(b"BPS1");
        b.extend_from_slice(&n.to_le_bytes());
        b.extend_from_slice(&(pc.width as u64).to_le_bytes());
        b.extend_from_slice(&1u64.to_le_bytes());
        b.extend_from_slice(&pc.window.to_le_bytes());
        let tmp = format!("{}/start.tmp", pc.dir);
        let fin = format!("{}/start", pc.dir);
        fs::write(&tmp, &b).unwrap_or_else(|_| die("write start.tmp"));
        fs::rename(&tmp, &fin).unwrap_or_else(|_| die("publish start"));
        pc
    }

    pub fn feed_row(&mut self, p: u64, c: u8) {
        let row = self.next;
        if self.have_pending && self.pend_c != c { self.flush_run(); }
        if !self.have_pending {
            self.pend_c = c; self.pend_len = 1;
            self.pend_head = p; self.pend_tail = p;
            self.pend_head_rank = row;
            self.have_pending = true;
        } else {
            self.pend_tail = p;
            self.pend_len += 1;
        }
        self.next += 1;
    }

    fn flush_run(&mut self) {
        if !self.have_pending { return; }
        let predicted = self.band_base_rank + self.band_cum_len;
        if predicted != self.pend_head_rank {
            assert!(self.pool.is_none(), "non-contiguous run in fenced v8 stream");
            self.run_exc.push([self.pend_head, self.pend_head_rank,
                               self.pend_head_rank + self.pend_len - 1]);
        }
        self.band_cum_len += self.pend_len;
        assert!(self.pend_len <= u32::MAX as u64, "piece run length overflow");
        self.counts[self.pend_c as usize] += self.pend_len;
        self.frequencies[self.pend_c as usize] += 1;
        self.band.push((self.pend_c, self.pend_len as u32,
                        self.pend_head, self.pend_tail));
        self.index += 1;
        self.have_pending = false;
        self.pend_len = 0;
        if self.index % self.band_size == 0 { self.close_band(); }
    }

    fn flow_wait(&self, band_no: u64) {
        // the finish's piece-window handshake (verbatim: no marker =
        // the window bounds; the standalone shape needs window >
        // the unit count)
        loop {
            let consumed: i64 = {
                let p = format!("{}.runcols.consumed", self.prefix);
                match fs::read_to_string(&p) {
                    Ok(s) => s.trim().parse().unwrap_or(-1),
                    Err(_) => -1,
                }
            };
            let limit = if consumed < 0 {
                self.window - 1
            } else {
                consumed as u64 + self.window
            };
            if band_no <= limit { return; }
            if std::path::Path::new(&self.prefix).with_file_name("modeb/stop.requested").exists() {
                eprintln!("STOP_OBSERVED where=piece-window exit=75");
                std::process::exit(75);
            }
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
    }

    fn close_band(&mut self) {
        let count = self.band.len() as u64;
        if count == 0 { return; }
        let band_no = self.band_start / self.band_size;
        let first_run = self.band_start;
        self.band_base_rank = self.next;
        self.band_cum_len = 0;
        self.flow_wait(band_no);
        // v2_write_piece (PRC1)
        let write_body = |f: &mut dyn Write| -> std::io::Result<()> {
            let mut hdr = [0u8; 44];
            hdr[0..4].copy_from_slice(&0x31435250u32.to_le_bytes());
            hdr[4..12].copy_from_slice(&band_no.to_le_bytes());
            hdr[12..16].copy_from_slice(&0u32.to_le_bytes());
            hdr[16..24].copy_from_slice(&first_run.to_le_bytes());
            hdr[24..32].copy_from_slice(&count.to_le_bytes());
            hdr[32..36].copy_from_slice(&self.width.to_le_bytes());
            hdr[36..44].copy_from_slice(&0u64.to_le_bytes());
            f.write_all(&hdr)?;
            let mut chars = Vec::with_capacity(count as usize);
            let mut lens = Vec::with_capacity(count as usize);
            let mut heads = Vec::with_capacity(count as usize);
            let mut tails = Vec::with_capacity(count as usize);
            for &(c, l, h, tl) in &self.band {
                chars.push(c); lens.push(l); heads.push(h); tails.push(tl);
            }
            f.write_all(&chars)?;
            let mut lb = Vec::with_capacity(count as usize * 4);
            for l in &lens { lb.extend_from_slice(&l.to_le_bytes()); }
            f.write_all(&lb)?;
            let w = self.width as u64;
            let words = ((w * count) + 63) / 64;
            let mut out = vec![0u64; words.max(1) as usize];
            let pack = |v: &[u64], out: &mut [u64]| {
                let mut res: u128 = 0;
                let mut avail = 0u32;
                let mut wi = 0usize;
                for i in 0..count as usize {
                    res |= (v[i] as u128) << avail;
                    avail += w as u32;
                    if avail >= 64 {
                        out[wi] = res as u64; wi += 1;
                        res >>= 64; avail -= 64;
                    }
                }
                if avail > 0 && wi < out.len() { out[wi] = res as u64; }
            };
            pack(&heads, &mut out);
            f.write_all(unsafe {
                std::slice::from_raw_parts(
                    out.as_ptr() as *const u8,
                    out.len() * 8)
            })?;
            pack(&tails, &mut out);
            f.write_all(unsafe {
                std::slice::from_raw_parts(
                    out.as_ptr() as *const u8,
                    out.len() * 8)
            })
        };
        if let Some(pool) = &mut self.pool {
            assert!(self.index <= pool.runs, "fenced v8 run limit before PRC1 growth");
            let bytes = v8_piece_pool::piece_bytes(self.width, count).expect("PRC1 wire geometry");
            pool.publish(band_no, bytes, write_body).unwrap_or_else(|e| die(&format!("PRC1 pool: {e}")));
        } else {
            let tmp = format!("{}/p-{}-0.partial", self.dir, band_no);
            let fin = format!("{}/p-{}-0", self.dir, band_no);
            let mut f = fs::File::create(&tmp).unwrap_or_else(|_| die("create the piece tmp"));
            write_body(&mut f).unwrap_or_else(|_| die("piece write"));
            f.sync_all().unwrap_or_else(|_| die("piece sync"));
            drop(f);
            fs::rename(&tmp, &fin).unwrap_or_else(|_| die("publish piece"));
        }
        // the g-mark (BPG1)
        {
            let mut g: Vec<u8> = Vec::new();
            g.extend_from_slice(b"BPG1");
            g.extend_from_slice(&band_no.to_le_bytes());
            g.extend_from_slice(&count.to_le_bytes());
            g.extend_from_slice(&0u64.to_le_bytes());
            g.extend_from_slice(&0u64.to_le_bytes());
            let tmp = format!("{}/g-tmp-{}", self.dir, band_no);
            let fin = format!("{}/g-{}", self.dir, band_no);
            fs::write(&tmp, &g).unwrap_or_else(|_| die("g tmp"));
            fs::rename(&tmp, &fin).unwrap_or_else(|_| die("g publish"));
        }
        self.band.clear();
        self.band_start = self.index;
    }

    pub fn finish(mut self) {
        if self.next != self.n {
            die(&format!("PIECE_CUTTER: rows_completed={} expected={}",
                         self.next, self.n));
        }
        self.flush_run();
        if !self.band.is_empty() { self.close_band(); }
        if let Some(pool) = self.pool.take() {
            assert_eq!(self.index, pool.runs, "fenced v8 run count");
            pool.retire().expect("retire all private PRC1 slots before terminal");
        }
        let mut terminal = Vec::from(*b"BPT1");
        for field in [(self.index - 1) / self.band_size, self.index, 0, 0] {
            terminal.extend_from_slice(&field.to_le_bytes());
        }
        let exc_path = format!("{}.runexc", self.prefix);
        let mut exc = std::io::BufWriter::new(fs::File::create(exc_path).expect("create runexc"));
        for field in [0x45525853u64, self.run_exc.len() as u64] {
            exc.write_all(&field.to_le_bytes()).expect("runexc header");
        }
        for fields in &self.run_exc {
            for field in fields { exc.write_all(&field.to_le_bytes()).expect("runexc record"); }
        }
        exc.flush().expect("flush runexc");
        let tmp = format!("{}/terminal.tmp", self.dir);
        fs::write(&tmp, terminal).unwrap_or_else(|_| die("write terminal"));
        fs::rename(tmp, format!("{}/terminal", self.dir))
            .unwrap_or_else(|_| die("publish terminal"));
    }
}

#[cfg(test)]
#[path="v8_piece_pool_test.rs"]
mod v8_piece_pool_test;

#[cfg(test)]
mod handoff_test {
    use super::PieceCutter;
    use std::{fs,path::Path};
    #[test]
    fn row43_sink_keeps_wide_piece_endpoints_and_slice_escapes() {
        let id=format!("xsa-row43-test-{}-{}",std::process::id(),std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos());
        let dir=std::env::temp_dir().join(id);fs::create_dir(&dir).unwrap();let path=dir.to_str().unwrap();
        let base=1u64<<32;let n=10_000_000_000u64;let mut c=PieceCutter::new(dir.join("frag").to_str().unwrap(),n+1,16);
        for (p,b) in [(n,42),(base+7,42),(base+3,8),(base+8,8)] {
            let row=crate::overlap_row43::encode(p,0,b);let(p,_,b)=crate::overlap_row43::decode(&row);c.feed_row(p,b as u8);
        }
        // Two 34-bit values span a u64 word boundary in each PRC1 column.
        c.flush_run();c.close_band();let piece=fs::read(dir.join("frag.pieces/p-0-0")).unwrap();
        assert_eq!(u32::from_le_bytes(piece[32..36].try_into().unwrap()),34);
        for (start,expected) in [(54,[n,base+3]),(70,[base+7,base+8])] {
            let bits=u128::from_le_bytes(piece[start..start+16].try_into().unwrap());assert_eq!([(bits&((1<<34)-1)) as u64,(bits>>34) as u64],expected);
        }
        let laws=crate::laws::Laws::new(dir.join("stop").display().to_string(),0,2*n,n,path.into(),path.into(),path.into());
        let (_,wide_lcp,_)=crate::overlap_row43::decode(&crate::overlap_row43::encode(base+2,base+1,9));
        super::emit_lcp_slice(path,0,2,&[0,wide_lcp],base+9,&mut std::collections::VecDeque::new(),16,&laws);
        let slice=fs::read(dir.join("band_0.lcp")).unwrap();let at=|i:usize|u64::from_le_bytes(slice[i..i+8].try_into().unwrap());
        assert_eq!(&slice[48..50],&[0,255]);assert_eq!([at(24),at(32),at(50),at(58)],[base+1,base+9,base+10,base+1]);fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn only_pieces_move_and_their_bytes_are_unchanged() {
        if !Path::new("/dev/shm").is_dir() {return;}
        let id=format!("xsa-piece-test-{}-{}",std::process::id(),std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos());
        let disk=std::env::temp_dir().join(&id); let ram=Path::new("/dev/shm").join(id);
        fs::create_dir(&disk).unwrap(); fs::create_dir(&ram).unwrap(); let pieces=ram.join("pieces");
        for (name,target) in [("normal",None),("volatile",Some(pieces.as_path()))] {
            let prefix=disk.join(name); let mut c=PieceCutter::with_piece_target(prefix.to_str().unwrap(),4,16,target);
            for (p,b) in [(3,3),(2,2),(0,0),(1,1)] {c.feed_row(p,b);} c.finish();
        }
        assert_eq!(fs::read_link(disk.join("volatile.pieces")).unwrap(),pieces);
        for f in ["start","terminal","p-0-0","g-0"] {assert_eq!(fs::read(disk.join("normal.pieces").join(f)).unwrap(),fs::read(pieces.join(f)).unwrap());}
        assert_eq!(fs::read(disk.join("normal.runexc")).unwrap(),fs::read(disk.join("volatile.runexc")).unwrap());
        fs::remove_dir_all(disk).unwrap();fs::remove_dir_all(ram).unwrap();
    }
}

// =============================================================
// THE PIPELINE (the per-level run)
// =============================================================
pub struct UnitStats {
    pub rows_sorted: u64,
    pub runs: u64,
    pub rows_emitted: u64,
    pub end_rows: u64,
    pub bins: u32,
    pub windows: u64,
    pub crossings_in: u64,
    pub crossings_fwd: u64,
}

struct RowMach<'a> {
    c: &'a LevelCtx<'a>,
    position_bounds: bool,
}

impl<'a> RowMach<'a> {
    // THE ROW (AMENDMENT 6's law): 16 bytes
    fn row_of_po(&self, p: u32, o: u32, occ: u32) -> BandRow {
        let c = self.c;
        // THE SOURCE INVARIANT: the occurrence is P's own (the list
        // came from P); the offset is inside the phrase
        if c.pa[occ as usize] != p
            || o as u64 >= c.dlen[p as usize] as u64 {
            die(&format!("ROW_OF_PO: p={} o={} occ={} pa[occ]={} \
                          dlen={} - the (P,o,occ) triple is broken",
                         p, o, occ, c.pa[occ as usize],
                         c.dlen[p as usize]));
        }
        let mut r = BandRow { occ_o: (occ as u64) << 32 | o as u64,
                              nrank: 0, cont4: 0 };
        r.nrank = if (occ as u64) + 1 < c.m {
            c.rank[occ as usize + 1] + 1
        } else { 0 };
        if self.position_bounds {
            let end = c.pstart[occ as usize] + u64::from(c.dlen[p as usize]);
            let bytes = &c.t[end as usize..(end+4).min(c.file_n) as usize];
            let mut word = [0;4]; word[..bytes.len()].copy_from_slice(bytes);
            r.cont4 = u32::from_be_bytes(word);
            return r;
        }
        // the continuation's first 4 bytes (BE; 0-padded), from the
        // next phrases' content at their references
        let mut c4: u32 = 0;
        let mut got = 0u32;
        let mut j2 = occ as u64 + 1;
        while got < 4 {
            if j2 >= c.m { break; }
            let v2 = c.pa[j2 as usize] as usize;
            let len2 = c.dlen[v2] as u32;
            let off2 = c.dref[v2];
            let take = len2.min(4 - got);
            for q in 0..take {
                c4 = (c4 << 8)
                    | c.t[(off2 + q as u64) as usize] as u32;
            }
            got += take;
            j2 += 1;
        }
        while got < 4 { c4 <<= 8; got += 1; }
        r.cont4 = c4;
        r
    }

    fn row_pos(&self, r: &BandRow) -> u64 {
        let occ = (r.occ_o >> 32) as usize;
        self.c.pstart[occ] + (r.occ_o & 0xFFFFFFFF)
    }

    // the aligned-LCP range min over the parse-LCP tree
    // (lcpRange(a, b) inclusive, 1 <= a <= b <= m-1, over L)
    fn lcp_range(&self, a: u64, b: u64) -> u64 {
        let c = self.c;
        if a > b || b == 0 { return 0; }
        // leaves: tree[segN + k] = L[k+1] -> L-index i at leaf i-1
        let mut lo = a - 1 + c.lcp_seg_n;
        let mut hi = b - 1 + c.lcp_seg_n;
        let tree = c.lcp_tree;
        let mut res = u64::MAX;
        while lo <= hi {
            if lo & 1 == 1 { res = res.min(tree[lo as usize]); lo += 1; }
            if hi & 1 == 0 { res = res.min(tree[hi as usize]); hi -= 1; }
            lo >>= 1; hi >>= 1;
            if lo == 0 { break; }
        }
        if res == u64::MAX { 0 } else { res }
    }

    // THE WALK: the row-value compare from the true state (the
    // rank-jump into the aligned order; the walk returns 2 = the
    // full-suffix fallback)
    fn walk(&self, mut pa: u64, mut ea: u64, mut rank_a: u64,
            mut pb: u64, mut eb: u64, mut rank_b: u64) -> i32 {
        let c = self.c;
        loop {
            let rem_a = ea - pa;
            let rem_b = eb - pb;
            let common = rem_a.min(rem_b);
            if common > 0 {
                let x = &c.t[pa as usize..(pa + common) as usize];
                let y = &c.t[pb as usize..(pb + common) as usize];
                match x.cmp(y) {
                    std::cmp::Ordering::Less => return -1,
                    std::cmp::Ordering::Greater => return 1,
                    std::cmp::Ordering::Equal => {}
                }
                pa += common; pb += common;
            }
            let da = pa == ea;
            let db = pb == eb;
            if da && db {
                return if rank_a != rank_b {
                    if rank_a < rank_b { -1 } else { 1 }
                } else { 2 };
            }
            if da {
                if rank_a == 0 { return -1; }
                let pos = c.sa[(rank_a - 1) as usize];
                let v = c.pa[pos as usize];
                pa = c.dref[v as usize];
                ea = pa + c.dlen[v as usize] as u64;
                rank_a = if (pos as u64) + 1 < c.m {
                    c.rank[pos as usize + 1] as u64 + 1
                } else { 0 };
            } else {
                if rank_b == 0 { return 1; }
                let pos = c.sa[(rank_b - 1) as usize];
                let v = c.pa[pos as usize];
                pb = c.dref[v as usize];
                eb = pb + c.dlen[v as usize] as u64;
                rank_b = if (pos as u64) + 1 < c.m {
                    c.rank[pos as usize + 1] as u64 + 1
                } else { 0 };
            }
        }
    }

    // the full-suffix compare (suf_cmp_from: remap is an
    // order-preserving bijection - the raw bytes give the same order)
    fn suf_cmp(&self, a: u64, b: u64, from: u64) -> i32 {
        let c = self.c;
        let cap = (c.file_n - a).min(c.file_n - b);
        if from >= cap {
            if a == b { return 0; }
            return if (c.file_n - a) < (c.file_n - b) { -1 } else { 1 };
        }
        let mut i = from;
        while i < cap {
            let xb = c.t[(a + i) as usize];
            let yb = c.t[(b + i) as usize];
            if xb != yb { return if xb < yb { -1 } else { 1 }; }
            i += 1;
        }
        if a == b { 0 }
        else if (c.file_n - a) < (c.file_n - b) { -1 } else { 1 }
    }

    fn suf_lcp(&self, a: u64, b: u64, from: u64, lcp: &mut u64) -> i32 {
        // THE WORD-CHUNK LAW (the C++ suf_cmp_from_counted shape): the
        // residual LCP walk goes 8 bytes at a time
        let c = self.c;
        let cap = (c.file_n - a).min(c.file_n - b);
        *lcp = from;
        let mut i = from;
        while i + 8 <= cap {
            let x: [u8; 8] = c.t[(a + i) as usize..(a + i + 8) as usize]
                .try_into().unwrap();
            let y: [u8; 8] = c.t[(b + i) as usize..(b + i + 8) as usize]
                .try_into().unwrap();
            if x != y {
                for j in 0..8usize {
                    if x[j] != y[j] {
                        *lcp = i + j as u64;
                        return if x[j] < y[j] { -1 } else { 1 };
                    }
                }
            }
            i += 8;
        }
        while i < cap {
            let xb = c.t[(a + i) as usize];
            let yb = c.t[(b + i) as usize];
            if xb != yb { *lcp = i; return if xb < yb { -1 } else { 1 }; }
            i += 1;
        }
        *lcp = cap;
        if a == b { 0 }
        else if (c.file_n - a) < (c.file_n - b) { -1 } else { 1 }
    }

    // THE INDUCED COMPARATOR (AMENDMENT 6 + the W5 bounds)
    fn induced_cmp(&self, a: &BandRow, b: &BandRow) -> i32 {
        let c = self.c;
        if a.occ_o == b.occ_o { return 0; }
        let occ_a = (a.occ_o >> 32) as usize;
        let occ_b = (b.occ_o >> 32) as usize;
        let oa = a.occ_o as u32;
        let ob = b.occ_o as u32;
        let va = c.pa[occ_a] as usize;
        let vb = c.pa[occ_b] as usize;
        let pa = c.dref[va] + oa as u64;
        let ra = c.dlen[va] as u64 - oa as u64;
        let pb = c.dref[vb] + ob as u64;
        let rb = c.dlen[vb] as u64 - ob as u64;
        if va == vb {
            // the same phrase: registers only
            if oa == ob {
                return if a.nrank < b.nrank { -1 }
                       else if a.nrank > b.nrank { 1 } else { 0 };
            }
            if ra > c.dlen[va] as u64 || rb > c.dlen[vb] as u64 {
                die(&format!("INDUCED_BAD_ROW: occA={} oA={} va={} \
                              dlenA={} occB={} oB={} vb={} dlenB={}",
                             occ_a, oa, va, c.dlen[va],
                             occ_b, ob, vb, c.dlen[vb]));
            }
            let cc = ra.min(rb);
            if cc > 0 {
                match c.t[pa as usize..(pa + cc) as usize]
                    .cmp(&c.t[pb as usize..(pb + cc) as usize]) {
                    std::cmp::Ordering::Less => return -1,
                    std::cmp::Ordering::Greater => return 1,
                    std::cmp::Ordering::Equal => {}
                }
            }
            if ra < rb {
                if a.nrank == 0 { return -1; }
                let w = self.walk(pa, pa, a.nrank as u64,
                                  pb + cc, pb + rb, b.nrank as u64);
                return if w == 2 {
                    self.suf_cmp(self.row_pos(a), self.row_pos(b), 0)
                } else { w };
            }
            if b.nrank == 0 { return 1; }
            let w = self.walk(pa + cc, pa + ra, a.nrank as u64,
                              pb, pb, b.nrank as u64);
            return if w == 2 {
                self.suf_cmp(self.row_pos(a), self.row_pos(b), 0)
            } else { w };
        }
        // DIFFERENT PHRASES: the 16-byte fast path
        let c0 = ra.min(rb).min(16);
        if c0 > 0 {
            match c.t[pa as usize..(pa + c0) as usize]
                .cmp(&c.t[pb as usize..(pb + c0) as usize]) {
                std::cmp::Ordering::Less => return -1,
                std::cmp::Ordering::Greater => return 1,
                std::cmp::Ordering::Equal => {}
            }
        }
        if ra > 16 || rb > 16 {
            return self.walk(pa, pa + ra, a.nrank as u64,
                             pb, pb + rb, b.nrank as u64);
        }
        if ra == rb {
            if a.cont4 != b.cont4 {
                return if a.cont4 < b.cont4 { -1 } else { 1 };
            }
            if a.nrank != b.nrank {
                return if a.nrank < b.nrank { -1 } else { 1 };
            }
            return self.suf_cmp(self.row_pos(a), self.row_pos(b), 0);
        }
        if ra < rb {
            if a.nrank == 0 { return -1; }
            let q = pb + ra;
            let rem_b = rb - ra;
            let c1 = rem_b.min(4);
            for j in 0..c1 {
                let xa = (a.cont4 >> (8 * (3 - j))) as u8;
                let yb = c.t[(q + j) as usize];
                if xa != yb {
                    return if xa < yb { -1 } else { 1 };
                }
            }
            return self.walk(pa, pa + ra, a.nrank as u64,
                             pb, pb + rb, b.nrank as u64);
        }
        if b.nrank == 0 { return 1; }
        let q = pa + rb;
        let rem_a = ra - rb;
        let c1 = rem_a.min(4);
        for j in 0..c1 {
            let yb = (b.cont4 >> (8 * (3 - j))) as u8;
            let xa = c.t[(q + j) as usize];
            if xa != yb {
                return if xa < yb { -1 } else { 1 };
            }
        }
        self.walk(pa, pa + ra, a.nrank as u64, pb, pb + rb, b.nrank as u64)
    }

    // THE BOUNDARY COMPARE: the row's VALUE vs the boundary pair's
    // slice (used ONLY in the landing walk)
    fn cmp_bound(&self, a: &BandRow, pbx: u64, eb: u64) -> i32 {
        let c = self.c;
        let occ_a = (a.occ_o >> 32) as usize;
        if self.position_bounds {
            // Compare the occurrence's bounded prefix at its text position, not
            // via rank -> SA -> parse ID -> reference chains on every probe.
            let pos = self.row_pos(a) as usize;
            let bound = &c.t[pbx as usize..eb as usize];
            let suffix = &c.t[pos..c.file_n as usize];
            let k = suffix.len().min(bound.len());
            return match suffix[..k].cmp(&bound[..k]) {
                std::cmp::Ordering::Less => -1, std::cmp::Ordering::Greater => 1,
                std::cmp::Ordering::Equal if k < bound.len() => -1,
                std::cmp::Ordering::Equal if k == suffix.len() && a.nrank == 0 => 0,
                std::cmp::Ordering::Equal => 1,
            };
        }
        let va = c.pa[occ_a] as usize;
        let pa = c.dref[va] + (a.occ_o & 0xFFFFFFFF);
        let ra = c.dlen[va] as u64 - (a.occ_o & 0xFFFFFFFF);
        let cc = ra.min(eb - pbx);
        if cc > 0 {
            match c.t[pa as usize..(pa + cc) as usize]
                .cmp(&c.t[pbx as usize..(pbx + cc) as usize]) {
                std::cmp::Ordering::Less => return -1,
                std::cmp::Ordering::Greater => return 1,
                std::cmp::Ordering::Equal => {}
            }
        }
        if ra == eb - pbx {
            return if a.nrank == 0 { 0 } else { 1 };
        }
        if ra > eb - pbx { return 1; }
        // ra < the boundary: the continuation vs the boundary's tail
        let mut rank_a = a.nrank as u64;
        let mut q = pbx + cc;
        let q_end = pbx + (eb - pbx);
        loop {
            if rank_a == 0 { return -1; }
            let pos = c.sa[(rank_a - 1) as usize];
            let v = c.pa[pos as usize] as usize;
            let p_p = c.dref[v];
            let r_p = c.dlen[v] as u64;
            let c2 = r_p.min(q_end - q);
            if c2 > 0 {
                match c.t[p_p as usize..(p_p + c2) as usize]
                    .cmp(&c.t[q as usize..(q + c2) as usize]) {
                    std::cmp::Ordering::Less => return -1,
                    std::cmp::Ordering::Greater => return 1,
                    std::cmp::Ordering::Equal => {}
                }
            }
            q += c2;
            if q == q_end { return 1; }
            rank_a = if (pos as u64) + 1 < c.m {
                c.rank[pos as usize + 1] as u64 + 1
            } else { 0 };
        }
    }
}

// the pair's cell (the first 2 bytes of the slice at the strided
// offset; b1 = 0 marks the 1-element tail)
fn cell_of(t: &[u8], p0: u64, o: u64, plen: u32, _stride: u32)
           -> usize {
    // the slice's first 2 bytes; b1 = 0 marks the 1-element tail
    // (the next byte must be INSIDE the phrase - the reference)
    let a = t[(p0 + o) as usize] as usize;
    let b = if o + 1 < plen as u64 {
        t[(p0 + o + 1) as usize] as usize
    } else { 0 };
    (a << 8) | b
}

// THE WINDOWED DISTRIBUTE: one window of bins live at a time
fn distribute_window(ctx: &LevelCtx, w_first: u32, w_last: u32,
                     cell_bin: &[u32; 65536]) {
    let t0 = std::time::Instant::now();
    let mut bfs: Vec<Option<std::io::BufWriter<fs::File>>> =
        Vec::with_capacity((w_last - w_first) as usize);
    for i in w_first..w_last {
        let p = format!("{}/pbin_{}.bin", ctx.modeb_dir, i);
        let f = fs::File::create(&p)
            .unwrap_or_else(|_| die("open pair bin"));
        bfs.push(Some(std::io::BufWriter::with_capacity(1 << 18, f)));
    }
    let mut wrote = 0u64;
    let physical = crate::laws::env_u64("XSA_SCAN_PHYSICAL", 0) == 1;
    crate::scan_order::phrases(physical, ctx.t.len(), ctx.dref, ctx.dlen, ctx.pa, ctx.pstart, |v,p0,len| {
        for o in (0..len as u64).step_by(ctx.stride as usize) {
            let cell = cell_of(ctx.t, p0, o, len, ctx.stride);
            let b = cell_bin[cell];
            if b < w_first || b >= w_last { continue; }
            let rec = [(v as u32).to_le_bytes(),
                        (o as u32).to_le_bytes()];
            let f = bfs[(b - w_first) as usize]
                .as_mut().unwrap();
            f.write_all(&rec[0]).unwrap_or_else(|_| die("pair bin write"));
            f.write_all(&rec[1]).unwrap_or_else(|_| die("pair bin write"));
            wrote += 1;
        }
    });
    for f in bfs.iter_mut() {
        f.take().unwrap().flush().unwrap_or_else(|_| die("flush pair bin"));
    }
    eprintln!("SS_PAIR_DISTRIBUTE window={}..{} bytes={} pass_s={:.2} \
               rule=windowed_regenerate;never_all_materialized",
              w_first, w_last - 1, wrote * 8,
              crate::laws::now_s(t0));
}

// Keep the four u32 wire fields, but append whole batches, not four syscalls/row.
fn append_arrivals(path: &str, arrivals: &[(u64, u32, u32, u32)]) {
    let file = fs::OpenOptions::new().create(true).append(true).open(path)
        .unwrap_or_else(|_| die("open arrival file"));
    let mut f = std::io::BufWriter::new(file);
    for &(g, p, o, oi) in arrivals {
        for field in [u32::try_from(g).expect("arrival group exceeds u32"), p, o, oi] {
            f.write_all(&field.to_le_bytes()).expect("arrival write");
        }
    }
    f.flush().expect("flush arrivals");
}

// THE FULL PER-LEVEL RUN
pub fn run_level(ctx: &mut LevelCtx, sink: &mut Sink) -> UnitStats {
    let t_all = std::time::Instant::now();
    let stride = ctx.stride;
    let trace_io = crate::laws::env_u64("XSA_TRACE_BIN_IO", 0) != 0;
    let integer_landing = crate::laws::env_u64("XSA_TEST_INTEGER_LANDING", 0) == 1;
    let (mut fixed_rows, mut searched_rows, mut fixed_groups, mut searched_groups) = (0u64,0u64,0u64,0u64);
    let mut owner_inbound = 0u64;
    let merge_positions = crate::laws::env_u64("XSA_TEST_POSITION_MERGE", 0) == 1;
    // Marks start (not end) of an interval. Deltas include all Rayon workers.
    let mark = |bin: u32, stage: &str| {
        if trace_io {
            let io = fs::read_to_string("/proc/self/io").expect("Linux process IO counters");
            let rb: u64 = io.lines().find_map(|s| s.strip_prefix("read_bytes: ")).unwrap().parse().unwrap();
            eprintln!("SS_BIN_IO level_stride={} bin={} stage={} rb={} sec={:.6}", stride, bin, stage, rb, t_all.elapsed().as_secs_f64());
        }
    };
    let laws = ctx.laws;
    laws.stop_check("unit-enter");
    // ---- (a) THE CENSUS: the cells' exact pair masses
    let t0 = std::time::Instant::now();
    let mut hist_b = [0u64; 65536];
    {
        let physical = crate::laws::env_u64("XSA_SCAN_PHYSICAL", 0) == 1;
        crate::scan_order::phrases(physical, ctx.t.len(), ctx.dref, ctx.dlen, ctx.pa, ctx.pstart, |_,p0,len| {
            for o in (0..len as u64).step_by(stride as usize) {
                hist_b[cell_of(ctx.t, p0, o, len, stride)] += 1;
            }
        });
    }
    eprintln!("SS_PAIR_CENSUS census_s={:.2} dict_bytes={} \
               rule=exact_cell_mass",
              crate::laws::now_s(t0), ctx.dict_bytes_total);
    // ---- (b) THE BIN MAP (the census draws enough bins to fit the
    //      budget; cells never split; never first-byte)
    let budget = laws.scratch_budget;
    let sort_ahead = 4u64;
    let bin_target = ((budget / 5 * 2) / 280).max(1024);
    let mut cell_bin = [0u32; 65536];
    let mut bin_mass: Vec<u64> = Vec::new();
    let mut pbk = 0u32;
    {
        let mut run2 = 0u64;
        bin_mass.push(0);
        for cell in 0..65536usize {
            run2 += hist_b[cell];
            cell_bin[cell] = pbk;
            bin_mass[pbk as usize] += hist_b[cell];
            if run2 >= bin_target {
                run2 = 0;
                pbk += 1;
                bin_mass.push(0);
            }
        }
        pbk += 1;
        bin_mass.truncate(pbk as usize);
        if pbk > 1 && bin_mass.last() == Some(&0) {
            pbk -= 1;
            bin_mass.pop();
            for b in cell_bin.iter_mut() { *b = (*b).min(pbk - 1); }
        }
    }
    // ---- the byte-law windows (census-exact; the rolling-4
    //      sort-ahead bound)
    let pairs_allowance = budget / 5 * 2;
    let mut win_start_of: Vec<u32> = Vec::with_capacity(pbk as usize);
    let mut window_count = 0u64;
    {
        let mut acc = 0u64;
        let mut max_m = 0u64;
        let mut w_first = 0u32;
        for k in 0..pbk as usize {
            let mm = bin_mass[k];
            if k as u32 > w_first
                && acc + mm * 8 + (mm.max(max_m).min(pairs_allowance / 8)) * 0
                   + (mm + max_m) * 24 > pairs_allowance {
                for _q in w_first..k as u32 {
                    win_start_of.push(w_first);
                }
                w_first = k as u32;
                acc = 0; max_m = 0;
                window_count += 1;
            }
            acc += mm * 8;
            if mm > max_m { max_m = mm; }
        }
        for _q in w_first..pbk {
            win_start_of.push(w_first);
        }
        window_count += 1;
    }
    eprintln!("SS_PAIR_WINDOWS windows={} pairs_allowance={} \
               max_bin_mass={} rule=census_exact_byte_law",
              window_count, pairs_allowance,
              bin_mass.iter().copied().max().unwrap_or(0));
    eprintln!("SS_PAIR_BINS target_per_bin={} bins={} windows={} \
               sort_ahead={} scratch_budget={} \
               rule=amendment_b_census_drawn_cell_ranges;cells_never_split;\
               never_first_byte;bin_order_is_key_order;no_merge",
              bin_target, pbk, window_count, sort_ahead, budget);
    laws.scratch_gate("pairs_plan");
    // ---- the run state
    let mach = RowMach { c: ctx, position_bounds: crate::laws::env_u64("XSA_TEST_POSITION_BOUNDS", 0) == 1 };
    let mut cutter: Option<PieceCutter> = match sink {
        Sink::Product { ref out_prefix, .. } => {
            let window = crate::laws::env_u64("CROSS_V2_PIECE_WINDOW", 1100);
            Some(PieceCutter::new(out_prefix, ctx.rows_n + 1, window))
        }
        Sink::SaColumn { .. } => None,
    };
    let mut sa_sink_n: u64 = 0;
    let mut stats = UnitStats { rows_sorted: 0, runs: 0,
        rows_emitted: 0, end_rows: 0, bins: pbk, windows: 0,
        crossings_in: 0, crossings_fwd: 0 };
    let mut prev_emitted_pos: u64 = u64::MAX;
    let mut pending_head = true;
    let lcp_slice_window =
        crate::laws::env_u64("SS_LCP_SLICE_WINDOW", 1000000).max(1);
    let mut lcp_live: std::collections::VecDeque<u64> =
        std::collections::VecDeque::new();
    let mut next_unit: u64 = 0;
    // Product ranks include the virtual END; rows_emitted counts text rows only.
    let mut product_rank: u64 = 0;
    let trie_tap = std::env::var("TRIE_TAP").is_ok();
    let bands_dir = match sink {
        Sink::Product { ref bands_dir, .. } => bands_dir.clone(),
        Sink::SaColumn { .. } => String::new(),
    };
    let remap: Option<Vec<u8>> = match sink {
        Sink::Product { ref remap, .. } => remap.clone(),
        Sink::SaColumn { .. } => None,
    };
    let head_pos = ctx.file_n;   // the virtual END row first

    // Only a one-byte dictionary tail lacks byte two of its ownership key.
    // The occurrence-list index alone recovers the phrase and its tail offset.
    let tail_owner = |cell: usize, occ: u32, len: u32| -> u32 {
        let end = ctx.pstart[occ as usize] + u64::from(len);
        cell_bin[cell | ctx.t.get(end as usize).copied().unwrap_or(0) as usize]
    };
    let mut moved = 0u64;
    if stride == 1 {
        let mut buffers = vec![Vec::<u8>::new(); pbk as usize];
        let flush = |b: usize, buf: &mut Vec<u8>| {
            if buf.is_empty() { return; }
            let path = format!("{}/arr_owner{}.bin", ctx.modeb_dir,b);
            fs::OpenOptions::new().append(true).create(true).open(path).expect("open ownership file")
                .write_all(buf).expect("write owner indices");
            buf.clear();
        };
        for p in 0..ctx.dict_n as usize {
            let len = ctx.dlen[p];
            let cell = (ctx.t[(ctx.dref[p]+u64::from(len)-1) as usize] as usize)<<8;
            if cell_bin[cell] == cell_bin[cell|255] { continue; }
            for oi in ctx.ostart[p]..ctx.ostart[p+1] {
                let dst = tail_owner(cell,ctx.olist[oi as usize],len);
                if dst != cell_bin[cell] {
                    let buf = &mut buffers[dst as usize];
                    buf.extend_from_slice(&oi.to_le_bytes()); moved+=1;
                    if buf.len() >= 8192 { flush(dst as usize, buf); }
                }
            }
        }
        for (b, buf) in buffers.iter_mut().enumerate() { flush(b,buf); }
    }
    laws.scratch_gate("final_owners");
    eprintln!("SS_FINAL_OWNERS moved={} routed_bytes={} rule=partition_before_emit", moved, moved*4);
    // ---- (c) THE BIN LOOP
    let mut bin_iter: u64 = 0;
    for bin_idx in 0..pbk {
        bin_iter += 1;
        if bin_iter > pbk as u64 + 1 {
            die(&format!("BIN_LOOP_STALL: iter={} pbk={} - the bin \
                          loop cannot exceed its census", bin_iter, pbk));
        }
        eprintln!("SS_BIN_START bin={} of {} rule=bin_order_is_key_order",
                  bin_idx, pbk);
        laws.stop_check("bin");
        let w_end = {
            let w0 = win_start_of[bin_idx as usize];
            let mut e = bin_idx + 1;
            while (e as usize) < pbk as usize
                && win_start_of[e as usize] == w0 { e += 1; }
            e as u32
        };
        if win_start_of[bin_idx as usize] == bin_idx {
            mark(bin_idx,"distribute");
            distribute_window(ctx, bin_idx, w_end, &cell_bin);
        }
        mark(bin_idx,"expand");
        // read + expand the bucket
        let bp = format!("{}/pbin_{}.bin", ctx.modeb_dir, bin_idx);
        let n_ent = match fs::metadata(&bp) {
            Ok(md) => md.len() / 8,
            Err(_) => 0,
        };
        if n_ent == 0 {
            let _ = fs::remove_file(&bp);
            continue;
        }
        let mut raw = Vec::with_capacity(n_ent as usize * 8);
        {
            let mut f = fs::File::open(&bp)
                .unwrap_or_else(|_| die("open the bucket"));
            f.read_to_end(&mut raw)
                .unwrap_or_else(|_| die("read the bucket"));
        }
        let _ = fs::remove_file(&bp);
        // THE SORT (the slice order; the (P,o) tie-break = unique keys)
        let mut ent: Vec<PEnt> = Vec::with_capacity(n_ent as usize);
        for r in 0..n_ent as usize {
            let p = u32::from_le_bytes(
                raw[r * 8..r * 8 + 4].try_into().unwrap()) as usize;
            let o = u32::from_le_bytes(
                raw[r * 8 + 4..r * 8 + 8].try_into().unwrap());
            let p0 = ctx.dref[p];
            ent.push(PEnt { p: p as u32, o, pos: p0 + o as u64,
                             end: p0 + ctx.dlen[p] as u64 });
        }
        drop(raw);
        {
            let t = ctx.t;
            use rayon::prelude::*;
            mark(bin_idx,"sort");
            ent.par_sort_unstable_by(|a, b| {
                let ra = a.end - a.pos;
                let rb = b.end - b.pos;
                let cc = (ra.min(rb)) as usize;
                let c = t[a.pos as usize..a.pos as usize + cc]
                    .cmp(&t[b.pos as usize..b.pos as usize + cc]);
                if c != std::cmp::Ordering::Equal { return c; }
                ra.cmp(&rb).then(a.p.cmp(&b.p)).then(a.o.cmp(&b.o))
            });
        }
        // THE RUN SEGMENT (the equal-slice groups) + the seam belt
        mark(bin_idx,"group");
        let mut runs: Vec<u64> = Vec::new();
        {
            let mut prev: Option<&PEnt> = None;
            for e in &ent {
                let same = prev.map_or(false, |q| {
                    let ra = q.end - q.pos;
                    let rb = e.end - e.pos;
                    ra == rb && ctx.t[q.pos as usize..q.end as usize]
                        == ctx.t[e.pos as usize..e.end as usize]
                });
                if !same {
                    runs.push(0);
                }
                *runs.last_mut().unwrap() += 1;
                prev = Some(e);
            }
        }
        // the bin's stream: the entries; the group starts = prefix sums
        let mut starts: Vec<u64> = Vec::with_capacity(runs.len() + 1);
        {
            let mut acc = 0u64;
            starts.push(0);
            for r in &runs { acc += r; starts.push(acc); }
        }
        stats.rows_sorted += n_ent;
        stats.runs += runs.len() as u64;
        // ---- THE WINDOWS within the bin (the mass pass)
        let win_rows_target = (laws.dial_bytes / 2 / 128).max(4096)
            .min(crate::laws::env_u64("XSA_TEST_EMIT_ROWS", u64::MAX).max(4096));
        let mut win_g: Vec<u64> = vec![0];
        {
            let mut cum = 0u64;
            let bin_g_end = runs.len() as u64;
            for g in 0..bin_g_end {
                for mi in starts[g as usize]
                              ..starts[g as usize + 1] {
                    let v = ent[mi as usize].p as usize;
                    cum += ctx.ostart[v + 1] as u64
                         - ctx.ostart[v] as u64;
                }
                if cum >= win_rows_target {
                    win_g.push(g + 1);
                    cum = 0;
                }
            }
            win_g.push(bin_g_end);
        }
        // the bin pre-pass (B3): re-park the previous bins' rows
        // that landed beyond their bin ends
        mark(bin_idx,"inbound");
        let mut bin_inb: Vec<Vec<(u64, u32, u32, u32)>> =
            vec![Vec::new(); win_g.len() - 1];
        let owner_path = format!("{}/arr_owner{}.bin", ctx.modeb_dir, bin_idx);
        if std::path::Path::new(&owner_path).exists() {
            let bytes = fs::read(&owner_path).expect("read final owners");
            assert_eq!(bytes.len()%4,0);
            for rec in bytes.chunks_exact(4) {
                let oi = u32::from_le_bytes(rec.try_into().unwrap());
                let occ = ctx.olist[oi as usize];
                let p = ctx.pa[occ as usize]; let o = ctx.dlen[p as usize]-1;
                let row = mach.row_of_po(p,o,occ);
                let lg = gallop(&mach,&row,0,runs.len() as u64,&starts,&ent);
                let w = win_g.partition_point(|&g| g <= lg)-1;
                bin_inb[w].push((lg,p,o,oi));
                owner_inbound += 1;
            }
            fs::remove_file(owner_path).expect("release final owners");
        }
        {
            let ip = format!("{}/arr_b{}.bin", ctx.modeb_dir, bin_idx);
            if let Ok(mut f) = fs::File::open(&ip) {
                let mut raw = Vec::new();
                f.read_to_end(&mut raw)
                    .unwrap_or_else(|_| die("read the bin arrivals"));
                let _ = fs::remove_file(&ip);
                let mut re_park: Vec<(u64, u32, u32, u32)> = Vec::new();
                let bin_g_end = runs.len() as u64;
                for r in (0..raw.len() / 16).map(|q| q * 16) {
                    let lg = u32::from_le_bytes(
                        raw[r..r + 4].try_into().unwrap());
                    let p = u32::from_le_bytes(
                        raw[r + 4..r + 8].try_into().unwrap());
                    let o = u32::from_le_bytes(
                        raw[r + 8..r + 12].try_into().unwrap());
                    let occ_lo = u32::from_le_bytes(
                        raw[r + 12..r + 16].try_into().unwrap());
                    let occ = ctx.olist[occ_lo as usize];
                    if ctx.pa[occ as usize] != p {
                        let kp = format!("/tmp/tr_smoke/arrb_bad.bin");
                        let _ = std::fs::write(&kp, &raw);
                        die(&format!("ARRB_BAD: bin={} rec={} of {} \
                                      lg={} p={} o={} occ_lo={} \
                                      pa[olist[occ_lo]]={} - dumped {}",
                                     bin_idx, r / 16, raw.len() / 16,
                                     lg, p, o, occ_lo,
                                     ctx.pa[occ as usize], kp));
                    }
                    let row = mach.row_of_po(p, o, occ);
                    // THE HEAD BELT (the C++ shape: BOTH terms)
                    {
                        let hd = &ent[starts[0] as usize];
                        let passes0 = {
                            if 1 >= bin_g_end { true }
                            else {
                                let nx = &ent[starts[1] as usize];
                                mach.cmp_bound(&row, nx.pos, nx.end) < 0
                            }
                        };
                        if mach.cmp_bound(&row, hd.pos, hd.end) < 0
                            && !passes0 {
                            die("BIN_ARRIVAL_ORDER: a re-parked row sorts \
                                 before the bin head");
                        }
                    }
                    // the gallop from the bin head
                    let lg2 = gallop(&mach, &row, 0, bin_g_end, &starts,
                                    &ent);
                    // the resolve-vs-repark test (the exact cell test)
                    let mut re = false;
                    if lg2 + 1 >= bin_g_end && (bin_idx as u64) + 1
                        < pbk as u64 {
                        let plen = ctx.dlen[p as usize];
                        if (o as u64) + 1 >= plen as u64 {
                            let c0 = ctx.t[ctx.dref[p as usize]
                                           as usize + o as usize];
                            let tcell = ((c0 as u64) << 8)
                                | (row.cont4 >> 24) as u64;
                            let tb = if tcell < 65536 {
                                cell_bin[tcell as usize]
                            } else { 0 };
                            if tb as u64 > bin_idx as u64 { re = true; }
                        }
                    }
                    if re { re_park.push((lg as u64, p, o, occ_lo)); }
                    else {
                        let k3 = win_g.iter()
                            .filter(|&&w| w <= lg2).count() - 1;
                        bin_inb[k3].push((lg2 as u64, p, o, occ_lo));
                        stats.crossings_in += 1;
                    }
                }
                if !re_park.is_empty() {
                    let np = format!("{}/arr_b{}.bin",
                                     ctx.modeb_dir, bin_idx + 1);
                    append_arrivals(&np, &re_park);
                    stats.crossings_fwd += re_park.len() as u64;
                }
            }
        }
        // ---- per window: L1 + L2 + L3 + the assembly
        for w_idx in 0..win_g.len() - 1 {
            mark(bin_idx,"landing");
            laws.stop_check("window");
            let g0 = win_g[w_idx];
            let g1 = win_g[w_idx + 1];
            let bin_g_end = runs.len() as u64;
            // ---- L1: the landing walk over the window's groups
            struct Own { pos: Vec<u64>, ids: Vec<u64>, occ: Vec<u32>,
                         facts: Vec<FactRow>, cnt: Vec<u32> }
            let mut own = Own { pos: Vec::new(), ids: Vec::new(),
                                occ: Vec::new(), facts: Vec::new(),
                                cnt: vec![0; (g1 - g0) as usize] };
            let mut cross: Vec<(u64, u32, u32, u32)> = Vec::new();
            let mut fwd: Vec<(u64, u32, u32, u32)> = Vec::new();
            let mut bin_fwd: Vec<(u64, u32, u32, u32)> = Vec::new();
            let mut rows_total = 0u64;
            let l1_t0 = std::time::Instant::now();
            let mut l1_rows: u64 = 0;
            let mut l1_gdone: u64 = 0;
            let l1_mark = std::time::Instant::now();
            let mut l1_lastmark = std::time::Duration::ZERO;
            for g in g0..g1 {
                l1_gdone += 1;
                if l1_mark.elapsed() >= l1_lastmark
                    + std::time::Duration::from_secs(30) {
                    l1_lastmark = l1_mark.elapsed();
                    eprintln!("SS_L1_PROGRESS groups={}/{} rows={} \
                               elapsed_s={:.1} \
                               rule=the_walk_is_linear;stalls_die",
                              l1_gdone, g1 - g0, l1_rows,
                              l1_t0.elapsed().as_secs_f64());
                }
                let lo = starts[g as usize];
                let hi = starts[g as usize + 1];
                // THE RANK MERGE (the C++'s inner heap): the group's
                // pairs' occurrence lists merge BY CONTINUATION RANK -
                // the rows emerge in value order (an equal-slice
                // group's order IS the continuation order)
                let mut grp: Vec<(u32, u64, u32)> = Vec::new();
                for mi in lo..hi {
                    let pe = &ent[mi as usize];
                    let v = pe.p as usize;
                    let a0 = ctx.ostart[v] as usize;
                    let b0 = ctx.ostart[v + 1] as usize;
                    for oi in a0..b0 {
                        let occ = ctx.olist[oi];
                        if stride == 1 && pe.o + 1 == ctx.dlen[v] {
                            let cell = (ctx.t[pe.pos as usize] as usize)<<8;
                            if tail_owner(cell,occ,ctx.dlen[v]) != bin_idx { continue; }
                        }
                        let nrank = if (occ as u64) + 1 < ctx.m {
                            ctx.rank[occ as usize + 1] + 1
                        } else { 0 };
                        grp.push((nrank, mi, oi as u32));
                    }
                }
                grp.sort_unstable_by_key(|r| r.0);
                if integer_landing {
                    // Extensions of a finite string form one contiguous lexical interval.
                    // After final-bin routing, only an extension in this bin can move
                    // an OWN row out of its group. Incoming shorter tails still merge.
                    let tail = &ent[lo as usize];
                    let possible_extension = ent.get(hi as usize).is_some_and(|next| {
                        next.end-next.pos > tail.end-tail.pos &&
                        ctx.t[next.pos as usize..next.end as usize].starts_with(&ctx.t[tail.pos as usize..tail.end as usize])
                    });
                    if !possible_extension {
                        fixed_groups += 1; fixed_rows += grp.len() as u64;
                        for &(n,mi,oi) in &grp {
                            let pe = &ent[mi as usize]; let occ = ctx.olist[oi as usize];
                            own.pos.push(ctx.pstart[occ as usize] + u64::from(pe.o));
                            own.ids.push((u64::from(pe.p)<<32) | u64::from(pe.o));
                            own.occ.push(oi);
                            own.facts.push(FactRow {v:pe.p,o:pe.o,n});
                        }
                        own.cnt[(g-g0) as usize] = u32::try_from(grp.len()).expect("group size");
                        rows_total += grp.len() as u64; l1_rows += grp.len() as u64;
                        continue;
                    }
                    searched_groups += 1; searched_rows += grp.len() as u64;
                }
                for &(_rnk, mi, oi) in &grp {
                    let pe = &ent[mi as usize];
                    let occ = ctx.olist[oi as usize];
                    let row = mach.row_of_po(pe.p, pe.o, occ);
                    rows_total += 1;
                    l1_rows += 1;
                    let lg = gallop(&mach, &row, g,
                                    bin_g_end, &starts, &ent);
                    // THE BIN CLAMP TEST (B3)
                    let mut bin_cross = false;
                    if lg + 1 >= bin_g_end
                        && (bin_idx as u64) + 1 < pbk as u64
                        && (pe.o as u64) + 1
                           >= ctx.dlen[pe.p as usize] as u64 {
                        let c0 = ctx.t[pe.pos as usize];
                        let tcell = ((c0 as u64) << 8)
                            | (row.cont4 >> 24) as u64;
                        let tb = if tcell < 65536 {
                            cell_bin[tcell as usize]
                        } else { 0 };
                        if tb as u64 > bin_idx as u64 {
                            bin_cross = true;
                        }
                    }
                    if bin_cross {
                        die("FINAL_OWNER: a partitioned occurrence escaped its bin");
                    } else if lg == g {
                        own.pos.push(mach.row_pos(&row));
                        own.ids.push((pe.p as u64) << 32 | pe.o as u64);
                        own.occ.push(oi);
                        own.facts.push(FactRow { v: pe.p,
                            o: pe.o as u32, n: row.nrank });
                        own.cnt[(g - g0) as usize] += 1;
                    } else if lg < g1 {
                        cross.push((lg, pe.p, pe.o, oi));
                    } else {
                        fwd.push((lg, pe.p, pe.o, oi));
                    }
                }
            }
            // the forward flush (the window files) + the bin parks
            {
                fwd.sort_unstable_by_key(|r| r.0);
                let mut i5 = 0usize;
                while i5 < fwd.len() {
                    let mut j5 = i5;
                    while j5 < fwd.len() && fwd[j5].0 == fwd[i5].0 {
                        j5 += 1;
                    }
                    let k3 = win_g.iter()
                        .filter(|&&w| w <= fwd[i5].0).count() - 1;
                    let p = format!("{}/arr_w{}_{}.bin",
                                    ctx.modeb_dir, bin_idx, k3);
                    append_arrivals(&p, &fwd[i5..j5]);
                    i5 = j5;
                }
                stats.crossings_fwd += fwd.len() as u64;
                stats.crossings_in += cross.len() as u64;
                if !bin_fwd.is_empty() {
                    let np = format!("{}/arr_b{}.bin",
                                     ctx.modeb_dir, bin_idx + 1);
                    append_arrivals(&np, &bin_fwd);
                    stats.crossings_fwd += bin_fwd.len() as u64;
                }
            }
            // ---- L2: the CSR (the local cross + the staged
            //      bin arrivals + the window's inbound file)
            mark(bin_idx,"csr");
            let mut inbound: Vec<(u64, u32, u32, u32)> = Vec::new();
            {
                let ip = format!("{}/arr_w{}_{}.bin",
                                 ctx.modeb_dir, bin_idx, w_idx);
                if let Ok(mut f) = fs::File::open(&ip) {
                    let mut raw = Vec::new();
                    let _ = f.read_to_end(&mut raw);
                    let _ = fs::remove_file(&ip);
                    for r in (0..raw.len() / 16).map(|q| q * 16) {
                        let lg = u32::from_le_bytes(
                            raw[r..r + 4].try_into().unwrap()) as u64;
                        let p = u32::from_le_bytes(
                            raw[r + 4..r + 8].try_into().unwrap());
                        let o = u32::from_le_bytes(
                            raw[r + 8..r + 12].try_into().unwrap());
                        let occ_lo = u32::from_le_bytes(
                            raw[r + 12..r + 16].try_into().unwrap());
                        if lg < g0 || lg >= g1 {
                            let kp = format!("/tmp/tr_smoke/arrw_bad.bin");
                            let _ = std::fs::write(&kp, &raw);
                            die(&format!("ARRIVAL_RANGE: bin={} w={} \
                                          g0={} g1={} lg={} rec={} of \
                                          {} - dumped {}",
                                         bin_idx, w_idx, g0, g1, lg,
                                         r / 16, raw.len() / 16, kp));
                        }
                        inbound.push((lg, p, o, occ_lo));
                    }
                }
                for r in &bin_inb[w_idx] {
                    inbound.push(*r);
                }
                for r in &cross { inbound.push(*r); }
            }
            // the CSR over the window's groups
            let mut arr_start = vec![0u32; (g1 - g0 + 1) as usize];
            {
                let mut cnt = vec![0u32; (g1 - g0) as usize];
                for r in &inbound { cnt[(r.0 - g0) as usize] += 1; }
                let mut sum = 0u32;
                for li in 0..(g1 - g0) as usize {
                    arr_start[li] = sum;
                    sum += cnt[li];
                }
                arr_start[(g1 - g0) as usize] = sum;
            }
            let mut arr_buf: Vec<(u64, u32, u32)> =
                vec![(0, 0, 0); inbound.len()];
            {
                let mut cur2 = arr_start.clone();
                for r in &inbound {
                    let idx = cur2[(r.0 - g0) as usize] as usize;
                    arr_buf[idx] = ((r.1 as u64) << 32 | r.2 as u64,
                                    r.1, r.3);
                    cur2[(r.0 - g0) as usize] += 1;
                }
            }
            // ---- L3: the per-group resolved merge (the own rows
            //      rank-ordered; every arrival inserted by value)
            mark(bin_idx,"local_merge");
            let mut final_pos: Vec<u64> = Vec::new();
            let mut final_facts: Vec<FactRow> = Vec::new();
            {
                let mut own_off = 0usize;
                for g in g0..g1 {
                    let own_n = own.cnt[(g - g0) as usize] as usize;
                    let arr_a = arr_start[(g - g0) as usize] as usize;
                    let arr_b = arr_start[(g - g0 + 1) as usize] as usize;
                    if arr_a == arr_b {
                        for i2 in 0..own_n {
                            final_pos.push(own.pos[own_off + i2]);
                            final_facts.push(own.facts[own_off + i2]);
                        }
                        own_off += own_n;
                        continue;
                    }
                    if merge_positions {
                        // Own rows are already ordered. Decode each inbound occurrence once,
                        // sort only the arrivals and merge the two streams without inserts.
                        let cmp = |ap:u64, af:FactRow, bp:u64, bf:FactRow| {
                            if af.v == bf.v && af.o == bf.o { af.n.cmp(&bf.n) }
                            else { ctx.t[ap as usize..].cmp(&ctx.t[bp as usize..]) }
                        };
                        let mut arrivals = Vec::with_capacity(arr_b-arr_a);
                        for &(pid, v, oi) in &arr_buf[arr_a..arr_b] {
                            let occ = ctx.olist[oi as usize] as usize;
                            let n = if occ+1 < ctx.m as usize {ctx.rank[occ+1]+1} else {0};
                            let o = pid as u32;
                            arrivals.push((ctx.pstart[occ]+u64::from(o),FactRow{v,o,n}));
                        }
                        arrivals.sort_unstable_by(|&(p,f),&(q,h)| cmp(p,f,q,h));
                        let end = own_off+own_n;
                        for (p,f) in arrivals {
                            while own_off<end && !cmp(own.pos[own_off],own.facts[own_off],p,f).is_gt() {
                                final_pos.push(own.pos[own_off]); final_facts.push(own.facts[own_off]); own_off+=1;
                            }
                            final_pos.push(p); final_facts.push(f);
                        }
                        final_pos.extend_from_slice(&own.pos[own_off..end]);
                        final_facts.extend_from_slice(&own.facts[own_off..end]);
                        own_off=end; continue;
                    }
                    let mut merged: Vec<u64> =
                        own.pos[own_off..own_off + own_n].to_vec();
                    let mut mfacts: Vec<FactRow> =
                        own.facts[own_off..own_off + own_n].to_vec();
                    let mut mrows: Vec<BandRow> =
                        (0..own_n).map(|i2| {
                            let id = own.ids[own_off + i2];
                            mach.row_of_po((id >> 32) as u32, id as u32,
                                ctx.olist[own.occ[own_off + i2] as usize])
                        }).collect();
                    for a in arr_a..arr_b {
                        let (pid, _p, occ_lo) = arr_buf[a];
                        let ar = mach.row_of_po((pid >> 32) as u32,
                                                pid as u32,
                                                ctx.olist[occ_lo as usize]);
                        let mut a3 = 0usize;
                        let mut b3 = merged.len();
                        while a3 < b3 {
                            let mid = a3 + (b3 - a3) / 2;
                            if mach.induced_cmp(&ar, &mrows[mid]) < 0 {
                                b3 = mid;
                            } else { a3 = mid + 1; }
                        }
                        let occ = ctx.olist[occ_lo as usize];
                        let pos = ctx.pstart[occ as usize]
                                  + (ar.occ_o & 0xFFFFFFFF);
                        merged.insert(a3, pos);
                        mfacts.insert(a3, FactRow {
                            v: (pid >> 32) as u32,
                            o: pid as u32, n: ar.nrank });
                        mrows.insert(a3, ar);
                    }
                    final_pos.extend_from_slice(&merged);
                    final_facts.extend_from_slice(&mfacts);
                    own_off += own_n;
                }
            }
            // ---- THE ASSEMBLY: the rows in order -> the sink
            {
                mark(bin_idx,"lcp_emit");
                let span_rows = final_pos.len();
                if span_rows > 0 {
                    let mut pout: Vec<u64> = Vec::with_capacity(span_rows);
                    let pack = |pos: u64| -> u64 {
                        let sym = if pos > 0 {
                            match remap.as_ref() {
                                Some(rm) => rm[ctx.t[(pos - 1) as usize] as usize],
                                None => ctx.t[(pos - 1) as usize],
                            }
                        } else { SENTINEL_SYMBOL };
                        pos | ((sym as u64) << 48)
                    };
                    for fp in &final_pos { pout.push(pack(*fp)); }
                    // the per-row LCPs (the W5 carry)
                    let mut lcp_class = (0u64, 0u64, 0u64, 0u64);
                    let mut lcpv: Vec<u64> = vec![0; span_rows];
                    for i2 in 1..span_rows {
                        let a = &final_facts[i2 - 1];
                        let b = &final_facts[i2];
                        let e1 = ctx.dlen[a.v as usize] as u64
                                 - a.o as u64;
                        let e2 = ctx.dlen[b.v as usize] as u64
                                 - b.o as u64;
                        let lt: u64;
                        if a.v == b.v && a.o == b.o {
                            lt = cont_lcp_w5(&mach, a, b, e1);
                            lcp_class.0 += 1;
                        } else {
                            let m3 = e1.min(e2);
                            let q1 = ctx.dref[a.v as usize] + a.o as u64;
                            let q2 = ctx.dref[b.v as usize] + b.o as u64;
                            let mut z = 0u64;
                            while z + 8 <= m3
                                   && ctx.t[(q1 + z) as usize
                                        ..(q1 + z + 8) as usize]
                                      == ctx.t[(q2 + z) as usize
                                        ..(q2 + z + 8) as usize] { z += 8; }
                            while z < m3
                                  && ctx.t[(q1 + z) as usize]
                                     == ctx.t[(q2 + z) as usize] { z += 1; }
                            if z < m3 {
                                lt = z;
                                lcp_class.1 += 1;
                            } else if e1 == e2 {
                                lt = cont_lcp_w5(&mach, a, b, e1);
                                lcp_class.2 += 1;
                            } else {
                                let mut l = 0u64;
                                mach.suf_lcp(final_pos[i2 - 1],
                                             final_pos[i2], m3, &mut l);
                                lt = l;
                                lcp_class.3 += 1;
                            }
                        }
                        lcpv[i2] = lt;
                    }
                    // the seam LCP (the head / the previous unit's last)
                    if prev_emitted_pos != u64::MAX && span_rows > 0 {
                        let mut cross = 0u64;
                        mach.suf_lcp(prev_emitted_pos, final_pos[0],
                                     0, &mut cross);
                        lcpv[0] = cross;
                    }
                    let mut head_in_unit = false;
                    let mut rows = span_rows;
                    if pending_head {
                        rows += 1;
                        head_in_unit = true;
                    }
                    // THE SINKS
                    match sink {
                        Sink::Product { .. } => {
                            let cutter = cutter.as_mut()
                                .unwrap_or_else(|| die("no cutter"));
                            let u = next_unit;
                            if head_in_unit { lcpv.insert(0, 0); }
                            assert_eq!(rows, lcpv.len());
                            emit_lcp_slice(&bands_dir, u, rows, &lcpv,
                                           product_rank, &mut lcp_live,
                                           lcp_slice_window, laws);
                            product_rank += rows as u64;
                            if trie_tap {
                                let sp = format!("{}/band_{}.sa",
                                                 bands_dir, u);
                                let mut f = fs::File::create(&sp)
                                    .unwrap_or_else(|_| die("band sa"));
                                if head_in_unit {
                                    f.write_all(&pack(head_pos)
                                        .to_le_bytes()).unwrap();
                                }
                                for v in &pout {
                                    f.write_all(&v.to_le_bytes()).unwrap();
                                }
                                drop(f);
                                let of = format!("{}.opened", sp);
                                fs::write(&of, b"")
                                    .unwrap_or_else(|_| die("opened"));
                            }
                            if head_in_unit {
                                let c = (pack(head_pos) >> 48) as u8;
                                if c == 0 { stats.end_rows += 1; }
                                cutter.feed_row(head_pos, c);
                                pending_head = false;
                            }
                            for (i2, pk) in pout.iter().enumerate() {
                                let c = (*pk >> 48) as u8;
                                if c == 0 { stats.end_rows += 1; }
                                cutter.feed_row(*pk & 0xFFFFFFFFFFFF, c);
                                let _ = i2;
                            }
                            next_unit += 1;
                        }
                        Sink::SaColumn { out } => {
                            if head_in_unit {
                                pending_head = false;
                            }
                            for i2 in 0..span_rows {
                                out.u32s_mut()[sa_sink_n as usize] =
                                    (final_pos[i2]
                                     / ctx.stride as u64) as u32;
                                sa_sink_n += 1;
                            }
                        }
                    }
                    stats.rows_emitted += span_rows as u64;
                    prev_emitted_pos = *final_pos.last().unwrap();
                    eprintln!("SS_EMIT_UNIT unit={} rows={} window={} \
                               lcp_fast={} lcp_bounded={} lcp_group={} \
                               lcp_residual={} rule=the_concat_walk\
;no_sorting",
                              next_unit, rows, w_idx, lcp_class.0,
                              lcp_class.1, lcp_class.2, lcp_class.3);
                }
            }
        }
        // B2's DELETION LAW: the bin's inputs are gone
        mark(bin_idx,"bin_done");
        laws.scratch_gate("bin_done");
    }
    // ---- the coverage laws + the receipts
    if stats.rows_emitted != ctx.rows_n {
        die(&format!("RESOLVE_FINAL: the landing walk resolved {} rows \
                      against {} - rows were lost or duplicated",
                     stats.rows_emitted, ctx.rows_n));
    }
    if let Some(c) = cutter.take() {
        assert_eq!(stats.end_rows, 1, "expected one END row");
        c.finish();
        fs::write(format!("{}/producer.done", bands_dir), b"").expect("producer.done");
    }
    if let Sink::SaColumn { out } = sink {
        if sa_sink_n != ctx.rows_n {
            die(&format!("LEVEL1_SINK_COUNT: the induction emitted {} \
                          rows against the level's M={}",
                         sa_sink_n, ctx.rows_n));
        }
        // the permutation belt
        let mut seen = vec![false; ctx.rows_n as usize];
        for i in 0..ctx.rows_n as usize {
            let v = out.u32s()[i] as usize;
            if v >= ctx.rows_n as usize || seen[v] {
                die(&format!("LEVEL1_SINK_PERM: row {} twice", v));
            }
            seen[v] = true;
        }
        eprintln!("SS_LEVEL_EXIT level_column_rows={} \
                   rule=the_induced_sa_column_permutation", sa_sink_n);
    }
    if integer_landing {
        assert_eq!(fixed_rows + searched_rows + owner_inbound, stats.rows_emitted);
        eprintln!("SS_INTEGER_LANDING fixed_rows={} searched_rows={} owner_inbound={} fixed_groups={} searched_groups={}",fixed_rows,searched_rows,owner_inbound,fixed_groups,searched_groups);
    }
    eprintln!("SS_LIST_RESOLVE rows={} crossings_in={} \
               crossings_fwd={} bins={} windows={} \
               rule=the_landing_walk_once_per_row;the_bin_order_is\
_the_key_order_no_merge",
              stats.rows_emitted, stats.crossings_in,
              stats.crossings_fwd, pbk, window_count);
    eprintln!("SS_EMIT_DONE runs={} rows_emitted={} end_rows={} \
               unit_s={:.2} rule=the_concat;rows_sorted_total==\
_the_phrase_count_asserted",
              stats.runs, stats.rows_emitted, stats.end_rows,
              crate::laws::now_s(t_all));
    laws.scratch_gate("pairs_emit_done");
    laws.scratch_peak();
    stats
}

// THE GALLOP: the landing group of the row's value (the first group
// whose next boundary it passes; the bin clamp at bin_g_end)
fn gallop(mach: &RowMach, row: &BandRow, g: u64, bin_g_end: u64,
          starts: &[u64], ent: &[PEnt]) -> u64 {
    let pass_at = |m: u64| -> bool {
        if m + 1 >= bin_g_end { return true; }
        let nx = &ent[starts[(m + 1) as usize] as usize];
        mach.cmp_bound(row, nx.pos, nx.end) < 0
    };
    let mut lg = g;
    let mut step = 1u64;
    let mut hi2 = g;
    while !pass_at(hi2) {
        lg = hi2;
        hi2 = lg + step;
        if hi2 >= bin_g_end {
            hi2 = bin_g_end - 1;
            if pass_at(hi2) { break; }
            hi2 = bin_g_end;
            break;
        }
        step <<= 1;
    }
    if hi2 == bin_g_end {
        lg = bin_g_end - 1;
    } else if hi2 > lg + 1 {
        let mut a2 = lg;
        let mut b2 = hi2;
        while a2 + 1 < b2 {
            let mid = a2 + (b2 - a2) / 2;
            if pass_at(mid) { b2 = mid; } else { a2 = mid; }
        }
        lg = b2;
    } else {
        lg = hi2;
    }
    // THE STRAY-PASS BELT
    if lg + 1 < bin_g_end - 1 && !pass_at(lg + 1) {
        die("LANDING_INVERT: the gallop is unsound here");
    }
    lg
}

// the continuation LCP for two rows with EQUAL slices (the W5 tree)
fn cont_lcp_w5(mach: &RowMach, a: &FactRow, b: &FactRow, e: u64) -> u64 {
    if a.n == 0 { return e; }
    if b.n == 0 {
        die("W5_ORDER: the END continuation sorts after a live one");
    }
    let r1 = a.n as u64 - 1;
    let r2 = b.n as u64 - 1;
    let ra = r1.min(r2);
    let rb = r1.max(r2);
    e + mach.lcp_range(ra + 1, rb)
}

// THE SXLS LCP SLICE (the finish's handshake, transliterated)
pub(crate) fn emit_lcp_slice(bands_dir: &str, u: u64, rows: usize,
                   lcpv: &[u64], rank_base: u64,
                   lcp_live: &mut std::collections::VecDeque<u64>,
                   window: u64, laws: &Laws) {
    emit_lcp_slice_mode(bands_dir,u,rows,lcpv,rank_base,lcp_live,window,laws,crate::laws::env_u64("XSA_BATCH_SXLS_ESCAPES",0)==1);
}

// The opt-in is an operator assertion: one private, non-restarting sink
// and one non-spooling finish, with no other credit reader or writer.
fn private_slice_reclaim(bands: &str, u: u64, window: u64) -> bool {
    if std::env::var_os("XSA_PRIVATE_SXLS_RECLAIM").as_deref() != Some(std::ffi::OsStr::new("1")) {
        return false;
    }
    if crate::laws::env_u64("XSA_TEST_RESIDENT_TRIPLES", 0) != 1 || window == 0 ||
        crate::laws::env_u64("XSA_FINISH_SPOOL", 0) != 0 {
        die("private SXLS reclaim requires resident sink, positive window, non-spooling finish");
    }
    if u == 0 {
        if !fs::symlink_metadata(bands).expect("private bands directory").is_dir() ||
            fs::read_dir(bands).expect("private bands directory").next().is_some() {
            die("private SXLS reclaim requires fresh empty bands directory");
        }
        eprintln!("SS_PRIVATE_SXLS_RECLAIM window={window} resume=unsupported");
    }
    true
}

fn retire_private_slice_credit(bands: &str, old: u64, consumed: &str) {
    use std::os::unix::fs::MetadataExt;
    let payload = format!("{bands}/band_{old}.lcp");
    let done = format!("{payload}.done");
    if !matches!(fs::symlink_metadata(&payload), Err(e) if e.kind() == std::io::ErrorKind::NotFound) {
        die("private SXLS credit without retired payload");
    }
    // Validate both names before removing either. Finish has advanced
    // nextSlice and unlinked the payload before creating consumed.
    for path in [done.as_str(), consumed] {
        let m = fs::symlink_metadata(path).unwrap_or_else(|e|
            die(&format!("private SXLS marker metadata {path}: {e}")));
        if !m.is_file() || m.len() != 0 || m.nlink() != 1 {
            die("private SXLS credit requires empty unshared regular markers");
        }
    }
    fs::remove_file(done).expect("retire private SXLS done");
    fs::remove_file(consumed).expect("retire private SXLS consumed");
}

fn emit_lcp_slice_mode(bands_dir:&str,u:u64,rows:usize,lcpv:&[u64],rank_base:u64,
                       lcp_live:&mut std::collections::VecDeque<u64>,window:u64,laws:&Laws,batch:bool) {
    assert_eq!(lcpv.len(),rows);
    let reclaim = private_slice_reclaim(bands_dir, u, window);
    // the flow: wait for the old slice to be consumed
    if u >= window {
        let want = format!("{}/band_{}.lcp.consumed",
                           bands_dir, u - window);
        while !std::path::Path::new(&want).exists() {
            laws.stop_check("lcp-slice");
            std::thread::sleep(
                std::time::Duration::from_millis(10));
        }
        if reclaim { retire_private_slice_credit(bands_dir, u - window, &want); }
        lcp_live.pop_front();
    }
    // the body: 255 markers + the escape list
    let mut esc=Vec::<u8>::new();let mut max_l=0u64;
    let mut band_lcp = vec![0u8; rows];
    for (i, &v) in lcpv.iter().enumerate() {
        if v >= 255 {
            esc.extend_from_slice(&(rank_base+i as u64).to_le_bytes());esc.extend_from_slice(&v.to_le_bytes());
            max_l=max_l.max(v);
            band_lcp[i] = 255;
        } else {
            band_lcp[i] = v as u8;
        }
    }
    let tmp = format!("{}/band_{}.lcp.tmp", bands_dir, u);
    let dst = format!("{}/band_{}.lcp", bands_dir, u);
    let mut f = fs::File::create(&tmp)
        .unwrap_or_else(|_| die("create lcp slice"));
    let mut h = [0u8; 48];
    h[0..4].copy_from_slice(&0x534C5853u32.to_le_bytes());
    h[4..8].copy_from_slice(&1u32.to_le_bytes());
    h[8..16].copy_from_slice(&(rows as u64).to_le_bytes());
    h[16..24].copy_from_slice(&(esc.len() as u64 / 16).to_le_bytes());
    h[24..32].copy_from_slice(&max_l.to_le_bytes());
    h[32..40].copy_from_slice(&rank_base.to_le_bytes());
    h[40..48].copy_from_slice(&1u64.to_le_bytes());
    f.write_all(&h).expect("lcp header");f.write_all(&band_lcp).expect("lcp markers");
    // Same bounded, little-endian escape tail. Change only the granularity
    // of its write calls; no unsafe struct casts or second copy of the tail.
    if batch {f.write_all(&esc).expect("lcp escape block");}
    else {for x in esc.chunks_exact(8){f.write_all(x).expect("lcp escape field");}}
    f.sync_all().expect("sync before publishing lcp slice");
    drop(f);
    fs::rename(&tmp, &dst).unwrap_or_else(|_| die("publish lcp slice"));
    let df = format!("{}.done", dst);
    fs::write(&df, b"").unwrap_or_else(|_| die("lcp done marker"));
    lcp_live.push_back(rows as u64 + esc.len() as u64);
}

#[cfg(test)]
#[path="sxls_batch_test.rs"]
mod sxls_batch_test;

#[cfg(test)]
#[path="sxls_reclaim_test.rs"]
mod sxls_reclaim_test;
