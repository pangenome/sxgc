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
    pub build_ms: f64,
    /// Bytes of run data this process actually decoded (the open-time block
    /// audit). The whole point of the format change: it is bounded, and it does
    /// not grow with the container.
    pub decoded_bytes: u64,
    /// Per-stage build walls, in order.
    pub stages: Vec<(&'static str, f64)>,
    pub audit_blocks: u64,
    pub block_runs: u64,
    pub rundir_stride: u64,
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

/// One decoded m1 block: `block_runs` runs of head symbol + run length. The
/// width comes from the container, so the buffers are sized from it.
struct Block {
    id: u64,
    n: u32,
    heads: Vec<u8>,
    lens: Vec<u32>,
}

impl Block {
    fn empty(width: u64) -> Block {
        Block {
            id: u64::MAX,
            n: 0,
            heads: vec![0u8; width as usize],
            lens: vec![0u32; width as usize],
        }
    }
}

/// Per-query-thread working set: the block cache. Small enough to stay hot
/// (BLOCK_CACHE x ~350 B) and direct-mapped, so a lookup is one array index.
pub struct Ctx<'a> {
    idx: &'a Index,
    cache: Box<[Block]>,
    hits: u64,
    misses: u64,
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
        if rd_l2 == 0 || rd_l2 % block_runs != 0 {
            return Err(format!(
                "m13 declares level-2 stride {rd_l2}; a usable stride is a positive multiple of the\n  \
                 block width {block_runs} this container declares for m1"
            ));
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
            build_ms: 0.0,
            decoded_bytes: 0,
            stages: Vec::new(),
            audit_blocks: 0,
            block_runs,
            rundir_stride: rd_l2,
        };
        if idx.payload_off + m1payload_len != idx.reader.member(1).offset + idx.reader.member(1).bytes {
            return Err("m1 payload area does not reach the end of its member".into());
        }

        // ---- OPEN-TIME FAIL-LOUD AUDIT. Structural validation proves the
        // directories are well formed; this proves they agree with the payload
        // and with n, without decoding the whole stream: a spread of blocks is
        // decoded and its runs are checked against the m12 samples that bracket
        // them (adjacent blocks must sum to the difference of their samples), and
        // the LAST window must close exactly on n.
        let audit = {
            let (checked, misses) = {
                let mut cx = idx.ctx();
                let mut bad = 0u64;
                let mut checked = 0u64;
                // Dev gate: XSA_M1_AUDIT_FULL=1 audits EVERY block (step 1) instead of
                // M1_AUDIT_BLOCKS samples. Not on any production path; it is how the
                // block payload is proven byte-exact over the whole run stream at 150 MB
                // (~0.9e6 decodes, well under a second).
                let full = std::env::var("XSA_M1_AUDIT_FULL").ok().as_deref() == Some("1");
                let step = if full { 1 } else { (nblocks / M1_AUDIT_BLOCKS).max(1) };
                let mut b = 0u64;
                while b + 1 < nblocks {
                    // block b occupies rows [l1(b), l1(b+1)), so its run lengths
                    // must sum to exactly that span. Decoded, not assumed.
                    let sum = cx.block_sum(b)?;
                    let (s0, s1) = (idx.l1(b), idx.l1(b + 1));
                    if s1 - s0 != sum {
                        bad += 1;
                    }
                    checked += 1;
                    b += step;
                }
                // the tail: the last window must close exactly on n
                let last = nblocks - 1;
                let last_sum = cx.block_sum(last)?;
                if idx.l1(last) + last_sum != n {
                    return Err(format!(
                        "m1 block audit: the last window starts at {} and sums to {}, but n={n}",
                        idx.l1(last),
                        last_sum
                    ));
                }
                if bad != 0 {
                    return Err(format!(
                        "m1 block audit: {bad} of {checked} sampled blocks do not sum to their m12\n  \
                         run-start span"
                    ));
                }
                (checked + 1, cx.misses)
            };
            (checked, misses)
        };
        let t_audit = t0.elapsed();
        let (n_audit, dec) = audit;
        idx.decoded_bytes = dec * BLOCK_BYTES_EST;
        idx.audit_blocks = n_audit;
        let stages = vec![
            ("m11-anchors", t_anchors.as_secs_f64() * 1e3),
            ("prefetch", (t_prefetch - t_anchors).as_secs_f64() * 1e3),
            ("block-audit", (t_audit - t_prefetch).as_secs_f64() * 1e3),
        ];
        let build_ms = t_audit.as_secs_f64() * 1e3;
        idx.stages = stages;
        idx.build_ms = build_ms;
        if crate::trace_on() {
            for (name, ms) in &idx.stages {
                eprintln!("TRACE stage=lite.{name} ms={ms:.1}");
            }
            eprintln!(
                "TRACE stage=lite.build-total ms={build_ms:.1} audit_blocks={n_audit} decoded_blocks={dec}"
            );
        }
        Ok(idx)
    }

    #[inline]
    pub fn n(&self) -> u64 {
        self.reader.n
    }
    #[inline]
    pub fn r(&self) -> u64 {
        self.reader.r
    }
    /// A fresh per-thread working set (block cache). One per pattern.
    pub fn ctx(&self) -> Ctx<'_> {
        Ctx {
            idx: self,
            cache: (0..BLOCK_CACHE)
                .map(|_| Block::empty(self.block_runs))
                .collect::<Vec<Block>>()
                .into_boxed_slice(),
            hits: 0,
            misses: 0,
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

// ------------------------------------------------------------ the query thread

impl<'a> Ctx<'a> {
    /// `(head, length)` of run `run`, decoding its 64-run block on a cache miss.
    #[inline]
    fn run(&mut self, run: u64) -> (u8, u32) {
        let b = run / self.idx.block_runs;
        let i = (run % self.idx.block_runs) as usize;
        let slot = (b % BLOCK_CACHE as u64) as usize;
        if self.cache[slot].id != b {
            self.fill(b, slot);
        }
        let blk = &self.cache[slot];
        debug_assert!((i as u32) < blk.n);
        (blk.heads[i], blk.lens[i])
    }
    #[cold]
    fn fill(&mut self, b: u64, slot: usize) {
        self.misses += 1;
        let idx = self.idx;
        let (off, hbits, lbits) = idx.dir(b);
        let hbytes = (hbits as u64 + 7) / 8;
        let lbytes = (lbits as u64 + 7) / 8;
        let start = idx.payload_off + off;
        let end = start + hbytes + lbytes;
        if end > idx.payload_end {
            die(&format!(
                "m1 block {b} runs past the payload area ({end} > {})",
                idx.payload_end
            ));
        }
        let p = &idx.reader.map.as_ref()[start as usize..end as usize];
        let first = b * idx.block_runs;
        let want = ((first + idx.block_runs).min(idx.reader.r) - first) as usize;
        let mut heads = vec![0u8; want];
        let mut lens = vec![0u32; want];
        let n = decode_block(
            &idx.huff,
            &idx.gamma,
            &p[..hbytes as usize],
            hbits as u64,
            &p[hbytes as usize..],
            lbits as u64,
            want,
            &mut heads,
            &mut lens,
            b,
        );
        self.cache[slot] = Block {
            id: b,
            n: n as u32,
            heads,
            lens,
        };
    }
    fn hits(&self) -> u64 {
        self.hits
    }
    /// Sum of the run lengths in block `b`: one decode, and the caller checks it
    /// against the m12 run-start span it must equal. Fail-loud, not advisory.
    fn block_sum(&mut self, b: u64) -> Result<u64, String> {
        let first = b * self.idx.block_runs;
        let want = ((first + self.idx.block_runs).min(self.idx.reader.r) - first) as usize;
        let slot = (b % BLOCK_CACHE as u64) as usize;
        if self.cache[slot].id != b {
            self.fill(b, slot);
        }
        let blk = &self.cache[slot];
        if blk.n as usize != want {
            return Err(format!("m1 block {b} decoded {} of {want} runs", blk.n));
        }
        Ok(blk.lens[..want].iter().map(|&l| l as u64).sum())
    }
    /// `(run, run start row)` of the run holding row `row`.
    ///
    /// Two bisects (a cache-resident level-2 array, then one 8 KB level-1 window)
    /// plus a forward scan of at most `block_runs` runs inside the block the
    /// row is in - so a lookup costs one decoded block, never a predecessor walk.
    fn run_of(&mut self, row: u64) -> (u64, u64) {
        let idx = self.idx;
        let r = idx.reader.r;
        let n = idx.reader.n;
        debug_assert!(row < n);
        // level 2
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
        let mut run = a * idx.rs.stride;
        let mut s = idx.l1(a);
        // walk forward inside the run-start sample window (<= 64 runs, one block)
        while run + 1 < r {
            let l = self.run(run).1 as u64;
            if s + l > row {
                break;
            }
            s += l;
            run += 1;
        }
        (run, s)
    }
    /// Occurrences of `ch` in the BWT before run `k` (i.e. rows of `ch` in runs
    /// `[0,k)`).
    ///
    /// Sampling every `occ_stride`-th occurrence of `ch` plus a `run_gap`-run cap
    /// means the scan from the sample to `k` is at most one 64-run block, and the
    /// `stride == 1` fast path (the next sample IS the next run of `ch`, which is
    /// what every rare symbol gets) needs no scan at all.
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
            return c0 + self.run(r0).1 as u64;
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
        while run < k {
            // one block decode answers up to `block_runs` consecutive runs
            let (h, l) = self.run(run);
            if h == ch {
                acc += l as u64;
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
        if i == 0 {
            return 0;
        }
        if i >= self.idx.reader.n {
            return self.idx.total[ch as usize];
        }
        let (run, s) = self.run_of(i);
        let (h, _) = self.run(run);
        let base = self.cum_before(ch, run);
        if h == ch {
            base + (i - s)
        } else {
            base
        }
    }
    /// Half-open BWT interval of the pattern, consumed left to right.
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
            let end = s + l as u64;
            if pos == end - 1 && run % stripe == 0 {
                let anchor = self.idx.anchors[(run / stripe) as usize];
                return ((anchor + steps) % n, steps);
            }
            let lf = self.idx.c[h as usize] + self.cum_before(h, run);
            pos = lf + (pos - s);
            steps += 1;
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
                patterns = Some(args[i + 1].clone());
                i += 2;
            }
            "--patterns-fasta" if i + 1 < args.len() => {
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
}

fn read_patterns(path: &str, fasta: bool) -> Vec<Vec<u8>> {
    let data = std::fs::read(path).unwrap_or_else(|e| die(&format!("read {path}: {e}")));
    let mut out: Vec<Vec<u8>> = Vec::new();
    if fasta {
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
    } else {
        for line in data.split(|&b| b == b'\n') {
            let line = line.strip_suffix(b"\r").unwrap_or(line);
            if !line.is_empty() {
                out.push(line.to_vec());
            }
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
         mmap_ms={open_ms:.2} decoded_bytes=0 stages=none n={} runs={} members={} end_symbol=none",
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
    let patterns = read_patterns(&a.patterns, a.fasta);
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
                die(&format!("pattern of length {} is not 1..65536", pat.len()));
            }
            // Index coordinates: the container's text is the corpus with the
            // m7 remap applied, in corpus order; backward search consumes the
            // pattern right to left, so the working pattern is
            //     sigma(pattern) reversed.
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
            Hit {
                positions,
                search_us,
                locate_us,
                steps,
                max_steps,
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
        match (a.output.as_str(), a.counts) {
            ("tsv", true) => writeln!(w, "{}\t{}", pid, positions.len()).unwrap(),
            ("tsv", false) => {
                for p in positions {
                    writeln!(w, "{}\t{}", pid, p).unwrap();
                }
            }
            (_, _) => {
                let list: Vec<String> = positions.iter().map(|p| p.to_string()).collect();
                writeln!(
                    w,
                    "{{\"pattern_id\":{pid},\"count\":{},\"offsets\":[{}]}}",
                    positions.len(),
                    list.join(",")
                )
                .unwrap();
            }
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
                 steps={} max_steps={} total_us={:.1}",
                pid,
                patterns[pid].len(),
                hit.positions.len(),
                hit.search_us,
                hit.locate_us,
                hit.steps,
                hit.max_steps,
                us
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
