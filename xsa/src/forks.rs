//! `xsa forks` — the χ fork-matrix extractor (paper two: the χ genotyping
//! platform). Contract: docs/GWAS_FORK_MATRIX.md + the ratified fork-matrix
//! semantics that answer the blockers in bit6/sxi_logs/forks/REPORT.md.
//!
//! THE MATRIX is STRING × (WITNESS, CLASS) BINARY PRESENCE. Each χ witness is
//! an END-positioned context event (lean/Sxgc.lean: a position whose prefix
//! ends a right-maximal context followed by a character). The artifact stores
//! witnesses as positions only; the audit found that no stored field picks a
//! context interval, so the ratified semantics are the artifact-honest
//! presence reading:
//!
//!   * a witness resolves to the run-boundary row it sits on — the row whose
//!     SA sample (ssa = run-head samples, ssa_t = mirrored run-tail samples)
//!     maps to the witness position x = (n - SA) mod n (0 ≡ the virtual end);
//!   * the run(s) whose interval witnesses it are the runs meeting at that
//!     boundary (the witness row's own run and its boundary neighbour; a
//!     singleton run whose only row is the witness also contributes the two
//!     outer neighbours);
//!   * the witness's occurrences are the rows of those runs; the classes are
//!     the DISTINCT next-symbol (BWT) characters realized among them — two
//!     separated runs of the same character are one class, never two (audit
//!     blocker 3);
//!   * `strings` lists the collection members containing each occurrence's
//!     branch-character position (member id from the embedded names TSV).
//!     A string may realize several classes at one witness; that multi-label
//!     presence is the honest semantics and is never collapsed.
//!
//! Output: JSONL records {"witness":x,"class_id":i,"char":c,"strings":[ids]}
//! streamed in text order (increasing position, the virtual end last).
//!
//! Both artifact generations are extracted directly: SXI1 (raw run table,
//! packed mirrored tails, raw heads) and SXI2 v3+ (Huffman runs, compact φ
//! edges). Positions of run rows are resolved text-free by a windowed LF
//! walk: the LF image of a run is a contiguous row interval, so whole row
//! segments march in lockstep and exit at run-head SA samples with
//! SA = head_sample + steps (mod n).
use super::{die, product, sxi, sxi2};
use std::fs::File;
use std::io::{BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;

const BLOCK: u64 = 1024; // rows per sampled row->run index entry

fn need(ok: bool, msg: &str) {
    if !ok {
        die(&format!("forks: {msg}"));
    }
}

/// Per-run tables shared by every phase. All arrays are run-indexed.
struct Table {
    n: u64,
    r: u64,
    chars: Vec<u8>,
    lens: Vec<u32>,
    start: Vec<u64>, // first BWT row of each run
    lf: Vec<u64>,   // LF() of each run's first row
    head_sa: Vec<u64>,
    tail_sa: Vec<u64>,
    sample: Vec<u32>, // run containing row BLOCK*i
    // φ-domain table (criterion C): sorted by u; φ(p) = v_dom(p) + (p - u_dom(p)) mod n
    // for p in [u_dom, next_u). Built from ssa/ssa_t: u = tail SA of a run,
    // v = head SA of the next run.
    phi_u: Vec<u64>,
    phi_v: Vec<u64>,
    phi_ok: bool, // char-frequency gcd == 1 certifies criterion C
}

impl Table {
    fn load(path: &str, c: &sxi::Container) -> Self {
        let (n, r) = (c.n, c.r);
        let (chars, lens, head_sa, tail_sa);
        let ctable;
        let (mut phi_u, mut phi_v);
        let mut phi_ok = false;
        if c.version == 1 {
            let (ct, ch, ln, hs, ts) = load_sxi1_runs(path, c);
            ctable = ct;
            chars = ch;
            lens = ln;
            head_sa = hs;
            tail_sa = ts;
            // φ domains from ssa/ssa_t: (u = tail SA of run i, v = head SA of run i+1).
            let mut pu: Vec<u64> = Vec::with_capacity(r as usize);
            let mut pv: Vec<u64> = Vec::with_capacity(r as usize);
            for i in 0..r as usize {
                pu.push(tail_sa[i]);
                pv.push(head_sa[(i + 1) % r as usize]);
            }
            let sorted = pu.windows(2).all(|w| w[0] < w[1]);
            if !sorted {
                radix_sort_pairs(&mut pu, &mut pv);
            }
            phi_u = pu;
            phi_v = pv;
        } else {
            let (ct, ch, ln) = sxi2::runs(path, c.member(1), n, r);
            ctable = ct;
            chars = ch;
            lens = ln;
            let (hs, ts, pu, pv) = load_sxi2_samples(path, c, &ctable);
            head_sa = hs;
            tail_sa = ts;
            phi_u = pu;
            phi_v = pv;
        }
        need(lens.len() == r as usize, "run count mismatch");
        // Row starts and LF starts, one pass over the runs.
        let mut start = Vec::with_capacity(r as usize);
        let mut lf = Vec::with_capacity(r as usize);
        let mut seen = [0u64; 256];
        let mut row = 0u64;
        for i in 0..r as usize {
            start.push(row);
            let ch = chars[i] as usize;
            lf.push(ctable[ch] + seen[ch]);
            seen[ch] += lens[i] as u64;
            row += lens[i] as u64;
        }
        need(row == n, "run lengths do not sum to n");
        // Sampled row->run index: the run containing row BLOCK*i.
        let blocks = ((n + BLOCK - 1) / BLOCK) as usize;
        let mut sample = vec![0u32; blocks];
        let mut block = 0usize;
        for i in 0..r as usize {
            let s = start[i];
            let end = s + lens[i] as u64;
            while block < blocks && (block as u64) * BLOCK < end {
                if (block as u64) * BLOCK >= s {
                    sample[block] = i as u32;
                }
                block += 1;
            }
        }
        need(r <= u32::MAX as u64, "run count exceeds u32");
        // Criterion C: gcd of the nonzero character counts == 1 certifies
        // that every φ domain is arithmetic (the same theorem the SXI2
        // writer's escape check relies on). φ(p) = v + (p - u) mod n.
        let mut g = 0u64;
        for i in 0..256 {
            let next = if i == 255 { n } else { ctable[i + 1] };
            let v = next.saturating_sub(ctable[i]);
            g = gcd_u64(g, v);
        }
        phi_ok = g == 1 || r == 1;
        Self { n, r, chars, lens, start, lf, head_sa, tail_sa, sample, phi_u, phi_v, phi_ok }
    }
    #[inline]
    fn run_of(&self, row: u64) -> u64 {
        let mut rho = self.sample[(row / BLOCK) as usize] as u64;
        while rho + 1 < self.r && self.start[rho as usize + 1] <= row {
            rho += 1;
        }
        rho
    }
    /// Smallest run-start (head row) >= row; n when none.
    #[inline]
    fn next_head(&self, row: u64) -> u64 {
        if row >= self.n {
            return self.n;
        }
        let rho = self.run_of(row);
        if self.start[rho as usize] == row {
            row
        } else if rho + 1 < self.r {
            self.start[rho as usize + 1]
        } else {
            self.n
        }
    }
    /// φ(p) = SA of the row following the row whose SA is p (cyclic), via
    /// the arithmetic domain table. Valid when phi_ok (criterion C).
    #[inline]
    fn phi(&self, p: u64) -> u64 {
        let i = match self.phi_u.binary_search(&p) {
            Ok(i) => i,
            Err(0) => self.phi_u.len() - 1, // wrap: last domain covers [u_last, n) + [0, u_first)
            Err(i) => i - 1,
        };
        let u = self.phi_u[i];
        let v = self.phi_v[i];
        (v + (p + self.n - u) % self.n) % self.n
    }
    /// Resolve SA for every row of run `rho` via the φ chain, with the
    /// ssa/ssa_t endpoint self-check: after len steps from the head sample
    /// the chain must land exactly on the tail sample.
    fn resolve_run(&self, rho: u64, sa_out: &mut Vec<u64>) {
        let len = self.lens[rho as usize] as usize;
        sa_out.clear();
        sa_out.resize(len, 0);
        let mut sa = self.head_sa[rho as usize];
        sa_out[0] = sa;
        for t in 1..len {
            sa = self.phi(sa);
            sa_out[t] = sa;
        }
        if sa != self.tail_sa[rho as usize] {
            die(&format!(
                "forks: φ chain for run {rho} ended at {sa} but ssa_t says {} (criterion C violated)",
                self.tail_sa[rho as usize]
            ));
        }
    }
    /// Text-free fallback (and cross-check) resolver: windowed LF walk.
    /// LF maps a run's rows to a contiguous row interval, so unresolved
    /// segments march in lockstep and exit at run-head SA samples with
    /// SA = head_sample + steps (mod n).
    fn resolve_run_lf(&self, rho: u64, sa_out: &mut Vec<u64>) {
        let a = self.start[rho as usize];
        let len = self.lens[rho as usize] as u64;
        sa_out.clear();
        sa_out.resize(len as usize, u64::MAX);
        let n = self.n;
        // Piece: rows [cur, cur+len) are LF^level images of [org, org+len);
        // cur -> org is affine with slope 1.
        let mut cur: Vec<(u64, u64, u64)> = vec![(a, len, a)];
        let mut nxt: Vec<(u64, u64, u64)> = Vec::new();
        let mut heads: Vec<u64> = Vec::new();
        let mut level = 0u64;
        while !cur.is_empty() {
            nxt.clear();
            for idx in 0..cur.len() {
                let (cs, cl, org) = cur[idx];
                let seg_end = cs + cl;
                let mut seg = cs;
                let mut rho2 = self.run_of(seg);
                while seg < seg_end {
                    let rstart = self.start[rho2 as usize];
                    let rend = rstart + self.lens[rho2 as usize] as u64;
                    let pend = seg_end.min(rend);
                    let img = self.lf[rho2 as usize] + seg - rstart;
                    let ilen = pend - seg;
                    // Run-head rows inside the image resolve their origins.
                    heads.clear();
                    let mut h = self.next_head(img);
                    while h < img + ilen {
                        heads.push(h);
                        h = self.next_head(h + 1);
                    }
                    for &h in &heads {
                        let hr = self.run_of(h) as usize;
                        let orow = org + (h - img);
                        sa_out[(orow - a) as usize] = (self.head_sa[hr] + level + 1) % n;
                    }
                    // Unresolved gaps continue one level deeper.
                    let mut s = img;
                    for &h in &heads {
                        if h > s {
                            nxt.push((s, h - s, org + (s - img)));
                        }
                        s = h + 1;
                    }
                    if s < img + ilen {
                        nxt.push((s, img + ilen - s, org + (s - img)));
                    }
                    seg = pend;
                    rho2 += 1;
                }
            }
            std::mem::swap(&mut cur, &mut nxt);
            level += 1;
            if level > n {
                die("forks: LF walk failed to reach a head sample (corrupt artifact)");
            }
        }
        if sa_out.iter().any(|&v| v == u64::MAX) {
            die("forks: LF walk left rows unresolved (corrupt artifact)");
        }
        let len = self.lens[rho as usize] as usize;
        if sa_out[0] != self.head_sa[rho as usize]
            || sa_out[len - 1] != self.tail_sa[rho as usize] {
            die(&format!("forks: LF walk for run {rho} disagrees with the ssa/ssa_t samples"));
        }
    }
}

fn gcd_u64(mut a: u64, mut b: u64) -> u64 {
    while b != 0 {
        let t = a % b;
        a = b;
        b = t;
    }
    a
}

/// LSD radix sort of (u, v) pairs by u, 8-bit passes.
fn radix_sort_pairs(u: &mut Vec<u64>, v: &mut Vec<u64>) {
    let n = u.len();
    let mut u2 = vec![0u64; n];
    let mut v2 = vec![0u64; n];
    for shift in (0..64).step_by(8) {
        let mut count = [0usize; 256];
        for &x in u.iter() {
            count[((x >> shift) & 255) as usize] += 1;
        }
        let mut starts = [0usize; 256];
        let mut off = 0usize;
        for i in 0..256 {
            starts[i] = off;
            off += count[i];
        }
        for i in 0..n {
            let b = ((u[i] >> shift) & 255) as usize;
            let t = starts[b];
            u2[t] = u[i];
            v2[t] = v[i];
            starts[b] += 1;
        }
        std::mem::swap(u, &mut u2);
        std::mem::swap(v, &mut v2);
    }
}

/// SXI1: C table + run chars + u32 lengths + raw head samples + packed
/// mirrored tail samples. Tail sample v stores n-1-SA, so SA = n-1-v.
fn load_sxi1_runs(path: &str, c: &sxi::Container) -> (Vec<u64>, Vec<u8>, Vec<u32>, Vec<u64>, Vec<u64>) {
    let (n, r) = (c.n, c.r);
    let mut f = BufReader::with_capacity(1 << 22, File::open(path).unwrap());
    let m1 = c.member(1);
    let mut ctab = vec![0u64; 256];
    f.seek(SeekFrom::Start(m1.offset)).unwrap();
    let mut eight = [0u8; 8];
    for v in ctab.iter_mut() {
        f.read_exact(&mut eight).unwrap();
        *v = u64::from_le_bytes(eight);
    }
    let mut chars = vec![0u8; r as usize];
    f.read_exact(&mut chars).unwrap();
    let mut lens = Vec::with_capacity(r as usize);
    let mut buf = vec![0u8; 1 << 20];
    let mut left = 4 * r; // u32 per run (the u8 chars were read separately)
    while left > 0 {
        let z = (left.min(buf.len() as u64)) as usize;
        f.read_exact(&mut buf[..z]).unwrap();
        let words = z / 4;
        for q in 0..words {
            lens.push(u32::from_le_bytes(buf[q * 4..q * 4 + 4].try_into().unwrap()));
        }
        left -= z as u64;
    }
    need(lens.len() == r as usize, "truncated run lengths");
    let m3 = c.member(3);
    need(m3.bytes == 8 * r, "head member size");
    f.seek(SeekFrom::Start(m3.offset)).unwrap();
    let mut head_sa = Vec::with_capacity(r as usize);
    left = 8 * r;
    while left > 0 {
        let z = (left.min(buf.len() as u64)) as usize;
        f.read_exact(&mut buf[..z]).unwrap();
        for q in 0..z / 8 {
            head_sa.push(u64::from_le_bytes(buf[q * 8..q * 8 + 8].try_into().unwrap()));
        }
        left -= z as u64;
    }
    need(head_sa.len() == r as usize, "truncated head samples");
    let m2 = c.member(2);
    f.seek(SeekFrom::Start(m2.offset)).unwrap();
    let mut hb = [0u8; 9];
    f.read_exact(&mut hb).unwrap();
    let bits = u64::from_le_bytes(hb[0..8].try_into().unwrap());
    let w = hb[8] as u32;
    need(w >= 1 && w <= 64 && bits == r * w as u64 && m2.bytes == 9 + (bits + 63) / 64 * 8,
        "packed tail header");
    let words = ((bits + 63) / 64) as usize;
    let mut packed = vec![0u8; words * 8];
    f.read_exact(&mut packed).unwrap();
    let mut tail_sa = Vec::with_capacity(r as usize);
    let mut reservoir = 0u128;
    let mut have = 0u32;
    let mut wi = 0usize;
    for _ in 0..r {
        while have < w {
            let word = u64::from_le_bytes(packed[wi..wi + 8].try_into().unwrap());
            reservoir |= (word as u128) << have;
            wi += 8;
            have += 64;
        }
        let v = (reservoir & ((1u128 << w) - 1)) as u64;
        need(v < n, "tail sample out of range");
        tail_sa.push(n - 1 - v);
        reservoir >>= w;
        have -= w;
    }
    (ctab, chars, lens, head_sa, tail_sa)
}

/// SXI2 v3+: head/tail SA per run and the φ domain table (u, v) from the
/// compact phi edges (u = tail SA of `run`, v = head SA of the next run;
/// edges arrive sorted by u).
fn load_sxi2_samples(path: &str, c: &sxi::Container, ctab: &[u64]) -> (Vec<u64>, Vec<u64>, Vec<u64>, Vec<u64>) {
    let (n, r) = (c.n, c.r);
    let phi = sxi2::Phi::load(path, c, ctab);
    let mut head_sa = vec![0u64; r as usize];
    let mut tail_sa = vec![0u64; r as usize];
    let mut phi_u = Vec::with_capacity(r as usize);
    let mut phi_v = Vec::with_capacity(r as usize);
    phi.for_each_edge(|u, v, run| {
        tail_sa[run as usize] = u;
        head_sa[((run + 1) % r) as usize] = v;
        phi_u.push(u);
        phi_v.push(v);
    });
    for i in 0..r as usize {
        need(head_sa[i] < n && tail_sa[i] < n, "phi sample range");
    }
    need(phi_u.windows(2).all(|w| w[0] < w[1]), "phi edges not sorted (corrupt artifact)");
    (head_sa, tail_sa, phi_u, phi_v)
}

/// χ witness set with a membership test. `Bits` for small n, `Sorted`
/// (with a cheap max filter) for large n / --n prefixes.
enum ChiSet {
    Bits(Vec<u64>),
    Sorted(Vec<u64>),
}

impl ChiSet {
    #[inline]
    fn contains(&self, x: u64) -> bool {
        match self {
            ChiSet::Bits(b) => b[(x / 64) as usize] & (1u64 << (x % 64)) != 0,
            ChiSet::Sorted(v) => {
                v.last().is_some_and(|&m| x <= m) && v.binary_search(&x).is_ok()
            }
        }
    }
    fn max(&self) -> u64 {
        match self {
            ChiSet::Bits(_) => u64::MAX,
            ChiSet::Sorted(v) => v.last().copied().unwrap_or(0),
        }
    }
    fn count(&self) -> u64 {
        match self {
            ChiSet::Bits(b) => b.iter().map(|w| w.count_ones() as u64).sum(),
            ChiSet::Sorted(v) => v.len() as u64,
        }
    }
    fn insert(&mut self, x: u64) {
        match self {
            ChiSet::Bits(b) => b[(x / 64) as usize] |= 1u64 << (x % 64),
            ChiSet::Sorted(v) => v.push(x),
        }
    }
}

/// χ value collector for the SXI2 EF decoder (writes u64 LE chunks).
struct ChiTap<'a> {
    set: &'a mut ChiSet,
    n: u64,
    limit: u64,
    seen: u64,
    prev: u64,
}

impl Write for ChiTap<'_> {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        if buf.len() != 8 {
            return Err(std::io::Error::other("chi decode width"));
        }
        let x = u64::from_le_bytes(buf.try_into().unwrap());
        if x > self.n || (self.seen > 0 && x <= self.prev) {
            return Err(std::io::Error::other("chi order/range"));
        }
        self.prev = x;
        self.seen += 1;
        if self.seen <= self.limit {
            self.set.insert(x);
        }
        Ok(8)
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

fn load_chi(path: &str, c: &sxi::Container, limit: u64) -> (ChiSet, u64) {
    let n = c.n;
    let m = c.member(5);
    need(c.complete, "chi member is not complete");
    let total = m.count;
    let take = limit.min(total);
    let mut set = if n / 8 <= 8 << 30 {
        ChiSet::Bits(vec![0u64; ((n + 63) / 64) as usize])
    } else {
        ChiSet::Sorted(Vec::with_capacity(take as usize))
    };
    if c.version == 1 {
        // Unsigned LEB128 gaps, canonical, strictly increasing, sorted.
        let mut f = BufReader::with_capacity(1 << 22, File::open(path).unwrap());
        f.seek(SeekFrom::Start(m.offset)).unwrap();
        let mut prev = 0u64;
        let mut used = 0u64;
        let mut one = [0u8; 1];
        for i in 0..total {
            let mut d = 0u64;
            let mut shift = 0u32;
            loop {
                need(used < m.bytes, "truncated chi varint");
                f.read_exact(&mut one).unwrap();
                used += 1;
                let payload = (one[0] & 127) as u64;
                need(shift < 64 && (shift < 63 || payload <= 1), "chi varint overflow");
                d |= payload << shift;
                if one[0] & 128 == 0 {
                    need(shift == 0 || payload != 0, "noncanonical chi varint");
                    break;
                }
                shift += 7;
            }
            need(i == 0 || d > 0, "duplicate chi");
            prev = prev.checked_add(d).unwrap_or_else(|| die("forks: chi overflow"));
            need(prev <= n, "chi range");
            if (i as u64) < take {
                set.insert(prev);
            } else {
                // --n prefix: the remaining deltas are not needed; the member
                // was already fully validated by Container::open.
                break;
            }
        }
        if take == total {
            need(used == m.bytes, "trailing chi bytes");
        }
    } else {
        // The EF decoder walks the full upper stream; keep only `take` values.
        let mut tap = ChiTap { set: &mut set, n, limit: take, seen: 0, prev: 0 };
        sxi2::chi(path, m, n, &mut tap);
    }
    (set, total)
}

struct Wit {
    key: u64, // text-order key: x==0 maps to n (virtual end last)
    x: u64,
    run: u32,
    side: u8, // 0 = head side, 1 = tail side, 2 = singleton row (both edges)
}

/// Adjacent runs whose intervals witness the witness row, in BWT order.
fn witnessing_runs(r: u64, run: u32, side: u8) -> Vec<u32> {
    let run = run as u64;
    let prev = if run == 0 { r - 1 } else { run - 1 };
    let next = if run + 1 == r { 0 } else { run + 1 };
    match side {
        0 => vec![prev as u32, run as u32],
        1 => vec![run as u32, next as u32],
        _ => vec![prev as u32, run as u32, next as u32],
    }
}

pub fn command(args: &[String]) {
    let t0 = Instant::now();
    let mut artifact = None;
    let mut limit = u64::MAX;
    let mut output = None;
    let mut jobs = std::thread::available_parallelism().map(|j| j.get()).unwrap_or(1) as u64;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--jsonl" if i + 1 < args.len() => {
                artifact = Some(args[i + 1].clone());
                i += 1;
            }
            "--n" if i + 1 < args.len() => {
                limit = args[i + 1]
                    .parse()
                    .unwrap_or_else(|_| die("forks: --n expects an integer"));
                i += 1;
            }
            "-o" | "--output" if i + 1 < args.len() => {
                output = Some(args[i + 1].clone());
                i += 1;
            }
            "-j" | "--threads" if i + 1 < args.len() => {
                jobs = args[i + 1]
                    .parse()
                    .unwrap_or_else(|_| die("forks: -j expects an integer"));
                i += 1;
            }
            other => die(&format!(
                "forks: unknown option '{other}' (usage: xsa forks --jsonl FILE [--n N] [-o OUT] [-j J])"
            )),
        }
        i += 1;
    }
    let path = artifact.unwrap_or_else(|| die("forks: --jsonl FILE is required"));
    let jobs = jobs.max(1);

    eprintln!("forks: opening and validating {path} ...");
    let c = sxi::Container::open(&path).unwrap_or_else(|| die("forks: expected an SXI artifact"));
    let (n, r, k) = (c.n, c.r, c.k);
    need(c.members.iter().any(|m| m.id == 6), "artifact has no names member; string ids need names");
    need(r >= 2, "single-run artifact has no run boundary to witness");
    eprintln!(
        "forks: SXI{} v{} n={} k={} runs={} chi={} (validated in {:.1}s)",
        if c.version == 1 { 1 } else { 2 },
        c.version, n, k, r, c.member(5).count,
        t0.elapsed().as_secs_f64()
    );

    let t = Instant::now();
    let table = Table::load(&path, &c);
    eprintln!("forks: run tables loaded in {:.1}s", t.elapsed().as_secs_f64());

    // Record boundaries from the embedded names TSV (stream coordinates).
    let t = Instant::now();
    let m6 = c.member(6);
    let mut names_bytes = vec![0u8; m6.bytes as usize];
    {
        let mut f = BufReader::with_capacity(1 << 22, File::open(&path).unwrap());
        f.seek(SeekFrom::Start(m6.offset)).unwrap();
        f.read_exact(&mut names_bytes).unwrap();
    }
    let records = product::names(&names_bytes, n).unwrap_or_else(|e| die(&format!("forks: {e}")));
    let starts: Vec<u64> = records.iter().map(|rec| rec.start).collect();
    let record_at = |pos: u64| -> u64 {
        let i = starts.partition_point(|&s| s <= pos);
        (i - 1) as u64
    };
    eprintln!(
        "forks: {} named records indexed in {:.1}s",
        starts.len(), t.elapsed().as_secs_f64()
    );

    let t = Instant::now();
    let (chi, chi_total) = load_chi(&path, &c, limit);
    let chi_count = chi.count();
    eprintln!(
        "forks: chi loaded: {} witnesses kept of {} stored{}, max filter {} in {:.1}s",
        chi_count, chi_total,
        if (chi_count) < chi_total { format!(" (--n {limit})") } else { String::new() },
        chi.max().min(n), t.elapsed().as_secs_f64()
    );
    need(!(chi.contains(0) && chi.contains(n)), "chi contains both 0 and the virtual end n");

    // Matcher: per-run head/tail sample positions against the chi set.
    let t = Instant::now();
    let per_thread = (r / jobs + 1).max(1);
    let local: Vec<Vec<Wit>> = std::thread::scope(|scope| {
        let handles: Vec<_> = (0..jobs)
            .map(|th| {
                let table = &table;
                let chi = &chi;
                scope.spawn(move || {
                    let mut hits: Vec<Wit> = Vec::new();
                    let lo = (th * per_thread).min(r);
                    let hi = ((th + 1) * per_thread).min(r);
                    for rho in lo..hi {
                        let xh = n.wrapping_sub(table.head_sa[rho as usize]) % n;
                        let xt = n.wrapping_sub(table.tail_sa[rho as usize]) % n;
                        let mh = chi.contains(xh);
                        let mt = chi.contains(xt);
                        if mh {
                            hits.push(Wit { key: if xh == 0 { n } else { xh }, x: xh, run: rho as u32, side: 0 });
                        }
                        if mt {
                            hits.push(Wit { key: if xt == 0 { n } else { xt }, x: xt, run: rho as u32, side: 1 });
                        }
                        // A singleton run's single row can witness at both
                        // edges; sort + fold merges those into one witness.
                    }
                    hits
                })
            })
            .collect();
        handles.into_iter().map(|h| h.join().unwrap()).collect()
    });
    let mut wits: Vec<Wit> = local.into_iter().flatten().collect();
    wits.sort_unstable_by(|a, b| a.key.cmp(&b.key).then(a.run.cmp(&b.run)));
    eprintln!(
        "forks: {} run-edge slots matched chi in {:.1}s; folding singleton rows",
        wits.len(), t.elapsed().as_secs_f64()
    );
    // Fold the two matches of a singleton run's single row into one witness.
    let mut folded: Vec<Wit> = Vec::with_capacity(wits.len());
    let mut i = 0;
    while i < wits.len() {
        let mut w = Wit { key: wits[i].key, x: wits[i].x, run: wits[i].run, side: wits[i].side };
        let mut j = i + 1;
        while j < wits.len() && wits[j].key == w.key {
            need(wits[j].run == w.run,
                "forks: two runs matched one witness position (corrupt artifact)");
            w.side = 2;
            j += 1;
        }
        folded.push(w);
        i = j;
    }
    wits = folded;
    need(
        wits.len() as u64 == chi_count,
        &format!(
            "forks: {} witnesses matched but chi holds {} — the run-edge invariant failed",
            wits.len(), chi_count
        ),
    );
    if wits.windows(2).any(|p| p[0].key == p[1].key) {
        die("forks: duplicate witness keys after folding");
    }

    // Touched runs and their total length.
    let mut touched = vec![0u64; ((r + 63) / 64) as usize];
    for w in &wits {
        for run in witnessing_runs(r, w.run, w.side) {
            touched[(run / 64) as usize] |= 1u64 << (run % 64);
        }
    }
    let touched_list: Vec<u32> = (0..r as u32)
        .filter(|run| touched[(run / 64) as usize] & (1u64 << (run % 64)) != 0)
        .collect();
    let touched_rows: u64 = touched_list.iter().map(|&run| table.lens[run as usize] as u64).sum();
    eprintln!(
        "forks: {} witnesses; {} distinct runs witness them ({} rows to resolve)",
        wits.len(), touched_list.len(), touched_rows
    );

    // Resolve touched runs in parallel: per-thread arenas of string ids plus
    // (run, thread, offset, len) triples. Each run's segment is sorted and
    // deduplicated in place.
    //
    // Fast path: the φ-domain arithmetic chain (criterion C — gcd of the
    // nonzero character counts), self-checked per run against the ssa/ssa_t
    // endpoints. Fallback: the windowed LF walk (used when criterion C is
    // unavailable); the two independent resolvers are cross-checked on a
    // bounded sample of runs either way.
    if table.phi_ok {
        eprintln!("forks: criterion C holds (char-count gcd 1); φ-domain fast path");
    } else {
        eprintln!("forks: criterion C unavailable (char-count gcd > 1); using the LF-walk resolver (slow)");
    }
    {
        let mut checked = 0;
        let mut a = Vec::new();
        let mut b = Vec::new();
        for &run in touched_list.iter() {
            if checked >= 16 {
                break;
            }
            if table.lens[run as usize] as u64 > 4096 {
                continue;
            }
            table.resolve_run_lf(run as u64, &mut a);
            table.resolve_run(run as u64, &mut b);
            if a != b {
                die(&format!("forks: φ resolver disagrees with the LF walk on run {run}"));
            }
            checked += 1;
        }
        eprintln!("forks: cross-checked φ chain vs LF walk on {checked} runs");
    }
    let t = Instant::now();
    let next_run = AtomicU64::new(0);
    let (arenas, mut triples): (Vec<Vec<u32>>, Vec<(u32, u32, u32, u32)>) = std::thread::scope(|scope| {
        let handles: Vec<_> = (0..jobs)
            .map(|th| {
                let table = &table;
                let touched_list = &touched_list;
                let next_run = &next_run;
                let record_at = &record_at;
                scope.spawn(move || {
                    let mut arena: Vec<u32> = Vec::new();
                    let mut triples: Vec<(u32, u32, u32, u32)> = Vec::new();
                    let mut sa = Vec::new();
                    loop {
                        let idx = next_run.fetch_add(1, Ordering::SeqCst);
                        if idx >= touched_list.len() as u64 {
                            break;
                        }
                        let run = touched_list[idx as usize];
                        if table.phi_ok {
                            table.resolve_run(run as u64, &mut sa);
                        } else {
                            table.resolve_run_lf(run as u64, &mut sa);
                        }
                        let off = arena.len() as u32;
                        // Row v has BWT char S[(v-1) mod n]: the occurrence's
                        // branch-character position. Forward-domain streams
                        // (validated: the forward-cyclic FM scan reproduces
                        // the artifact chi and every witness maps to a
                        // forward run-edge row) keep this position directly.
                        for &v in sa.iter() {
                            let pos = (v + n - 1) % n;
                            arena.push(record_at(pos) as u32);
                        }
                        let seg = &mut arena[off as usize..];
                        seg.sort_unstable();
                        let mut w = 0usize;
                        for j2 in 1..seg.len() {
                            if seg[j2] != seg[w] {
                                w += 1;
                                seg[w] = seg[j2];
                            }
                        }
                        let keep = if seg.is_empty() { 0 } else { w + 1 };
                        triples.push((run, th as u32, off, keep as u32));
                    }
                    (arena, triples)
                })
            })
            .collect();
        let mut arenas = Vec::with_capacity(handles.len());
        let mut triples = Vec::new();
        for h in handles {
            let (a, tr) = h.join().unwrap();
            arenas.push(a);
            triples.extend(tr);
        }
        (arenas, triples)
    });
    triples.sort_unstable_by_key(|t| t.0);
    eprintln!(
        "forks: resolved {} rows over {} runs in {:.1}s",
        touched_rows, touched_list.len(), t.elapsed().as_secs_f64()
    );

    // Emit JSONL in text order.
    let t = Instant::now();
    let out_file = match &output {
        Some(p) => File::create(p).unwrap_or_else(|e| die(&format!("forks: create {p}: {e}"))),
        None => File::create("/dev/stdout").unwrap_or_else(|e| die(&format!("forks: stdout: {e}"))),
    };
    let mut out = BufWriter::with_capacity(1 << 24, out_file);
    let mut line = Vec::with_capacity(1 << 16);
    let mut class_strings: Vec<u32> = Vec::new();
    let mut records_emitted = 0u64;
    let mut strings_emitted = 0u64;
    let mut classes_emitted = 0u64;
    for w in &wits {
        // Group the witnessing runs by BWT character, first-appearance order;
        // separated runs with the same character merge into one class.
        let runs = witnessing_runs(r, w.run, w.side);
        let mut classes: Vec<(u8, Vec<u32>)> = Vec::new();
        for &run in &runs {
            let ch = table.chars[run as usize];
            match classes.iter_mut().find(|(c, _)| *c == ch) {
                Some((_, list)) => list.push(run),
                None => classes.push((ch, vec![run])),
            }
        }
        for (cid, (ch, list)) in classes.iter().enumerate() {
            class_strings.clear();
            for &run in list {
                let key = run;
                let found = triples
                    .binary_search_by_key(&key, |t| t.0)
                    .ok()
                    .and_then(|i| triples.get(i));
                let (_, th, off, len) = found.unwrap_or_else(|| {
                    die(&format!("forks: run {run} was not resolved (internal error)"))
                });
                class_strings.extend_from_slice(&arenas[*th as usize][*off as usize..(*off + *len) as usize]);
            }
            class_strings.sort_unstable();
            class_strings.dedup();
            line.clear();
            write!(line, "{{\"witness\":{},\"class_id\":{},\"char\":{},\"strings\":[",
                w.x, cid, ch).unwrap();
            for (i, s) in class_strings.iter().enumerate() {
                if i > 0 {
                    line.push(b',');
                }
                push_u32(&mut line, *s);
            }
            line.extend_from_slice(b"]}\n");
            out.write_all(&line).unwrap();
            records_emitted += 1;
            strings_emitted += class_strings.len() as u64;
            classes_emitted += 1;
        }
    }
    out.flush().unwrap();
    let out_bytes = match &output {
        Some(p) => std::fs::metadata(p).map(|m| m.len()).unwrap_or(0),
        None => 0,
    };
    eprintln!(
        "forks: emitted {} records ({} classes) over {} witnesses in {:.1}s; {} string slots; {} output bytes",
        records_emitted, classes_emitted, wits.len(), t.elapsed().as_secs_f64(), strings_emitted, out_bytes
    );
    eprintln!(
        "forks: total wall {:.1}s; {} witnesses/s end-to-end",
        t0.elapsed().as_secs_f64(),
        wits.len() as f64 / t0.elapsed().as_secs_f64().max(1e-9)
    );
}

fn push_u32(buf: &mut Vec<u8>, mut v: u32) {
    let mut tmp = [0u8; 10];
    let mut i = tmp.len();
    loop {
        i -= 1;
        tmp[i] = b'0' + (v % 10) as u8;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    buf.extend_from_slice(&tmp[i..]);
}
