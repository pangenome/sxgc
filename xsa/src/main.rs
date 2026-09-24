//! xsa — chi sa.
//!
//! Suffixient-array tools over the r-space artifact family:
//!   .ri4        v4 self-contained index core (rlbwt + C + run-end SA samples)
//!   <name>.sA   chi position set (u64 LE stream)
//!   .bwt.heads/.bwt.len  run-length BWT (u8 heads, u40 LE lengths)
//!
//! The index is sovereign: every subcommand runs from these artifacts alone.

use std::fs::File;
use std::io::{BufRead, BufReader, Read, Write};
use std::process::exit;

fn die(msg: &str) -> ! {
    eprintln!("xsa: error: {}", msg);
    exit(1);
}

/// Parsed .ri4 header (v4): n, k, R. Layout (little-endian):
///   u32 magic 0x52585349 ("ISXR"), u32 version, u64 n, u64 k, u64 R, ...
#[derive(Debug, Clone, Copy)]
struct Ri4Header {
    version: u32,
    n: u64,
    k: u64,
    r: u64,
}

fn read_ri4_header(path: &str) -> Ri4Header {
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
            Ri4Header { version, n, k, r }
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

/// sdsl int_vector bitstream: entries of `width` bits, LSB-first per u64 word.
struct PackedSa {
    bits: u64,          // total size IN BITS
    width: u8,
    data: Vec<u8>,      // little-endian u64 words
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

struct Ri4 {
    n: u64,
    r: u64,
    c: Vec<u64>,
    run_char: Vec<u8>,
    run_len: Vec<u32>,
    sa: PackedSa,
    run_start_blk: Vec<u64>,
    cruns: Vec<Vec<u32>>,
    csum: Vec<Vec<u64>>,
    total: Vec<u64>,
}
const BLK: u64 = 64;

impl Ri4 {
    fn load(path: &str) -> Ri4 {
        let mut f = File::open(path).unwrap_or_else(|e| die(&format!("open {}: {}", path, e)));
        let mut b = [0u8; 32];
        f.read_exact(&mut b).unwrap_or_else(|e| die(&format!("read {}: {}", path, e)));
        let magic = u32::from_le_bytes(b[0..4].try_into().unwrap());
        if magic != 0x52585349 { die(&format!("{}: bad magic", path)); }
        let version = u32::from_le_bytes(b[4..8].try_into().unwrap());
        if version != 4 { die(&format!("{}: need v4, got v{}", path, version)); }
        let n = u64::from_le_bytes(b[8..16].try_into().unwrap());
        let k = u64::from_le_bytes(b[16..24].try_into().unwrap());
        let r = u64::from_le_bytes(b[24..32].try_into().unwrap());
        let _ = k;
        let mut c = vec![0u64; 256];
        f.read_exact(unsafe { std::slice::from_raw_parts_mut(c.as_mut_ptr() as *mut u8, 2048) })
            .unwrap_or_else(|e| die(&format!("read C: {}", e)));
        let mut run_char = vec![0u8; r as usize];
        f.read_exact(&mut run_char).unwrap_or_else(|e| die(&format!("read runs: {}", e)));
        let mut rl = vec![0u8; (r * 4) as usize];
        f.read_exact(&mut rl).unwrap_or_else(|e| die(&format!("read lens: {}", e)));
        let mut run_len = Vec::with_capacity(r as usize);
        for i in 0..r as usize {
            run_len.push(u32::from_le_bytes(rl[4 * i..4 * i + 4].try_into().unwrap()));
        }
        drop(rl);
        // sdsl int_vector header: u64 size-in-bits, u8 width, then words
        let mut hb = [0u8; 9];
        f.read_exact(&mut hb).unwrap_or_else(|e| die(&format!("read sa header: {}", e)));
        let bits = u64::from_le_bytes(hb[0..8].try_into().unwrap());
        let width = hb[8];
        if width == 0 || width > 64 || bits != r * width as u64 {
            die(&format!("{}: bad sa array (bits={} width={}?)", path, bits, width));
        }
        let nbytes = ((bits + 63) / 64 * 8) as usize;
        let mut data = vec![0u8; nbytes];
        f.read_exact(&mut data).unwrap_or_else(|e| die(&format!("read sa data: {}", e)));
        let sa = PackedSa { bits, width, data };

        let mut run_start_blk = Vec::with_capacity((r / BLK + 2) as usize);
        let mut acc = 0u64;
        for x in 0..r {
            if x % BLK == 0 { run_start_blk.push(acc); }
            acc += run_len[x as usize] as u64;
        }
        run_start_blk.push(acc);
        let mut cruns: Vec<Vec<u32>> = vec![Vec::new(); 256];
        let mut csum: Vec<Vec<u64>> = vec![Vec::new(); 256];
        let mut acc256 = vec![0u64; 256];
        for x in 0..r as usize {
            let c = run_char[x] as usize;
            cruns[c].push(x as u32);
            csum[c].push(acc256[c]);
            acc256[c] += run_len[x] as u64;
        }
        let total = acc256.clone();
        for c in 0..256 { csum[c].push(total[c]); }
        Ri4 { n, r, c, run_char, run_len, sa, run_start_blk, cruns, csum, total }
    }

    fn run_start(&self, r: u64) -> u64 {
        let mut s = self.run_start_blk[(r / BLK) as usize];
        let mut q = (r / BLK) * BLK;
        while q < r { s += self.run_len[q as usize] as u64; q += 1; }
        s
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
        if self.run_char[r as usize] == c { self.csum[c as usize][j] + (i - s) }
        else { self.csum[c as usize][j] }
    }
    fn lf(&self, i: u64) -> u64 {
        let c = self.run_char[self.run_of(i) as usize];
        self.c[c as usize] + self.rank(c, i)
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
        let mut pos = j;
        let mut steps = 0u64;
        loop {
            let r = self.run_of(pos);
            let e = self.run_start(r) + self.run_len[r as usize] as u64;
            if pos == e - 1 {
                return self.sa.get(r).wrapping_sub(steps);
            }
            pos = self.lf(pos);
            steps += 1;
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
    let mut ms_out: Option<String> = None;
    let mut sample: Option<u64> = None;
    let mut seed: u64 = 0;
    let mut i = 0;
    while i < args.len() {
        let step;
        match args[i].as_str() {
            "--ri4" if i + 1 < args.len() => { ri4 = Some(args[i + 1].clone()); step = 2; }
            "--patterns" if i + 1 < args.len() => { pats = Some(args[i + 1].clone()); step = 2; }
            "--output" if i + 1 < args.len() => { out = Some(args[i + 1].clone()); step = 2; }
            "--sidecar" if i + 1 < args.len() => { sidecar = Some(args[i + 1].clone()); step = 2; }
            "--ms-out" if i + 1 < args.len() => { ms_out = Some(args[i + 1].clone()); step = 2; }
            "--sample" if i + 1 < args.len() => { sample = Some(args[i + 1].parse::<u64>().unwrap_or_else(|_| die("--sample: integer expected"))); step = 2; }
            "--seed" if i + 1 < args.len() => { seed = args[i + 1].parse::<u64>().unwrap_or_else(|_| die("--seed: integer expected")); step = 2; }
            "--plain" => { plain = true; step = 1; }
            "--ms" => { ms = true; step = 1; }
            other => die(&format!("query: unknown arg {}", other)),
        }
        i += step;
    }
    let (ri4, pats) = (
        ri4.unwrap_or_else(|| die("query: need --ri4")),
        pats.unwrap_or_else(|| die("query: need --patterns (FASTA)")),
    );
    if plain && sidecar.is_none() && !ms {
        die("query: --plain needs --sidecar (v4 samples mirror positions on plain chains)");
    }
    eprintln!("xsa query: loading {} ...", ri4);
    let idx = Ri4::load(&ri4);
    eprintln!("  n={} R={}", idx.n, idx.r);

    // plain-chain decode: v4 samples are mirrored (S = fstart+fend-pos);
    // undo per owning string: pos = fstart_i + fend_i - S
    let (mut p_fstart, mut p_fend): (Vec<u64>, Vec<u64>) = (Vec::new(), Vec::new());
    if let Some(sc) = &sidecar {
        let sf = File::open(sc).unwrap_or_else(|e| die(&format!("open {}: {}", sc, e)));
        let mut acc = 0u64;
        for line in BufReader::new(sf).lines() {
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
                let s = idx.s_at(j);
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
        for j in l..r {
            let s = idx.s_at(j);
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
            "--ri4" if i + 1 < args.len() => ri4 = Some(args[i + 1].clone()),
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
    if let Some(p) = &ri4 {
        let h = read_ri4_header(p);
        n = h.n;
        k = h.k;
        r = h.r;
        println!(".ri4      {}  (v{}: n={}, k={} strings, R={} runs)", p, h.version, h.n, h.k, h.r);
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
    });

    println!();
    println!("n            = {}", n);
    if k > 0 { println!("k (strings)  = {}", k); }
    println!("r (runs)     = {}", r);
    if let Some(xc) = x {
        println!("chi          = {}", xc);
        println!("n/r          = {}", fmt_ratio(n as f64, r as f64));
        println!("chi/r        = {}   (law: ~0.86)", fmt_ratio(xc as f64, r as f64));
    }
    println!("index bytes  ~ {} (rlbwt 6R + samples 8R)", r * 14);
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
    eprintln!("  xsa stats --ri4 <f.ri4> [--chi <f.sA>]   header + law check");
    eprintln!("  xsa stats --rlbwt <prefix> [--chi ...]   from rlbwt pair");
    eprintln!("  xsa tags --chi <f.sA> --sidecar <names.tsv> [--revlines]");
    eprintln!("           [--output tags.tsv]                  first-appearance attribution");
    eprintln!("  xsa query --ri4 <f.ri4> --patterns <p.fa> [--output occs.txt]");
    eprintln!("  xsa query --ri4 <f.ri4> --patterns <p.fa> --ms [--ms-out lens.bin] [--plain]");
    eprintln!("           --ms: matching-statistics length vector per read (binary)");
    eprintln!();
    eprintln!("artifacts: .ri4 (v4: rlbwt + C + run-end SA samples), .sA (chi, u64 LE)");
    exit(2);
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(|s| s.as_str()) {
        Some("stats") => cmd_stats(&args[1..]),
        Some("tags") => cmd_tags(&args[1..]),
        Some("query") => cmd_query(&args[1..]),
        Some("-h") | Some("--help") | None => usage(),
        Some(other) => die(&format!("unknown subcommand '{}' (try --help)", other)),
    }
}
