//! Native query kernel for the one published container: **SXI2 v6 lite**.
//!
//! Why this module exists: the legacy `Ri4` reader treats `sxi::Container::open`
//! (a *validation* entry point) as a *loader*. On a 10 GB v6 container that is
//! 385 s of fixed cost before the first answer — the whole run stream decoded
//! twice, the LF-start array materialised twice (~28 GB each), the chi set
//! decoded into a sink, every member byte CRC'd, and a 256-way `Vec<u64>`
//! transpose (`cruns`, 28 GB of scattered appends) — see
//! `tools/binbwt/RESULTS_QUERY_PROFILE.md`. None of it is phi: a v6 container
//! has no m8.
//!
//! This module answers from the members the query actually needs, and nothing
//! else:
//!
//! | member | role |
//! |---|---|
//! | m1  | RLBWT runs (codec 101: canonical-Huffman heads + gamma lengths) and the C table |
//! | m5  | chi witness set — **not read**: exact matching never needs it |
//! | m10 | LF starts — **declared, zero bytes** in v6 ("derived"); we derive them |
//! | m11 | locate anchors: the tail SA of every 1024th run |
//! | m4, m7 | record anchors (empty here) and the byte permutation |
//!
//! Cost model: one sequential pass over m1 builds a random-access rank/locate
//! kernel in three streaming arrays with no scatter —
//!
//!   heads[i]  run symbol                              (1 B/run)
//!   rlen[i]   run length                              (4 B/run)
//!   seen[i]   count of heads[i] in runs before i       (4 B/run)
//!
//! `seen` is the whole point: `start[run] = C[ch] + seen[run]` is the LF start
//! that a lite container no longer stores, and `rank(ch, i)` needs no
//! predecessor search over per-symbol run lists (`cruns`) at all. After that a
//! pattern costs O(k) rank steps plus one LF walk per occurrence, i.e. µs, and
//! startup is mmap + one streaming build.
//!
//! There is no `--ri4`/`--sxi`/`--mode`: the container tells us what it is, and
//! anything that is not a v6 lite container is refused by name (see
//! `describe_format`).
//!
//! ORIENTATION: a v6 lite container has **no alphabet**. Its text is a byte
//! string and its patterns are raw byte strings, so there is exactly ONE
//! orientation to search (the pattern as given). There is no reverse-complement
//! pass and no strand bookkeeping anywhere in this module: on a byte index a
//! reverse complement is not a second orientation of the same pattern, it is a
//! different byte string, and searching it would double every per-pattern cost
//! while producing absent counts that mean nothing. (The one transform applied
//! to a pattern is not an orientation: `sigma` is the container's own m7 byte
//! permutation and the reverse is the backward-search representation — see the
//! comment at the search call.) `--mode` is refused by name for the same reason
//! it would be meaningless: there is no alphabet to be `dna` or `text` about.
//!
//! OUTPUT (two contracts, both keyed by 0-based pattern id):
//!
//!   `--output tsv` (default) `id<TAB>offset`, one line per occurrence.
//!   Absent patterns contribute no line, so `wc -l` is the occurrence count.
//!   `--output jsonl` is the old read-mapping product,
//!   `{"pattern_id":i,"count":c,"offsets":[...]}`, one object per pattern.


/// SANITY BOUND on the m1 block width, not the width itself. The width is
/// DECLARED by the container (m1 header word 1, `block_runs`; the writer's knob
/// is bit6/xsa_finish.cpp M1_BLOCK_RUNS) and every address computation below
/// uses that declared value. This bound only says how wide a block this reader
/// is willing to decode: the block decode buffers are allocated per index, and
/// a block wider than this is refused by name rather than trusted.
pub const M1_BLOCK_RUNS_MAX: u64 = 1 << 16;

// ---------------------------------------------------------------------------
// Reader-only tuning. None of these has any format meaning: the container
// declares the block width, the level-2 strides and the member sizes, and every
// one of those declarations is read from its header and validated by closure
// (e.g. m13's level-2 array must exactly fill the span between its header and
// its entry area). What is left over is purely "how much does this process
// spend on itself", so it lives here as a constant.
// ---------------------------------------------------------------------------

/// Reader-only: direct-mapped m1 block cache entries per pattern thread.
/// 4096 x ~0.34 KB = ~1.4 MB of working set; the container says nothing about it.
const BLOCK_CACHE: usize = 4096;
/// Reader-only: how many blocks the open-time fail-loud audit samples. Shapes
/// the audit's cost (~2 decodes each), never how a run is addressed.
const M1_AUDIT_BLOCKS: u64 = 1024;
/// Reader-only: bytes charged per decoded block in `decoded_bytes` (a sizing
/// estimate for the accounting line; the true block is ~190 B + rounding and is
/// declared nowhere, so this must not be used for anything but the estimate).
const BLOCK_BYTES_EST: u64 = 192;
/// Reader-only: the row granularity of the row -> run directory (one entry, 12 B,
/// per ROW_DIR rows). 256 rows is ~98 runs at the 150 MB fixture's 2.62 rows/run,
/// so the scan it saves is short and its 7 MB stays L3-friendly; it never changes
/// an answer, only how many loads the scan needs.
const ROW_DIR: u64 = 256;
/// Reader-only slack on the run-space scan bound in `cum_before`. The bound
/// itself comes from the m13 header (`rs.run_gap`, the writer's sampling rule);
/// this is the tiny allowance for the inclusive/exclusive edge of that rule.
const M1_SCAN_SLACK: u64 = 2;

use crate::{die, sxi};
use memmap2::Mmap;
use rayon::prelude::*;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::sync::Arc;
use std::time::Instant;

/// Secondary Huffman table width.
const HTABLE_BITS: u32 = 12;
/// Gamma fast-window width (bits peeked per length code).
const GAMMA_BITS: u32 = 12;

// ---------------------------------------------------------------- bit reader

struct Bits<'a> {
    d: &'a [u8],
    /// bit position of the next unread bit
    pos: u64,
    /// total bits available
    bits: u64,
}

impl<'a> Bits<'a> {
    fn new(d: &'a [u8], bits: u64) -> Self {
        assert!(bits <= d.len() as u64 * 8, "bit stream longer than its bytes");
        Self { d, pos: 0, bits }
    }
    /// Eight bytes little-endian at `b`, zero past the end. The stream is
    /// padded to a whole byte, so reads never walk off the member.
    #[inline(always)]
    fn word(&self, b: usize) -> u64 {
        if b + 8 <= self.d.len() {
            unsafe { u64::from_le_bytes(*(self.d.as_ptr().add(b) as *const [u8; 8])) }
        } else {
            let mut w = [0u8; 8];
            let n = self.d.len().saturating_sub(b);
            if n > 0 {
                w[..n].copy_from_slice(&self.d[b..b + n]);
            }
            u64::from_le_bytes(w)
        }
    }
    /// Next `n` (1..=64) bits, LSB-first, as a right-aligned integer.
    #[inline(always)]
    fn peek(&self, n: u32) -> u64 {
        debug_assert!(self.pos + n as u64 <= self.bits);
        let sh = (self.pos & 7) as u32;
        let b = (self.pos >> 3) as usize;
        let lo = self.word(b) >> sh;
        if sh + n <= 64 {
            lo & mask(n)
        } else {
            (lo | (self.word(b + 8) << (64 - sh))) & mask(n)
        }
    }
    /// Next 128 bits, for the rare long gamma code.
    fn peek128(&self) -> u128 {
        let sh = (self.pos & 7) as u32;
        let b = (self.pos >> 3) as usize;
        ((self.word(b) as u128) | ((self.word(b + 8) as u128) << 64)) >> sh
    }
    #[inline(always)]
    fn skip(&mut self, n: u32) {
        self.pos += n as u64;
    }
}

#[inline(always)]
fn mask(n: u32) -> u64 {
    if n >= 64 {
        u64::MAX
    } else {
        (1u64 << n) - 1
    }
}

// ------------------------------------------------------------------- huffman

/// Canonical Huffman decoder for the m1 head stream.
///
/// The writer emits each code MSB-first into a stream packed LSB-first, so the
/// first stream bit is the code's most significant bit: the table is indexed by
/// the next `HTABLE_BITS` stream bits with the *first* bit in bit 0.
struct Huff {
    /// 0 = code longer than HTABLE_BITS (walk `nodes`)
    table: Vec<u32>,
    nodes: Vec<[i32; 2]>,
    sym: Vec<i32>,
    longest: u32,
    short_codes: u64,
    /// The published per-symbol lengths, kept for the block diagnostic.
    len: [u8; 256],
}

impl Huff {
    fn build(lens: &[u8; 256]) -> Result<Huff, String> {
        let mut order: Vec<(u8, u8)> = lens
            .iter()
            .enumerate()
            .filter(|(_, &l)| l != 0)
            .map(|(s, &l)| (l, s as u8))
            .collect();
        if order.is_empty() {
            return Err("m1 Huffman codebook is empty".into());
        }
        order.sort_unstable();
        if order.iter().any(|&(l, _)| l > 63) {
            return Err("m1 Huffman length above 63".into());
        }
        let mut codes = [0u64; 256];
        let mut code = 0u64;
        let mut prev = 0u8;
        for &(w, sym) in &order {
            let shift = u32::from(w - prev);
            code = code
                .checked_shl(shift)
                .ok_or("m1 Huffman code overflow")?;
            if w < 64 && code >= (1u64 << w) {
                return Err("m1 Huffman code overflow".into());
            }
            codes[sym as usize] = code;
            code += 1;
            prev = w;
        }
        // Prefix tree: needed only for codes longer than the fast table.
        let mut nodes: Vec<[i32; 2]> = vec![[-1, -1]];
        let mut sym: Vec<i32> = vec![-1];
        let longest = order.last().unwrap().0 as u32;
        let mut short_codes = 0u64;
        let mut table = vec![0u32; 1usize << HTABLE_BITS];
        for &(w, s) in &order {
            let c = codes[s as usize];
            let mut node = 0usize;
            for j in (0..w).rev() {
                if sym[node] >= 0 {
                    return Err("m1 Huffman prefix conflict".into());
                }
                let side = ((c >> j) & 1) as usize;
                if nodes[node][side] < 0 {
                    nodes[node][side] = nodes.len() as i32;
                    nodes.push([-1, -1]);
                    sym.push(-1);
                }
                node = nodes[node][side] as usize;
            }
            if sym[node] >= 0 || nodes[node] != [-1, -1] {
                return Err("m1 Huffman collision".into());
            }
            sym[node] = s as i32;
            if u32::from(w) <= HTABLE_BITS {
                short_codes += 1;
                let mut path = 0u32;
                for i in 0..u32::from(w) {
                    path |= (((c >> (u32::from(w) - 1 - i)) & 1) as u32) << i;
                }
                let mut idx = path as usize;
                while idx < table.len() {
                    table[idx] = (u32::from(w) << 16) | u32::from(s);
                    idx += 1usize << w;
                }
            }
        }
        Ok(Huff {
            table,
            nodes,
            sym,
            longest,
            short_codes,
            len: *lens,
        })
    }
    #[inline(always)]
    fn decode(&self, bits: &mut Bits) -> u8 {
        let e = self.table[bits.peek(HTABLE_BITS) as usize];
        if e != 0 {
            bits.skip(e >> 16);
            return (e & 0xffff) as u8;
        }
        // Long code: walk the tree one bit at a time.
        let mut node = 0usize;
        let mut taken = 0u32;
        loop {
            if taken >= self.longest {
                die("Huffman: code longer than the longest published length");
            }
            let side = bits.peek(1) as usize;
            bits.skip(1);
            taken += 1;
            let next = self.nodes[node][side];
            if next < 0 {
                die("Huffman: invalid code path");
            }
            node = next as usize;
            if self.sym[node] >= 0 {
                return self.sym[node] as u8;
            }
        }
    }
}

// -------------------------------------------------------------------- gamma

/// The m1 length stream is Elias gamma written *inside* an LSB-first bit
/// stream, so each code reads zero-bits, then a terminating 1, then the value
/// bits **most significant first** (the same per-code convention the Huffman
/// heads use). `peek(12)` gives the code length and value for every z <= 5,
/// which is 99.7% of the runs on the 10 GB pile container; longer codes take
/// the generic path (one `reverse_bits`).
struct Gamma {
    /// (value << 4) | length, 0 = longer than the fast window
    table: Vec<u32>,
}

impl Gamma {
    fn build() -> Gamma {
        let size = 1usize << GAMMA_BITS;
        let mut table = vec![0u32; size];
        for f in 0..size as u32 {
            let z = f.trailing_zeros();
            if z + 1 > GAMMA_BITS || 2 * z + 1 > GAMMA_BITS {
                continue;
            }
            let mut value = 0u32;
            for i in 0..=z {
                value = (value << 1) | ((f >> (z + i)) & 1);
            }
            table[f as usize] = (value << 4) | (2 * z + 1);
        }
        Gamma { table }
    }
    #[inline(always)]
    fn decode(&self, bits: &mut Bits) -> u32 {
        let e = self.table[bits.peek(GAMMA_BITS) as usize];
        if e != 0 {
            bits.skip(e & 0xf);
            return e >> 4;
        }
        // z >= 6: read the whole code from a 128-bit window.
        let wide = bits.peek128();
        let z = wide.trailing_zeros();
        if z > 63 {
            die("m1 length stream: gamma prefix above 63 zero bits");
        }
        let field = ((wide >> z) & ((1u128 << (z + 1)) - 1)) as u64;
        let value = field.reverse_bits() >> (64 - (z + 1));
        bits.skip(2 * z + 1);
        value as u32
    }
}

// ------------------------------------------------------- container (mmap only)

/// A v6 lite container opened for reading: header + directory + mmap. No
/// member is decoded, no member CRC is verified (see `--verify`).
pub struct Reader {
    pub path: String,
    pub n: u64,
    pub r: u64,
    pub version: u32,
    pub flags: u64,
    pub members: Vec<sxi::Member>,
    pub sigma: [u8; 256],
    map: Arc<Mmap>,
}

/// What the file is, for the refusal message.
pub fn describe_format(path: &str) -> String {
    let f = match File::open(path) {
        Ok(f) => f,
        Err(e) => return format!("{path}: cannot open ({e})"),
    };
    let map = match unsafe { Mmap::map(&f) } {
        Ok(m) => m,
        Err(e) => return format!("{path}: cannot mmap ({e})"),
    };
    if map.len() < 64 {
        return format!("{path}: {} bytes, too short for any index header", map.len());
    }
    let magic = &map[..4];
    let version = u32::from_le_bytes(map[4..8].try_into().unwrap());
    match magic {
        b"SXI2" if version == 6 => format!("{path}: SXI2 v6 lite container"),
        b"SXI2" => format!("{path}: SXI2 v{version} (phi-era container)"),
        b"SXI1" => format!("{path}: SXI1 v{version} container"),
        b"ISXR" => format!("{path}: .ri4 v{version} flat index"),
        _ => format!(
            "{path}: unrecognised file (magic {:02x}{:02x}{:02x}{:02x})",
            magic[0], magic[1], magic[2], magic[3]
        ),
    }
}

impl Reader {
    pub fn open(path: &str) -> Result<Reader, String> {
        let f = File::open(path).map_err(|e| format!("open {path}: {e}"))?;
        let map = unsafe { Mmap::map(&f) }.map_err(|e| format!("mmap {path}: {e}"))?;
        if map.len() < 64 {
            return Err(format!("{path}: too short for an SXI header"));
        }
        let h = &map[..64];
        if &h[..4] != b"SXI2" {
            return Err(format!("{} is not a queryable index", describe_format(path)));
        }
        let version = u32::from_le_bytes(h[4..8].try_into().unwrap());
        if version != 6 {
            return Err(format!("{} is not a queryable index", describe_format(path)));
        }
        let u32at = |o: usize| u32::from_le_bytes(h[o..o + 4].try_into().unwrap());
        let u64at = |o: usize| u64::from_le_bytes(h[o..o + 8].try_into().unwrap());
        let (n, r) = (u64at(8), u64at(24));
        let count = u32at(32);
        let hs = u32at(36) as usize;
        let length = u64at(40);
        let flags = u64at(48);
        if length != map.len() as u64 {
            return Err(format!(
                "{path}: header length {length} != file length {} (truncated or trailing bytes)",
                map.len()
            ));
        }
        // 8 slots is the block-compressed lite shape (m1,m4,m5,m7,m10,m11,m12,m13);
        // anything outside 5..=8 is not a lite container.
        if !(5..=8).contains(&count) || hs != 64 + 40 * count as usize {
            return Err(format!(
                "{path}: v6 directory must hold 5-8 members (found {count})"
            ));
        }
        if n == 0 || r == 0 || r > n {
            return Err(format!("{path}: invalid n={n} R={r}"));
        }
        // Header+directory CRC: 64 + 40*members bytes. Cheap, and the only
        // checksum that can be verified without reading the whole file.
        let expected = u32at(56);
        let mut head = map[..hs].to_vec();
        head[56..60].fill(0);
        if sxi::crc32_of(&head) != expected {
            return Err(format!("{path}: header CRC mismatch"));
        }
        let mut members = Vec::with_capacity(count as usize);
        for i in 0..count as usize {
            let d = &map[64 + 40 * i..64 + 40 * (i + 1)];
            let m = sxi::Member {
                id: u32::from_le_bytes(d[0..4].try_into().unwrap()),
                codec: u32::from_le_bytes(d[4..8].try_into().unwrap()),
                offset: u64::from_le_bytes(d[8..16].try_into().unwrap()),
                bytes: u64::from_le_bytes(d[16..24].try_into().unwrap()),
                count: u64::from_le_bytes(d[24..32].try_into().unwrap()),
                checksum: u32::from_le_bytes(d[32..36].try_into().unwrap()),
            };
            if m.offset > length || m.bytes > length - m.offset {
                return Err(format!("{path}: member {} out of bounds", m.id));
            }
            members.push(m);
        }
        // The directory is sorted by ID; the members partition [header, file)
        // by physical offset (the writer streams m5/m4/m7/m10/m11, then m1).
        {
            let mut spans: Vec<(u64, u64)> = members.iter().map(|m| (m.offset, m.bytes)).collect();
            spans.sort_unstable();
            let mut prev = hs as u64;
            for (off, bytes) in spans {
                let aligned = (prev + 7) / 8 * 8;
                if off != aligned {
                    return Err(format!("{path}: members do not partition the file"));
                }
                prev = off + bytes;
            }
            if prev != length {
                return Err(format!("{path}: members do not reach end of file"));
            }
        }
        let mut sigma = std::array::from_fn(|i| i as u8);
        if let Some(m) = members.iter().find(|m| m.id == 7) {
            if m.bytes != 256 {
                return Err(format!("{path}: remap member must be 256 bytes"));
            }
            let s = &map[m.offset as usize..m.offset as usize + 256];
            let mut seen = [false; 256];
            for (i, &v) in s.iter().enumerate() {
                if seen[v as usize] {
                    return Err(format!("{path}: remap member is not a permutation"));
                }
                seen[v as usize] = true;
                sigma[i] = v;
            }
        }
        for id in [1u32, 5, 10, 11] {
            if !members.iter().any(|m| m.id == id) {
                return Err(format!("{path}: v6 lite container is missing member {id}"));
            }
        }
        Ok(Reader {
            path: path.to_string(),
            n,
            r,
            version,
            flags,
            members,
            sigma,
            map: Arc::new(map),
        })
    }
    pub fn member(&self, id: u32) -> &sxi::Member {
        self.members
            .iter()
            .find(|m| m.id == id)
            .unwrap_or_else(|| die(&format!("{}: missing member {}", self.path, id)))
    }
    fn slice(&self, off: u64, len: u64) -> &[u8] {
        &self.map[off as usize..(off + len) as usize]
    }
    /// Verify every member CRC. Reads the whole file; opt-in only.
    pub fn verify_members(&self) -> Result<(), String> {
        for m in &self.members {
            if sxi::crc32_of(self.slice(m.offset, m.bytes)) != m.checksum {
                return Err(format!("{}: member {} CRC mismatch", self.path, m.id));
            }
        }
        Ok(())
    }
}

// --------------------------------------------------------------------- index

pub struct Index {
    pub reader: Reader,
    /// C table from m1 (cumulative character counts).
    pub c: [u64; 256],
    /// Total occurrences of each symbol, derived from the C table: total[i] =
    /// c[i+1] - c[i] (total[255] = n - c[255]). No run decode is needed for it -
    /// that equality is exactly what the C table means.
    pub total: [u64; 256],
    /// m1 v2 (codec 102): the block directory and the payload.
    dir_off: u64,
    payload_off: u64,
    /// One past the last payload byte (end of the m1 member), for the block
    /// read bound: a directory entry is only trusted as far as this.
    payload_end: u64,
    /// The canonical Huffman codebook over run heads (m1 codec 102 header).
    huff: Huff,
    /// The gamma length decoder (built once: it is a 4096-entry table, and
    /// rebuilding it per block cost 26 us per rank step).
    gamma: Gamma,
    nblocks: u64,
    /// m12: run-start samples (row of every `rs_stride`-th run).
    rs: RunStarts,
    /// m13: per-symbol sampled run directory.
    rd: RunDir,
    anchors: Vec<u64>,
    stripe: u64,
    /// THE RUN COLUMNS, decoded once at open from the m1 block stream and held in
    /// RAM: `heads[run]`, `lens[run]` (both from the container, byte-exact) and
    /// `seen[run]` (derived: how many runs of `heads[run]` precede `run`).
    ///
    /// Why this exists, measured (RESULTS_QUERY_HOTPATH.md): a warm 256-run m1
    /// block decode is 2.33 us (9.1 ns/run, XSA_M1_AUDIT_FULL=1 over all 223,747
    /// blocks) and one LF step used to pay for 2.04 of them plus ~525 per-run
    /// `run()` probes. Page faults were NOT the cost: the same query on tmpfs (all
    /// pages resident) is 114.8 ms against 115.0 ms on NVMe, with 0 major faults.
    /// A rank step must therefore not decode at all, which is exactly what these
    /// columns buy: 9 B/run, one streaming decode at open, no container change
    /// (the format, `chi` and the positions are untouched).
    ///
    /// SCALING - READ THIS BEFORE SHIPPING THIS SHAPE. 9 B/run is 3.44 n at this
    /// fixture (runs/byte = 0.382) and it is HEAP: 515 MB at 150 MB of text,
    /// ~19 GB at 10 GB, ~210 GB at 90 GB. It is a measured stepping stone: it
    /// establishes the achievable rank cost (0.49 us per LF step, see
    /// tools/binbwt/RESULTS_QUERY_HOTPATH.md) and the exactness baseline, and
    /// `materialise` refuses in words when MemAvailable cannot hold it. The
    /// container-side shapes are priced in section 6 of that journal: heads +
    /// byte-lengths as container members is 1.111 n and mmap-lazy, but leaves no
    /// per-run rank value, and a measured 2.035 us/step (m13 plus a 1024-run
    /// scan) does not meet the 1 us target. Row-anchored m11 (stride 256 rows,
    /// +0.031 n, ~10x fewer steps) is the lever that makes the paged shape pay.
    heads: Vec<u8>,
    runs: Runs,
    /// ROW -> RUN DIRECTORY: `dir_run[j]`/`dir_row[j]` are the run containing row
    /// `j*ROW_DIR` and that run's start row, so `run_of` is ONE load plus a short
    /// scan instead of a bisect chain. Measured (rdtsc, pattern 6 of p10): the
    /// m12 bisect alone was 1458 of 1509 cycles per LF step -- ~6 DEPENDENT cache
    /// misses in a 1.79 MB window array -- while this lookup is one dependent miss
    /// in 7 MB. Cost: n/ROW_DIR * 12 B = 0.047n at 150 MB.
    /// `dir[j] = (start row of that run, run)`, in ONE 16 B record so the lookup
    /// is one cache line and one TLB entry (two separate columns cost two
    /// dependent misses; measured 411-1358 cycles per step with them apart).
    dir: Vec<[u64; 2]>,
    pub build_ms: f64,
    /// Bytes of per-run columns this process materialised at open. Reported so the
    /// open's memory cost is auditable against the container's 0.71n.
    pub decoded_bytes: u64,
    /// Per-stage build walls, in order.
    pub stages: Vec<(&'static str, f64)>,
    /// Blocks the open-time audit checked run-length sums against m12 for.
    pub audit_blocks: u64,
    pub block_runs: u64,
    pub rundir_stride: u64,
    /// Threads the open-time decode ran on, journaled (`-j` does not bound it:
    /// the open is a bulk step, the query is what `-j` parallelises).
    pub open_threads: usize,
    /// Whether the run columns got MADV_HUGEPAGE, journaled.
    pub thp: bool,
}

/// m12: `row(j) = (mmap[l1_off + 8j])` is the row of run `j*stride`; the level-2
/// array every `l2stride` runs makes a lookup a bisect in a cache-resident array
/// followed by a bisect in one 8 KB window.
struct RunStarts {
    l1_off: u64,
    l1_cnt: u64,
    l2_off: u64,
    l2_cnt: u64,
    stride: u64,
    l2stride: u64,
}

/// m13: per symbol `c`, entries `j` in `offs[c]..offs[c+1]` at
/// `ent_off + 16*j` are `(rows of c before run, run index, occurrence stride to
/// the next sample)`. The level-2 array every `l2` entries per symbol is at
/// `l2_off + 16*l2offs[c]`.
struct RunDir {
    ent_off: u64,
    l2_off: u64,
    l2: u64,
    counts: Vec<u64>,
    offs: Vec<u64>,
    l2offs: Vec<u64>,
    occ_stride: u64,
    run_gap: u64,
}

/// THE RUN COLUMNS, in the layout the hot path wants.
///
/// `W8` is the normal one: one u64 per run, `len | seen << 32`, where `seen` is
/// the number of ROWS of `heads[run]` before this run (exactly what an LF step's
/// rank needs). Length and rank in the same word means an LF step's window scan
/// and its rank read touch ONE column, not two independent random streams -- the
/// first measurement of the split layout was 0.80 us/step, and every random
/// stream a step opens costs a TLB entry and a cache line.
///
/// `Split` is the fallback for a container where some symbol occurs more than
/// 2^32-1 times (n > 4.29e9 with a skewed alphabet): the rank then needs its own
/// u64 column. Same answers, one extra stream, chosen from the container's own
/// totals and journaled (`seen_width`).
enum Runs {
    W8(Vec<u64>),
    Split { len: Vec<u32>, seen: Vec<u64> },
}

impl Runs {
    #[inline(always)]
    fn len(&self, run: u64) -> u64 {
        match self {
            Runs::W8(w) => (unsafe { *w.get_unchecked(run as usize) }) & 0xffff_ffff,
            Runs::Split { len, .. } => u64::from(unsafe { *len.get_unchecked(run as usize) }),
        }
    }
    #[inline(always)]
    fn seen(&self, run: u64) -> u64 {
        match self {
            Runs::W8(w) => (unsafe { *w.get_unchecked(run as usize) }) >> 32,
            Runs::Split { seen, .. } => unsafe { *seen.get_unchecked(run as usize) },
        }
    }
    fn name(&self) -> &'static str {
        match self {
            Runs::W8(_) => "interleaved-len-seen-u32",
            Runs::Split { .. } => "split-len-u32-seen-u64",
        }
    }
    fn bytes(&self) -> u64 {
        match self {
            Runs::W8(w) => 8 * w.len() as u64,
            Runs::Split { len, seen } => 4 * len.len() as u64 + 8 * seen.len() as u64,
        }
    }
}

/// The window scan of `run_of`: walk forward over run lengths from `start_run`
/// (whose start row is `s`) until the run containing `row` is found. Four runs at
/// a time, because the one-at-a-time form is a loop-carried `s += l` dependency
/// chain; generic over the length accessor so the interleaved and split layouts
/// both monomorphise into the same straight-line code.
#[inline(always)]
fn scan_window<F: Fn(usize) -> u64>(
    lens_at: F,
    start_run: u64,
    mut s: u64,
    row: u64,
    avail: usize,
    r: u64,
) -> (u64, u64) {
    let mut i = 0usize;
    while i + 4 < avail {
        let t = lens_at(i) + lens_at(i + 1) + lens_at(i + 2) + lens_at(i + 3);
        if s + t > row {
            break;
        }
        s += t;
        i += 4;
    }
    while i < avail {
        // the last run is never stepped past, exactly like the scalar loop this
        // replaces (`while run + 1 < r`)
        if start_run as usize + i + 1 >= r as usize {
            break;
        }
        let l = lens_at(i);
        if s + l > row {
            break;
        }
        s += l;
        i += 1;
    }
    (start_run + i as u64, s)
}

/// Per-query-thread working set. It used to be a 4096-entry direct-mapped cache
/// of decoded m1 blocks; now every run column lives in RAM (built once at open),
/// so this is only the diagnostic counter block: one `Ctx` per pattern, so two
/// patterns on one rayon thread never mix their counters.
pub struct Ctx<'a> {
    idx: &'a Index,
    /// Diagnostic counters, printed under XSA_TRACE. They answer "what does one
    /// rank/LF step actually pay for": `scan_of` runs walked in `run_of`'s
    /// forward scan, `scan_cum` runs walked in `cum_before`'s sample-to-run scan,
    /// `steps` LF steps, `ranks` rank() calls.
    scan_of: u64,
    scan_cum: u64,
    steps: u64,
    ranks: u64,
}

fn u64_at(d: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(d[o..o + 8].try_into().unwrap())
}

impl Index {
    /// Build the query kernel. This touches only metadata: the m1 header + the
    /// codebook + the two directories' headers, the m11 anchors, and one bounded
    /// block audit. The 4.3 GB run stream is NOT decoded - a rank or a locate
    /// decodes the one or two 64-run blocks it needs.
    pub fn open(reader: Reader, _threads: usize) -> Result<Index, String> {
        let t0 = Instant::now();
        let r = reader.r;
        let n = reader.n;

        // ---- the format declaration: old containers are refused by shape, not
        // decoded and hoped over.
        let m1 = reader.member(1);
        if m1.codec != 102 {
            return Err(format!(
                "{}: m1 codec {} is not 102 (block-compressed) - this is a pre-block-compression\n  \
                 v6 lite container: one sequential Huffman+gamma run stream with no restart\n  \
                 directory, so run k needs runs 0..k. Rebuild it with `xsa build --tape`.",
                reader.path, m1.codec
            ));
        }
        if reader.flags & 32 == 0 {
            return Err(format!(
                "{}: header flags {:#x} do not declare the block-compressed m1 + run directories\n  \
                 (bit 32); rebuild with `xsa build --tape`.",
                reader.path, reader.flags
            ));
        }
        for id in [12u32, 13] {
            if !reader.members.iter().any(|m| m.id == id) {
                return Err(format!(
                    "{}: missing run directory member m{id}; the block-compressed format needs m12\n  \
                     (run starts) and m13 (per-symbol run directory) - rebuild with `xsa build --tape`.",
                    reader.path
                ));
            }
        }
        let data = reader.slice(m1.offset, m1.bytes);
        if data.len() < 2336 {
            return Err("m1 member too short for its v2 header".into());
        }
        let mut c = [0u64; 256];
        for (i, v) in c.iter_mut().enumerate() {
            *v = u64_at(data, 8 * i);
        }
        let mut lens = [0u8; 256];
        lens.copy_from_slice(&data[2048..2304]);
        let nblocks = u64_at(data, 2304);
        let block_runs = u64_at(data, 2312);
        let total_runs = u64_at(data, 2320);
        let payload_off = u64_at(data, 2328);
        if block_runs == 0 || block_runs > M1_BLOCK_RUNS_MAX {
            return Err(format!(
                "m1 declares block_runs={block_runs}; this reader decodes blocks of 1..={M1_BLOCK_RUNS_MAX}\n  \
                 runs (the width is the container's to declare - this is only the sanity bound)"
            ));
        }
        if total_runs != r {
            return Err(format!("m1 declares {total_runs} runs, header says {r}"));
        }
        let want_blocks = (r + block_runs - 1) / block_runs;
        if nblocks != want_blocks {
            return Err(format!("m1 declares {nblocks} blocks, expected {want_blocks}"));
        }
        if payload_off != 2336 + 8 * nblocks || payload_off > m1.bytes {
            return Err(format!(
                "m1 payload offset {payload_off} does not follow its {nblocks}-entry directory"
            ));
        }
        // C table -> symbol totals (its own definition, no decode needed).
        let mut total = [0u64; 256];
        let mut acc = 0u64;
        for ch in 0..256 {
            if c[ch] != acc {
                return Err(format!("m1 C table entry {ch} is {}, expected {acc}", c[ch]));
            }
            total[ch] = if ch + 1 < 256 { c[ch + 1] } else { n } - c[ch];
            acc += total[ch];
        }
        if acc != n {
            return Err(format!("m1 symbol counts sum to {acc}, header n={n}"));
        }
        let huff = Huff::build(&lens)?;
        let m1dir_off = m1.offset + 2336;
        let m1payload_off = m1.offset + payload_off;
        let m1payload_len = m1.bytes - payload_off;
        // taken here: `reader` is moved into the index below, so the m1 borrow
        // must not survive past this point.
        let m1_end = m1.offset + m1.bytes;

        // ---- m12
        let m = reader.member(12);
        let a = reader.slice(m.offset, m.bytes);
        if a.len() < 64 {
            return Err("m12 member too short".into());
        }
        let rs = RunStarts {
            stride: u64_at(a, 0),
            l1_cnt: u64_at(a, 8),
            l2stride: u64_at(a, 32),
            l2_cnt: u64_at(a, 40),
            l2_off: m.offset + u64_at(a, 48),
            l1_off: m.offset + u64_at(a, 56),
        };
        if u64_at(a, 16) != r || u64_at(a, 24) != n {
            return Err("m12 declares a different (n, R)".into());
        }
        if rs.stride != block_runs || rs.l2stride == 0 || rs.l2stride % rs.stride != 0 {
            return Err(format!(
                "m12 stride {} / level-2 stride {} is not usable: m1 declares a block width of\n  \
                 {block_runs} runs, and the level-2 stride must be a positive multiple of it",
                rs.stride, rs.l2stride
            ));
        }
        if rs.l1_cnt != (r + rs.stride - 1) / rs.stride
            || rs.l2_cnt != (r + rs.l2stride - 1) / rs.l2stride
            || m.bytes != u64_at(a, 56) + 8 * rs.l1_cnt
        {
            return Err("m12 directory shape is inconsistent".into());
        }
        // ---- m13
        let m = reader.member(13);
        let a = reader.slice(m.offset, m.bytes);
        if a.len() < 72 {
            return Err("m13 member too short".into());
        }
        let nsym = u64_at(a, 16);
        if nsym != 256 || u64_at(a, 40) != n || u64_at(a, 48) != r {
            return Err("m13 declares a different (nsym, n, R)".into());
        }
        // m13 header word 3 is the level-2 stride of the per-symbol directory
        // (bit6/xsa_finish.cpp: container.num(PF_RUNDIR_L2)). It is the
        // container's number, not ours: read it, then prove it closes on the
        // member's own size below. Only its intrinsic validity is asserted here.
        let rd_l2 = u64_at(a, 24);
        // The level-2 stride is the CONTAINER's number and is independent of the
        // block width (the writer's PF_RUNDIR_L2 counts directory entries per
        // symbol, not runs), so nothing here demands a multiple of block_runs:
        // what proves it is the member-size closure checked below, plus
        // positivity. Refusing a well-formed container over a relationship the
        // format does not require is how a reader becomes a format.
        if rd_l2 == 0 {
            return Err("m13 declares level-2 stride 0".into());
        }
        let mut counts = Vec::with_capacity(256);
        let mut offs = Vec::with_capacity(257);
        for i in 0..256 {
            counts.push(u64_at(a, 72 + 8 * i));
        }
        for i in 0..257 {
            offs.push(u64_at(a, 72 + 8 * 256 + 8 * i));
        }
        let total_entries = u64_at(a, 32);
        if offs[256] != total_entries {
            return Err("m13 entry offsets do not close on the entry count".into());
        }
        let mut l2offs = Vec::with_capacity(257);
        {
            let mut l2 = 0u64;
            for ci in 0..256 {
                l2offs.push(l2);
                l2 += (counts[ci] + rd_l2 - 1) / rd_l2;
            }
            l2offs.push(l2);
        }
        let ent_off = m.offset + u64_at(a, 56);
        let l2_off = m.offset + u64_at(a, 64);
        if ent_off + 16 * total_entries != m.offset + m.bytes
            || l2_off + 16 * l2offs[256] != ent_off
        {
            return Err("m13 member size does not match its own declaration".into());
        }
        let rd = RunDir {
            ent_off,
            l2_off,
            l2: rd_l2,
            counts,
            offs,
            l2offs,
            occ_stride: u64_at(a, 0),
            run_gap: u64_at(a, 8),
        };
        if l2_off % 8 != 0 || ent_off % 8 != 0 {
            return Err("m13 areas are not 8-byte aligned".into());
        }

        // ---- m11: tail SA of every 1024th run (unchanged by this format change)
        let m = reader.member(11);
        let a = reader.slice(m.offset, m.bytes);
        if m.count != (r + 1023) / 1024 || m.bytes != 16 + 8 * m.count {
            return Err(format!("m11 anchor size {} / count {}", m.bytes, m.count));
        }
        let exponent = u64_at(a, 0);
        let stripe = 1u64 << exponent;
        if stripe != 1024 || u64_at(a, 8) != m.count {
            return Err(format!(
                "m11 anchor header mismatch: stride {stripe}, count field {}, directory {}",
                u64_at(a, 8),
                m.count
            ));
        }
        let anchors: Vec<u64> = (0..m.count as usize)
            .map(|i| u64_at(a, 16 + 8 * i))
            .collect();
        for (i, &v) in anchors.iter().enumerate() {
            if v >= n {
                return Err(format!("m11 anchor {i} value {v} outside [0,n)"));
            }
        }
        let t_anchors = t0.elapsed();

        // ---- page the three hot members in, asynchronously. A locate walks the
        // run-start array and the per-symbol directory at 2000+ essentially random
        // positions; without this every one of those is a minor fault, and the
        // walk is a dependent chain so they cannot overlap.
        let mut prefetched = 0u64;
        for m in reader.members.iter().filter(|m| [1u32, 12, 13].contains(&m.id)) {
            if let Err(e) = reader.map.advise_range(
                memmap2::Advice::WillNeed,
                m.offset as usize,
                m.bytes as usize,
            ) {
                return Err(format!("madvise(WILLNEED) on m{}: {e}", m.id));
            }
            prefetched += m.bytes;
        }
        if prefetched == 0 {
            return Err("no hot members were prefetched".into());
        }
        let t_prefetch = t0.elapsed();

        let mut idx = Index {
            reader,
            c,
            total,
            dir_off: m1dir_off,
            payload_off: m1payload_off,
            payload_end: m1_end,
            huff,
            gamma: Gamma::build(),
            nblocks,
            rs,
            rd,
            anchors,
            stripe,
            heads: Vec::new(),
            runs: Runs::W8(Vec::new()),
            dir: Vec::new(),
            build_ms: 0.0,
            decoded_bytes: 0,
            stages: Vec::new(),
            audit_blocks: 0,
            block_runs,
            rundir_stride: rd_l2,
            open_threads: 0,
            thp: false,
        };
        if idx.payload_off + m1payload_len
            != idx.reader.member(1).offset + idx.reader.member(1).bytes
        {
            return Err("m1 payload area does not reach the end of its member".into());
        }

        // ---- THE OPEN-TIME DECODE, AND THE AUDIT THAT COMES FREE WITH IT.
        //
        // The run stream is decoded exactly once, block by block, into three
        // per-run columns. That is what makes a rank step decode-free; the
        // previous reader decoded ~2 blocks PER LF STEP out of the mmap and was
        // measured at 8.5-10 us/step (RESULTS_QUERY_HOTPATH.md, and the reason is
        // a warm 2.33 us/256-run block decode, NOT page faults: the same query on
        // tmpfs is 114.8 ms against 115.0 ms on NVMe, 0 major faults).
        //
        // The audit gets STRONGER for free: every block's run lengths must sum to
        // exactly the m12 run-start span that brackets it, and the last block must
        // close exactly on n. The old reader checked 1024 sampled blocks; this
        // checks all 223,747 of them because the decode is happening anyway.
        let t_mat0 = t0.elapsed();
        idx.materialise()?;
        let t_mat = t0.elapsed();
        let n_audit = idx.nblocks;
        let dec = idx.decoded_bytes;
        let stages = vec![
            ("m11-anchors", t_anchors.as_secs_f64() * 1e3),
            ("prefetch", (t_prefetch - t_anchors).as_secs_f64() * 1e3),
            ("run-columns", (t_mat - t_mat0).as_secs_f64() * 1e3),
        ];
        let build_ms = t_mat.as_secs_f64() * 1e3;
        idx.stages = stages;
        idx.build_ms = build_ms;
        if crate::trace_on() {
            for (name, ms) in &idx.stages {
                eprintln!("TRACE stage=lite.{name} ms={ms:.1}");
            }
            eprintln!(
                "TRACE stage=lite.build-total ms={build_ms:.1} audit_blocks={n_audit} \
                 run_column_bytes={dec} open_threads={} layout={} thp={}",
                idx.open_threads,
                idx.runs.name(),
                idx.thp
            );
        }
        Ok(idx)
    }

    /// Decode every m1 block once and build the three per-run columns:
    /// `heads`, `lens` (both byte-exact from the container) and `seen`
    /// (`seen[run]` = how many runs of `heads[run]` precede `run`).
    ///
    /// Parallel over a partition of the BLOCK RANGE into contiguous windows of
    /// `blocks_per_chunk` blocks. Window `ci` writes only `heads`/`lens` rows
    /// `[ci*run_per_chunk, min(r, (ci+1)*run_per_chunk))` -- `par_chunks_mut` on
    /// both columns, zipped, so the disjointness is the type system's problem and
    /// not a comment's. `seen` is then filled by a chunked prefix sum: phase 1
    /// counts symbols per window (a scan of the just-decoded `heads`, no second
    /// decode), a sequential pass makes those counts an exclusive prefix, phase 2
    /// fills `seen` inside each window. Only `windows x 256` additions are
    /// sequential; both phases are otherwise parallel.
    fn materialise(&mut self) -> Result<(), String> {
        let r = self.reader.r;
        let br = self.block_runs;
        let nblocks = self.nblocks;
        let n = self.reader.n;
        let threads = rayon::current_num_threads().max(1);
        self.open_threads = threads;
        // FAIL LOUD RATHER THAN GET KILLED: the columns are ~9 B/run plus 16 B per
        // 256 rows, and a container big enough to overrun RAM must say so in words
        // instead of taking an OOM kill in the middle of a parallel decode.
        let projected = 9 * r + 16 * ((r + 255) / 256);
        if let Some(avail) = mem_available() {
            if projected > avail {
                return Err(format!(
                    "{}: this reader materialises the run stream at open ({} bytes of run \
                     columns for {} runs) and only {avail} bytes of RAM are available; \
                     free memory, or query a container this box can hold at 9 B/run",
                    self.reader.path, projected, r
                ));
            }
        }
        // One window per thread, rounded UP to whole blocks, at least one block.
        let blocks_per_chunk = ((nblocks + threads as u64 - 1) / threads as u64).max(1);
        let nchunks = ((nblocks + blocks_per_chunk - 1) / blocks_per_chunk) as usize;
        let runs_per_chunk = blocks_per_chunk * br;
        let mut heads = vec![0u8; r as usize];
        let mut words = vec![0u64; r as usize];
        self.thp = advise_huge(&mut heads) && advise_huge(&mut words);

        // ---- phase 0: decode + complete audit + per-window symbol counts.
        // per window: ([runs per symbol], [rows per symbol]). The row totals must
        // equal the m1 C table exactly (that is the container's own definition of
        // a symbol's count); the run counts are `seen`'s prefix bases.
        let per_chunk: Vec<Result<([u64; 256], [u64; 256]), String>> = {
            let idx: &Index = self;
            heads
                .par_chunks_mut(runs_per_chunk as usize)
                .zip(words.par_chunks_mut(runs_per_chunk as usize))
                .enumerate()
                .map(|(ci, (hc, lc))| -> Result<([u64; 256], [u64; 256]), String> {
                    let b0 = ci as u64 * blocks_per_chunk;
                    let b1 = (b0 + blocks_per_chunk).min(nblocks);
                    if b1 <= b0 {
                        return Err(format!("run-column window {ci} owns no block"));
                    }
                    let mut acc = [0u64; 256];
                    let mut rows = [0u64; 256];
                    let mut ltmp = vec![0u32; br as usize];
                    for b in b0..b1 {
                        let first = b * br;
                        let lo = (first - ci as u64 * runs_per_chunk) as usize;
                        let want = ((first + br).min(r) - first) as usize;
                        let (off, hbits, lbits) = idx.dir(b);
                        let hbytes = (hbits as u64 + 7) / 8;
                        let start = idx.payload_off + off;
                        let end = start + hbytes + (lbits as u64 + 7) / 8;
                        if end > idx.payload_end {
                            return Err(format!(
                                "m1 block {b} runs past the payload area ({end} > {})",
                                idx.payload_end
                            ));
                        }
                        let p = &idx.reader.map.as_ref()[start as usize..end as usize];
                        let got = decode_block(
                            &idx.huff,
                            &idx.gamma,
                            &p[..hbytes as usize],
                            hbits as u64,
                            &p[hbytes as usize..],
                            lbits as u64,
                            want,
                            &mut hc[lo..lo + want],
                            &mut ltmp[..want],
                            b,
                        );
                        debug_assert_eq!(got, want);
                        // low half of the interleaved word: the run length. The
                        // high half (the rank) needs the window prefix, so it is
                        // written in the fill phase below.
                        for i in 0..want {
                            lc[lo + i] = u64::from(ltmp[i]);
                        }
                        // the m12 bracketing span, and the audit that the decode
                        // agrees with it run for run.
                        let (s0, s1) = (idx.l1(b), if b + 1 < nblocks { idx.l1(b + 1) } else { n });
                        let mut sum = 0u64;
                        for i in lo..lo + want {
                            let l = lc[i] & 0xffff_ffff;
                            sum += l;
                            rows[hc[i] as usize] += l;
                            acc[hc[i] as usize] += 1;
                        }
                        if s1 - s0 != sum {
                            return Err(format!(
                                "m1 block audit: block {b} run lengths sum to {sum}, but the m12\n  \
                                 span it occupies is [{s0},{s1}) = {}",
                                s1 - s0
                            ));
                        }
                    }
                    Ok((acc, rows))
                })
                .collect()
        };
        // `base[ci]` is how many ROWS of each symbol precede window `ci`: `seen` is
        // a ROW cumulative count (what rank returns), not a run count.
        let mut base: Vec<[u64; 256]> = Vec::with_capacity(nchunks);
        let mut acc = [0u64; 256];
        let mut rows = [0u64; 256];
        for c in per_chunk {
            let (cr, cw) = c?;
            base.push(rows);
            for s in 0..256 {
                acc[s] += cr[s];
                rows[s] += cw[s];
            }
        }
        if let Some(bad) = (0..256).find(|&s| rows[s] != self.total[s]) {
            return Err(format!(
                "run columns disagree with the m1 C table: the decoded run lengths give symbol \
                 {bad} {} rows and the C table says {}",
                rows[bad], self.total[bad]
            ));
        }
        if acc.iter().sum::<u64>() != r {
            return Err(format!(
                "run columns: the windows counted {} runs, the header says {r}",
                acc.iter().sum::<u64>()
            ));
        }

        // ---- phase 1: fill the rank half, width chosen from the container's own
        // totals (a u32 rank covers every container whose biggest symbol count
        // fits, which is the normal case and journals as `seen_width=`).
        let wide = rows.iter().any(|&t| t > u64::from(u32::MAX));
        let runs = if !wide {
            words
                .par_chunks_mut(runs_per_chunk as usize)
                .zip(heads.par_chunks(runs_per_chunk as usize))
                .enumerate()
                .for_each(|(ci, (wc, hc))| {
                    let mut a = base[ci];
                    for i in 0..wc.len() {
                        let h = hc[i] as usize;
                        debug_assert!(a[h] <= u64::from(u32::MAX));
                        wc[i] |= a[h] << 32;
                        a[h] += wc[i] & 0xffff_ffff;
                    }
                });
            Runs::W8(words)
        } else {
            // rare: keep the two columns apart so the rank can be a u64
            let len: Vec<u32> = words.iter().map(|&w| w as u32).collect();
            let mut seen = vec![0u64; r as usize];
            seen.par_chunks_mut(runs_per_chunk as usize)
                .zip(heads.par_chunks(runs_per_chunk as usize))
                .zip(len.par_chunks(runs_per_chunk as usize))
                .enumerate()
                .for_each(|(ci, ((sc, hc), lc))| {
                    let mut a = base[ci];
                    for i in 0..sc.len() {
                        let h = hc[i] as usize;
                        sc[i] = a[h];
                        a[h] += u64::from(lc[i]);
                    }
                });
            Runs::Split { len, seen }
        };
        // ---- phase 2: the row -> run directory (see the `dir` field's doc comment).
        let ndir = ((n + ROW_DIR - 1) / ROW_DIR) as usize;
        let mut dirv = vec![[0u64; 2]; ndir];
        {
            let idx: &Index = self;
            let runs_ref = &runs;
            let per = ((ndir + threads - 1) / threads).max(1);
            dirv.par_chunks_mut(per)
                .enumerate()
                .try_for_each(|(ci, dc)| -> Result<(), String> {
                    let j0 = (ci * per) as u64;
                    let row0 = j0 * ROW_DIR;
                    // the run containing the chunk's first row: the same bisect the
                    // query used to do on EVERY step, now done once per 256 rows.
                    let (mut run, mut s) = bisect_run_of(idx, runs_ref, row0);
                    for (k, d) in dc.iter_mut().enumerate() {
                        let row = (j0 + k as u64) * ROW_DIR;
                        if row < s {
                            return Err(format!("row directory: row {row} precedes run {run}"));
                        }
                        while s + runs_ref.len(run) <= row {
                            s += runs_ref.len(run);
                            run += 1;
                            if run >= r {
                                return Err(format!(
                                    "row directory: rows ran out at run {run} of {r} looking \
                                     for row {row}"
                                ));
                            }
                        }
                        *d = [s, run];
                    }
                    Ok(())
                })?;
        }
        self.decoded_bytes = heads.len() as u64 + runs.bytes() + 16 * dirv.len() as u64;
        self.audit_blocks = nblocks;
        self.heads = heads;
        self.runs = runs;
        self.dir = dirv;
        Ok(())
    }

    #[inline]
    pub fn n(&self) -> u64 {
        self.reader.n
    }
    #[inline]
    pub fn r(&self) -> u64 {
        self.reader.r
    }
    /// A fresh per-pattern working set. The run columns are shared (read-only),
    /// so this is just this pattern's counters.
    pub fn ctx(&self) -> Ctx<'_> {
        Ctx {
            idx: self,
            scan_of: 0,
            scan_cum: 0,
            steps: 0,
            ranks: 0,
        }
    }

    #[inline(always)]
    fn l1(&self, j: u64) -> u64 {
        debug_assert!(j < self.rs.l1_cnt);
        u64_at(self.reader.map.as_ref(), (self.rs.l1_off + 8 * j) as usize)
    }
    #[inline(always)]
    fn l2(&self, j: u64) -> u64 {
        debug_assert!(j < self.rs.l2_cnt);
        u64_at(self.reader.map.as_ref(), (self.rs.l2_off + 8 * j) as usize)
    }
    /// m13 entry `i` of symbol `ch`: (cum rows of ch before run, run index, stride)
    #[inline(always)]
    fn entry(&self, ch: usize, i: u64) -> (u64, u64, u32) {
        let o = (self.rd.ent_off + 16 * (self.rd.offs[ch] + i)) as usize;
        let m = &self.reader.map;
        let cum = u64_at(m.as_ref(), o);
        let w = u64_at(m.as_ref(), o + 8);
        (cum, w & 0xffff_ffff, (w >> 32) as u16 as u32)
    }
    #[inline(always)]
    fn l2entry(&self, ch: usize, i: u64) -> (u64, u64) {
        let o = (self.rd.l2_off + 16 * (self.rd.l2offs[ch] + i)) as usize;
        let m = &self.reader.map;
        (u64_at(m.as_ref(), o), u64_at(m.as_ref(), o + 8) & 0xffff_ffff)
    }
    /// The block directory entry: (payload word offset, head bits, length bits)
    #[inline(always)]
    fn dir(&self, b: u64) -> (u64, u32, u32) {
        let w = u64_at(self.reader.map.as_ref(), (self.dir_off + 8 * b) as usize);
        (((w & 0xffff_ffff) as u64) * 8, (w >> 32) as u16 as u32, (w >> 48) as u32)
    }
}


/// `MemAvailable` from /proc/meminfo, if this kernel reports it. Used only to
/// refuse an allocation that cannot fit, in words, before the allocator is asked
/// for hundreds of megabytes per column.
fn mem_available() -> Option<u64> {
    let s = std::fs::read_to_string("/proc/meminfo").ok()?;
    for line in s.lines() {
        if let Some(rest) = line.strip_prefix("MemAvailable:") {
            let kb: u64 = rest.trim().trim_end_matches(" kB").trim().parse().ok()?;
            return Some(kb * 1024);
        }
    }
    None
}

// ------------------------------------------------------------ huge pages
//
// The run columns are the query's random-access working set (hundreds of MB at
// 150 MB of text), and a random 4 KB page per LF step is a TLB miss on every
// step of a dependent chain. THP is in `madvise` mode on this box
// (/sys/kernel/mm/transparent_hugepage/enabled), so it has to be asked for: one
// advice call per column turns 114k small entries into 229 huge ones. Failure is
// NOT fatal (a kernel without THP must still run the query), it is journaled.
#[cfg(target_os = "linux")]
const MADV_HUGEPAGE: i32 = 14;

#[cfg(target_os = "linux")]
extern "C" {
    fn madvise(addr: *mut core::ffi::c_void, len: usize, advice: i32) -> i32;
}

/// Ask for huge pages over a whole allocation. Returns false if the kernel said
/// no, so the caller can journal it; never an error (see above).
fn advise_huge<T>(v: &mut [T]) -> bool {
    #[cfg(target_os = "linux")]
    {
        let bytes = std::mem::size_of_val(v);
        if bytes == 0 {
            return false;
        }
        // madvise(2) demands a page-aligned range, and a `Vec` from the allocator
        // starts 16 bytes into its mapping, so advise the page-aligned interior
        // (all but the first and last partial page -- 99.9% of a column this big).
        let lo = v.as_mut_ptr() as usize;
        let start = (lo + 4095) & !4095usize;
        let end = (lo + bytes) & !4095usize;
        if end <= start {
            return false;
        }
        // SAFETY: `v` is a live slice, so [start, end) is inside it and mapped.
        let rc = unsafe {
            madvise(
                start as *mut core::ffi::c_void,
                end - start,
                MADV_HUGEPAGE,
            )
        };
        rc == 0
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = v;
        false
    }
}

// ------------------------------------------------------------ the query thread

/// The m12 two-level bisect: `(run, start row)` of the run containing `row`.
///
/// This is what `run_of` used to do on every step. It is now used ONCE PER 256
/// ROWS, to build the row -> run directory, because as a per-step primitive it is
/// a chain of ~6 dependent cache misses in the 1.79 MB window array and measured
/// 1458 cycles of the 1509 a step cost (rdtsc, pattern 6 of the p10 fixture).
#[inline]
fn bisect_run_of(idx: &Index, runs: &Runs, row: u64) -> (u64, u64) {
    let mut lo = 0u64;
    let mut hi = idx.rs.l2_cnt;
    if hi > 1 {
        while hi - lo > 1 {
            let mid = lo + (hi - lo) / 2;
            if idx.l2(mid) <= row {
                lo = mid
            } else {
                hi = mid
            }
        }
    }
    let per = idx.rs.l2stride / idx.rs.stride;
    let j0 = lo * per;
    let j1 = (j0 + per).min(idx.rs.l1_cnt);
    let mut a = j0;
    let mut b = j1;
    if b - a > 1 {
        while b - a > 1 {
            let mid = a + (b - a) / 2;
            if idx.l1(mid) <= row {
                a = mid
            } else {
                b = mid
            }
        }
    }
    let run = a * idx.rs.stride;
    let s = idx.l1(a);
    let avail = (idx.reader.r - run) as usize;
    scan_window(|i| runs.len(run + i as u64), run, s, row, avail, idx.reader.r)
}

impl<'a> Ctx<'a> {
    /// `(head, length)` of run `run`. Two array reads: the run columns are the
    /// whole point of the open-time decode.
    #[inline(always)]
    fn run(&self, run: u64) -> (u8, u32) {
        debug_assert!(run < self.idx.reader.r);
        (
            unsafe { *self.idx.heads.get_unchecked(run as usize) },
            self.idx.runs.len(run) as u32,
        )
    }
    /// `cum_before` for a run's OWN symbol, which is the only rank an LF step
    /// needs (`h = heads[run]`): one column read, no bisect, no scan.
    #[inline(always)]
    fn seen(&self, run: u64) -> u64 {
        self.idx.runs.seen(run)
    }
    fn diag(&self) -> (u64, u64, u64, u64) {
        (self.scan_of, self.scan_cum, self.steps, self.ranks)
    }
    /// `(run, run start row)` of the run holding row `row`.
    ///
    /// Two bisects (a cache-resident level-2 array, then one level-1 window) plus
    /// a forward scan of at most `block_runs` runs over the RAM `lens` column -
    /// so a lookup never decodes anything.
    fn run_of(&mut self, row: u64) -> (u64, u64) {
        let idx = self.idx;
        debug_assert!(row < idx.reader.n);
        // ONE load: the row -> run directory entry for this 256-row block.
        let j = (row / ROW_DIR) as usize;
        let d = unsafe { *idx.dir.get_unchecked(j) };
        let (s, run0) = (d[0], d[1]);
        let run = run0;
        let run_next = match unsafe { idx.dir.get(j + 1) } {
            Some(v) => v[1],
            None => idx.reader.r,
        };
        let avail = (run_next.min(idx.reader.r) - run) as usize;
        let (run, s) = match &idx.runs {
            Runs::W8(w) => {
                let base = w.as_ptr();
                let start = run as usize;
                scan_window(
                    |i| unsafe { *base.add(start + i) } & 0xffff_ffff,
                    run,
                    s,
                    row,
                    avail,
                    idx.reader.r,
                )
            }
            Runs::Split { len, .. } => {
                let base = len.as_ptr();
                let start = run as usize;
                scan_window(
                    |i| u64::from(unsafe { *base.add(start + i) }),
                    run,
                    s,
                    row,
                    avail,
                    idx.reader.r,
                )
            }
        };
        self.scan_of += run - run0;
        (run, s)
    }
    /// Occurrences of `ch` in the BWT before run `k` (i.e. rows of `ch` in runs
    /// `[0,k)`), for an ARBITRARY `ch`.
    ///
    /// Sampling every `occ_stride`-th occurrence of `ch` plus a `run_gap`-run cap
    /// means the scan from the sample to `k` is at most `run_gap` runs, and the
    /// `stride == 1` fast path (the next sample IS the next run of `ch`, which is
    /// what every rare symbol gets) needs no scan at all. This is the SEARCH path
    /// (a pattern character, not a run's own symbol); the LF walk uses the
    /// `seen` column instead and pays no bisect at all.
    fn cum_before(&mut self, ch: u8, k: u64) -> u64 {
        let ci = ch as usize;
        let m = self.idx.rd.counts[ci];
        if m == 0 || k == 0 {
            return 0;
        }
        let idx = self.idx;
        let l2n = (m + idx.rd.l2 - 1) / idx.rd.l2;
        let mut lo = 0u64;
        let mut hi = l2n;
        if hi > 1 {
            while hi - lo > 1 {
                let mid = lo + (hi - lo) / 2;
                if idx.l2entry(ci, mid).1 <= k {
                    lo = mid
                } else {
                    hi = mid
                }
            }
        }
        let j0 = lo * idx.rd.l2;
        let j1 = (j0 + idx.rd.l2).min(m);
        let mut a = j0;
        let mut b = j1;
        if b - a > 1 {
            while b - a > 1 {
                let mid = a + (b - a) / 2;
                if idx.entry(ci, mid).1 <= k {
                    a = mid
                } else {
                    b = mid
                }
            }
        }
        let (c0, r0, stride) = idx.entry(ci, a);
        if k <= r0 {
            return c0;
        }
        if stride == 1 {
            // the next sample is the next run of `ch`: the only `ch` run in
            // [r0, k) is r0 itself.
            if a + 1 < m {
                return idx.entry(ci, a + 1).0;
            }
            return c0 + u64::from(self.run(r0).1);
        }
        // THE SCAN IS BOUNDED BY THE HEADER'S OWN run_gap, and the reader never
        // scans past it. The writer samples `ch` at any occurrence that is
        // `run_gap` runs past the previous sample (bit6/xsa_finish.cpp:
        // `k - lastSample[ch] >= PF_RUNDIR_GAP`), so every occurrence between two
        // consecutive samples lies within `run_gap` runs of the earlier one. Past
        // that point all of this sample interval's occurrences are already behind
        // us and the answer is the next sample's cumulative count (or, for the
        // last sample, its own stride = occurrences after it). Without this the
        // scan is unbounded in principle and was measured at 4203 runs on a
        // symbol with a 253,795-run occurrence gap.
        let gap = idx.rd.run_gap;
        if k > r0 + gap {
            return if a + 1 < m {
                idx.entry(ci, a + 1).0
            } else {
                c0 + u64::from(stride)
            };
        }
        let mut acc = c0;
        let mut run = r0;
        let heads = &idx.heads;
        let runs = &idx.runs;
        while run < k {
            self.scan_cum += 1;
            let h = unsafe { *heads.get_unchecked(run as usize) };
            if h == ch {
                acc += runs.len(run);
            }
            run += 1;
        }
        if run - r0 > gap + M1_SCAN_SLACK {
            die(&format!(
                "cum_before: scanned {} runs from sample {r0} to {k} for symbol {ch}, past the\n  \
                 m13 run_gap {gap} the directory declares - the directory's sampling is not the\n  \
                 one this reader requires",
                run - r0
            ));
        }
        acc
    }
    /// Occurrences of `ch` in the BWT before row `i`.
    #[inline]
    fn rank(&mut self, ch: u8, i: u64) -> u64 {
        self.ranks += 1;
        if i == 0 {
            return 0;
        }
        if i >= self.idx.reader.n {
            return self.idx.total[ch as usize];
        }
        let (run, s) = self.run_of(i);
        if self.run(run).0 == ch {
            // the row's own run carries `ch`: cum_before(ch, run) + (i - s) is
            // exactly `seen[run]` + (i - s) because ch IS the run's head.
            self.seen(run) + (i - s)
        } else {
            self.cum_before(ch, run)
        }
    }
    /// Half-open BWT interval of the pattern, consumed right to left.
    pub fn search(&mut self, pat: &[u8]) -> (u64, u64) {
        let mut l = 0u64;
        let mut r = self.idx.reader.n;
        for &ch in pat {
            let base = self.idx.c[ch as usize];
            let (nl, nr) = (base + self.rank(ch, l), base + self.rank(ch, r));
            l = nl;
            r = nr;
            if l >= r {
                return (l, r);
            }
        }
        (l, r)
    }
    /// `(text position, LF steps walked)` of BWT row `row`.
    ///
    /// m11 stores the *raw* SA coordinate of the tail row of runs 0, 1024, 2048,
    /// ... (bit6/xsa_finish.cpp: `if (idx % 1024 == 0) write(v.t)`), in the same
    /// coordinate system as the container's `n`. LF maps row i to the row whose
    /// SA is i-1, so walking LF `steps` times RAISES the text position by
    /// `steps`: `position = anchor + steps (mod n)`.
    pub fn locate_steps(&mut self, row: u64) -> (u64, u64) {
        let n = self.idx.reader.n;
        let stripe = self.idx.stripe;
        let mut pos = row;
        let mut steps = 0u64;
        loop {
            let (run, s) = self.run_of(pos);
            let (h, l) = self.run(run);
            let end = s + u64::from(l);
            if pos == end - 1 && run % stripe == 0 {
                let anchor = self.idx.anchors[(run / stripe) as usize];
                return ((anchor + steps) % n, steps);
            }
            let lf = self.idx.c[h as usize] + self.seen(run);
            pos = lf + (pos - s);
            steps += 1;
            self.steps += 1;
            if steps >= n {
                die("locate: LF walk has no anchored sample");
            }
        }
    }
    pub fn locate(&mut self, row: u64) -> u64 {
        self.locate_steps(row).0
    }
}

/// Decode one m1 block. Bit-exactness is checked, not assumed: the head stream
/// must be consumed to its last bit and the length stream likewise, so a wrong
/// offset or a wrong block width cannot silently produce plausible runs.
#[allow(clippy::too_many_arguments)]
fn decode_block(
    huff: &Huff,
    gamma: &Gamma,
    hbytes: &[u8],
    hbits: u64,
    lbytes: &[u8],
    lbits: u64,
    want: usize,
    heads: &mut [u8],
    lens: &mut [u32],
    b: u64,
) -> usize {
    if hbits > hbytes.len() as u64 * 8 || lbits > lbytes.len() as u64 * 8 {
        die(&format!("m1 block {b}: bit counts exceed the payload bytes"));
    }
    if want > heads.len() || want > lens.len() {
        die(&format!(
            "m1 block {b}: decoded width {want} exceeds the {}-run buffers",
            heads.len()
        ));
    }
    let mut hb = Bits::new(hbytes, hbits);
    let mut lb = Bits::new(lbytes, lbits);
    let dbg = std::env::var("M1_DBG").ok().and_then(|v| v.parse::<u64>().ok()) == Some(b);
    let mut sum_len = 0u64;
    for i in 0..want {
        heads[i] = huff.decode(&mut hb);
        sum_len += huff.len[heads[i] as usize] as u64;
        if dbg {
            eprintln!(
                "M1_DBG block={b} i={i} sym={} code_len={} hb.pos={}",
                heads[i], huff.len[heads[i] as usize], hb.pos
            );
        }
        let v = gamma.decode(&mut lb);
        if v == 0 {
            die(&format!("m1 block {b}: run length 0"));
        }
        lens[i] = v;
    }
    if hb.pos != hbits || sum_len != hbits {
        die(&format!(
            "m1 block {b}: head stream consumed {} of {hbits} bits (code lengths sum to {sum_len})",
            hb.pos
        ));
    }
    if lb.pos != lbits {
        die(&format!(
            "m1 block {b}: length stream consumed {} of {lbits} bits",
            lb.pos
        ));
    }
    want
}

// ---------------------------------------------------------------- the command

struct Args {
    container: String,
    patterns: String,
    /// `--patterns-fasta`: force the FASTA reader. `--patterns` auto-detects
    /// instead (a file whose first byte is `>` is FASTA, anything else is the
    /// 0x1E record contract).
    fasta: bool,
    output: String,
    counts: bool,
    threads: usize,
    verify: bool,
}

fn parse(args: &[String]) -> Args {
    let mut container: Option<String> = None;
    let mut patterns: Option<String> = None;
    let mut fasta = false;
    let mut output = "tsv".to_string();
    let mut counts = false;
    let mut threads = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1);
    let mut verify = false;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--patterns" if i + 1 < args.len() => {
                if fasta {
                    die("query: --patterns and --patterns-fasta are exclusive: pick one\n  \
                         --patterns auto-detects FASTA by its leading '>'");
                }
                patterns = Some(args[i + 1].clone());
                i += 2;
            }
            "--patterns-fasta" if i + 1 < args.len() => {
                if patterns.is_some() && !fasta {
                    die("query: --patterns and --patterns-fasta are exclusive: pick one\n  \
                         --patterns auto-detects FASTA by its leading '>'");
                }
                patterns = Some(args[i + 1].clone());
                fasta = true;
                i += 2;
            }
            "--output" if i + 1 < args.len() => {
                output = args[i + 1].clone();
                i += 2;
            }
            "-j" if i + 1 < args.len() => {
                threads = args[i + 1]
                    .parse::<usize>()
                    .unwrap_or_else(|_| die("-j: integer expected"))
                    .max(1);
                i += 2;
            }
            "--counts" => {
                counts = true;
                i += 1;
            }
            "--verify" => {
                verify = true;
                i += 1;
            }
            "--ri4" | "--sxi" => die(
                "query: --ri4/--sxi are gone; the container is positional and identifies itself\n  \
                 xsa query <container> --patterns <file> [-j N] [--output tsv|jsonl]",
            ),
            // THE ONE ORIENTATION RULE, enforced at the front door: this reader
            // has no alphabet (see the module doc), so a strand/mode switch has
            // nothing to switch. Refusing it by name is cheaper than silently
            // ignoring it and letting a caller believe half its work was done.
            "--mode" => die(
                "query: --mode is not accepted for a lite container: a lite container has no \
                 alphabet; patterns are raw bytes\n  \
                 a byte-string index has exactly one orientation, so there is no dna/text mode \
                 and no reverse complement",
            ),
            other if other.starts_with('-') => die(&format!("query: unknown option {other}")),
            other => {
                if container.is_some() {
                    die(&format!("query: unexpected positional argument {other}"));
                }
                container = Some(other.to_string());
                i += 1;
            }
        }
    }
    let container = container.unwrap_or_else(|| {
        die("query: need a container path\n  xsa query <container> --patterns <file> [-j N]")
    });
    let patterns = patterns.unwrap_or_else(|| die("query: need --patterns <file>"));
    if output != "tsv" && output != "jsonl" {
        die("query: --output must be tsv or jsonl");
    }
    // Refused here, not in the emit loop: a run that has already built the
    // kernel and answered 100 patterns must not discover a flag conflict.
    if counts && output != "tsv" {
        die(&format!(
            "query: --counts is a tsv option; --output {output} already reports a count per id"
        ));
    }
    Args {
        container,
        patterns,
        fasta,
        output,
        counts,
        threads,
        verify,
    }
}

/// One pattern's result: positions plus the walls the kernel charged for it.
struct Hit {
    positions: Vec<u64>,
    search_us: f64,
    /// mean over occurrences
    locate_us: f64,
    /// total LF steps over all occurrences, and the worst single walk
    steps: u64,
    max_steps: u64,
    /// Trace-only work counters for this pattern: (run_of scan runs, cum_before
    /// scan runs, LF steps, rank calls).
    diag: (u64, u64, u64, u64),
}

/// The two pattern-file contracts.
///
/// `Records` is the corpus contract, verbatim: records separated by 0x1E,
/// `rec \x1E rec \x1E ... rec \x1E`, ARBITRARY BYTES, no escaping, no alphabet,
/// id = 0-based record index. That is the only thing a byte-string index can be
/// asked about, and it is the default.
///
/// `Fasta` is the legacy reader, kept behind `--patterns-fasta` (and reached by
/// auto-detect, because a `--patterns` file that starts with `>` cannot be a
/// 0x1E record stream in any useful sense: its header line would be queried as
/// a pattern and come back absent, which is exactly the 200-patterns/100-absent
/// reading this CLI used to produce when a FASTA file was handed to a newline
/// parser).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum PatFormat {
    Records,
    Fasta,
}

impl PatFormat {
    fn name(self) -> &'static str {
        match self {
            PatFormat::Records => "records-0x1e",
            PatFormat::Fasta => "fasta",
        }
    }
}

/// 1..=65536 bytes per pattern: the search kernel's contract (the working copy
/// is pattern-length local, and a pattern longer than this is not a read).
const PAT_LEN_MAX: usize = 65536;

/// Read a pattern file and say which contract was used, once, at startup.
fn read_patterns(path: &str, force_fasta: bool) -> (Vec<Vec<u8>>, PatFormat) {
    let data = std::fs::read(path).unwrap_or_else(|e| die(&format!("read {path}: {e}")));
    let (fmt, why) = if force_fasta {
        (PatFormat::Fasta, "forced-fasta")
    } else {
        detect_format(&data)
    };
    let out = match fmt {
        PatFormat::Fasta => read_fasta(path, &data),
        PatFormat::Records => read_records(path, &data),
    };
    if out.is_empty() {
        die(&format!("{path}: {} file holds 0 patterns", fmt.name()));
    }
    for (i, p) in out.iter().enumerate() {
        if p.len() > PAT_LEN_MAX {
            die(&format!(
                "{path}: record {i} is {} bytes, over the {PAT_LEN_MAX}-byte pattern limit",
                p.len()
            ));
        }
    }
    eprintln!(
        "PATTERNS_USED path={} format={} detected={} records={} bytes={} ids=0..{} separator={}",
        path,
        fmt.name(),
        why,
        out.len(),
        data.len(),
        out.len() - 1,
        match fmt {
            PatFormat::Records => "0x1e",
            PatFormat::Fasta => "newline",
        }
    );
    (out, fmt)
}

/// `--patterns` auto-detect, and why it is not just `first byte == '>'`: the
/// record contract allows ARBITRARY bytes, so a legitimate first record may
/// begin with '>'. A leading '>' opens a FASTA header only if that header is a
/// LINE, i.e. only if the first newline comes before the first 0x1E. If a 0x1E
/// arrives first the file is a record stream whose first record happens to start
/// with '>' -- and the choice is journaled either way (`detected=`), so nobody
/// has to guess which reader ran.
fn detect_format(data: &[u8]) -> (PatFormat, &'static str) {
    if data.first() != Some(&b'>') {
        return (PatFormat::Records, "no-leading-gt");
    }
    let nl = data.iter().position(|&b| b == b'\n');
    let sep = data.iter().position(|&b| b == 0x1e);
    match (nl, sep) {
        (Some(n), Some(s)) if s < n => (PatFormat::Records, "gt-then-0x1e-before-newline"),
        (None, Some(_)) => (PatFormat::Records, "gt-then-0x1e-before-newline"),
        _ => (PatFormat::Fasta, "leading-gt"),
    }
}

/// `rec \x1E rec \x1E ... rec \x1E`: arbitrary bytes, no escaping, no alphabet.
///
/// The 0x1E is the separator, so the trailing 0x1E of the contract terminates
/// the last record and does not open an empty one; a file that ends without it
/// is accepted (the last record is terminated by EOF) and said so in the journal
/// (`sep=no`). Any OTHER empty record -- `a\x1E\x1Eb\x1E`, or a 0x1E-terminated
/// file with nothing in it -- is a malformed pattern file, not a zero-length
/// pattern, so it is refused rather than searched for.
fn read_records(path: &str, data: &[u8]) -> Vec<Vec<u8>> {
    let (body, ends_with_sep) = match data.strip_suffix(b"\x1e") {
        Some(b) => (b, true),
        None => (data, false),
    };
    if body.is_empty() {
        die(&format!(
            "{path}: 0x1E record file holds no record ({} bytes{})",
            data.len(),
            if ends_with_sep { ", one bare separator" } else { "" }
        ));
    }
    let mut out: Vec<Vec<u8>> = Vec::with_capacity(16);
    for (i, rec) in body.split(|&b| b == 0x1e).enumerate() {
        if rec.is_empty() {
            die(&format!(
                "{path}: record {i} is empty; the 0x1E contract has no empty record \
                 (and no escaping: a bare 0x1E is always a record boundary)"
            ));
        }
        out.push(rec.to_vec());
    }
    if !ends_with_sep {
        eprintln!(
            "PATTERNS_NOTE {path}: last record is not 0x1E-terminated (EOF ends it); \
             the contract is rec\\x1E rec\\x1E ... rec\\x1E"
        );
    }
    out
}

fn read_fasta(path: &str, data: &[u8]) -> Vec<Vec<u8>> {
    let mut out: Vec<Vec<u8>> = Vec::new();
    let mut cur: Option<Vec<u8>> = None;
    for line in data.split(|&b| b == b'\n') {
        let line = line.strip_suffix(b"\r").unwrap_or(line);
        if line.first() == Some(&b'>') {
            if let Some(p) = cur.take() {
                out.push(p);
            }
            cur = Some(Vec::new());
        } else if let Some(p) = cur.as_mut() {
            p.extend_from_slice(line);
        } else if !line.is_empty() {
            die(&format!("{path}: FASTA sequence data before the first header"));
        }
    }
    if let Some(p) = cur.take() {
        out.push(p);
    }
    for (i, p) in out.iter().enumerate() {
        if p.is_empty() {
            die(&format!("{path}: FASTA record {i} is empty"));
        }
    }
    out
}

pub fn command(args: &[String]) {
    // Wall clock from process start: the acceptance question is "how long
    // until the first answer", so every stage is reported against this.
    let t_proc = Instant::now();
    let a = parse(args);
    let t_open0 = Instant::now();
    let reader = match Reader::open(&a.container) {
        Ok(r) => r,
        Err(e) => die(&format!(
            "{e}\n  supported: SXI2 v6 lite (members m1,m4,m5,m7,m10,m11)\n  \
             migration: a legacy container must be rebuilt with `xsa build` (v6 lite is the\n  \
             only published query format); the phi-era readers live behind the deprecated\n  \
             `xsa query-legacy`, which is ~1e6x slower per pattern and takes minutes to open."
        )),
    };
    let open_ms = t_open0.elapsed().as_secs_f64() * 1e3;
    let members: Vec<String> = reader.members.iter().map(|m| format!("m{}", m.id)).collect();
    // OPEN IS MMAP-ONLY: header, directory, header CRC and the 256-byte remap.
    // Nothing in the run stream, the chi set or the anchors is decoded here, so
    // `decoded_bytes` is 0 and no stage has run. This is the line that makes the
    // 405 s legacy open auditable against the 1 ms it costs now.
    eprintln!(
        "QLITE_OPEN container={} format=sxi2-v6-lite magic=SXI2 version={} \
         mmap_ms={open_ms:.2} decoded_bytes=0 stages=none n={} runs={} members={} end_symbol=none \
         orientation=none alphabet=none",
        reader.path,
        reader.version,
        reader.n,
        reader.r,
        members.join(",")
    );
    eprintln!(
        "SQLITE_QUERY_OPEN format=sxi2-v6-lite mmap_ms={open_ms:.2} decoded_bytes=0 stages=none"
    );
    if a.verify {
        if let Err(e) = reader.verify_members() {
            die(&e);
        }
        eprintln!("QUERY_VERIFY members=ok");
    }
    // Format choice is journaled once, inside read_patterns (PATTERNS_USED).
    let (patterns, _pformat) = read_patterns(&a.patterns, a.fasta);
    if a.threads > 1 {
        let _ = rayon::ThreadPoolBuilder::new()
            .num_threads(a.threads)
            .build_global();
    }
    let index = match Index::open(reader, a.threads) {
        Ok(i) => i,
        Err(e) => die(&e),
    };
    let stage_list: Vec<String> = index
        .stages
        .iter()
        .map(|(n, ms)| format!("{n}={ms:.1}ms"))
        .collect();
    eprintln!(
        "QLITE_BUILD runs={} decoded_bytes={} stages=[{}] build_ms={:.1} n={} threads={} patterns={} \
         m1_block_runs={} m1_blocks={} rundir_l2={} rundir_occ_stride={} rundir_gap={}",
        index.r(),
        index.decoded_bytes,
        stage_list.join(" "),
        index.build_ms,
        index.n(),
        a.threads,
        patterns.len(),
        index.block_runs,
        index.nblocks,
        index.rundir_stride,
        index.rd.occ_stride,
        index.rd.run_gap
    );
    let sigma = index.reader.sigma;
    let stdout = std::io::stdout();
    let mut w: Box<dyn Write> = Box::new(BufWriter::with_capacity(1 << 20, stdout.lock()));
    let t0 = Instant::now();
    // Per-pattern results, produced in parallel, emitted in input order.
    let results: Vec<Hit> = patterns
        .par_iter()
        .map(|pat| {
            if pat.is_empty() || pat.len() > 65536 {
                die(&format!("pattern of length {} is not 1..={PAT_LEN_MAX}", pat.len()));
            }
            // Index coordinates: the container's text is the corpus with the
            // m7 remap applied, in corpus order; backward search consumes the
            // pattern right to left, so the working pattern is
            //     sigma(pattern) reversed.
            // One pattern in, one search out: `orientation=none`, exactly one
            // occurrence set. No reverse complement is ever built here.
            // Measured against the 150 MB fixture oracle: remap+reverse gives
            // 100/100 counts and 574/574 positions; every other combination
            // (identity, reverse-only, remap-only) gives 0/100.
            let mut work: Vec<u8> = pat.iter().map(|&c| sigma[c as usize]).collect();
            work.reverse();
            // One block cache per pattern: the working set of a single rank/locate
            // walk, so two patterns sharing a rayon thread never evict each other.
            let mut cx = index.ctx();
            let ts = Instant::now();
            let (l, r) = cx.search(&work);
            let search_us = ts.elapsed().as_secs_f64() * 1e6;
            let tl = Instant::now();
            let mut positions = Vec::with_capacity((r - l).min(1 << 20) as usize);
            let mut steps = 0u64;
            let mut max_steps = 0u64;
            for row in l..r {
                let (p, st) = cx.locate_steps(row);
                positions.push(p);
                steps += st;
                max_steps = max_steps.max(st);
            }
            let locate_us = if r > l {
                tl.elapsed().as_secs_f64() * 1e6 / (r - l) as f64
            } else {
                0.0
            };
            let (d_of, d_cum, d_steps, d_ranks) = cx.diag();
            Hit {
                positions,
                search_us,
                locate_us,
                steps,
                max_steps,
                diag: (d_of, d_cum, d_steps, d_ranks),
            }
        })
        .collect();
    let elapsed = t0.elapsed();
    let first_answer_ms = t_proc.elapsed().as_secs_f64() * 1e3;
    let mut nocc = 0u64;
    let mut nabs = 0u64;
    for (pid, hit) in results.iter().enumerate() {
        let positions = &hit.positions;
        if positions.is_empty() {
            nabs += 1;
        }
        nocc += positions.len() as u64;
        // TWO OUTPUT CONTRACTS, both keyed by the 0-based pattern id (which for a
        // 0x1E pattern file IS the record index, so a caller can join the answer
        // back to its own file without a header).
        //
        // tsv (default) is the text product: one line per occurrence,
        // `id<TAB>offset`, nothing else -- no header, no escaping, no counts
        // mixed in. A pattern with no occurrence contributes no line, so the
        // line count is the occurrence count (574 for the 150 MB fixture) and
        // the ids that do not appear are the absent patterns.
        //
        // jsonl is the old read-mapping product, one object per pattern:
        // {"pattern_id":i,"count":c,"offsets":[...]}. It carries counts for
        // every pattern, so `--counts` has nothing to add to it and is refused
        // rather than silently ignored.
        match (a.output.as_str(), a.counts) {
            ("tsv", true) => writeln!(w, "{}\t{}", pid, positions.len()).unwrap(),
            ("tsv", false) => {
                for p in positions {
                    writeln!(w, "{}\t{}", pid, p).unwrap();
                }
            }
            ("jsonl", false) => {
                let list: Vec<String> = positions.iter().map(|p| p.to_string()).collect();
                writeln!(
                    w,
                    "{{\"pattern_id\":{pid},\"count\":{},\"offsets\":[{}]}}",
                    positions.len(),
                    list.join(",")
                )
                .unwrap();
            }
            (other, true) => die(&format!(
                "query: --counts is a tsv option; --output {other} already reports a count per id"
            )),
            (other, false) => die(&format!("query: output contract {other} is not implemented")),
        }
    }
    w.flush().unwrap();
    // Per-pattern timing, for the rows the caller can actually time (a
    // high-count pattern is dominated by materialising tens of millions of
    // positions and says nothing about the search kernel).
    let (mut worst_us, mut low_sum_us, mut nlow_us, mut high) = (0.0f64, 0.0f64, 0u64, 0u64);
    let (mut low_steps, mut low_max_steps, mut low_occ) = (0u64, 0u64, 0u64);
    for (pid, hit) in results.iter().enumerate() {
        let us = hit.search_us + hit.locate_us * hit.positions.len() as f64;
        if crate::trace_on() {
            eprintln!(
                "TRACE_PATTERN id={} len={} occ={} search_us={:.1} locate_us={:.1} \
                 steps={} max_steps={} total_us={:.1} lf_us_per_step={:.3} scan_of={} scan_cum={} \
                 diag_steps={} ranks={}",
                pid,
                patterns[pid].len(),
                hit.positions.len(),
                hit.search_us,
                hit.locate_us,
                hit.steps,
                hit.max_steps,
                us,
                // THE per-LF-step figure (acceptance C1): the locate wall over the
                // sample points it walked, NOT over the pattern's total (which
                // also carries the search and the output materialisation).
                if hit.steps > 0 {
                    hit.locate_us * hit.positions.len() as f64 / hit.steps as f64
                } else {
                    0.0
                },
                hit.diag.0,
                hit.diag.1,
                hit.diag.2,
                hit.diag.3
            );
        }
        if hit.positions.len() <= 3 {
            low_steps += hit.steps;
            low_max_steps = low_max_steps.max(hit.max_steps);
            low_occ += hit.positions.len() as u64;
            low_sum_us += us;
            nlow_us += 1;
            if us > worst_us {
                worst_us = us;
            }
        } else {
            high += 1;
        }
    }
    eprintln!(
        "QUERY_DONE patterns={} occurrences={} absent={} query_ms={:.1} first_answer_ms={:.1} \
         low_count_patterns={} low_count_mean_us={:.1} low_count_max_us={:.1} \
         low_count_lf_steps={} low_count_mean_steps={:.0} low_count_max_steps={} \
         high_count_patterns={}",
        patterns.len(),
        nocc,
        nabs,
        elapsed.as_secs_f64() * 1e3,
        first_answer_ms,
        nlow_us,
        if nlow_us > 0 { low_sum_us / nlow_us as f64 } else { f64::NAN },
        worst_us,
        low_steps,
        if low_occ > 0 { low_steps as f64 / low_occ as f64 } else { f64::NAN },
        low_max_steps,
        high
    );
}

#[cfg(test)]
mod pattern_contract_tests {
    use super::*;

    // The 0x1E record contract, exercised at the scale where a wrong answer is
    // visible: ids are record indices, bytes are arbitrary, and "empty" is an
    // error rather than a zero-length pattern.

    #[test]
    fn records_are_record_indexed_and_keep_arbitrary_bytes() {
        // rec0 holds a newline, a NUL and a 0x1F: none of them is special.
        let data = b"a\nb\x00c\x1f\x1ed\x1e";
        let recs = read_records("test.pat", data);
        assert_eq!(recs.len(), 2);
        assert_eq!(recs[0], b"a\nb\x00c\x1f");
        assert_eq!(recs[1], b"d");
    }

    #[test]
    fn trailing_separator_does_not_open_an_empty_record() {
        let recs = read_records("test.pat", b"x\x1ey\x1e");
        assert_eq!(recs, vec![b"x".to_vec(), b"y".to_vec()]);
    }

    #[test]
    fn missing_final_separator_still_terminates_by_eof() {
        let recs = read_records("test.pat", b"x\x1ey");
        assert_eq!(recs, vec![b"x".to_vec(), b"y".to_vec()]);
    }

    #[test]
    fn fasta_is_detected_only_when_the_header_is_a_line() {
        assert_eq!(detect_format(b">r1\nACGT\n").0, PatFormat::Fasta);
        assert_eq!(detect_format(b">r1\nACGT\n").1, "leading-gt");
        // A record stream whose FIRST RECORD starts with '>' is still a record
        // stream: the 0x1E arrives before any newline.
        assert_eq!(detect_format(b">r1\x1eACGT\x1e").0, PatFormat::Records);
        assert_eq!(detect_format(b">r1\x1eACGT\x1e").1, "gt-then-0x1e-before-newline");
        assert_eq!(detect_format(b"ACGT\x1e").0, PatFormat::Records);
        assert_eq!(detect_format(b"ACGT\x1e").1, "no-leading-gt");
        // No newline at all, no separator: nothing to read as a header/record
        // pair, and the FASTA reader is what says so out loud.
        assert_eq!(detect_format(b">r1").0, PatFormat::Fasta);
    }

    #[test]
    fn fasta_records_are_concatenated_lines() {
        let recs = read_fasta("test.fa", b">a\nAC\r\nGT\n>b\nTT\n");
        assert_eq!(recs, vec![b"ACGT".to_vec(), b"TT".to_vec()]);
    }
}

#[cfg(test)]
mod window_scan_tests {
    use super::*;

    /// The naive form the 4-wide `scan_window` replaces, run exactly as the
    /// scalar loop did: step while `run + 1 < r` and `s + len <= row`.
    fn naive(lens: &[u64], start_run: u64, mut s: u64, row: u64, r: u64) -> (u64, u64) {
        let mut run = start_run;
        while run + 1 < r {
            let l = lens[(run - start_run) as usize];
            if s + l > row {
                break;
            }
            s += l;
            run += 1;
        }
        (run, s)
    }

    fn check(lens: &[u64], start_run: u64, s0: u64, r: u64) {
        let total: u64 = s0 + lens.iter().sum::<u64>();
        let avail = lens.len();
        for row in s0..total.min(s0 + 4000) {
            let want = naive(lens, start_run, s0, row, r);
            let got = scan_window(|i| lens[i], start_run, s0, row, avail, r);
            assert_eq!(
                got, want,
                "lens={:?} start_run={start_run} s0={s0} row={row} r={r}",
                &lens[..lens.len().min(8)]
            );
            // and the invariant the walk relies on: the row is inside [s, s+l)
            let (run, s) = got;
            assert!(s <= row, "row {row} below the returned start {s}");
            if run + 1 < r {
                assert!(s + lens[(run - start_run) as usize] > row, "row {row} above its run");
            }
        }
    }

    /// The adversarial shapes: a single huge run, all-ones, mixed magnitudes, a
    /// window that ends on the very last run of the container, and a single-run
    /// container (where the scalar loop can never step).
    #[test]
    fn scan_window_matches_the_scalar_reference() {
        check(&[1], 0, 0, 1);
        check(&[7], 0, 0, 1);
        check(&[1, 1], 0, 0, 2);
        check(&[1; 9], 0, 0, 9);
        check(&[1; 10], 3, 100, 20);
        check(&[u32::MAX as u64], 0, 0, 1);
        check(&[1, 1, 1, u32::MAX as u64, 1, 1, 1], 0, 5, 7);
        check(&[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13], 0, 0, 13);
        // a window whose rows are hit exactly on run boundaries, which is where an
        // off-by-one in the 4-wide step would hide
        check(&[4; 16], 0, 0, 16);
        check(&[3, 1, 3, 1, 3, 1, 3, 1], 0, 0, 8);
        check(&[256; 40], 0, 1000, 40);
    }
}
