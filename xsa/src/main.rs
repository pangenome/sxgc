//! xsa — chi sa.
//!
//! Suffixient-array tools over the r-space artifact family:
//!   .sxi        SXI1 checksummed runs, endpoints, anchors, chi and optional names
//!   .ri4        v4 self-contained index core (rlbwt + C + run-end SA samples)
//!   <name>.sA   chi position set (u64 LE stream)
//!   .bwt.heads/.bwt.len  run-length BWT (u8 heads, u40 LE lengths)
//!
//! The index is sovereign: every subcommand runs from these artifacts alone.

mod sxi;
mod sxi2;
mod witness;
mod build;
mod bundle;
mod product;

use std::fs::File;
use std::io::{BufRead, BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::process::exit;

fn die(msg: &str) -> ! {
    eprintln!("xsa: error: {}", msg);
    exit(1);
}

/// Parsed .ri4 header (v4): n, k, R. Layout (little-endian):
///   u32 magic 0x52585349 ("ISXR"), u32 version, u64 n, u64 k, u64 R, ...
#[derive(Debug, Clone, Copy)]
struct Ri4Header {
    runs_offset: u64,
    version: u32,
    n: u64,
    k: u64,
    r: u64,
}

fn read_ri4_header(path: &str) -> Ri4Header {
    if let Some(c) = sxi::Container::open(path) { return Ri4Header { runs_offset: c.member(1).offset + 2048, version: c.version, n: c.n, k: c.k, r: c.r }; }
    let mut f = File::open(path).unwrap_or_else(|e| die(&format!("open {}: {}", path, e)));
    let mut b = [0u8; 32];
    f.read_exact(&mut b)
        .unwrap_or_else(|e| die(&format!("read {}: {}", path, e)));
    let magic = u32::from_le_bytes(b[0..4].try_into().unwrap());
    if magic != 0x52585349 {
        die(&format!("{}: bad magic {:08x} (want ISXR)", path, magic));
    }
    let version = u32::from_le_bytes(b[4..8].try_into().unwrap());
    let n = u64::from_le_bytes(b[8..16].try_into().unwrap());
    match version {
        4 => {
            let k = u64::from_le_bytes(b[16..24].try_into().unwrap());
            let r = u64::from_le_bytes(b[24..32].try_into().unwrap());
            Ri4Header { runs_offset: 2080, version, n, k, r }
        }
        _ => die(&format!("{}: version {} not supported yet (this build reads v4)", path, version)),
    }
}

/// chi position count from a .sA (u64 LE stream): entries = filesize / 8.
fn chi_count(path: &str) -> u64 {
    let md = std::fs::metadata(path).unwrap_or_else(|e| die(&format!("stat {}: {}", path, e)));
    if md.len() % 8 != 0 {
        die(&format!("{}: size {} not a multiple of 8 (u64 stream expected)", path, md.len()));
    }
    md.len() / 8
}

/// rlbwt pair: R = heads size; n = sum of u40 LE lengths.
fn read_rlbwt(prefix: &str) -> (u64, u64) {
    let heads = format!("{}.bwt.heads", prefix);
    let lens = format!("{}.bwt.len", prefix);
    let r = std::fs::metadata(&heads)
        .unwrap_or_else(|e| die(&format!("stat {}: {}", heads, e)))
        .len();
    let lensz = std::fs::metadata(&lens)
        .unwrap_or_else(|e| die(&format!("stat {}: {}", lens, e)))
        .len();
    if lensz != r * 5 {
        die(&format!("{}: size {} != R*5 (u40 lengths expected)", lens, lensz));
    }
    let mut f = File::open(&lens).unwrap_or_else(|e| die(&format!("open {}: {}", lens, e)));
    let mut n: u64 = 0;
    let mut buf = [0u8; 5];
    let mut nread = 0u64;
    loop {
        match f.read(&mut buf) {
            Ok(0) => break,
            Ok(_) => {
                n += u64::from(buf[0])
                    | (u64::from(buf[1]) << 8)
                    | (u64::from(buf[2]) << 16)
                    | (u64::from(buf[3]) << 24)
                    | (u64::from(buf[4]) << 32);
                nread += 1;
            }
            Err(e) => die(&format!("read {}: {}", lens, e)),
        }
    }
    if nread != r {
        die(&format!("{}: read {} records, expected {}", lens, nread, r));
    }
    (r, n)
}

fn fmt_ratio(num: f64, den: f64) -> String {
    if den == 0.0 { "n/a".to_string() } else { format!("{:.3}", num / den) }
}

// ---------- full .ri4 index (rank/LF/search/locate) ----------

/// Immutable file-backed members shared by all query workers.
enum Bytes { Owned(Vec<u8>), Mapped(memmap2::Mmap, usize, usize) }
impl Bytes {
    fn mapped(path: &str, offset: u64, len: usize) -> Self {
        let file = File::open(path).unwrap_or_else(|e| die(&e.to_string()));
        // Published containers are immutable for the lifetime of the index.
        let map = unsafe { memmap2::Mmap::map(&file) }.unwrap_or_else(|e| die(&e.to_string()));
        Self::Mapped(map, offset as usize, len)
    }
}
impl std::ops::Deref for Bytes {
    type Target = [u8];
    fn deref(&self) -> &[u8] { match self {
        Self::Owned(v) => v, Self::Mapped(m, o, n) => &m[*o..*o + *n],
    }}
}

/// sdsl int_vector bitstream: entries of `width` bits, LSB-first per u64 word.
struct PackedSa {
    bits: u64,          // total size IN BITS
    width: u8,
    data: Bytes,      // little-endian u64 words
}
impl PackedSa {
    fn get(&self, i: u64) -> u64 {
        let w = self.width as u64;
        let off = i * w;
        let wi = ((off / 64) * 8) as usize;   // BYTE offset of the u64 word
        let bi = off % 64;
        let words: &[u8] = &self.data;
        let rd = |k: usize| -> u64 {
            if k + 8 > words.len() { 0 } else { u64::from_le_bytes(words[k..k + 8].try_into().unwrap()) }
        };
        let mask: u64 = if w >= 64 { u64::MAX } else { (1u64 << w) - 1 };
        if bi + w <= 64 {
            (rd(wi) >> bi) & mask
        } else {
            let lo = rd(wi) >> bi;
            let hi = rd(wi + 8) << (64 - bi);
            (lo | hi) & mask
        }
    }
}

// ---------- string-start anchor table (the .ri4 sentinel-erasure fix) ----------
// The 466 autopsy (2026-09-24): remapped sentinels make LF steps FROM a
// 0x0A row undefined (sentinel-identity erasure); mega-run toehold walks
// cross string starts and land on foreign samples. FUNDAMENTAL FIX:
// string-start rows become extra sample anchors (k entries) and every
// walk terminates on EITHER a run-end sample OR a string-start anchor:
//   S(row j) = terminal_S - steps      (both cases; S rises 1 per LF step)
struct Anchors { rows: Vec<u64>, s: Vec<u64> }
impl Anchors {
    fn load(path: &str) -> Anchors {
        let mut f = File::open(path).unwrap_or_else(|e| die(&format!("open {}: {}", path, e)));
        let mut b = [0u8; 12];
        f.read_exact(&mut b).unwrap_or_else(|e| die(&format!("read {}: {}", path, e)));
        let magic = u32::from_le_bytes(b[0..4].try_into().unwrap());
        if magic != 0x434E4158 { die("anchors: bad magic"); }
        let k = u64::from_le_bytes(b[4..12].try_into().unwrap());
        let mut rows = Vec::with_capacity(k as usize);
        let mut s = Vec::with_capacity(k as usize);
        for _ in 0..k {
            let mut e = [0u8; 16];
            f.read_exact(&mut e).unwrap_or_else(|e| die(&format!("read {}: {}", path, e)));
            rows.push(u64::from_le_bytes(e[0..8].try_into().unwrap()));
            s.push(u64::from_le_bytes(e[8..16].try_into().unwrap()));
        }
        Anchors { rows, s }
    }
    fn lookup(&self, row: u64) -> Option<u64> {
        self.rows.binary_search(&row).ok().map(|i| self.s[i])
    }
}

struct Ri4 {
    phi: Option<sxi2::Phi>,
    lf_map: Option<sxi2::LfMap>,
    head_sa: Option<Bytes>,
    gcd_one: bool,
    embedded_anchors: Option<Anchors>,
    names: Option<(String, u64, u64)>,
    n: u64,
    k: u64,
    r: u64,
    c: Vec<u64>,
    run_char: Bytes,
    cyclic: bool,
    run_len: Vec<u32>,
    sa: Option<PackedSa>,
    run_start_blk: Vec<u64>,
    cruns: Vec<Vec<u32>>,
    csum: Option<Vec<Vec<u64>>>,
    total: Vec<u64>,
}
const BLK: u64 = 64;

fn read_raw_runs(f: &mut File, r: u64) -> (Vec<u64>,Vec<u8>,Vec<u32>) {
    let mut cb=[0u8;2048];f.read_exact(&mut cb).unwrap_or_else(|e|die(&format!("read C: {e}")));
    let c=cb.chunks_exact(8).map(|x|u64::from_le_bytes(x.try_into().unwrap())).collect();
    let mut heads=vec![0;r as usize];f.read_exact(&mut heads).unwrap_or_else(|e|die(&format!("read runs: {e}")));
    let mut raw=vec![0;(r*4) as usize];f.read_exact(&mut raw).unwrap_or_else(|e|die(&format!("read lens: {e}")));
    let lengths=raw.chunks_exact(4).map(|x|u32::from_le_bytes(x.try_into().unwrap())).collect();
    (c,heads,lengths)
}

impl Ri4 {
    fn load(path: &str) -> Ri4 {
        Self::load_validated(path, sxi::Container::open(path))
    }
    fn load_validated(path: &str, container: Option<sxi::Container>) -> Ri4 {
        let mut f = File::open(path).unwrap_or_else(|e| die(&format!("open {}: {}", path, e)));
        let mut b = [0u8; 32];
        f.read_exact(&mut b).unwrap_or_else(|e| die(&format!("read {}: {}", path, e)));
        let magic = u32::from_le_bytes(b[0..4].try_into().unwrap());
        if container.is_none() && magic != 0x52585349 { die(&format!("{}: bad magic", path)); }
        let version = u32::from_le_bytes(b[4..8].try_into().unwrap());
        if container.is_none() && version != 4 { die(&format!("{}: need v4, got v{}", path, version)); }
        let n = u64::from_le_bytes(b[8..16].try_into().unwrap());
        let k = u64::from_le_bytes(b[16..24].try_into().unwrap());
        let r = u64::from_le_bytes(b[24..32].try_into().unwrap());
        let _ = k;
        let (c, run_char, run_len) = if let Some(ref sx)=container {
            if sx.version>=2 {
                sxi2::runs(path,sx.member(1),n,r)
            } else {
                f.seek(SeekFrom::Start(sx.member(1).offset)).unwrap();
                read_raw_runs(&mut f,r)
            }
        } else { read_raw_runs(&mut f,r) };
        // sdsl int_vector header: u64 size-in-bits, u8 width, then words
        let sa=if !container.as_ref().is_some_and(|sx|sx.version>=3) {
            if let Some(ref sx) = container { f.seek(SeekFrom::Start(sx.member(2).offset)).unwrap(); }
            let mut hb = [0u8; 9];
            f.read_exact(&mut hb).unwrap_or_else(|e| die(&format!("read sa header: {}", e)));
            let bits = u64::from_le_bytes(hb[0..8].try_into().unwrap());let width = hb[8];
            if width == 0 || width > 64 || bits != r * width as u64 {
                die(&format!("{}: bad sa array (bits={} width={}?)", path, bits, width));
            }
            let nbytes = ((bits + 63) / 64 * 8) as usize;
            let mut data = vec![0u8; nbytes];
            f.read_exact(&mut data).unwrap_or_else(|e| die(&format!("read sa data: {}", e)));
            let data = if let Some(ref sx) = container {Bytes::mapped(path,sx.member(2).offset+9,nbytes)}
                else {Bytes::Owned(data)};
            Some(PackedSa { bits, width, data })
        } else {None};

        let mut run_start_blk = Vec::with_capacity((r / BLK + 2) as usize);
        let mut acc = 0u64;
        for x in 0..r {
            if x % BLK == 0 { run_start_blk.push(acc); }
            acc += run_len[x as usize] as u64;
        }
        run_start_blk.push(acc);
        let mut cruns: Vec<Vec<u32>> = vec![Vec::new(); 256];
        let compact=container.as_ref().is_some_and(|sx|sx.version>=3);
        let mut csum: Vec<Vec<u64>> = vec![Vec::new(); 256];
        let mut acc256 = vec![0u64; 256];
        for x in 0..r as usize {
            let c = run_char[x] as usize;
            cruns[c].push(x as u32);
            if !compact {csum[c].push(acc256[c]);}
            acc256[c] += run_len[x] as u64;
        }
        let total = acc256.clone();
        let cyclic = total[0x1e] > 0 || total[0x0a] == 0;
        let gcd_one={
            let mut gcd=0u64;
            for &x in &total {if x!=0 {let mut a=gcd;let mut b=x;while b!=0{let t=a%b;a=b;b=t;}gcd=a;}}
            gcd==1
        };
        let run_char = if let Some(sx) = container.as_ref().filter(|sx|sx.version==1) {
            Bytes::mapped(path, sx.member(1).offset + 2048, r as usize)
        } else { Bytes::Owned(run_char) };
        if !compact {for c in 0..256 { csum[c].push(total[c]); }}
        let _ = k;   // stored
        let embedded_anchors = container.as_ref().map(|sx| sx.anchors(path));
        let names = container.as_ref().and_then(|sx| sx.members.iter().find(|m| m.id == 6))
            .map(|m| (path.to_string(), m.offset, m.bytes));
        let phi=container.as_ref().filter(|sx|sx.version>=2)
            .map(|sx|sxi2::Phi::load(path,sx,&c));
        let head_sa=container.as_ref().filter(|sx|sx.version==2)
            .map(|sx|Bytes::mapped(path,sx.member(3).offset,(8*r) as usize));
        let lf_map=container.as_ref().filter(|sx|sx.version>=3)
            .map(|sx|sxi2::LfMap::load(path,sx,&run_char,&run_len,&c));
        Ri4 { phi, lf_map, head_sa, gcd_one, cyclic, embedded_anchors, names, n, k, r, c,
            run_char, run_len, sa, run_start_blk, cruns, csum:if compact {None}else{Some(csum)}, total }
    }

    fn run_start(&self, r: u64) -> u64 {
        let mut s = self.run_start_blk[(r / BLK) as usize];
        let mut q = (r / BLK) * BLK;
        while q < r { s += self.run_len[q as usize] as u64; q += 1; }
        s
    }
    fn tail_sample(&self, run:u64)->u64 {
        if self.lf_map.is_some() {self.n-1-self.phi.as_ref().unwrap().tail(run)}
        else {self.sa.as_ref().unwrap().get(run)}
    }
    fn run_of(&self, i: u64) -> u64 {
        let mut lo = 0usize;
        let mut hi = self.run_start_blk.len() - 1;
        while lo + 1 < hi {
            let mid = (lo + hi) / 2;
            if self.run_start_blk[mid] <= i { lo = mid; } else { hi = mid; }
        }
        let mut r = lo as u64 * BLK;
        let mut s = self.run_start_blk[lo];
        while r + 1 < self.r && s + self.run_len[r as usize] as u64 <= i {
            s += self.run_len[r as usize] as u64; r += 1;
        }
        r
    }
    fn rank(&self, c: u8, i: u64) -> u64 {
        if i == 0 { return 0; }
        if i >= self.n { return self.total[c as usize]; }
        let r = self.run_of(i);
        let s = self.run_start(r);
        let v = &self.cruns[c as usize];
        let j = v.partition_point(|&x| (x as u64) < r);
        if let Some(ref lf)=self.lf_map {
            if self.run_char[r as usize]==c {lf.start(r)-self.c[c as usize]+(i-s)}
            else if j==0 {0}
            else {let p=v[j-1] as u64;lf.start(p)-self.c[c as usize]+self.run_len[p as usize] as u64}
        } else {
            let csum=self.csum.as_ref().unwrap();
            if self.run_char[r as usize] == c { csum[c as usize][j] + (i - s) }
            else { csum[c as usize][j] }
        }
    }
    fn lf(&self, i: u64) -> u64 {
        let r=self.run_of(i);
        if let Some(ref map)=self.lf_map {map.start(r)+i-self.run_start(r)}
        else {let c=self.run_char[r as usize];self.c[c as usize]+self.rank(c,i)}
    }
    /// backward search; returns half-open [l, r) of suffixes prefixed by p.
    /// Chars are consumed LEFT-TO-RIGHT AS GIVEN, extending the match leftward
    /// in the INDEXED text (the v4 chain convention: revlines chains index
    /// reversed contigs, so forward-original patterns are consumed forward;
    /// plain chains must pass the pattern reversed — handled by the caller).
    fn search(&self, tchars: &[u8]) -> (u64, u64) {
        let mut l = 0u64;
        let mut r = self.n;
        for &ch in tchars {
            let (nl, nr) = self.step(l, r, ch);
            l = nl;
            r = nr;
            if l >= r { return (l, r); }
        }
        (l, r)
    }
    fn search_toehold(&self,tchars:&[u8])->(u64,u64,Option<u64>){
        if self.phi.is_none()||!self.gcd_one{let(l,r)=self.search(tchars);return(l,r,None);}
        let head=|run:u64| -> u64 {
            if let Some(ref phi)=self.phi {
                if self.lf_map.is_some() {return phi.head(run);}
            }
            let heads=self.head_sa.as_ref().unwrap();
            u64::from_le_bytes(heads[(8*run) as usize..(8*run+8) as usize].try_into().unwrap())
        };
        let mut l=0u64;let mut r=self.n;let mut sa=head(0);
        for &ch in tchars{
            let(nl,nr)=self.step(l,r,ch);
            if nl>=nr{return(nl,nr,None);}
            let run=self.run_of(l);
            let selected=if self.run_char[run as usize]==ch {sa}
            else {
                let ids=&self.cruns[ch as usize];
                let j=ids.partition_point(|&x|x as u64<=run);
                let next=*ids.get(j).unwrap_or_else(||die("SXI2: missing toehold run")) as u64;
                if self.run_start(next)>=r {die("SXI2: toehold run outside interval");}
                head(next)
            };
            sa=if selected==0{self.n-1}else{selected-1};
            l=nl;r=nr;
        }
        (l,r,Some(sa))
    }
    /// one backward-search step: rank-extend the half-open interval [l, r) by
    /// char `ch`. The result is the interval of `ch` immediately preceding the
    /// current indexed-text match. Empty (nl >= nr) means no such extension.
    fn step(&self, l: u64, r: u64, ch: u8) -> (u64, u64) {
        let c = ch as usize;
        let rl = self.rank(ch, l);
        let rr = self.rank(ch, r);
        (self.c[c] + rl, self.c[c] + rr)
    }
    /// v4 sample value at BWT position j: walk LF to the nearest run end;
    /// S decreases by 1 per LF step from the run-end sample.
    fn s_at(&self, j: u64) -> u64 {
        self.s_at_opt(j, None)
    }
    /// Two-case toehold (the fundamental fix): terminate on a run-end
    /// sample OR a string-start anchor. LF from a 0x0A row is undefined
    /// (erasure law) — anchors make that step never happen.
    fn s_at_opt(&self, j: u64, anc: Option<&Anchors>) -> u64 {
        let mut pos = j;
        let mut steps = 0u64;
        loop {
            let r = self.run_of(pos);
            let e = self.run_start(r) + self.run_len[r as usize] as u64;
            if pos == e - 1 {
                let sample=self.tail_sample(r);
                return if self.cyclic { (sample + self.n - steps % self.n) % self.n }
                    else { sample.wrapping_sub(steps) };
            }
            if !self.cyclic && self.run_char[r as usize] == 0x0A {
                match anc.and_then(|a| a.lookup(pos)) {
                    Some(s0) => return s0.wrapping_sub(steps),
                    None => die("s_at: walk reached a 0x0A row with no anchor table (--anchors)"),
                }
            }
            pos = self.lf(pos);
            steps += 1;
            if self.cyclic && pos == j && steps < self.n && self.n % steps == 0 {
                // Periodic text has equal-rotation classes of n/period rows.
                // An interior lane can be an LF cycle without an endpoint.
                // The final lane reaches a run tail. Resolve its rotation,
                // then enumerate equivalent coordinates in increasing order.
                let copies = self.n / steps;
                let representative = (j / copies + 1) * copies - 1;
                if representative == j { die("locate: unsampled periodic representative"); }
                let last = self.n - 1 - self.s_at_opt(representative, anc);
                let coordinate = last % steps + (j % copies) * steps;
                return self.n - 1 - coordinate;
            }
            if steps >= self.n { die("locate: LF cycle has no sample"); }
        }
    }
}

/// interval of the substring `s` in the indexed text, feeding `s` forward
/// (the same convention the exact-mode caller uses for a default/revlines
/// read: a forward-fed match grows to the right).
fn search_sub(idx: &Ri4, s: &[u8]) -> (u64, u64) {
    idx.search(s)
}

/// Matching-statistics length vector for one (already orientation-normalised)
/// read `seq`.
///
/// ms[i] = length of the longest suffix of seq[..=i] that occurs in the
/// indexed text (0 if not even seq[i] occurs). The result is returned in the
/// emitted (position-reversed) order: out[k] = ms[m-1-k].
///
/// Exact v0: we keep the interval [l, r) of the current longest match ending
/// at i-1 and first try the O(1) rank-extension by c = seq[i]. On failure we
/// shorten to every shorter suffix ending at i-1, recomputing each candidate's
/// interval by a fresh backward search, and try extending it by c; the first
/// success wins. Each read is <= ~150bp, so the worst-case O(m^2) is fine.
///
/// ORIENTATION (empirically gated): the corpus matches the brute oracle only
/// when the read is consumed in its REVERSED orientation — equivalent to a
/// plain-chain read, i.e. exactly what exact-mode `--plain` does (reverse the
/// whole pattern once) — and the resulting vector is emitted position-reversed.
/// The caller performs that whole-read reversal; this routine always feeds
/// forward, which keeps the interval extension O(1).
fn ms_vector(idx: &Ri4, seq: &[u8]) -> Vec<u32> {
    let m = seq.len();
    let mut ms = vec![0u32; m];
    // interval of the empty match is the whole text; len is the match length
    let (mut l, mut r) = (0u64, idx.n);
    let mut len: u64 = 0;
    for i in 0..m {
        let c = seq[i];
        // 1. extend the current match to the right by c
        let (el, er) = idx.step(l, r, c);
        if el < er {
            l = el;
            r = er;
            len += 1;
            ms[i] = len as u32;
            continue;
        }
        // 2. shorten: candidate match seq[i-j..i] of length j, then extend by c
        let mut found = false;
        let mut j = len.saturating_sub(1);
        loop {
            let s = &seq[(i - j as usize)..i];
            let (cl, cr) = search_sub(idx, s);
            if cl < cr {
                let (fl, fr) = idx.step(cl, cr, c);
                if fl < fr {
                    l = fl;
                    r = fr;
                    len = j + 1;
                    ms[i] = len as u32;
                    found = true;
                    break;
                }
            }
            if j == 0 {
                break;
            }
            j -= 1;
        }
        if !found {
            // c does not occur: reset to the empty match for the next round
            l = 0;
            r = idx.n;
            len = 0;
            ms[i] = 0;
        }
    }
    ms
}

fn cmd_query(args: &[String]) {
    let mut ri4: Option<String> = None;
    let mut pats: Option<String> = None;
    let mut out: Option<String> = None;
    let mut sidecar: Option<String> = None;
    let mut plain = false;
    let mut ms = false;
    let mut trace = false;
    let mut anchor_path: Option<String> = None;
    let mut ms_out: Option<String> = None;
    let mut sample: Option<u64> = None;
    let mut seed: u64 = 0;
    let mut i = 0;
    while i < args.len() {
        let step;
        match args[i].as_str() {
            "--ri4" | "--sxi" if i + 1 < args.len() => { ri4 = Some(args[i + 1].clone()); step = 2; }
            "--patterns" if i + 1 < args.len() => { pats = Some(args[i + 1].clone()); step = 2; }
            "--output" if i + 1 < args.len() => { out = Some(args[i + 1].clone()); step = 2; }
            "--sidecar" if i + 1 < args.len() => { sidecar = Some(args[i + 1].clone()); step = 2; }
            "--ms-out" if i + 1 < args.len() => { ms_out = Some(args[i + 1].clone()); step = 2; }
            "--sample" if i + 1 < args.len() => { sample = Some(args[i + 1].parse::<u64>().unwrap_or_else(|_| die("--sample: integer expected"))); step = 2; }
            "--seed" if i + 1 < args.len() => { seed = args[i + 1].parse::<u64>().unwrap_or_else(|_| die("--seed: integer expected")); step = 2; }
            "--plain" => { plain = true; step = 1; }
            "--trace-samples" => { trace = true; step = 1; }
            "--anchors" if i + 1 < args.len() => { anchor_path = Some(args[i + 1].clone()); step = 2; }
            "--ms" => { ms = true; step = 1; }
            other => die(&format!("query: unknown arg {}", other)),
        }
        i += step;
    }
    let (ri4, pats) = (
        ri4.unwrap_or_else(|| die("query: need --ri4")),
        pats.unwrap_or_else(|| die("query: need --patterns (FASTA)")),
    );
    eprintln!("xsa query: loading {} ...", ri4);
    let idx = Ri4::load(&ri4);
    eprintln!("  n={} R={}", idx.n, idx.r);
    if plain && sidecar.is_none() && idx.names.is_none() && !ms {
        die("query: --plain needs --sidecar or embedded names");
    }

    // plain-chain decode: v4 samples are mirrored (S = fstart+fend-pos);
    // undo per owning string: pos = fstart_i + fend_i - S
    let (mut p_fstart, mut p_fend): (Vec<u64>, Vec<u64>) = (Vec::new(), Vec::new());
    let name_reader: Option<(String, Box<dyn BufRead>)> = if let Some(sc) = &sidecar {
        let sf = File::open(sc).unwrap_or_else(|e| die(&format!("open {}: {}", sc, e)));
        Some((sc.clone(), Box::new(BufReader::new(sf))))
    } else { idx.names.as_ref().map(|(path, offset, bytes)| {
        let mut sf = File::open(path).unwrap(); sf.seek(SeekFrom::Start(*offset)).unwrap();
        (path.clone(), Box::new(BufReader::new(sf.take(*bytes))) as Box<dyn BufRead>)
    }) };
    if let Some((sc, reader)) = name_reader {
        let mut acc = 0u64;
        for line in reader.lines() {
            let line = line.unwrap_or_else(|e| die(&format!("read {}: {}", sc, e)));
            if line.is_empty() { continue; }
            let c: Vec<&str> = line.split('\t').collect();
            if c.len() < 3 { die(&format!("{}: need name\\tfstart\\tlen", sc)); }
            let len: u64 = c[2].trim().parse().unwrap_or_else(|_| die("bad len"));
            p_fstart.push(acc);
            p_fend.push(acc + len);
            acc += len + 1;
        }
        if acc != idx.n { die(&format!("{}: lens sum {} != n {}", sc, acc, idx.n)); }
    }

    // FASTA patterns
    let mut patterns: Vec<(String, Vec<u8>)> = Vec::new();
    {
        let pf = File::open(&pats).unwrap_or_else(|e| die(&format!("open {}: {}", pats, e)));
        let mut name = String::new();
        let mut seq: Vec<u8> = Vec::new();
        for line in BufReader::new(pf).lines() {
            let line = line.unwrap_or_else(|e| die(&format!("read {}: {}", pats, e)));
            if let Some(r) = line.strip_prefix('>') {
                if !name.is_empty() { patterns.push((std::mem::take(&mut name), std::mem::take(&mut seq))); }
                name = r.trim_end().to_string();
            } else if !line.is_empty() {
                seq.extend_from_slice(line.trim_end().as_bytes());
            }
        }
        if !name.is_empty() { patterns.push((name, seq)); }
    }
    eprintln!("  {} patterns", patterns.len());

    if ms {
        // matching statistics: binary lens per read, no occurrence output
        let stdout = std::io::stdout();
        let mut w: Box<dyn Write> = match &ms_out {
            Some(p) => Box::new(File::create(p).unwrap_or_else(|e| die(&format!("create {}: {}", p, e)))),
            None => Box::new(stdout.lock()),
        };
        let mut npos = 0u64;
        for (name, seq) in patterns.iter() {
            // ORIENTATION: the corpus matches the brute oracle only when the
            // read is consumed reversed (mirrors exact-mode --plain: reverse
            // the whole pattern once). The MS routine itself always feeds
            // forward so its interval extension stays O(1).
            let work: Vec<u8> = if plain {
                seq.iter().rev().copied().collect()
            } else {
                seq.clone()
            };
            let msv = ms_vector(&idx, &work);
            // emitted order: position-reversed (out[k] = ms[m-1-k])
            w.write_all(format!(">{}\n", name).as_bytes()).unwrap();
            w.write_all(&(msv.len() as u64).to_le_bytes()).unwrap();
            for &v in msv.iter().rev() {
                w.write_all(&v.to_le_bytes()).unwrap();
            }
            w.write_all(b"\n").unwrap();
            npos += msv.len() as u64;
        }
        eprintln!("xsa query: MS {} reads, {} positions{}", patterns.len(), npos,
            if plain { " [plain]" } else { " [revlines]" });
        return;
    }

    let stdout = std::io::stdout();
    let external_anchors = anchor_path.map(|p| Anchors::load(&p));
    let anchors = external_anchors.as_ref().or(idx.embedded_anchors.as_ref());
    let mut w: Box<dyn Write> = match &out {
        Some(p) => Box::new(File::create(p).unwrap_or_else(|e| die(&format!("create {}: {}", p, e)))),
        None => Box::new(stdout.lock()),
    };
    let mut noccs = 0u64;
    let mut nabs = 0u64;
    // xorshift64*: deterministic per-pattern row sampler for --sample
    let mut next_rng = |mut x: u64| -> u64 {
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        x.wrapping_mul(0x2545F4914F6CDD1D)
    };
    for (pi, (name, seq)) in patterns.iter().enumerate() {
        let m = seq.len();
        // consume pattern chars in INDEXED-text order: revlines (default) =
        // forward-original; plain = reversed
        let (l, r) = if plain {
            let rev: Vec<u8> = seq.iter().rev().copied().collect();
            idx.search(&rev)
        } else {
            idx.search(seq)
        };
        writeln!(w, ">{}", name).unwrap();
        if l >= r {
            writeln!(w, "-1 {}", m).unwrap();
            nabs += 1;
            continue;
        }
        let occ = r - l;
        if let Some(k) = sample {
            // k (or occ if fewer) uniformly random occurrences via seeded rows:
            // row j in the interval IS one distinct occurrence; sampling rows
            // uniformly samples occurrences uniformly. O(k) toeholds, not O(occ).
            let kk = k.min(occ);
            let mut rng = seed ^ (pi as u64).wrapping_mul(0x9E3779B97F4A7C15) ^ 0xA0761D6478BD642F;
            let mut rows: Vec<u64> = (l..r).collect();   // interval rows, not [0,occ)
            for t in 0..kk {
                rng = next_rng(rng);
                let j = (rng % (occ - t)) + t;
                rows.swap(t as usize, j as usize);
            }
            rows.truncate(kk as usize);
            for j in rows {
                let s = idx.s_at_opt(j, anchors);
                let pos: i64 = if plain {
                    let ii = p_fstart.partition_point(|&x| x <= s);
                    let si = if ii == 0 { usize::MAX } else { ii - 1 };
                    if si == usize::MAX || s > p_fend[si] { die(&format!("query: S={} outside any string", s)); }
                    (p_fstart[si] + p_fend[si] - s) as i64
                } else {
                    s.wrapping_sub(m as u64) as i64
                };
                writeln!(w, "{} {}", pos, m).unwrap();
                noccs += 1;
            }
            continue;
        }
        let mut previous_phi_sa=None;
        for j in l..r {
            let mut trow = j;
            let mut tsteps = 0u64;
            let mut tanchor = 0u64;
            let mut tsample = 0u64;
            let s = if trace {
                // re-derive s while capturing the anchor run + raw sample
                loop {
                    let r = idx.run_of(trow);
                    let e = idx.run_start(r) + idx.run_len[r as usize] as u64;
                    if trow == e - 1 {
                        tanchor = r;
                        tsample = idx.tail_sample(r);
                        break;
                    }
                    trow = idx.lf(trow);
                    tsteps += 1;
                }
                if trace {
                    writeln!(w, "TRACE\t{}\t{}\t{}\t{}\t{}", name, j, tanchor, tsample, tsteps).unwrap();
                }
                tsample.wrapping_sub(tsteps)
            } else if idx.lf_map.is_some() {
                let actual=if let Some(previous)=previous_phi_sa {
                    idx.phi.as_ref().unwrap().successor(previous)
                } else {idx.n-1-idx.s_at_opt(j,anchors)};
                previous_phi_sa=Some(actual);
                idx.n-1-actual
            } else {
                idx.s_at_opt(j, anchors)
            };
            let pos: i64 = if plain {
                // S lies in [fstart_i, fend_i] of the owning string (mirror)
                let ii = p_fstart.partition_point(|&x| x <= s);
                let i = if ii == 0 { usize::MAX } else { ii - 1 };
                if i == usize::MAX || s > p_fend[i] {
                    die(&format!("query: S={} outside any string", s));
                }
                (p_fstart[i] + p_fend[i] - s) as i64
            } else {
                s.wrapping_sub(m as u64) as i64
            };
            writeln!(w, "{} {}", pos, m).unwrap();
            noccs += 1;
        }
    }
    eprintln!("xsa query: {} occurrences over {} patterns ({} absent){}", noccs, patterns.len(), nabs,
        if sample.is_some() { " [sampled]" } else { "" });
}

fn cmd_stats(args: &[String]) {
    let mut ri4: Option<String> = None;
    let mut rlbwt: Option<String> = None;
    let mut chi: Option<String> = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--ri4" | "--sxi" if i + 1 < args.len() => ri4 = Some(args[i + 1].clone()),
            "--rlbwt" if i + 1 < args.len() => rlbwt = Some(args[i + 1].clone()),
            "--chi" if i + 1 < args.len() => chi = Some(args[i + 1].clone()),
            other => die(&format!("stats: unknown arg {}", other)),
        }
        i += 2;
    }
    if ri4.is_none() && rlbwt.is_none() {
        die("stats: need --ri4 and/or --rlbwt (and optionally --chi)");
    }

    let mut n = 0u64;
    let mut k = 0u64;
    let mut r = 0u64;
    let mut embedded_chi = None;
    let mut index_bytes = None;
    if let Some(p) = &ri4 {
        let container = sxi::Container::open(p);
        let h = if let Some(ref c) = container {
            embedded_chi = c.complete.then_some(c.member(5).count);
            index_bytes = Some(std::fs::metadata(p).unwrap().len());
            Ri4Header { runs_offset: c.member(1).offset + 2048, version: c.version, n: c.n, k: c.k, r: c.r }
        } else { read_ri4_header(p) };
        n = h.n;
        k = h.k;
        r = h.r;
        println!("{}      {}  (v{}: n={}, k={} {}, R={} runs)", if h.version <= 2 { ".sxi" } else { ".ri4" }, p, h.version, h.n, h.k, if h.version <= 2 { "records" } else { "strings" }, h.r);
        if let Some(c) = container { c.print_members(); }
    }
    if let Some(p) = &rlbwt {
        let (rr, nn) = read_rlbwt(p);
        if r != 0 && (rr != r || nn != n) {
            die(&format!(
                "stats: rlbwt disagrees with .ri4 (rlbwt R={} n={} vs ri4 R={} n={})",
                rr, nn, r, n
            ));
        }
        r = rr;
        n = nn;
        println!("rlbwt     {}  (R={} runs, n={})", p, rr, nn);
    }
    let x = chi.map(|p| {
        let c = chi_count(&p);
        println!("chi       {}  (chi={} positions)", p, c);
        c
    }).or(embedded_chi);

    println!();
    println!("n            = {}", n);
    if k > 0 { println!("k ({})  = {}", if index_bytes.is_some() { "records" } else { "strings" }, k); }
    println!("r (runs)     = {}", r);
    if let Some(xc) = x {
        println!("chi          = {}", xc);
        println!("n/r          = {}", fmt_ratio(n as f64, r as f64));
        println!("chi/r        = {}   (law: ~0.86)", fmt_ratio(xc as f64, r as f64));
    }
    if let Some(bytes) = index_bytes {
        println!("index bytes  = {}", bytes);
    } else {
        println!("index bytes  ~ {} (rlbwt 6R + samples 8R)", r * 14);
    }
}

fn cmd_build_anchors(args: &[String]) {
    let mut ri4: Option<String> = None;
    let mut flat: Option<String> = None;
    let mut sidecar: Option<String> = None;
    let mut out: Option<String> = None;
    let mut i = 0;
    while i < args.len() {
        let step;
        match args[i].as_str() {
            "--ri4" | "--sxi" if i + 1 < args.len() => { ri4 = Some(args[i + 1].clone()); step = 2; }
            "--flat" if i + 1 < args.len() => { flat = Some(args[i + 1].clone()); step = 2; }
            "--sidecar" if i + 1 < args.len() => { sidecar = Some(args[i + 1].clone()); step = 2; }
            "--output" if i + 1 < args.len() => { out = Some(args[i + 1].clone()); step = 2; }
            other => die(&format!("build-anchors: unknown arg {}", other)),
        }
        i += step;
    }
    let (ri4p, flatp, scp, outp) = (
        ri4.unwrap_or_else(|| die("build-anchors: need --ri4")),
        flat.unwrap_or_else(|| die("build-anchors: need --flat (the indexed stream text)")),
        sidecar.unwrap_or_else(|| die("build-anchors: need --sidecar")),
        out.unwrap_or_else(|| die("build-anchors: need --output")),
    );
    eprintln!("xsa build-anchors: loading {}", ri4p);
    let idx = Ri4::load(&ri4p);
    // sidecar: name \t fsFwd \t len  (stream fstarts derived from lens)
    let mut fs: Vec<u64> = Vec::new();
    let mut lens: Vec<u64> = Vec::new();
    {
        let sf = File::open(&scp).unwrap_or_else(|e| die(&format!("open {}: {}", scp, e)));
        for line in BufReader::new(sf).lines() {
            let line = line.unwrap_or_else(|e| die(&format!("read {}: {}", scp, e)));
            if line.is_empty() { continue; }
            let c: Vec<&str> = line.split('\t').collect();
            if c.len() < 3 { die("build-anchors: bad sidecar line"); }
            fs.push(c[1].trim().parse().unwrap_or_else(|_| die("bad fsFwd")));
            lens.push(c[2].trim().parse().unwrap_or_else(|_| die("bad len")));
        }
    }
    let k = lens.len() as u64;
    if k != idx.k { die(&format!("build-anchors: sidecar k {} != ri4 k {}", k, idx.k)); }
    let mut flatf = File::open(&flatp).unwrap_or_else(|e| die(&format!("open {}: {}", flatp, e)));
    use std::io::Seek;
    let mut read_flat = |flatf: &mut File, pos: u64, want: usize| -> Vec<u8> {
        flatf.seek(std::io::SeekFrom::Start(pos)).unwrap();
        let mut buf = vec![0u8; want];
        flatf.read_exact(&mut buf).unwrap_or_else(|e| die(&format!("flat read: {}", e)));
        buf
    };
    // ---- pass 0: enumerate ALL 0x0A runs; assert total rows == k ----
    let mut runs: Vec<(u64, u64, u64)> = Vec::new();  // (run index, row start, row end excl)
    {
        let mut rs = 0u64;
        for ri in 0..idx.r as usize {
            let re = rs + idx.run_len[ri] as u64;
            if idx.run_char[ri] == 0x0A { runs.push((ri as u64, rs, re)); }
            rs = re;
        }
    }
    let total_na = runs.iter().map(|&(_, a, b)| b - a).sum::<u64>();
    if total_na != k { die(&format!("build-anchors: 0x0A rows {} != k {} — run structure violated", total_na, k)); }
    // ---- pass 1: SAMPLE-DIRECT assignment: every run end's sample IS the S of its row ----
    let mut s_to_string: std::collections::HashMap<u64, usize> = std::collections::HashMap::new();
    for si in 0..k as usize {
        let s_val = fs[si] + lens[si];
        if s_to_string.insert(s_val, si).is_some() { die("build-anchors: duplicate S values in sidecar"); }
    }
    let mut assigned: Vec<Option<u64>> = vec![None; k as usize];   // string -> start row
    let mut owner: std::collections::HashMap<u64, usize> = std::collections::HashMap::new(); // row -> string
    let mut multi: Vec<(u64, u64, u64)> = Vec::new();              // multi-row 0x0A runs
    for &(ri, rs, re) in &runs {
        let last = re - 1;
        let s_val = idx.tail_sample(ri);
        let si = *s_to_string.get(&s_val).unwrap_or_else(|| die(&format!("build-anchors: run-end sample S={} matches no string — corrupt samples?", s_val)));
        if assigned[si].is_some() { die(&format!("build-anchors: string {} assigned twice by samples", si)); }
        assigned[si] = Some(last);
        owner.insert(last, si);
        if re - rs > 1 { multi.push((ri, rs, re)); }
    }
    let n_multi: u64 = multi.iter().map(|&(_, a, b)| b - a - 1).sum();
    eprintln!("build-anchors: {} runs; {} multi-row ({} rows to resolve by search)", runs.len(), multi.len(), n_multi);
    // ---- pass 2: content-extension search for every still-unassigned string ----
    let mut leftovers: Vec<usize> = Vec::new();
    for si in 0..k as usize {
        if assigned[si].is_some() { continue; }
        let len = lens[si] as usize;
        let fstart: u64 = (0..si as u64).map(|j| lens[j as usize] + 1).sum();
        let mut window: usize = 256;
        let mut done = false;
        loop {
            let w = window.min(len);
            let pat = read_flat(&mut flatf, fstart, w);
            let rev: Vec<u8> = pat.iter().rev().copied().collect();
            let (l, r) = idx.search(&rev);
            if l >= r { die(&format!("build-anchors: string {} search empty at window {}", si, w)); }
            let mut cands: Vec<u64> = Vec::new();
            let mut run = idx.run_of(l);
            loop {
                let rs2 = idx.run_start(run);
                let re2 = rs2 + idx.run_len[run as usize] as u64;
                if idx.run_char[run as usize] == 0x0A {
                    let lo = rs2.max(l); let hi = re2.min(r);
                    for row in lo..hi { if !owner.contains_key(&row) { cands.push(row); } }
                }
                if re2 >= r { break; }
                run += 1;
            }
            if cands.len() == 1 {
                let row = cands[0];
                if owner.contains_key(&row) { die(&format!("build-anchors: internal: owned candidate for string {}", si)); }
                assigned[si] = Some(row);
                owner.insert(row, si);
                done = true;
                break;
            }
            if cands.len() == 0 { die(&format!("build-anchors: string {} has no unowned 0x0A row at window {} — table inconsistent", si, w)); }
            if w >= len { break; }
            window *= 2;
        }
        if !done { leftovers.push(si); }
    }
    // ---- pass 3: identical-content leftovers — order by following text (sentinels erased),
    //      rows ascending; fail loudly if the run-end law is violated ----
    if !leftovers.is_empty() {
        eprintln!("build-anchors: {} identical-content strings to order by following text", leftovers.len());
        // per multi-run, zip leftover rows (ascending, minus owned) with leftover strings sorted by following bytes
        let mut left_by_run: std::collections::HashMap<u64, Vec<u64>> = std::collections::HashMap::new();
        for &(ri, rs, re) in &multi {
            let free: Vec<u64> = (rs..re).filter(|row| !owner.contains_key(row)).collect();
            if !free.is_empty() { left_by_run.insert(ri, free); }
        }
        let total_free: usize = left_by_run.values().map(|v| v.len()).sum();
        if total_free != leftovers.len() { die(&format!("build-anchors: leftover mismatch ({} free rows vs {} leftover strings)", total_free, leftovers.len())); }
        // which run does each leftover string belong to? its full-content search interval
        // group leftover strings by run via full-content search, then order by following text
        let mut groups: std::collections::HashMap<u64, Vec<usize>> = std::collections::HashMap::new();
        for &si in &leftovers {
            let len = lens[si] as usize;
            let fstart: u64 = (0..si as u64).map(|j| lens[j as usize] + 1).sum();
            let pat = read_flat(&mut flatf, fstart, len);
            let rev: Vec<u8> = pat.iter().rev().copied().collect();
            let (l, r) = idx.search(&rev);
            let mut hit: Option<u64> = None;
            for (&ri, rows) in left_by_run.iter() {
                if rows.iter().any(|&row| l <= row && row < r) { hit = Some(ri); break; }
            }
            let ri = hit.unwrap_or_else(|| die(&format!("build-anchors: leftover string {} unmappable", si)));
            groups.entry(ri).or_default().push(si);
        }
        for (ri, mut g) in groups {
            g.sort_by_key(|&si| {
                let len = lens[si] as usize;
                let fstart: u64 = (0..si as u64).map(|j| lens[j as usize] + 1).sum();
                // following text starts right after this string's sentinel; EOF-ending (last string) = empty = smallest
                let after = fstart + len as u64 + 1;
                let want = 512usize;
                let file_len = flatf.seek(std::io::SeekFrom::End(0)).unwrap();
                let avail = if after >= file_len { 0 } else { (file_len - after).min(want as u64) as usize };
                read_flat(&mut flatf, after, avail)
            });
            let mut rows = left_by_run.remove(&ri).unwrap();
            if rows.len() != g.len() { die(&format!("build-anchors: run {} has {} free rows but {} member strings", ri, rows.len(), g.len())); }
            rows.sort();
            for (&si, &row) in g.iter().zip(rows.iter()) {
                assigned[si] = Some(row);
                owner.insert(row, si);
            }
        }
    }
    // ---- final invariants ----
    for si in 0..k as usize {
        if assigned[si].is_none() { die(&format!("build-anchors: string {} never assigned", si)); }
    }
    if owner.len() != k as usize { die("build-anchors: owner map size mismatch"); }
    // every 0x0A row must be owned, and the run-end sample law must hold
    let mut checked = 0u64;
    for &(ri, rs, re) in &runs {
        for row in rs..re {
            let si = *owner.get(&row).unwrap_or_else(|| die(&format!("build-anchors: 0x0A row {} unowned", row)));
            if assigned[si] != Some(row) { die("build-anchors: invariant violated"); }
        }
        let last = re - 1;
        let si = owner[&last];
        if idx.tail_sample(ri) != fs[si] + lens[si] { die(&format!("build-anchors: run {} end law violated (sample {} != S of string {})", ri, idx.tail_sample(ri), si)); }
        checked += re - rs;
    }
    let mut anchors: Vec<(u64, u64)> = Vec::with_capacity(k as usize);
    for si in 0..k as usize {
        anchors.push((assigned[si].unwrap(), fs[si] + lens[si]));
    }
    anchors.sort();
    for w in anchors.windows(2) { if w[0].0 == w[1].0 { die("build-anchors: duplicate anchor rows"); } }
    let mut f = File::create(&outp).unwrap_or_else(|e| die(&format!("create {}: {}", outp, e)));
    f.write_all(&b"XANC"[..]).unwrap();
    f.write_all(&k.to_le_bytes()).unwrap();
    for (row, s) in &anchors {
        f.write_all(&row.to_le_bytes()).unwrap();
        f.write_all(&s.to_le_bytes()).unwrap();
    }
    eprintln!("xsa build-anchors: {} runs checked; wrote {} anchors to {}", checked, k, outp);
    anchors.sort();
    for w in anchors.windows(2) { if w[0].0 == w[1].0 { die("build-anchors: duplicate anchor rows"); } }
    let mut f = File::create(&outp).unwrap_or_else(|e| die(&format!("create {}: {}", outp, e)));
    f.write_all(&b"XANC"[..]).unwrap();
    f.write_all(&k.to_le_bytes()).unwrap();
    for (row, s) in &anchors {
        f.write_all(&row.to_le_bytes()).unwrap();
        f.write_all(&s.to_le_bytes()).unwrap();
    }
    eprintln!("xsa build-anchors: wrote {} anchors to {}", k, outp);
}

// ---------- chi-rspace: the r-space chi/sA construction ----------
// Consumes the per-run aggregates sidecar (bit6/chi_rspace_dump.cpp:
// topLCP, saFirst, saLast, interiorMin per run -- all computed r-space via
// PFP resolve + LCE + the phi-piece law) and runs the production scan-rs
// state machine VERBATIM (bit6/teralcp_chi.cpp): no walks, no SA, no text
// access. Legacy witnesses use n+1-pos; cyclic witnesses use (n-pos)%n.
fn cmd_chi_rspace(args: &[String]) {
    let mut ri4p: Option<String> = None;
    let mut aggp: Option<String> = None;
    let mut outp: Option<String> = None;
    let mut stream_agg = false;
    let mut i = 0;
    while i < args.len() {
        let step;
        match args[i].as_str() {
            "--ri4" | "--sxi" if i + 1 < args.len() => { ri4p = Some(args[i + 1].clone()); step = 2; }
            "--stream-agg" => { stream_agg = true; step = 1; }
            "--agg" if i + 1 < args.len() => { aggp = Some(args[i + 1].clone()); step = 2; }
            "-o" | "--output" if i + 1 < args.len() => { outp = Some(args[i + 1].clone()); step = 2; }
            other => die(&format!("chi-rspace: unknown arg {}", other)),
        }
        i += step;
    }
    let (rp, ap) = (
        ri4p.unwrap_or_else(|| die("chi-rspace: need --ri4")),
        aggp.unwrap_or_else(|| die("chi-rspace: need --agg")),
    );
    let header = read_ri4_header(&rp);
    if header.version>=2 {die("chi-rspace requires SXI1 or ri4 source runs");}
    // Streaming sweep needs only the run characters, not LF tables/samples.
    let idx = if stream_agg { None } else { Some(Ri4::load(&rp)) };
    let mut char_file = BufReader::with_capacity(1 << 20, File::open(&rp).unwrap());
    char_file.seek(SeekFrom::Start(header.runs_offset)).unwrap();
    let mut f = File::open(&ap).unwrap_or_else(|e| die(&format!("open {}: {}", ap, e)));
    let mut hdr = [0u8; 12];
    f.read_exact(&mut hdr).unwrap_or_else(|e| die(&format!("read {}: {}", ap, e)));
    let magic = u32::from_le_bytes(hdr[0..4].try_into().unwrap());
    if magic != 0x31415243 { die("agg: bad magic (need CRA1)"); }
    let r = u64::from_le_bytes(hdr[4..12].try_into().unwrap());
    if r != header.r { die(&format!("agg R {} != ri4 R {}", r, header.r)); }
    let rd_u64 = |f: &mut File, n: usize| -> Vec<u64> {
        let mut v = vec![0u8; n * 8];
        f.read_exact(&mut v).unwrap_or_else(|e| die(&format!("read agg: {}", e)));
        v.chunks_exact(8).map(|c| u64::from_le_bytes(c.try_into().unwrap())).collect()
    };
    let expected = r.checked_mul(32).and_then(|x| x.checked_add(12))
        .unwrap_or_else(|| die("agg length overflow"));
    if f.metadata().unwrap().len() != expected { die("agg: truncated or trailing data"); }
    let arrays: Vec<Vec<u64>> = if stream_agg { Vec::new() }
        else { (0..4).map(|_| rd_u64(&mut f, r as usize)).collect() };
    // Independent descriptors: cloned File handles would SHARE seek offsets.
    let mut streams: Vec<BufReader<File>> = if stream_agg {
        (0..4).map(|field| {
            let mut file = File::open(&ap).unwrap();
            file.seek(SeekFrom::Start(12 + field * r * 8)).unwrap();
            BufReader::with_capacity(512 * 1024, file)
        }).collect()
    } else { Vec::new() };
    const INF: u64 = u64::MAX;
    const IINF: i64 = i64::MAX;
    // Match slim's alphabet-based cyclic convention without reading text.
    let mut has_rs = false;
    let mut has_nl = false;
    for _ in 0..header.r {
        let mut ch = [0u8; 1];
        char_file.read_exact(&mut ch).unwrap();
        has_rs |= ch[0] == 0x1e;
        has_nl |= ch[0] == 0x0a;
    }
    char_file.seek(SeekFrom::Start(header.runs_offset)).unwrap();
    let cyclic = has_rs || !has_nl;
    let big_n = if cyclic { header.n } else { header.n + 1 };
    // The candidate is the BWT character preceding SA=pos. Reversing its
    // cyclic coordinate gives n-pos, with pos=0 wrapping to coordinate 0.
    let witness = |pos: u64| -> u64 {
        if pos >= header.n { die("chi-rspace: endpoint outside text"); }
        if cyclic { if pos == 0 { 0 } else { header.n - pos } }
        else { big_n - pos }
    };
    const SIGMA: usize = 256;
    let mut rr_len = vec![-1i64; SIGMA];
    let mut rr_pos = vec![0u64; SIGMA];
    let mut rr_act = vec![false; SIGMA];
    let mut out: Vec<u64> = Vec::new();
    let mut streamed_out: Option<Box<dyn Write>> = if stream_agg {
        Some(match &outp {
            Some(path) => Box::new(BufWriter::with_capacity(1 << 20,
                File::create(path).unwrap_or_else(|e| die(&format!("create {}: {}", path, e))))),
            None => Box::new(BufWriter::new(std::io::stdout())),
        })
    } else { None };
    let mut chi: usize = 0;
    let mut emit = |value: u64| {
        chi += 1;
        if let Some(writer) = streamed_out.as_mut() {
            if outp.is_some() { writer.write_all(&value.to_le_bytes()).unwrap(); }
            else { writeln!(writer, "{}", value).unwrap(); }
        } else { out.push(value); }
    };
    let mut previous_interior = INF;
    let mut m: i64 = IINF;
    let mut p: i32 = -1;
    let mut p_sa_last: u64 = 0;
    for i in 0..header.r as usize {
        let mut values = [0u64; 4];
        for field in 0..4 {
            values[field] = if stream_agg {
                let mut bytes = [0u8; 8];
                streams[field].read_exact(&mut bytes).unwrap_or_else(|e| die(&format!("agg read: {}", e)));
                u64::from_le_bytes(bytes)
            } else { arrays[field][i] };
        }
        let mut ch = [0u8; 1];
        let raw = if let Some(ref index) = idx { index.run_char[i] }
            else { char_file.read_exact(&mut ch).unwrap(); ch[0] };
        let c: i32 = if !cyclic && raw == 0x0A { 0 } else { raw as i32 };
        if c as usize >= SIGMA { die("chi-rspace: character outside supported alphabet"); }
        let lcp_b = values[0] as i64;
        if i == 0 {
            p = c;
            p_sa_last = values[2];
            previous_interior = values[3];
            m = IINF;
            continue;
        }
        let m2: i64 = if previous_interior == INF { IINF } else { previous_interior as i64 };
        previous_interior = values[3];
        let mm = m.min(m2);
        if c != p {
            let m3 = mm.min(lcp_b);
            for cc in 1..SIGMA {
                if m3 < rr_len[cc] {
                    if rr_act[cc] { emit(rr_pos[cc]); }
                    rr_len[cc] = m3; rr_pos[cc] = 0; rr_act[cc] = false;
                }
            }
            if lcp_b > rr_len[p as usize] { rr_len[p as usize] = lcp_b; rr_pos[p as usize] = witness(p_sa_last); rr_act[p as usize] = true; }
            if lcp_b > rr_len[c as usize] { rr_len[c as usize] = lcp_b; rr_pos[c as usize] = witness(values[1]); rr_act[c as usize] = true; }
            m = IINF;
        } else {
            m = mm.min(lcp_b);
        }
        p = c;
        p_sa_last = values[2];
    }
    for cc in 1..SIGMA {
        if -1i64 < rr_len[cc] {
            if rr_act[cc] { emit(rr_pos[cc]); }
            rr_len[cc] = -1; rr_pos[cc] = 0; rr_act[cc] = false;
        }
    }
    drop(emit);
    if let Some(mut writer) = streamed_out {
        writer.flush().unwrap();
        eprintln!("xsa chi-rspace: chi = {} (N={}, R={}) stream-agg=true buffers=4194304", chi, big_n, header.r);
        return;
    }
    eprintln!("xsa chi-rspace: chi = {} (N={}, R={})", chi, big_n, header.r);
    match &outp {
        Some(path) => {
            let mut o = File::create(path).unwrap_or_else(|e| die(&format!("create {}: {}", path, e)));
            for v in &out {
                o.write_all(&v.to_le_bytes()).unwrap();
            }
            eprintln!("xsa chi-rspace: wrote {} witness values to {}", chi, path);
        }
        None => { for v in &out { println!("{}", v); } }
    }
}

fn cmd_tags(args: &[String]) {
    let mut chi: Option<String> = None;
    let mut sidecar: Option<String> = None;
    let mut revlines = false;
    let mut out: Option<String> = None;
    let mut i = 0;
    while i < args.len() {
        let step;
        match args[i].as_str() {
            "--chi" if i + 1 < args.len() => { chi = Some(args[i + 1].clone()); step = 2; }
            "--sidecar" if i + 1 < args.len() => { sidecar = Some(args[i + 1].clone()); step = 2; }
            "--output" if i + 1 < args.len() => { out = Some(args[i + 1].clone()); step = 2; }
            "--revlines" => { revlines = true; step = 1; }
            other => die(&format!("tags: unknown arg {}", other)),
        }
        i += step;
    }
    let (chi_path, sc_path) = (
        chi.unwrap_or_else(|| die("tags: need --chi <f.sA>")),
        sidecar.unwrap_or_else(|| die("tags: need --sidecar <names.tsv>")),
    );

    // sidecar: name \t fstart \t len (names may contain spaces; tab-split only)
    struct Str { name: String, fs: u64, len: u64, fstart_stream: u64 }
    let mut strs: Vec<Str> = Vec::new();
    let sf = File::open(&sc_path).unwrap_or_else(|e| die(&format!("open {}: {}", sc_path, e)));
    for (ln, line) in BufReader::new(sf).lines().enumerate() {
        let line = line.unwrap_or_else(|e| die(&format!("read {}: {}", sc_path, e)));
        if line.is_empty() { continue; }
        let c: Vec<&str> = line.split('\t').collect();
        if c.len() < 3 {
            die(&format!("{}:{}: expected 3 tab-separated fields", sc_path, ln + 1));
        }
        let (fs, len): (u64, u64) = (
            c[1].trim().parse().unwrap_or_else(|_| die(&format!("{}:{}: bad fstart", sc_path, ln + 1))),
            c[2].trim().parse().unwrap_or_else(|_| die(&format!("{}:{}: bad len", sc_path, ln + 1))),
        );
        // stream fstart derived from lens (+ sentinels), NEVER from the
        // sidecar's forward offset (sub-collection sidecars carry offsets
        // into a larger original text; only len is authoritative here)
        let fstart_stream: u64 = if ln == 0 { 0 } else {
            strs.last().map(|s: &Str| s.fstart_stream + s.len + 1).unwrap()
        };
        strs.push(Str { name: c[0].to_string(), fs, len, fstart_stream });
    }
    if strs.is_empty() { die("tags: empty sidecar"); }
    let n_streams: u64 = strs.last().unwrap().fstart_stream + strs.last().unwrap().len + 1;
    let chi_n = chi_count(&chi_path);   // sanity: count vs sidecar n below

    // chi positions: u64 LE stream
    let cf = File::open(&chi_path).unwrap_or_else(|e| die(&format!("open {}: {}", chi_path, e)));
    let mut cnt: Vec<u64> = vec![0; strs.len()];
    let mut tags: Vec<(u64, u64)> = Vec::new();   // (chi position, forward position)
    let mut buf = [0u8; 8];
    let mut nchi = 0u64;
    let mut virtual_end = 0u64;
    let mut first_err: Option<String> = None;
    {
        let mut r = cf;
        loop {
            match r.read_exact(&mut buf) {
                Ok(()) => {}
                Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => break,
                Err(e) => die(&format!("read {}: {}", chi_path, e)),
            }
            let p = u64::from_le_bytes(buf);
            nchi += 1;
            if p == n_streams {
                // virtual end witness (S subseteq [n]): the theory includes
                // position n; attribute to no string, emit fwd = -1
                virtual_end += 1;
                tags.push((p, u64::MAX));
                continue;
            }
            // owning string: fstart_stream <= p <= fstart_stream + len
            // (endmarker positions are legitimate chi members: the walk starts there)
            let ii = strs.partition_point(|s| s.fstart_stream <= p);
            let i = if ii == 0 { usize::MAX } else { ii - 1 };
            if i == usize::MAX || p > strs[i].fstart_stream + strs[i].len {
                if first_err.is_none() {
                    first_err = Some(format!("position {} outside any string", p));
                }
                break;
            }
            let s = &strs[i];
            let j = p - s.fstart_stream;               // 0-based within-string stream offset
            let fwd = if revlines {
                s.fs + (s.len - j)                     // reversed stream -> forward coords
            } else {
                s.fs + j
            };
            cnt[i] += 1;
            tags.push((p, fwd));
        }
    }
    if let Some(e) = first_err { die(&format!("tags: {}", e)); }
    if nchi == 0 { die("tags: empty chi set"); }

    if let Some(op) = &out {
        let mut f = File::create(op).unwrap_or_else(|e| die(&format!("create {}: {}", op, e)));
        for (p, fwd) in &tags {
            if *fwd == u64::MAX {
                writeln!(f, "{}\t-1", p).unwrap();   // virtual end witness
            } else {
                writeln!(f, "{}\t{}", p, fwd).unwrap();
            }
        }
    }

    println!("chi positions  = {} over {} strings (n = {})", nchi, strs.len(), n_streams);
    if virtual_end > 0 {
        println!("virtual end    = {} (position n; S subseteq [n] convention)", virtual_end);
    }
    let _ = chi_n;
    println!("per-string first-appearance counts (top 15):");
    let mut idx: Vec<usize> = (0..strs.len()).collect();
    idx.sort_by(|&a, &b| cnt[b].cmp(&cnt[a]).then(strs[a].name.cmp(&strs[b].name)));
    for &i in idx.iter().take(15) {
        if cnt[i] == 0 { break; }
        println!("  {:>12}  {}", cnt[i], strs[i].name);
    }
    let tot: u64 = cnt.iter().sum::<u64>() + virtual_end;
    println!("sum check      = {} {}", tot, if tot == nchi { "OK" } else { "MISMATCH" });
    if tot != nchi { exit(1); }
}

fn usage() -> ! {
    eprintln!("xsa — chi sa. suffixient-array tools over r-space artifacts");
    eprintln!();
    eprintln!("usage:");
    eprintln!("  xsa build --agc <archive.agc> -o <out.sxi> [--verbose]");
    eprintln!("  xsa build --fasta <refs.fa> -o <out.sxi> [--verbose]");
    eprintln!("  xsa build --text <file.txt> -o <out.sxi> [--verbose]");
    eprintln!("  xsa mems --sxi FILE --reads FA|FQ|GZ [-j N] [--min-len 20] [--mode auto|dna|text]");
    eprintln!("  xsa witness-build --sxi FILE --output FILE.wit");
    eprintln!("  xsa mems --first --sxi FILE --witness-index FILE.wit --reads FA|FQ|GZ");
    eprintln!("  xsa serve --sxi FILE [-j N] [--bind 127.0.0.1:7331]");
    eprintln!("  xsa sxi-info <f.sxi> [--chi-out sorted.sA]");
    eprintln!("  --sxi is an alias for --ri4; format is detected by magic");
    eprintln!("  xsa stats --ri4 <f.ri4> [--chi <f.sA>]   header + law check");
    eprintln!("  xsa stats --rlbwt <prefix> [--chi ...]   from rlbwt pair");
    eprintln!("  xsa tags --chi <f.sA> --sidecar <names.tsv> [--revlines]");
    eprintln!("           [--output tags.tsv]                  first-appearance attribution");
    eprintln!("  xsa query --ri4 <f.ri4> --patterns <p.fa> [--output occs.txt]");
    eprintln!("  xsa query --ri4 <f.ri4> --patterns <p.fa> --ms [--ms-out lens.bin] [--plain]");
    eprintln!("           --ms: matching-statistics length vector per read (binary)");
    eprintln!();
    eprintln!("artifacts: .sxi (SXI1: rlbwt + runs + head/tail samples + anchors + delta-chi)");
    eprintln!("           .ri4 (v4; pilot-era files stay loadable), .sA (chi, u64 LE)");
    exit(2);
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(|s| s.as_str()) {
        Some("build") => build::command(&args[1..]),
        Some("sxi-info") => sxi::command(&args[1..]),
        Some("stats") => cmd_stats(&args[1..]),
        Some("tags") => cmd_tags(&args[1..]),
        Some("query") if args.windows(2).any(|a| matches!(a[0].as_str(), "--sxi" | "--ri4") && {
            let mut magic = [0;4]; File::open(&a[1]).and_then(|mut f| f.read_exact(&mut magic)).is_ok()
                && (&magic == b"SXI1" || &magic == b"SXI2")
        }) => product::command("query", &args[1..]),
        Some("query") => cmd_query(&args[1..]),
        Some("mems") => product::command("mems", &args[1..]),
        Some("witness-build") => product::witness_build(&args[1..]),
        Some("serve") => product::command("serve", &args[1..]),
        Some("build-anchors") => cmd_build_anchors(&args[1..]),
        Some("chi-rspace") => cmd_chi_rspace(&args[1..]),
        Some("-h") | Some("--help") | None => usage(),
        Some(other) => die(&format!("unknown subcommand '{}' (try --help)", other)),
    }
}
