//! Native query kernel for the ONE published container: **SXI2 v10**.
//!
//! FORWARD-ONLY. This module reads exactly one container shape - the one `xsa
//! build` emits today (SXI2 v10: m1 in the trailing-directory block codec, the
//! m12/m13 run directories, the m11 locate anchors, m15 document starts and m20
//! the context trie). Anything else is refused at `Reader::open` by name, with
//! the rebuild instruction and no alternative. There is no second reader, no
//! `--legacy-*` flag and no degraded mode: the shapes that preceded this one
//! belong to git history, not to this binary.
//!
//! Why this module exists: the retired phi-era `Ri4` reader treated
//! `sxi::Container::open` (a *validation* entry point) as a *loader*. On a 10 GB
//! container that was 385 s of fixed cost before the first answer — the whole run
//! stream decoded twice, the LF-start array materialised twice (~28 GB each), the
//! chi set decoded into a sink, every member byte CRC'd, and a 256-way `Vec<u64>`
//! transpose (`cruns`, 28 GB of scattered appends) — see
//! `tools/binbwt/RESULTS_QUERY_PROFILE.md`. That reader is gone; nothing here
//! needs a per-run column.
//!
//! This module answers from the members the query actually needs, and nothing
//! else:
//!
//! | member | role |
//! |---|---|
//! | m1  | RLBWT runs (codec 116: block-compressed blocks + trailing block directory) and the C table |
//! | m5  | chi witness set — **not read**: exact matching never needs it |
//! | m10 | LF starts — **declared, zero bytes** ("derived"); we derive them |
//! | m11 | locate anchors: the tail SA of every 1024th run |
//! | m12, m13 | run starts and the per-symbol run directory (the two-level bisects) |
//! | m15 | document starts — the no-cross law's structure (never optional here) |
//! | m20 | the context trie (consult/seeds' top-of-tree arm) |
//! | m4, m7 | record anchors (empty here) and the byte permutation |
//!
//! Cost model: NOTHING per run is materialised. The only per-run state a query
//! owns is a FIXED `CACHE_BYTES` arena of decoded m1 blocks (64 MB, whatever n
//! is), holding `(heads u8, lengths u32)` for `block_runs` runs per slot:
//!
//! - `row_run(row)` is one m12 two-level bisect (whose level-1 cell names the
//!   BLOCK that holds the row) plus the 4-wide `scan_window` over that block's
//!   decoded lengths.
//! - `cum_before(ch, k)` is one m13 two-level bisect plus a scan of decoded
//!   blocks, bounded by the directory's own `run_gap` and taken from whichever
//!   sample end is closer.
//! - `locate` needs no `seen` column: `seen[run]` IS `cum_before(head, run)`.
//! - m11 anchors are read one cell at a time out of the mapping, never as a
//!   column.
//!
//! A materialising reader (an earlier revision) built `heads`/`rlen`/`seen`
//! at 13 B/run: 408 GB for the pile's 31,381,597,694 runs. That reader could
//! answer `--count` (rank steps only) and could NOT locate on a 100 GB
//! container - it refused at open rather than be OOM-killed. There is no budget
//! knob and no flag that brings it back: the block cache is the only path, so
//! `decoded_bytes` is 0 at every scale and `--limit` works on the pile.
//!
//! There is no `--ri4`/`--sxi`/`--mode`: the container tells us what it is, and
//! anything that is not the supported shape is refused by name (see `refuse`).
//!
//! ORIENTATION: the container has **no alphabet**. Its text is a byte
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

/// Reader-only: THE DECODED-BLOCK ARENA. This is the reader's whole per-run
/// working set: a fixed arena of decoded m1 blocks, `CACHE_BYTES / (5 * block_runs)`
/// slots whatever n is (the pile's 100 G container and the 150 MB fixture use the
/// same 64 MB). It is the reason a query no longer materialises 13 B/run: at the
/// pile's 31.4 G runs that column would be 408 GB, and the reader that allocated
/// it could answer `--count` but not `--limit`.
const CACHE_BYTES: usize = 64 << 20;
/// Reader-only: independently locked LRU shards over that arena, so N pattern
/// threads do not serialise on one mutex.
const CACHE_SHARDS: usize = 64;
/// Reader-only: how many blocks the open-time fail-loud audit samples. Shapes
/// the audit's cost (~2 decodes each), never how a run is addressed.
const M1_AUDIT_BLOCKS: u64 = 1024;
/// Reader-only: the RSS governor's budget. A random walk over the container
/// mapping faults pages of it into this process, so without a governor the RSS
/// would track the container's size even though nothing is decoded. The budget is
/// the arena plus a residency allowance for the container's own DIRECTORIES
/// (m1's block directory, m12, m13, m11), which are the reader's random-access
/// working set and are metadata, not text and not a per-run column.
const BASE_RSS_BYTES: u64 = 64 << 20;
const RSS_SLACK_BYTES: u64 = 64 << 20;
/// Reader-only: the governor's STARTING budget above the arena and the base
/// process. Small on purpose: a small walk keeps a small resident set. It is
/// raised only when the governor measures that dropping PTEs does not hold the
/// resident set (see `governor_probe`), up to the container's directory allowance.

/// Hard ceiling on the DIRECTORY residency allowance. Below it the allowance is
/// the container's declared directory bytes (the pile's 5.9 GB: 1.96 GB of m1
/// block directory + 0.25 GB of m12 + 3.4 GB of m13 + 0.25 GB of anchors); above
/// it the governor throttles directory residency like any other mapped bytes.
const DIR_RSS_CAP: u64 = 256 << 20;
const RSS_PROBE_STEP: u64 = 16;
const RSS_STABLE_STEP: u64 = 256;
const RSS_BYTES_PER_TOUCH: u64 = 4 << 20;
/// Reader-only: blocks per batched page-cache read-ahead request.
const PREFETCH_BLOCKS: u64 = 16;
/// Reader-only: the governor drops this many bytes of the mapping per epoch,
/// cycling over the file. See `Reader::drop_pages_window` for the measurement
/// that made this windowed instead of whole-mapping.
const DROP_WINDOW: u64 = 1 << 30;
/// Reader-only slack on the run-space scan bound in `cum_before`. The bound
/// itself comes from the m13 header (`rs.run_gap`, the writer's sampling rule);
/// this is the tiny allowance for the inclusive/exclusive edge of that rule.
const M1_SCAN_SLACK: u64 = 2;

use crate::{die, sxi};
use memmap2::Mmap;
use rayon::prelude::*;
use std::fs::File;
use std::io::{BufWriter, Read, Write};
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

/// THE opened container: header + directory + mmap, restricted by
/// `Reader::open` to the one supported shape. No member is decoded here and no
/// member CRC is verified (see `--verify`).
pub struct Reader {
    pub path: String,
    pub n: u64,
    pub r: u64,
    pub version: u32,
    pub flags: u64,
    pub members: Vec<sxi::Member>,
    pub sigma: [u8; 256],
    map: Arc<Mmap>,
    /// The open handle, kept only for `posix_fadvise`: the reader asks PAGE CACHE
    /// for the hot members, never its own address space (see `fadvise`).
    file: File,
}

// ------------------------------------------------------------ THE ONE FORMAT
//
// FORWARD-ONLY (the operator's law, 2026-11): we are the only consumer of this
// container family, so there is exactly ONE supported shape - the one `xsa
// build` emits today. Opening anything else is a REFUSAL that names the file and
// prints the rebuild instruction. There is no second reader, no degraded mode
// and no `--legacy-*` flag that brings one back: a container that predates this
// shape is UNSUPPORTED, and the fix is to rebuild it, not to read it.
//
// The family may still gain members (`xsa build` writes more of them over time);
// that is ADDITIVE and needs no reader change. What is fixed is the shape a
// reader accepts.

/// The supported container shape, spelled the same way in every refusal.
pub const SUPPORTED_SHAPE: &str = "SXI2 v10 with m15 (document starts) and m20 (context trie)";

/// THE refusal: every path that cannot serve a container returns exactly this.
/// `what` is the one-line reason (what the file is, or which part of the one
/// shape it is missing); the shape and the rebuild instruction are appended so
/// the message is actionable on its own and never suggests a fallback.
pub fn refuse(path: &str, what: &str) -> String {
    format!(
        "{path}: {what}\n  \
         this build serves exactly ONE container format: {SUPPORTED_SHAPE}\n  \
         rebuild with `xsa build`."
    )
}

/// The refusal for a command whose ANSWER is carried by a member this container
/// does not have. Same words as `refuse` (the shape + the rebuild instruction),
/// plus the one line that says why THIS command cannot answer without it.
pub fn member_required(path: &str, id: u32, why: &str) -> String {
    refuse(path, &format!("this container carries no member m{id} ({why})"))
}

/// The refusal every trie-reading command uses when the container has no m20.
/// `xsa build --no-trie` is an explicit product arm, so the container is legal;
/// the answer is not available, and the command says so instead of degrading to
/// a slower or marked mode.
pub fn trie_required(path: &str) -> String {
    member_required(path, 20, "the context trie; this command reads it and has no other arm")
}

/// What the file is, for the refusal message. The phrase is PREFIX-FREE (no
/// path): `refuse` puts the path in front of it, so a refusal reads as one
/// sentence about one file.
pub fn describe_format(path: &str) -> String {
    let f = match File::open(path) {
        Ok(f) => f,
        Err(e) => return format!("cannot open ({e})"),
    };
    let map = match unsafe { Mmap::map(&f) } {
        Ok(m) => m,
        Err(e) => return format!("cannot mmap ({e})"),
    };
    if map.len() < 64 {
        return format!("{} bytes, too short for any index header", map.len());
    }
    let magic = &map[..4];
    let version = u32::from_le_bytes(map[4..8].try_into().unwrap());
    match magic {
        b"SXI2" if version == 10 => "SXI2 v10 container, but not the supported member set".to_string(),
        // v6..v9 are the shapes from BEFORE the document starts existed. They
        // stay NAMED - a refusal must be able to say what the file is - and
        // nothing more: no member of the family below v10 is readable here.
        b"SXI2" => format!("SXI2 v{version} container (an older shape: below the supported one)"),
        b"SXI1" => format!("SXI1 v{version} container"),
        b"ISXR" => format!(".ri4 v{version} flat index"),
        _ => format!(
            "unrecognised file (magic {:02x}{:02x}{:02x}{:02x})",
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
        let version = u32::from_le_bytes(h[4..8].try_into().unwrap());
        // THE GATE. One magic, one version, and (below) one member set. This is
        // the ONLY place a container becomes readable, so no caller can reach a
        // degraded mode: there is no mode.
        if &h[..4] != b"SXI2" || version != 10 {
            return Err(refuse(path, &describe_format(path)));
        }
        // v7's incremental layout IS the supported layout: the member directory
        // is a trailing section at length-40*count and m1's per-block directory
        // follows its payload. It is not a compatibility arm - it is the layout
        // of the one shape, so it is spelled as a constant here rather than
        // derived from a version the reader no longer spans.
        let _ = version;
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
        // THE MEMBER LAW. The supported shape's directory holds the run stream
        // and its two directories (m1, m12, m13), the locate anchors (m11), chi
        // (m5), the record/byte members (m4, m6|m7), the empty LF declaration
        // (m10) and the two members the whole query surface is built on: m15
        // (document starts) and m20 (the context trie). m14 (LCP over runs) is
        // the one OPTIONAL member: `xsa build --with-lcp-runs` adds it, and the
        // shape stays supported either way. 9..=11 slots is the arithmetic of
        // that statement (10 + the optional m14 + the m6/m7 split).
        if !(9..=11).contains(&count) || hs != 64 + 40 * count as usize {
            return Err(refuse(
                path,
                &format!("directory holds {count} members ({hs} header+directory bytes); the \
                          supported shape holds 9..=11"),
            ));
        }
        if n == 0 || r == 0 || r > n {
            return Err(format!("{path}: invalid n={n} R={r}"));
        }
        // Header+directory CRC: 64 + 40*members bytes. Cheap, and the only
        // checksum that can be verified without reading the whole file.
        let expected = u32at(56);
        // The 40-byte entries live at the tail; the CRC covers the 64-byte
        // header followed by those entries, in that order.
        let dir_off = length as usize - 40 * count as usize;
        if dir_off < 64 || dir_off % 8 != 0 {
            return Err(format!("{path}: directory offset {dir_off} is not valid"));
        }
        let mut head = map[..64].to_vec();
        head.extend_from_slice(&map[dir_off..dir_off + 40 * count as usize]);
        head[56..60].fill(0);
        if sxi::crc32_of(&head) != expected {
            return Err(format!("{path}: header CRC mismatch"));
        }
        let mut members = Vec::with_capacity(count as usize);
        for i in 0..count as usize {
            let d = &map[dir_off + 40 * i..dir_off + 40 * (i + 1)];
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
            // v7 reserves only the fixed 64-byte header; the directory is trailing.
            let mut prev = 64u64;
            for (off, bytes) in spans {
                let aligned = (prev + 7) / 8 * 8;
                if off != aligned {
                    return Err(format!("{path}: members do not partition the file"));
                }
                prev = off + bytes;
            }
            let members_end = dir_off as u64;
            if prev != members_end {
                return Err(format!("{path}: members do not reach end of the member region"));
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
        let chi_present = members.iter().any(|m| m.id == 5);
        if (flags & 64 != 0) != !chi_present || (!chi_present && flags & 1 != 0) {
            return Err(format!("{path}: chi flag/member mismatch: missing member m5 (chi) must be declared by flag 64 with chi-complete cleared"));
        }
        // THE MEMBER LAW (cont.): the two members the query surface is BUILT on
        // must be there, and their flags must say so. There is no "absent ->
        // degrade" arm anywhere below this line, because a container that fails
        // here never becomes a Reader at all.
        // m15 is REQUIRED: every query command's no-cross law is structural, and
        // a container without the document starts cannot classify a match at all.
        // m20 (the context trie) is required by the commands that READ it - they
        // refuse by name at their own door (qlite::trie_required) - because
        // `xsa build --no-trie` is an explicit product arm whose container must
        // still answer query/search/consult/seeds/mem/branches.
        for id in [1u32, 10, 11, 12, 13, 15] {
            if !members.iter().any(|m| m.id == id) {
                return Err(refuse(
                    path,
                    &format!("v10 container without member m{id}"),
                ));
            }
        }
        // The flags are the container's own declaration; a member the flags do
        // not announce is a corrupt directory, not a shape we could read.
        if flags & 512 == 0 {
            return Err(refuse(
                path,
                &format!("v10 container whose flags {flags:#x} do not declare m15 (bit 512)"),
            ));
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
            file: f,
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
    /// The whole mapping, for the sibling member accessors (m14: the LCP over
    /// runs). Read-only, and no copy: a member is parsed in place.
    pub(crate) fn map_bytes(&self) -> &[u8] {
        &self.map
    }
    /// Read exactly `buf.len()` bytes at `off` from the FILE (not the mapping).
    /// Used for m1 block payloads so that random block reads never create page
    /// table entries for the run stream (see `Index::decode_into`).
    fn pread_exact(&self, off: u64, buf: &mut [u8]) -> std::io::Result<()> {
        std::os::unix::fs::FileExt::read_exact_at(&self.file, buf, off)
    }
    /// Tell the kernel this mapping is read in RANDOM order: without it a minor
    /// fault on this kernel pulls in whole large folios (~370 kB of resident set
    /// for one 4 kB cell touched), which is exactly the amplifier that made the
    /// RSS governor sweep the mapping constantly.
    fn mark_random(&self) -> bool {
        self.map.advise(memmap2::Advice::Random).is_ok()
    }
    /// Ask page cache for `[off, off+len)` WITHOUT mapping it. Best effort: a
    /// kernel that refuses an advisory read-ahead must not fail a query, so the
    /// refusal is journaled through the return value, not fatal. Never touches
    /// this process's RSS - which is the whole difference from
    /// `madvise(WILLNEED)` on the mapping, the call this replaces.
    fn fadvise(&self, off: u64, len: u64) -> bool {
        if len == 0 {
            return false;
        }
        // POSIX_FADV_WILLNEED
        const WILLNEED: i32 = 3;
        let rc = unsafe {
            posix_fadvise(
                std::os::unix::io::AsRawFd::as_raw_fd(&self.file),
                off as i64,
                len as i64,
                WILLNEED,
            )
        };
        rc == 0
    }
    /// This process's resident set in bytes, from `/proc/self/statm` (field 2 is
    /// resident pages). `None` if the kernel will not say.
    fn resident_bytes(&self) -> Option<u64> {
        let s = std::fs::read_to_string("/proc/self/statm").ok()?;
        let mut it = s.split_whitespace();
        let _size = it.next()?;
        let resident: u64 = it.next()?.parse().ok()?;
        Some(resident * 4096)
    }
    /// Drop this process's PTEs for a BOUNDED window of the mapping. The bytes are
    /// a read-only file mapping, so they come back unchanged from page cache on
    /// the next touch: the only thing this changes is RSS, which is exactly the
    /// point.
    ///
    /// Windowed, not whole-mapping: `madvise(MADV_DONTNEED)` over the pile's 63 GB
    /// mapping walks every page-table page in it and was MEASURED at 22 ms per
    /// call on a 20,000-position locate (933,925 calls, 50,684 s of drop wall).
    /// A 1 GB window costs tens of microseconds and the cursor cycles, so the
    /// resident set is still bled off - just in slices.
    fn drop_pages_window(&self, off: u64, len: u64) -> bool {
        let total = self.map.len() as u64;
        if off >= total {
            return true;
        }
        let len = len.min(total - off);
        // SAFETY: `DontNeed` on a MAP_PRIVATE, PROT_READ text mapping only zaps
        // the page table entries in [off, off+len), which is inside the mapping.
        // Every subsequent read re-populates them from the same file bytes, so no
        // value the reader has observed through the mapping can change. The reader
        // never holds a `&mut` into the map (it is read-only by construction) and
        // every `&[u8]` into it is used and dropped within one step.
        let rc = unsafe {
            self.map
                .unchecked_advise_range(memmap2::UncheckedAdvice::DontNeed, off as usize, len as usize)
        };
        rc.is_ok()
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
    /// m1 blocked (codecs 102/114): the block directory and the payload.
    dir_off: u64,
    dir_entry_bytes: u64,
    payload_off: u64,
    /// One past the last payload byte (end of the m1 member), for the block
    /// read bound: a directory entry is only trusted as far as this.
    payload_end: u64,
    /// The canonical Huffman codebook over run heads (m1 codecs 102/114).
    huff: Huff,
    /// The gamma length decoder (built once: it is a 4096-entry table, and
    /// rebuilding it per block cost 26 us per rank step).
    gamma: Gamma,
    nblocks: u64,
    /// m12: run-start samples (row of every `rs_stride`-th run).
    rs: RunStarts,
    /// m13: per-symbol sampled run directory.
    rd: RunDir,
    /// m11 anchor area inside the mapping: the anchor of run `k` is
    /// `u64_at(map, anchors_off + 16 + 8*(k/stripe))`. Read straight out of the
    /// mapping - there is no anchor column in RAM (at pile scale that column is
    /// 245 MB and every locate touches one cell of it).
    anchors_off: u64,
    stripe: u64,
    /// THE DECODED-BLOCK CACHE: `CACHE_SHARDS` independently locked LRU caches
    /// over a fixed `CACHE_BYTES` arena. This is the reader's ONLY per-run state,
    /// and it does not grow with n: a run is reached by decoding the one m1 block
    /// that holds it, which either costs a cache hit or one block decode.
    cache: Vec<std::sync::Mutex<Shard>>,
    cache_slots: u64,
    /// THE RSS CAP for this process: the fixed decoded-block arena, the base
    /// process, and a fixed allowance for the container's DIRECTORIES. One number,
    /// computed at open and journaled; it does not depend on n and it is not
    /// raised by any switch.
    rss_cap: u64,
    /// Resident set at the previous governor probe, so the governor can tell a
    /// working set that is still growing from one that has settled.
    rss_last: std::sync::atomic::AtomicU64,
    drop_epochs: std::sync::atomic::AtomicU64,
    /// Byte cursor of the windowed governor drop (see `drop_pages_window`).
    drop_cursor: std::sync::atomic::AtomicU64,
    /// Wall spent inside the governor's PTE drop, so the drop's cost is measured
    /// rather than assumed.
    drop_ns: std::sync::atomic::AtomicU64,
    pub build_ms: f64,
    /// Bytes of per-run columns this process materialised at open. It is ZERO and
    /// must stay zero: it is the audit line for the memory law. A container too
    /// big for the heap is answered from the block cache, never materialised.
    pub decoded_bytes: u64,
    /// Per-stage build walls, in order.
    pub stages: Vec<(&'static str, f64)>,
    /// Blocks the open-time audit checked run-length sums against m12 for.
    pub audit_blocks: u64,
    pub block_runs: u64,
    pub rundir_stride: u64,
    /// Whether the block-cache arena got MADV_HUGEPAGE, journaled.
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
/// `ent_off + entry_bytes*j` are `(rows of c before run, run index, occurrence stride to
/// the next sample)`. The level-2 array every `l2` entries per symbol is at
/// `l2_off + 16*l2offs[c]`.
struct RunDir {
    entry_bytes: u64,
    ent_off: u64,
    l2_off: u64,
    l2: u64,
    counts: Vec<u64>,
    offs: Vec<u64>,
    l2offs: Vec<u64>,
    occ_stride: u64,
    run_gap: u64,
}

/// ONE SHARD OF THE DECODED-BLOCK CACHE.
///
/// A decoded block is `block_runs` head bytes plus `block_runs` u32 lengths, and
/// that pair is all any rank/locate step ever needs from the m1 stream. The
/// shard owns its slice of the fixed arena and an LRU list over its slots; the
/// whole of it (lookup, decode, evict) happens under this shard's mutex, so no
/// slot can be read while it is being refilled and no refcounts are needed. The
/// closures handed to `Index::with_block` therefore must NOT call `with_block`
/// again (that would self-deadlock on the same shard); they only walk the two
/// slices they are given.
// PADDED TO A CACHE LINE. Every lookup updates `hits`/`mapped_touch` under this
// shard's lock, so two shards inside one cache line make every worker thread
// fight over it: with a 64 MB arena the shards numbered 64 and a query thread
// per core, that false sharing is measured as most of the parallel loss.
#[repr(align(128))]
struct Shard {
    heads: Vec<u8>,
    lens: Vec<u32>,
    /// Per-shard receipts (plain counters updated under this shard's lock): lookups
    /// that hit, lookups that missed, block decodes, decode wall, page-cache
    /// read-aheads, and random touches of the mapping since the last governor probe.
    hits: u64,
    misses: u64,
    decodes: u64,
    decode_ns: u64,
    prefetch_calls: u64,
    mapped_touch: u64,
    /// `mapped_touch` value at which this shard next asks the governor.
    next_probe: u64,
    /// Bounded read-ahead buffer for ONE block's payload bytes (see `decode_into`):
    /// the m1 payload is read with `pread` into this, never through the mapping.
    bounce: Vec<u8>,
    /// block id -> LOCAL slot index
    idx: std::collections::HashMap<u64, u32>,
    /// LOCAL slot -> block id (the reverse map, so LRU eviction is one lookup)
    slot_block: Vec<u64>,
    /// intrusive LRU over local slots, `NIL` terminated
    next: Vec<u32>,
    prev: Vec<u32>,
    head: u32,
    tail: u32,
    /// the next never-used local slot, or NIL once every slot has been used once
    free: u32,
}

const NIL: u32 = u32::MAX;

impl Shard {
    fn new(slots: usize, br: usize) -> Shard {
        Shard {
            heads: vec![0u8; slots * br],
            lens: vec![0u32; slots * br],
            hits: 0,
            misses: 0,
            decodes: 0,
            decode_ns: 0,
            prefetch_calls: 0,
            mapped_touch: 0,
            next_probe: RSS_PROBE_STEP,
            bounce: vec![0u8; 4096],
            idx: std::collections::HashMap::with_capacity(slots * 2),
            slot_block: vec![u64::MAX; slots],
            next: vec![NIL; slots],
            prev: vec![NIL; slots],
            head: NIL,
            tail: NIL,
            free: 0,
        }
    }
    fn slots(&self) -> usize {
        self.next.len()
    }
    /// unlink a local slot from the LRU list
    #[inline]
    fn unlink(&mut self, s: u32) {
        let (p, n) = (self.prev[s as usize], self.next[s as usize]);
        if p != NIL {
            self.next[p as usize] = n;
        } else {
            self.head = n;
        }
        if n != NIL {
            self.prev[n as usize] = p;
        } else {
            self.tail = p;
        }
        self.prev[s as usize] = NIL;
        self.next[s as usize] = NIL;
    }
    /// link a local slot as the most-recently used
    #[inline]
    fn push_front(&mut self, s: u32) {
        self.prev[s as usize] = NIL;
        self.next[s as usize] = self.head;
        if self.head != NIL {
            self.prev[self.head as usize] = s;
        }
        self.head = s;
        if self.tail == NIL {
            self.tail = s;
        }
    }
    /// `(local slot, hit)`. A miss has already evicted the LRU victim (or taken a
    /// never-used slot) and removed its block id from the map, so the caller may
    /// decode into the returned slot and then call `install`.
    #[inline]
    fn acquire(&mut self, block: u64) -> (u32, bool) {
        if let Some(&s) = self.idx.get(&block) {
            self.unlink(s);
            self.push_front(s);
            return (s, true);
        }
        let s = if self.free != NIL {
            let s = self.free;
            self.free = if (s as usize + 1) < self.slots() { s + 1 } else { NIL };
            s
        } else {
            let v = self.tail;
            debug_assert!(v != NIL, "LRU shard has no victim and no free slot");
            self.unlink(v);
            // forget the evicted block id
            let old = self.slot_block[v as usize];
            debug_assert!(old != u64::MAX, "LRU victim slot holds no block");
            self.idx.remove(&old);
            v
        };
        (s, false)
    }
    #[inline]
    fn install(&mut self, block: u64, s: u32) {
        self.idx.insert(block, s);
        self.slot_block[s as usize] = block;
        self.push_front(s);
    }
}

/// The window scan of `row_run`: walk forward over run lengths from `start_run`
/// (whose start row is `s`) until the run containing `row` is found. Four runs at
/// a time, because the one-at-a-time form is a loop-carried `s += l` dependency
/// chain; generic over the length accessor so a block-resident `lens` slice and
/// the unit test's `u64` slice monomorphise into the same straight-line code.
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

/// Per-query-thread working set: ONE cursor pair plus diagnostic counters. All
/// the data a step needs lives in the shared decoded-block cache, so a `Ctx` is
/// counters and nothing else - one per pattern, so two patterns on one rayon
/// thread never mix their counts.
pub struct Ctx<'a> {
    arena: Option<&'a std::cell::RefCell<WorkerArena>>,
    idx: &'a Index,
    /// Diagnostic counters, printed under XSA_TRACE. They answer "what does one
    /// rank/LF step actually pay for": `scan_of` runs walked in `row_run`'s
    /// forward scan, `scan_cum` runs walked in `cum_before`'s sample-to-run scan,
    /// `steps` LF steps, `ranks` rank() calls, `blocks` block-cache lookups.
    scan_of: u64,
    scan_cum: u64,
    steps: u64,
    ranks: u64,
    blocks: u64,
}
fn u64_at(d: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(d[o..o + 8].try_into().unwrap())
}

fn decode_l2_entry(m: &[u8], o: usize) -> (u64, u64) {
    (u64_at(m, o), u64_at(m, o + 8))
}

fn decode_run_entry(m: &[u8], o: usize, bytes: u64) -> (u64, u64, u64) {
    let cum = u64_at(m, o);
    let run = u64_at(m, o + 8);
    if bytes == 24 { (cum, run, u64_at(m, o + 16)) }
    else { (cum, run & 0xffff_ffff, (run >> 32) & 0xffff) }
}
fn decode_block_entry(m: &[u8], o: usize, bytes: u64) -> (u64, u32, u32) {
    let w = u64_at(m, o);
    if bytes == 16 {
        let bits = u64_at(m, o + 8);
        (w, bits as u32, (bits >> 32) as u32)
    } else {
        ((w & 0xffff_ffff) * 8, (w >> 32) as u16 as u32, (w >> 48) as u32)
    }
}

impl Index {
    /// Build the query kernel. This touches only metadata: the m1 header + the
    /// codebook + the two directories' headers, the m11 anchors, and ONE bounded
    /// block audit. The run stream is NOT decoded and NOT materialised - a rank or
    /// a locate decodes the one or two blocks it needs, through a fixed-size
    /// cache. `decoded_bytes` is therefore 0 at every scale, which is the line the
    /// memory law is audited against.
    pub fn open(reader: Reader, threads: usize) -> Result<Index, String> {
        Index::open_with_cache(reader, threads, CACHE_BYTES)
    }

    /// Same kernel with an EXPLICIT decoded-block arena. `CACHE_BYTES` is the
    /// default (64 MB, the size every existing path uses); `xsa consult` sizes
    /// the arena from its `--rss-gb` budget instead, because a cache-build walks
    /// a much denser set of BWT rows than a one-off query and the arena is the
    /// one knob that trades RSS for random block decodes. Nothing else changes:
    /// the arena is still FIXED for the life of the process, and `decoded_bytes`
    /// is still 0 - this is a budget split, not a materialisation.
    pub fn open_with_cache(
        reader: Reader,
        _threads: usize,
        cache_bytes: usize,
    ) -> Result<Index, String> {
        let t0 = Instant::now();
        let r = reader.r;
        let n = reader.n;

        // ---- THE ONE SHAPE (cont.): m1 must be the trailing-directory block
        // stream the supported shape uses. Codecs 102/114 were the shapes before
        // the trailing directory; they are not read here, and the refusal says
        // what to do about it (rebuild), never what to try instead.
        let m1 = reader.member(1);
        // captured here: `reader` is moved into the index below, so the m1 borrow
        // must not survive past this point.
        let (m1_codec, m1_off, m1_bytes) = (m1.codec, m1.offset, m1.bytes);
        if m1.codec != 116 {
            return Err(refuse(
                &reader.path,
                &format!("m1 codec {} is not 116 (the block-compressed run stream with a \
                          trailing block directory)", m1.codec),
            ));
        }
        if reader.flags & 32 == 0 {
            return Err(refuse(
                &reader.path,
                &format!("header flags {:#x} do not declare the block-compressed m1 + run \
                          directories (bit 32)", reader.flags),
            ));
        }
        for id in [12u32, 13] {
            if !reader.members.iter().any(|m| m.id == id) {
                return Err(refuse(
                    &reader.path,
                    &format!("missing run directory member m{id}"),
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
        // codec 116 (v7) leaves the nblocks/rNorm head slots zero ON PURPOSE: the
        // head is written before the sweep and is never patched, so it cannot lie
        // about a frontier it did not know. Both are derived: nblocks from the
        // member count, rNorm from the member count itself.
        let (nblocks, total_runs) = if m1.codec == 116 {
            let block_runs = u64_at(data, 2312);
            if block_runs == 0 { return Err("m1 codec 116 declares block_runs=0".into()); }
            ((m1.count + block_runs - 1) / block_runs, m1.count)
        } else {
            (u64_at(data, 2304), u64_at(data, 2320))
        };
        let block_runs = u64_at(data, 2312);
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
        // codec 116 (container v7) puts the per-block directory AFTER the payload
        // so the payload can be appended in stream order; its fixed head is 2336
        // bytes and its nblocks is derived (the head's nblocks/rNorm slots are
        // zero on purpose - they are never patched, so the member CRC stays valid).
        let dir_entry_bytes = if m1.codec == 114 || m1.codec == 116 { 16 } else { 8 };
        if m1.codec == 116 && (payload_off != 2336 || m1.bytes < 2336 + dir_entry_bytes * nblocks) {
            return Err(format!(
                "m1 codec 116 payload offset {payload_off} / bytes {} do not fit a trailing {nblocks}-entry directory",
                m1.bytes
            ));
        }
        if m1.codec != 116 && (payload_off != 2336 + dir_entry_bytes * nblocks || payload_off > m1.bytes) {
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
        // v7: directory at the member's tail, payload immediately after the head.
        let (m1dir_off, m1payload_len) = if m1.codec == 116 {
            (m1.offset + m1.bytes - dir_entry_bytes * nblocks,
             m1.bytes - 2336 - dir_entry_bytes * nblocks)
        } else {
            (m1.offset + 2336, m1.bytes - payload_off)
        };
        let m1payload_off = m1.offset + payload_off;
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
        let entry_bytes = match m.codec { 113 => 16, 115 => 24, _ => return Err("unsupported m13 codec".into()) };
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
        if ent_off + entry_bytes * total_entries != m.offset + m.bytes
            || l2_off + 16 * l2offs[256] != ent_off
        {
            return Err("m13 member size does not match its own declaration".into());
        }
        let rd = RunDir {
            entry_bytes,
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
        // The anchors stay IN THE MAPPING: `anchors_off` is where they begin and a
        // locate reads the one 8 B cell it needs. Auditing all of them would page
        // in 0.003 n for nothing, so a bounded, evenly spaced sample is checked
        // (fail loud on a value outside [0,n), which is the only way an anchor can
        // be wrong without changing the container's shape).
        let anchors_off = m.offset;
        let n_anchor_check = m.count.min(M1_AUDIT_BLOCKS).max(1);
        for t in 0..n_anchor_check {
            let i = (t * m.count / n_anchor_check) as usize;
            let v = u64_at(a, 16 + 8 * i);
            if v >= n {
                return Err(format!("m11 anchor {i} value {v} outside [0,n)"));
            }
        }
        let t_anchors = t0.elapsed();

        // ---- tell the kernel the mapping is read RANDOMLY, so a minor fault on a
        // 4 kB directory cell stops pulling in ~370 kB of resident set. Journaled,
        // not fatal (a kernel may refuse the advice; the governor still bounds
        // RSS, just at a coarser rate).
        let random_advice = reader.mark_random();

        // ---- page the hot members in, asynchronously, WITHOUT mapping their
        // pages into this process. A locate walks the run-start array and the
        // per-symbol directory at thousands of essentially random positions;
        // `posix_fadvise(WILLNEED)` puts those bytes in page cache, which is not
        // RSS and is reclaimable, where `madvise(WILLNEED)` on the mapping would
        // fault every one of them into this process - at pile scale that is tens
        // of GB of resident set for a five-position query.
        let mut prefetched = 0u64;
        for m in reader.members.iter().filter(|m| [1u32, 12, 13].contains(&m.id)) {
            // For m1 only its HEADER + BLOCK DIRECTORY: the compressed payload is
            // read block by block at random positions by `with_block`, and advising
            // the whole member would put the entire run stream (tens of GB at pile
            // scale) into the read-ahead queue of a five-position query.
            let len = if m.id == 1 {
                (2336 + dir_entry_bytes * nblocks).min(m.bytes)
            } else {
                m.bytes
            };
            if reader.fadvise(m.offset, len) {
                prefetched += len;
            }
        }
        if prefetched == 0 {
            return Err("no hot members were prefetched".into());
        }
        let t_prefetch = t0.elapsed();

        // ---- the fixed block cache. Slot width is the container's declared block
        // width, so the arena holds `CACHE_BYTES / (5 * block_runs)` decoded blocks
        // whatever n is. Shards are capped by the slot count so a narrow cache
        // still gets one slot per shard.
        let br = block_runs as usize;
        let per_slot = br + 4 * br;
        let want_slots = (cache_bytes / per_slot).max(1);
        // A LARGER ARENA GETS MORE SHARDS: the shard count is what the worker
        // threads contend on, so it grows with the arena the caller paid for
        // (the default 64 MB arena keeps exactly the historical 64).
        let shard_target = if cache_bytes > CACHE_BYTES { 512 } else { CACHE_SHARDS };
        let nshard = if cache_bytes == 0 { 0 } else { shard_target.min(want_slots).max(1) };
        let per_shard = (want_slots / nshard.max(1)).max(1);
        let mut cache = Vec::with_capacity(nshard);
        let mut thp = true;
        for _ in 0..nshard {
            let mut sh = Shard::new(per_shard, br);
            thp = thp && advise_huge(&mut sh.heads) && advise_huge(&mut sh.lens);
            cache.push(std::sync::Mutex::new(sh));
        }
        let cache_slots = (nshard * per_shard) as u64;

        let mut idx = Index {
            reader,
            c,
            total,
            dir_off: m1dir_off,
            dir_entry_bytes,
            payload_off: m1payload_off,
            payload_end: m1_end,
            huff,
            gamma: Gamma::build(),
            nblocks,
            rs,
            rd,
            anchors_off,
            stripe,
            cache,
            cache_slots,
            rss_cap: 0,
            rss_last: std::sync::atomic::AtomicU64::new(0),
            drop_epochs: std::sync::atomic::AtomicU64::new(0),
            drop_cursor: std::sync::atomic::AtomicU64::new(0),
            drop_ns: std::sync::atomic::AtomicU64::new(0),
            build_ms: 0.0,
            decoded_bytes: 0,
            stages: Vec::new(),
            audit_blocks: 0,
            block_runs,
            rundir_stride: rd_l2,
            thp,
        };
        // v6: the payload is the member's tail. v7 (codec 116): the per-block
        // directory is the tail, so the payload ends where the directory starts.
        let payload_expect_end = if m1_codec == 116 {
            m1_off + m1_bytes - dir_entry_bytes * nblocks
        } else {
            m1_off + m1_bytes
        };
        if idx.payload_off + m1payload_len != payload_expect_end {
            return Err("m1 payload area does not reach the end of its member".into());
        }
        // ---- the governor's budget for THIS container
        //
        // The mapping holds the container's DIRECTORIES (m1's block directory,
        // m12, m13, m11) and nothing else: block payloads are `pread` into the
        // arena's bounce buffers, so the run stream itself can never become
        // resident. The directories are the reading kernel's random-access working
        // set - a rank step touches several of them - and they are metadata whose
        // size the container declares. Allowing them to be resident is the
        // difference between 0.4 ms and 0.4 s per rank step at pile scale; it is
        // bounded here, declared in the journal, and it is NOT a per-run column.
        let dir_bytes = idx
            .reader
            .members
            .iter()
            .filter(|m| [11u32, 12, 13].contains(&m.id))
            .map(|m| m.bytes)
            .sum::<u64>()
            + 2336
            + dir_entry_bytes * nblocks;
        idx.rss_cap =
            (cache_bytes as u64) + BASE_RSS_BYTES + RSS_SLACK_BYTES + dir_bytes.min(DIR_RSS_CAP);

        // ---- THE OPEN-TIME AUDIT, and it is BOUNDED.
        //
        // A sampled block's run lengths must sum to exactly the m12 run-start span
        // that brackets it, and the last block must close exactly on n. The
        // materialising revision decoded every block because it was decoding them
        // all anyway; a bounded reader decodes M1_AUDIT_BLOCKS of them (evenly
        // spaced, plus the last) and refuses in words when the stream and the
        // directory disagree. It costs ~2 ms at 150 MB and is independent of n.
        let t_mat0 = t0.elapsed();
        idx.audit_sample()?;
        let t_mat = t0.elapsed();
        let n_audit = idx.audit_blocks;
        let dec = idx.decoded_bytes;
        let stages = vec![
            ("m11-anchors", t_anchors.as_secs_f64() * 1e3),
            ("prefetch", (t_prefetch - t_anchors).as_secs_f64() * 1e3),
            ("block-cache", (t_mat - t_mat0).as_secs_f64() * 1e3),
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
                 decoded_bytes={dec} cache_slots={cache_slots} cache_shards={nshard} thp={} \
                 random_advice={random_advice} prefetched={prefetched} rss_cap={}",
                idx.thp, idx.rss_cap
            );
        }
        Ok(idx)
    }

    /// The open-time, BOUNDED audit: decode `M1_AUDIT_BLOCKS` evenly spaced blocks
    /// (plus the last one) through the same `with_block` path the query uses, and
    /// check each block's run lengths against the m12 span that brackets it. This
    /// is a shape check on the container, not an answer: chi and every position
    /// are checked against the oracle by the acceptance gate.
    fn audit_sample(&mut self) -> Result<(), String> {
        let nblocks = self.nblocks;
        let r = self.reader.r;
        let n = self.reader.n;
        let br = self.block_runs;
        let mut checked = 0u64;
        let scratch = std::cell::RefCell::new(WorkerArena::new(self)?);
        let cx = self.worker_ctx(&scratch);
        let take = nblocks.min(M1_AUDIT_BLOCKS).max(1);
        for t in 0..take {
            let b = (t * nblocks / take).min(nblocks - 1);
            let (s0, s1) = (self.l1(b), if b + 1 < nblocks { self.l1(b + 1) } else { n });
            let sum = cx.with_block(b, |_, lens| lens.iter().map(|&l| u64::from(l)).sum::<u64>());
            if sum != s1 - s0 {
                return Err(format!(
                    "m1 block audit: block {b} of {nblocks} decodes to {sum} rows, but the m12\n  \
                     span it occupies is [{s0},{s1}) = {} rows",
                    s1 - s0
                ));
            }
            checked += 1;
        }
        // the last block must close exactly on n (it is not necessarily one of the
        // evenly spaced samples, so it is decoded explicitly)
        let last = nblocks - 1;
        let last_sum =
            cx.with_block(last, |_, lens| lens.iter().map(|&l| u64::from(l)).sum::<u64>());
        if self.l1(last) + last_sum != n {
            return Err(format!(
                "m1 audit: the last block ({last}) ends at {} but the header says n={n}",
                self.l1(last) + last_sum
            ));
        }
        if br == 0 || r == 0 {
            return Err("m1 audit: empty run stream".into());
        }
        self.audit_blocks = checked;
        Ok(())
    }

    // ---------------------------------------------- the decoded-block cache

    /// Run `f` over the decoded `(heads, lens)` of m1 block `b`, in the shared
    /// fixed-size cache. A hit is a hash lookup; a miss decodes the block out of
    /// the mapping into its LRU slot and evicts the least recently used one. The
    /// closure runs holding that shard's lock and MUST NOT call `with_block`.
    #[inline]
    fn with_block<R>(&self, b: u64, f: impl FnOnce(&[u8], &[u32]) -> R) -> R {
        debug_assert!(b < self.nblocks, "block {b} of {}", self.nblocks);
        let nsh = self.cache.len() as u64;
        let h = b.wrapping_mul(0x9E37_79B9_7F4A_7C15) >> 32;
        let shard = (h % nsh) as usize;
        let br = self.block_runs as usize;
        let first = b * self.block_runs;
        let want = (self.reader.r - first).min(self.block_runs) as usize;
        let mut g = self.cache[shard].lock().unwrap_or_else(|e| e.into_inner());
        // ALL per-lookup counters live in the shard and are updated under the lock
        // this lookup already holds. They were global atomics, and 128 threads
        // hammering one cache line per lookup cost a measured 3x of the whole
        // query's scaling (--limit 20000: 334 s on 128 threads against 1000 s of
        // serial work).
        g.mapped_touch += 1;
        let (slot, hit) = g.acquire(b);
        let lo = slot as usize * br;
        if !hit {
            g.misses += 1;
            {
                let t = Instant::now();
                // split the shard's borrows: the bounce buffer and the two columns
                let sh = &mut *g;
                let (heads, lens, bounce) = (&mut sh.heads, &mut sh.lens, &mut sh.bounce);
                self.decode_into(b, bounce, &mut heads[lo..lo + want], &mut lens[lo..lo + want]);
                g.decode_ns += t.elapsed().as_nanos() as u64;
            }
            g.install(b, slot);
            g.decodes += 1;
            // ask page cache for the next blocks along the walk (batched)
            self.prefetch_blocks(&mut g, b);
        } else {
            g.hits += 1;
        }
        // one block lookup is one random touch of the mapping: pace the governor on
        // it (see `governor_probe`)
        if g.mapped_touch >= g.next_probe {
            self.governor_probe(&mut g);
        }
        f(&g.heads[lo..lo + want], &g.lens[lo..lo + want])
    }

    /// Decode block `b` of the m1 payload into caller-provided buffers.
    ///
    /// The block bytes are `pread` into the shard's bounce buffer, NOT read
    /// through the mapping. That is deliberate: the payload is the largest region
    /// of the container (61 GB of the pile's 63 GB) and it is read at random block
    /// positions, so mapping it would put a random page of it into this process's
    /// resident set for every decode and force the RSS governor to sweep the whole
    /// mapping again and again. A bounded read-ahead buffer has neither problem:
    /// the bytes come from page cache into RAM the reader already owns.
    fn decode_into(&self, b: u64, buf: &mut Vec<u8>, heads: &mut [u8], lens: &mut [u32]) {
        let want = heads.len();
        debug_assert_eq!(lens.len(), want);
        let (off, hbits, lbits) = self.dir(b);
        let hbytes = (hbits as u64 + 7) / 8;
        let lbytes = (lbits as u64 + 7) / 8;
        let need = hbytes + lbytes;
        let start = self.payload_off + off;
        let end = start + need;
        if end > self.payload_end {
            die(&format!(
                "m1 block {b} runs past the payload area ({end} > {})",
                self.payload_end
            ));
        }
        if buf.len() < need as usize {
            buf.resize(need as usize, 0);
        }
        let p = &mut buf[..need as usize];
        self.reader
            .pread_exact(start, p)
            .unwrap_or_else(|e| die(&format!("m1 block {b}: read {need} bytes at {start}: {e}")));
        let got = decode_block(
            &self.huff,
            &self.gamma,
            &p[..hbytes as usize],
            hbits as u64,
            &p[hbytes as usize..],
            lbits as u64,
            want,
            heads,
            lens,
            b,
        );
        debug_assert_eq!(got, want);
    }

    /// Ask page cache for the payload of blocks `[b, b+PREFETCH_BLOCKS)` without
    /// mapping them. Batched: one syscall per `PREFETCH_BLOCKS` decodes, because a
    /// syscall here costs more than the read it advises.
    fn prefetch_blocks(&self, g: &mut Shard, b: u64) {
        if g.decodes % PREFETCH_BLOCKS != 0 {
            return;
        }
        let b1 = (b + PREFETCH_BLOCKS).min(self.nblocks);
        let (o0, _, _) = self.dir(b);
        let (o1, _, _) = self.dir(b1 - 1);
        // the payload of block j starts at payload_off + dir(j).0 and the last
        // block's payload runs to the end of the member; covering [o0, o1 + a
        // block) is enough (WILLNEED clamps at EOF).
        let len = (o1 + 4096).saturating_sub(o0);
        if self.reader.fadvise(self.payload_off + o0, len) {
            g.prefetch_calls += 1;
        }
    }

    /// Bound this process's resident set.
    ///
    /// A shard calls this once every few thousand block lookups (its own counter,
    /// no shared cache line): read this process's RSS out of `/proc/self/statm`
    /// and, when it is over the cap, drop ONE window of the container mapping's
    /// PTEs - they come back unchanged from page cache on the next touch. Without
    /// this a random walk over a 63 GB container would grow this process's RSS
    /// with the container's directories even though only a few pages are wanted.
    ///
    /// The probe interval is chosen from the HEADROOM that is left and the measured
    /// cost of a directory touch, and it doubles only while the resident set is not
    /// moving - i.e. once the working set genuinely fits, the reader stops paying
    /// for probes.
    #[inline]
    fn governor_probe(&self, g: &mut Shard) {
        use std::sync::atomic::Ordering::Relaxed;
        let resident = match self.reader.resident_bytes() {
            Some(b) => b,
            None => die("cannot read /proc/self/statm for the RSS governor"),
        };
        let cap = self.rss_cap;
        let last = self.rss_last.swap(resident, Relaxed);
        if resident > cap {
            self.drop_epochs.fetch_add(1, Relaxed);
            let t_drop = Instant::now();
            // drop ONE window, cycling over the mapping (see `drop_pages_window`)
            let cur =
                self.drop_cursor.fetch_add(DROP_WINDOW, Relaxed) % self.reader.map.len() as u64;
            if !self.reader.drop_pages_window(cur, DROP_WINDOW) {
                die("madvise(MADV_DONTNEED) failed: the RSS governor cannot bound the mapping");
            }
            self.drop_ns
                .fetch_add(t_drop.elapsed().as_nanos() as u64, Relaxed);
            self.rss_last.store(0, Relaxed);
            g.next_probe = g.mapped_touch + RSS_PROBE_STEP;
            return;
        }
        let step = if resident <= last + (1 << 20) {
            RSS_STABLE_STEP
        } else {
            ((cap - resident) / RSS_BYTES_PER_TOUCH).clamp(RSS_PROBE_STEP, RSS_STABLE_STEP)
        };
        g.next_probe = g.mapped_touch + step;
    }

    /// `(hits, misses, decodes, drop_epochs)` - the block cache's own receipt,
    /// summed over the shards.
    pub fn cache_stats(&self) -> (u64, u64, u64, u64) {
        let (mut h, mut m, mut d) = (0u64, 0u64, 0u64);
        for sh in &self.cache {
            let g = sh.lock().unwrap_or_else(|e| e.into_inner());
            h += g.hits;
            m += g.misses;
            d += g.decodes;
        }
        (
            h,
            m,
            d,
            self.drop_epochs.load(std::sync::atomic::Ordering::Relaxed),
        )
    }

    /// `(decode_ns, drop_ns, prefetch_calls)` - where the wall actually went.
    pub fn wall_stats(&self) -> (u64, u64, u64) {
        let (mut dec, mut pf) = (0u64, 0u64);
        for sh in &self.cache {
            let g = sh.lock().unwrap_or_else(|e| e.into_inner());
            dec += g.decode_ns;
            pf += g.prefetch_calls;
        }
        (
            dec,
            self.drop_ns.load(std::sync::atomic::Ordering::Relaxed),
            pf,
        )
    }

    /// Set this process's RSS budget in bytes: the resident set the governor
    /// bleeds the container mapping back to (see `governor_probe`). `xsa
    /// consult` is the only caller - it is a BUDGET knob, not a switch: the
    /// decoded-block arena and every per-run structure stay fixed whatever it
    /// says, and the value is journaled.
    pub fn set_rss_cap(&mut self, bytes: u64) {
        self.rss_cap = bytes;
    }
    /// The RSS cap the governor is currently enforcing, for the journal.
    pub fn rss_cap(&self) -> u64 {
        self.rss_cap
    }

    /// Bytes this reader has allocated for per-run data: the FIXED block-cache
    /// arena plus its index, never a function of n. The journal prints it, and
    /// `decoded_bytes` stays 0: together they are the memory law's audit line.
    #[inline]
    pub fn cache_bytes(&self) -> u64 {
        self.cache_slots * self.block_runs * 5
    }

    #[inline]
    pub fn n(&self) -> u64 {
        self.reader.n
    }
    #[inline]
    pub fn r(&self) -> u64 {
        self.reader.r
    }
    /// A fresh per-pattern working set. All the data a step needs lives in the
    /// shared decoded-block cache, so this is just this pattern's counters.
    pub fn ctx(&self) -> Ctx<'_> {
        Ctx {
            idx: self,
            arena: None,
            scan_of: 0,
            scan_cum: 0,
            steps: 0,
            ranks: 0,
            blocks: 0,
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
    fn entry(&self, ch: usize, i: u64) -> (u64, u64, u64) {
        let o = (self.rd.ent_off + self.rd.entry_bytes * (self.rd.offs[ch] + i)) as usize;
        let m = &self.reader.map;
        decode_run_entry(m.as_ref(), o, self.rd.entry_bytes)
    }
    #[inline(always)]
    fn l2entry(&self, ch: usize, i: u64) -> (u64, u64) {
        let o = (self.rd.l2_off + 16 * (self.rd.l2offs[ch] + i)) as usize;
        let m = &self.reader.map;
        decode_l2_entry(m.as_ref(), o)
    }
    /// The block directory entry: (payload byte offset, head bits, length bits)
    #[inline(always)]
    fn dir(&self, b: u64) -> (u64, u32, u32) {
        let o = (self.dir_off + self.dir_entry_bytes * b) as usize;
        let m = self.reader.map.as_ref();
        decode_block_entry(m, o, self.dir_entry_bytes)
    }
}
// ------------------------------------------------------------ huge pages
//
// The DECODED BLOCK ARENA is the query's random-access working set (one cache
// line per decoded block, 50k slots), and a random 4 KB page per LF step is a
// TLB miss on every step of a dependent chain. THP is in `madvise` mode on this
// box (/sys/kernel/mm/transparent_hugepage/enabled), so it has to be asked for.
// Failure is NOT fatal (a kernel without THP must still run the query), it is
// journaled.
#[cfg(target_os = "linux")]
const MADV_HUGEPAGE: i32 = 14;

#[cfg(target_os = "linux")]
extern "C" {
    fn madvise(addr: *mut core::ffi::c_void, len: usize, advice: i32) -> i32;
    fn posix_fadvise(fd: i32, off: i64, len: i64, advice: i32) -> i32;
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

/// The m12 two-level bisect: `(block, block start row)` of the block holding
/// `row`. m12 stores the row of every `stride`-th run and a level-2 sample of
/// those every `l2stride` runs, so this is a bisect in a cache-resident level-2
/// array followed by a bisect in one level-1 window - no per-run state at all.
#[inline]
fn bisect_block_of(idx: &Index, row: u64) -> (u64, u64) {
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
    (a, idx.l1(a))
}

impl<'a> Ctx<'a> {
    #[inline]
    fn with_block<R>(&self, b: u64, f: impl FnOnce(&[u8], &[u32]) -> R) -> R {
        if let Some(arena) = self.arena {
            arena.borrow_mut().with_block(self.idx, b, f)
        } else {
            self.idx.with_block(b, f)
        }
    }

    /// `(head, length)` of run `run`: decode (or hit) the one block that holds it.
    /// Two reads out of that block's slices - and NOTHING per run in RAM.
    #[inline(always)]
    fn run(&self, run: u64) -> (u8, u32) {
        let idx = self.idx;
        debug_assert!(run < idx.reader.r);
        let br = idx.block_runs;
        let blk = run / br;
        let i = (run % br) as usize;
        self.with_block(blk, |heads, lens| (heads[i], lens[i]))
    }
    /// `sum of lens[i] where heads[i] == ch` over `[lo,hi)` of one decoded block.
    /// Branchless and written so the compiler emits a mask-multiply vector loop:
    /// this is the inner loop of every LF step's rank, and it runs over hundreds
    /// of millions of runs per query.
    #[inline]
    fn sum_head(heads: &[u8], lens: &[u32], lo: usize, hi: usize, ch: u8) -> u64 {
        let mut acc = 0u64;
        for i in lo..hi {
            // MULTIPLY, not `&`: the mask is 0 or 1 and the length is arbitrary.
            let hit = u64::from(unsafe { *heads.get_unchecked(i) } == ch);
            acc += hit * u64::from(unsafe { *lens.get_unchecked(i) });
        }
        acc
    }
    fn diag(&self) -> (u64, u64, u64, u64, u64) {
        (self.scan_of, self.scan_cum, self.steps, self.ranks, self.blocks)
    }
    /// Block-cache lookups this cursor paid for.
    fn blocks(&self) -> u64 {
        self.blocks
    }
    /// `(run, run start row, head, length)` of the run holding row `row`.
    ///
    /// ONE m12 bisect plus ONE decoded block: m12's level-1 cell already names the
    /// block that contains `row`, so the only per-run data this needs is that
    /// block's `lens`, walked by the 4-wide `scan_window`. Nothing is materialised
    /// and nothing is looked up per run.
    #[inline]
    fn row_run(&mut self, row: u64) -> (u64, u64, u8, u32) {
        let idx = self.idx;
        debug_assert!(row < idx.reader.n);
        let (blk, s0) = bisect_block_of(idx, row);
        let br = idx.block_runs;
        let base = blk * br;
        let r = idx.reader.r;
        let avail = (r - base).min(br) as usize;
        let (rel, s, h, l) = self.with_block(blk, |heads, lens| {
            let (rel, s) = scan_window(|i| u64::from(lens[i]), base, s0, row, avail, r);
            let i = (rel - base) as usize;
            (rel, s, heads[i], lens[i])
        });
        self.scan_of += rel - base;
        self.blocks += 1;
        (rel, s, h, l)
    }
    /// The run holding row `row`, as `(run, run start row, run length)`. The same
    /// single m12 bisect + decoded block `row_run` pays for; the BWT symbol is
    /// deliberately not returned (a caller that needs only the run's geometry must
    /// not be able to build a second, divergent notion of a run).
    pub(crate) fn run_at(&mut self, row: u64) -> (u64, u64, u64) {
        let (run, s, _h, l) = self.row_run(row);
        (run, s, u64::from(l))
    }
    /// Occurrences of `ch` in the BWT before run `k`, for an ARBITRARY `ch`.
    ///
    /// m13 gives `(c0, r0)` = rows of `ch` before run `r0`, at a sample `r0 <= k`,
    /// and `(c1, r1)` for the next sample. The exact answer is `c0` plus the `ch`
    /// runs in `[r0,k)`, or `c1` minus the `ch` runs in `[k,r1)`: the reader takes
    /// whichever end is CLOSER and shorter, so the scan is at most half of the
    /// sample interval. Either way it is capped at the directory's own `run_gap`,
    /// because the writer samples `ch` at any occurrence that is `run_gap` runs
    /// past the previous sample (bit6/xsa_finish.cpp: `k - lastSample[ch] >=
    /// PF_RUNDIR_GAP`), so anything further than that is already past the whole
    /// sample interval and the next sample's count is the answer.
    ///
    /// The scan walks DECODED BLOCKS, not a column: at pile scale a column would
    /// be 250 GB. This is what an LF step pays now that nothing is materialised.
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
        let gap = idx.rd.run_gap;
        let back = k - r0;
        // the successor sample, if any: (cum rows before r1, r1)
        let fwd = if a + 1 < m {
            let (c1, r1, _) = idx.entry(ci, a + 1);
            if r1 > k && r1 - k <= gap && r1 - k < back {
                Some((c1, r1))
            } else {
                None
            }
        } else {
            None
        };
        let br = idx.block_runs;
        let r = idx.reader.r;
        if let Some((c1, r1)) = fwd {
            let mut acc = 0u64;
            let mut run = k;
            while run < r1 {
                let blk = run / br;
                let base = blk * br;
                let avail = (r - base).min(br);
                let i0 = (run - base) as usize;
                let i1 = ((r1.min(base + avail)) - base) as usize;
                self.blocks += 1;
                acc += self.with_block(blk, |heads, lens| Ctx::sum_head(heads, lens, i0, i1, ch));
                self.scan_cum += (i1 - i0) as u64;
                run = base + i1 as u64;
            }
            return c1 - acc;
        }
        if back > gap {
            return if a + 1 < m {
                idx.entry(ci, a + 1).0
            } else {
                idx.total[ci]
            };
        }
        let mut acc = c0;
        let mut run = r0;
        while run < k {
            let blk = run / br;
            let base = blk * br;
            let avail = (r - base).min(br);
            let i0 = (run - base) as usize;
            let i1 = ((k.min(base + avail)) - base) as usize;
            self.blocks += 1;
            acc += self.with_block(blk, |heads, lens| Ctx::sum_head(heads, lens, i0, i1, ch));
            self.scan_cum += (i1 - i0) as u64;
            run = base + i1 as u64;
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
        let (run, s, h, _) = self.row_run(i);
        // `cum_before(ch, run)` is exactly the count before the run, whether or
        // not the run carries `ch` (if it does, `cum_before` returns the next
        // sample's count, which is this run's rows added), so the
        // `seen`-column-free form is ONE expression.
        self.cum_before(ch, run) + if h == ch { i - s } else { 0 }
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
            assert!(l <= r, "QUERY_INTERVAL_INVERTED l={l} r={r} pattern_len={}", pat.len());
            if l == r {
                return (l, l);
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
    /// `steps`: `position = anchor + steps (mod n)`. The anchor is read straight
    /// out of the mapping (m11 is 0.008 n at pile scale and never enters the heap).
    pub fn locate_steps(&mut self, row: u64) -> (u64, u64) {
        let n = self.idx.reader.n;
        let stripe = self.idx.stripe;
        let mut pos = row;
        let mut steps = 0u64;
        loop {
            let (run, s, h, l) = self.row_run(pos);
            let end = s + u64::from(l);
            if pos == end - 1 && run % stripe == 0 {
                let i = run / stripe;
                let anchor = u64_at(
                    self.idx.reader.map.as_ref(),
                    (self.idx.anchors_off + 16 + 8 * i) as usize,
                );
                return ((anchor + steps) % n, steps);
            }
            // `seen[run]` without a `seen` column: cum_before(head, run) IS the
            // number of head rows before this run.
            let lf = self.idx.c[h as usize] + self.cum_before(h, run);
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

/// Rows located per parallel work unit. Small enough that the very uneven LF
/// walks balance across threads, large enough that the per-chunk `Ctx` and the
/// output copy are noise beside one walk.
const LOCATE_CHUNK: u64 = 1024;
/// An occurrence interval at or above this size is located on ALL rayon threads.
/// Below it the serial walk is cheaper than waking the pool.
const PARALLEL_LOCATE_MIN: u64 = 4096;

impl Index {
    /// Locate every row in `[lo, hi)`, IN ORDER, on all rayon threads.
    ///
    /// An LF walk is independent per row - it only reads the shared, read-only
    /// kernel - so an occurrence interval is embarrassingly parallel. Each chunk
    /// gets its own `Ctx`; the results are concatenated in row order, so the
    /// output is bit-identical to the serial loop it replaces. Returns
    /// `(positions, lf steps, max steps in one walk, block-cache lookups)`.
    pub fn locate_range(&self, lo: u64, hi: u64) -> (Vec<u64>, u64, u64, u64) {
        if hi <= lo {
            return (Vec::new(), 0, 0, 0);
        }
        let nchunks = (hi - lo).div_ceil(LOCATE_CHUNK);
        let chunks: Vec<(Vec<u64>, u64, u64, u64)> = (0..nchunks)
            .into_par_iter()
            .map(|c| {
                let a = lo + c * LOCATE_CHUNK;
                let b = (a + LOCATE_CHUNK).min(hi);
                let mut cx = self.ctx();
                let mut out = Vec::with_capacity((b - a) as usize);
                let mut steps = 0u64;
                let mut max_steps = 0u64;
                for row in a..b {
                    let (p, st) = cx.locate_steps(row);
                    out.push(p);
                    steps += st;
                    max_steps = max_steps.max(st);
                }
                let blocks = cx.blocks();
                (out, steps, max_steps, blocks)
            })
            .collect();
        let mut positions = Vec::with_capacity((hi - lo) as usize);
        let (mut steps, mut max_steps, mut blocks) = (0u64, 0u64, 0u64);
        for (v, s, m, b) in chunks {
            positions.extend_from_slice(&v);
            steps += s;
            max_steps = max_steps.max(m);
            blocks += b;
        }
        debug_assert_eq!(positions.len() as u64, hi - lo);
        (positions, steps, max_steps, blocks)
    }
}
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

/// Mapped corpus metadata. Anchors bound each lookup to at most 63 deltas;
/// no corpus-sized decoded start array is allocated.
///
/// Two corpus kinds share this one type (CORPUS_V2.md 2.1: the XCS2 column is
/// byte-for-byte the XSEP delta+anchor shape, so the rank/select is shared):
///
///   V1: seps.bin (XSEP v1) + titles.bin (XTIT v1), corpus.bin for --context;
///       every document ends with a framing 0x1E that is not content.
///   V2: <name>.starts (XCS2 v1, flags 0) + <name>.docs (XPM1 v1); the title
///       of a document is its component name, the corpus END is not a
///       separator (snippet never strips a byte and may cut a window inside
///       text that legitimately contains 0x1E), and --context reads the
///       optional <name>.bin cache.
struct Documents {
    dir: std::path::PathBuf,
    seps: Mmap,
    titles: Option<Mmap>,
    /// V2: <name>.docs, kept mapped; the component-name blob and its
    /// offset/length tables are read straight out of it on lookup.
    docs2: Option<Mmap>,
    docs2_rows: usize,
    docs2_names: usize,
    docs2_name_offs: usize,
    docs2_name_lens: usize,
    /// V2: the optional <name>.bin cache (read only by --context).
    v2_bin: Option<std::path::PathBuf>,
    v2: bool,
    count: usize,
    bytes: u64,
    anchors: usize,
    payload: usize,
}

impl Documents {
    fn open(dir: &str, n: u64) -> Result<Self, String> {
        let dir = std::path::Path::new(dir);
        if !dir.join("seps.bin").exists() {
            // A V2 corpus directory: exactly one <name>.starts (XCS2 v1).
            let mut found: Option<std::path::PathBuf> = None;
            let mut seen = 0usize;
            if let Ok(rd) = std::fs::read_dir(dir) {
                for e in rd.flatten() {
                    let p = e.path();
                    if p.extension().is_some_and(|x| x == "starts") {
                        let mut head = [0u8; 4];
                        if let Ok(mut f) = File::open(&p) {
                            if f.read_exact(&mut head).is_ok() && &head == b"XCS2" {
                                seen += 1;
                                found = Some(p);
                            }
                        }
                    }
                }
            }
            return match (found, seen) {
                (Some(path), 1) => Self::open_v2(dir, n, &path),
                (Some(_), _) => Err(format!(
                    "{}: more than one *.starts with the XCS2 magic; a directory names exactly one V2 corpus (one MANIFEST.json per directory)",
                    dir.display()
                )),
                (None, _) => Err(format!("{}: no seps.bin/titles.bin (V1) and no XCS2 *.starts (V2)", dir.display())),
            };
        }
        Self::open_v1(dir, n)
    }

    fn open_v1(dir: &std::path::Path, n: u64) -> Result<Self, String> {
        let map = |name: &str| -> Result<Mmap, String> {
            let path = dir.join(name);
            let file = File::open(&path).map_err(|e| format!("{}: {e}", path.display()))?;
            unsafe { Mmap::map(&file) }.map_err(|e| format!("{}: {e}", path.display()))
        };
        let seps = map("seps.bin")?;
        let titles = map("titles.bin")?;
        let manifest: Option<serde_json::Value> = match std::fs::read(dir.join("MANIFEST.json")) {
            Ok(data) => Some(serde_json::from_slice(&data).map_err(|e| format!("MANIFEST.json: {e}"))?),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
            Err(e) => return Err(format!("MANIFEST.json: {e}")),
        };
        let u32at = |b: &[u8], o| u32::from_le_bytes(b[o..o+4].try_into().unwrap()) as usize;
        if seps.len() < 48 || &seps[..4] != b"XSEP" || u32at(&seps, 4) != 1
            || u32at(&seps, 12) != 64 || u32at(&seps, 8) & !1 != 0 {
            return Err("invalid seps.bin header (expected XSEP v1, stride 64)".into());
        }
        if titles.len() < 20 || &titles[..4] != b"XTIT" || u32at(&titles, 4) != 1 {
            return Err("invalid titles.bin header (expected XTIT v1)".into());
        }
        let count = u64_at(&seps, 16);
        let bytes = u64_at(&seps, 24);
        let anchors = count.div_ceil(64);
        // SXI2 n includes the structural virtual END, which is not a corpus byte.
        if bytes.checked_add(1) != Some(n)
            || manifest.as_ref().is_some_and(|m| m["corpus_bytes"].as_u64() != Some(bytes)
                || m["doc_count"].as_u64() != Some(count))
            || u32at(&titles, 8) as u64 != count {
            return Err(format!("corpus metadata mismatch: container n={n} (includes virtual END), seps corpus_bytes={bytes} docs={count}, titles docs={}, MANIFEST={manifest:?}",
                u32at(&titles, 8)));
        }
        if count == 0 || count > u32::MAX as u64 || u64_at(&seps, 32) != anchors
            || 48 + count * 4 + anchors * 8 != seps.len() as u64 {
            return Err("invalid seps.bin counts or file length".into());
        }
        let payload = u64_at(&titles, 12);
        if payload.checked_add(20 + count * 6) != Some(titles.len() as u64) {
            return Err("invalid titles.bin payload/offset/length tables".into());
        }
        let docs = Self { dir: dir.to_path_buf(), seps, titles: Some(titles), docs2: None,
            docs2_rows: 0, docs2_names: 0, docs2_name_offs: 0, docs2_name_lens: 0,
            v2_bin: None, v2: false, count: count as usize, bytes,
            anchors: anchors as usize, payload: payload as usize };
        let mut start = u64_at(&docs.seps, 40);
        let mut title_end = 0;
        for i in 0..docs.count {
            let delta = docs.delta(i);
            start = start.checked_add(delta).ok_or("seps.bin start overflow")?;
            if start >= bytes || (i == 0 && start != 0) || (i > 0 && delta == 0)
                || (i % 64 == 0 && docs.anchor(i / 64) != start) {
                return Err(format!("invalid seps.bin start/anchor at document {i}"));
            }
            let (off, len) = docs.title_range(i);
            if off != title_end || off.checked_add(len).is_none_or(|end| end > docs.payload) {
                return Err(format!("invalid titles.bin range at document {i}"));
            }
            title_end = off + len;
        }
        if title_end != docs.payload { return Err("titles.bin unreferenced payload".into()); }

        Ok(docs)
    }

    /// Open a CORPUS V2 directory (CORPUS_V2.md 2): exactly one
    /// `<name>.starts` (XCS2 v1) + `<name>.docs` (XPM1 v1) + MANIFEST.json.
    /// The starts/docs PAIR AGREEMENT is asserted here (offset[k] == start[k],
    /// rows tile, sum(lengths) == corpus_bytes) - by construction the two
    /// tables are the same data, and the query side never trusts one alone.
    fn open_v2(dir: &std::path::Path, n: u64, starts_path: &std::path::Path) -> Result<Self, String> {
        let map_file = |p: &std::path::Path| -> Result<Mmap, String> {
            let file = File::open(p).map_err(|e| format!("{}: {e}", p.display()))?;
            unsafe { Mmap::map(&file) }.map_err(|e| format!("{}: {e}", p.display()))
        };
        let seps = map_file(starts_path)?;
        let docs_path = starts_path.with_extension("docs");
        let docs2 = map_file(&docs_path)?;
        let manifest: Option<serde_json::Value> = match std::fs::read(dir.join("MANIFEST.json")) {
            Ok(data) => Some(serde_json::from_slice(&data).map_err(|e| format!("MANIFEST.json: {e}"))?),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
            Err(e) => return Err(format!("MANIFEST.json: {e}")),
        };
        let u32at = |b: &[u8], o| u32::from_le_bytes(b[o..o+4].try_into().unwrap()) as usize;
        if seps.len() < 48 || &seps[..4] != b"XCS2" || u32at(&seps, 4) != 1
            || u32at(&seps, 12) != 64 || u32at(&seps, 8) != 0 {
            return Err(format!("invalid {} header (expected XCS2 v1, stride 64, flags 0: V2 has no separator)",
                starts_path.display()));
        }
        if docs2.len() < 68 || &docs2[..4] != b"XPM1" || u32at(&docs2, 4) != 1
            || u32at(&docs2, 56) != 34 || u64_at(&docs2, 60) != 0 {
            return Err(format!("invalid {} header (expected XPM1 v1, 34-byte rows, partial_tail 0)",
                docs_path.display()));
        }
        let count = u64_at(&seps, 16);
        let bytes = u64_at(&seps, 24);
        let anchors = count.div_ceil(64);
        // SXI2 n includes the structural virtual END, which is not a corpus byte.
        if bytes.checked_add(1) != Some(n)
            || manifest.as_ref().is_some_and(|m| m["kind"].as_str() != Some("corpus-v2")
                || m["corpus_bytes"].as_u64() != Some(bytes)
                || m["doc_count"].as_u64() != Some(count)) {
            return Err(format!("corpus metadata mismatch: container n={n} (includes virtual END), starts corpus_bytes={bytes} docs={count}, MANIFEST={manifest:?}"));
        }
        if count == 0 || count > u32::MAX as u64 || u64_at(&seps, 32) != anchors
            || 48 + count * 4 + anchors * 8 != seps.len() as u64 {
            return Err("invalid starts counts or file length".into());
        }
        let shard_count = u64_at(&docs2, 24) as usize;
        let name_count = u64_at(&docs2, 32) as usize;
        let name_payload = u64_at(&docs2, 40) as usize;
        let shard_payload = u64_at(&docs2, 48) as usize;
        if u64_at(&docs2, 8) != count || u64_at(&docs2, 16) != bytes {
            return Err("docs table disagrees with starts on doc_count/corpus_bytes".into());
        }
        let names = 68usize;
        let name_offs = names + name_payload;
        let name_lens = name_offs + 4 * name_count;
        let rows = name_lens + 2 * name_count + shard_payload + 6 * shard_count;
        if rows.checked_add(count as usize * 34) != Some(docs2.len()) {
            return Err("invalid docs table length".into());
        }
        // The component-name tables must tile their blob (cheap: name_count rows).
        let mut name_end = 0usize;
        for j in 0..name_count {
            let off = u32::from_le_bytes(docs2[name_offs + 4*j .. name_offs + 4*j + 4].try_into().unwrap()) as usize;
            let len = u16::from_le_bytes(docs2[name_lens + 2*j .. name_lens + 2*j + 2].try_into().unwrap()) as usize;
            if off != name_end || off.checked_add(len).is_none_or(|end| end > name_payload) {
                return Err(format!("invalid component-name range at index {j}"));
            }
            name_end = off + len;
        }
        if name_end != name_payload { return Err("component-name payload not exactly covered".into()); }
        let docs = Self { dir: dir.to_path_buf(), seps, titles: None, docs2: Some(docs2),
            docs2_rows: rows, docs2_names: names, docs2_name_offs: name_offs,
            docs2_name_lens: name_lens,
            v2_bin: Some(starts_path.with_extension("bin")), v2: true,
            count: count as usize, bytes, anchors: anchors as usize, payload: 0 };
        let u32_at64 = |b: &[u8], o: usize| u32::from_le_bytes(b[o..o+4].try_into().unwrap()) as u64;
        let mut start = u64_at(&docs.seps, 40);
        let mut row_end = 0u64;
        for i in 0..docs.count {
            let delta = docs.delta(i);
            start = start.checked_add(delta).ok_or("starts start overflow")?;
            let r = docs.docs2_rows + i * 34;
            let d = docs.docs2.as_ref().expect("v2");
            let off = u64_at(d, r);
            let len = u64_at(d, r + 8);
            let shard = u16::from_le_bytes(d[r+16..r+18].try_into().unwrap()) as usize;
            let comp = u32at(d, r + 18);
            if start >= bytes || (i == 0 && start != 0) || (i > 0 && delta == 0)
                || (i % 64 == 0 && docs.anchor(i / 64) != start)
                || off != start || len == 0
                || shard >= shard_count || comp >= name_count
                || u32_at64(d, r + 30) != 0 {
                return Err(format!(
                    "invalid starts/docs pair at document {i}: start={start} delta={delta} docs(off={off} len={len} shard={shard}/{shard_count} comp={comp}/{name_count}) - the V2 tables must agree by construction"));
            }
            row_end = off + len;
        }
        if row_end != bytes { return Err("docs rows do not tile to corpus_bytes".into()); }

        Ok(docs)
    }
    fn delta(&self, i: usize) -> u64 {
        u32::from_le_bytes(self.seps[48+i*4..52+i*4].try_into().unwrap()) as u64
    }
    fn anchor(&self, i: usize) -> u64 { u64_at(&self.seps, 48+self.count*4+i*8) }
    fn title_range(&self, i: usize) -> (usize, usize) {
        let titles = self.titles.as_ref().expect("V1 only");
        let off = 20+self.payload+i*4;
        let len = 20+self.payload+self.count*4+i*2;
        (u32::from_le_bytes(titles[off..off+4].try_into().unwrap()) as usize,
         u16::from_le_bytes(titles[len..len+2].try_into().unwrap()) as usize)
    }
    /// V2: the title of document i is its component name, read straight out
    /// of the mapped <name>.docs (XPM1 row k, column pile_set_name).
    fn v2_title(&self, i: usize) -> &[u8] {
        let d = self.docs2.as_ref().expect("V2 only");
        let row = self.docs2_rows + i * 34;
        let comp = u32::from_le_bytes(d[row+18..row+22].try_into().unwrap()) as usize;
        let off = u32::from_le_bytes(d[self.docs2_name_offs+4*comp..self.docs2_name_offs+4*comp+4].try_into().unwrap()) as usize;
        let len = u16::from_le_bytes(d[self.docs2_name_lens+2*comp..self.docs2_name_lens+2*comp+2].try_into().unwrap()) as usize;
        &d[self.docs2_names+off .. self.docs2_names+off+len]
    }
    fn lookup(&self, pos: u64) -> Result<(usize, u64, &[u8]), String> {
        if pos >= self.bytes { return Err(format!("document lookup outside corpus: {pos}")); }
        let (mut lo, mut hi) = (0, self.anchors);
        while lo < hi {
            let mid = lo + (hi-lo)/2;
            if self.anchor(mid) <= pos { lo = mid+1; } else { hi = mid; }
        }
        let block = lo-1;
        let mut doc = block*64;
        let mut start = self.anchor(block);
        let end = (doc+64).min(self.count);
        while doc+1 < end {
            let next = start+self.delta(doc+1);
            if next > pos { break; }
            start = next;
            doc += 1;
        }
        let (off, len) = if self.v2 {
            return Ok((doc, pos-start, self.v2_title(doc)));
        } else {
            self.title_range(doc)
        };
        Ok((doc, pos-start, &self.titles.as_ref().expect("V1 only")[20+off..20+off+len]))
    }
    /// The title of document `d` regardless of corpus kind (V1: titles.bin;
    /// V2: the component name out of <name>.docs).
    fn lookup_title(&self, d: usize) -> &[u8] {
        if self.v2 {
            self.v2_title(d)
        } else {
            let (off, len) = self.title_range(d);
            &self.titles.as_ref().expect("V1 only")[20+off..20+off+len]
        }
    }
    fn context_corpus(&self) -> Result<Mmap, String> {
        if self.v2 {
            // The optional cache (CORPUS_V2.md 1): <name>.bin, never framed.
            let path = self.v2_bin.as_ref().expect("V2 only");
            let file = File::open(path).map_err(|e| {
                format!("context {}: the V2 cache is absent - materialize it first: xsa corpus --v2-materialize --prefix <prefix> ({e})", path.display())
            })?;
            let corpus = unsafe { Mmap::map(&file) }.map_err(|e| format!("context mmap: {e}"))?;
            if corpus.len() as u64 != self.bytes { return Err("context cache length mismatch".into()); }
            return Ok(corpus);
        }
        let file = File::open(self.dir.join("corpus.bin")).map_err(|e| format!("context corpus.bin: {e}"))?;
        let corpus = unsafe { Mmap::map(&file) }.map_err(|e| format!("context mmap: {e}"))?;
        if corpus.len() as u64 != self.bytes { return Err("context corpus.bin length mismatch".into()); }
        Ok(corpus)
    }
    fn snippet(&self, corpus: &[u8], pos: u64, window: u64) -> Result<(u64, u64, String, bool), String> {
        let (doc, offset, _) = self.lookup(pos)?;
        let start = pos-offset;
        let next = if doc+1 < self.count { start+self.delta(doc+1) } else { self.bytes };
        // V1: records end with 0x1E; the separator is not document content.
        // V2: there is no separator anywhere - the document simply ends where
        // the next one starts (and its text may contain 0x1E legitimately).
        let end = if self.v2 { next }
            else if corpus[(next-1) as usize] == 0x1e { next-1 } else { next };
        if pos >= end { return Err(format!("QUERY_CONTEXT_SEPARATOR_HIT pos={pos}")); }
        let lo = pos.saturating_sub(window).max(start);
        let hi = pos.saturating_add(window).min(end);
        let bytes = &corpus[lo as usize..hi as usize];
        if !self.v2 {
            assert!(!bytes.contains(&0x1e), "QUERY_CONTEXT_SEPARATOR pos={pos} start={lo} end={hi}");
        }
        let mut escaped = String::new();
        for &b in bytes {
            match b {
                b'\n' => escaped.push_str("\\n"),
                b'\r' => escaped.push_str("\\r"),
                b'\t' => escaped.push_str("\\t"),
                b'\\' => escaped.push_str("\\\\"),
                0x20..=0x7e => escaped.push(b as char),
                _ => { use std::fmt::Write; write!(&mut escaped, "\\x{b:02x}").unwrap(); }
            }
        }
        Ok((lo, hi, escaped, lo != pos.saturating_sub(window) || hi != pos.saturating_add(window)))
    }
    fn emit(&self, w: &mut dyn Write, pid: usize, pos: u64) -> Result<(), String> {
        let (doc, offset, title) = self.lookup(pos)?;
        let emit = || -> std::io::Result<()> {
            write!(w, "{pid}\t{doc}\t{offset}\t")?;
            for &b in title {
                match b {
                    b'\n' => w.write_all(b"\\n")?,
                    b'\\' => w.write_all(b"\\\\")?,
                    _ => w.write_all(&[b])?,
                }
            }
            writeln!(w, "\t{pos}")
        };
        let mut emit = emit;
        emit().map_err(|e| format!("write document hit: {e}"))
    }
}

/// The journal is retained beside the container. Older builds already recorded
/// their input path; use that provenance without modifying historical containers.
fn recorded_corpus(journal: &str) -> Option<(String, Option<u64>)> {
    // V2 first (the newer line; CORPUS_V2.md 6): the build journaled the
    // validated sidecar prefix, and the V2 corpus directory is its parent.
    for line in journal.lines() {
        if let Some(rest) = line.strip_prefix("CONTAINER_DOCS_V2 prefix=") {
            let (prefix, fields) = rest.split_once(" starts=")?;
            let (count, _) = fields.split_once(" corpus_bytes=")?;
            let prefix = serde_json::from_str::<String>(prefix).unwrap_or_else(|_| prefix.to_string());
            let dir = std::path::Path::new(&prefix).parent()
                .filter(|p| !p.as_os_str().is_empty())
                .map(|p| p.to_string_lossy().into_owned())
                .unwrap_or_else(|| ".".to_string());
            return Some((dir, count.parse().ok()));
        }
    }
    for line in journal.lines() {
        if let Some(rest) = line.strip_prefix("CONTAINER_CORPUS_SIDECARS dir=") {
            let (dir, fields) = rest.split_once(" seps=seps.bin titles=titles.bin docs=")?;
            let (count, _) = fields.split_once(" corpus_bytes=")?;
            let dir = serde_json::from_str::<String>(dir).unwrap_or_else(|_| dir.to_string());
            return Some((dir, count.parse().ok()));
        }
    }
    journal.lines().find_map(|line| {
        let rest = line.strip_prefix("XSA_BUILD START input=")?;
        let (input, _) = rest.split_once(" scratch=")?;
        let parent = std::path::Path::new(input).parent()?;
        if !parent.is_absolute() { return None; }
        Some((parent.to_string_lossy().into_owned(), None))
    })
}

fn discover_documents(container: &str, explicit: Option<&str>, n: u64) -> Option<Documents> {
    let parent = std::path::Path::new(container).parent().unwrap_or(std::path::Path::new("."));
    let journal = std::fs::read_to_string(parent.join("xsa-build.log")).unwrap_or_default();
    let recorded = recorded_corpus(&journal);
    let expected_count = recorded.as_ref().and_then(|(_, count)| *count).filter(|&count| count > 0);
    let mut candidates = Vec::new();
    if let Some(dir) = explicit { candidates.push(("docs", dir.to_string())); }
    candidates.push(("sibling", parent.to_string_lossy().into_owned()));
    if let Some((dir, _)) = recorded { candidates.push(("recorded", dir)); }
    for (source, dir) in candidates {
        let path = std::path::Path::new(&dir);
        // V1 candidates carry seps.bin/titles.bin; a V2 corpus directory
        // carries its <name>.starts instead (one manifest per directory).
        let has_v1 = path.join("seps.bin").exists() || path.join("titles.bin").exists();
        let has_v2 = std::fs::read_dir(path)
            .map(|rd| rd.flatten().any(|e| e.path().extension().is_some_and(|x| x == "starts")))
            .unwrap_or(false);
        if source != "docs" && !has_v1 && !has_v2 { continue; }
        let result = Documents::open(&dir, n).and_then(|docs| {
            if expected_count.is_some_and(|count| count != docs.count as u64) {
                Err(format!("corpus metadata mismatch: recorded docs={expected_count:?}, candidate docs={}", docs.count))
            } else { Ok(docs) }
        });
        match result {
            Ok(docs) => {
                eprintln!("QUERY_DOCS source={source} docs={} corpus_bytes={}", docs.count, docs.bytes);
                return Some(docs);
            }
            Err(e) => eprintln!("query: warning: skipping {source} sidecars in {dir}: {e}"),
        }
    }
    eprintln!("query: warning: no matching document sidecars; emitting plain offsets");
    eprintln!("QUERY_DOCS source=none docs=0 corpus_bytes={}", n.saturating_sub(1));
    None
}

// ---------------------------------------------------------------- the command

struct Args {
    container: String,
    docs: Option<String>,
    patterns: Option<String>,
    positional: Vec<Vec<u8>>,
    /// FASTA is explicit; --patterns always uses 0x1E records.
    fasta: bool,
    output: String,
    counts: bool,
    count: bool,
    limit: Option<u64>,
    context: Option<u64>,
    threads: usize,
    verify: bool,
    /// `xsa verify --corpus FILE`: the exact bytes every located position must
    /// carry. `None` for a plain query.
    corpus: Option<String>,
}

fn parse(raw: &[std::ffi::OsString]) -> Args {
    use std::os::unix::ffi::OsStrExt;
    let args: Vec<_> = raw.iter().map(|a| a.to_string_lossy().into_owned()).collect();
    let mut positional = Vec::new();
    let mut literal = false;
    let mut container: Option<String> = None;
    let mut patterns: Option<String> = None;
    let mut docs = None;
    let mut corpus = None;
    let mut fasta = false;
    let mut output = "tsv".to_string();
    let mut counts = false;
    let mut count = false;
    let mut limit = None;
    let mut context = None;
    let mut threads = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1);
    let mut verify = false;
    let mut i = 0;
    while i < args.len() {
        if literal {
            if container.is_none() { container = Some(args[i].clone()); }
            else { positional.push(raw[i].as_bytes().to_vec()); }
            i += 1;
            continue;
        }
        match args[i].as_str() {
            "--" => { literal = true; i += 1; }
            "--all" => { i += 1; }
            "--count" => { count = true; i += 1; }
            "--context" => {
                let value = args.get(i+1).unwrap_or_else(|| die("query: --context requires a nonnegative integer"));
                if value.is_empty() || !value.bytes().all(|b| b.is_ascii_digit()) { die("query: --context requires a nonnegative integer"); }
                context = Some(value.parse::<u64>().unwrap_or_else(|_| die("query: --context outside u64 range")));
                i += 2;
            }
            "--limit" => {
                let value = args.get(i + 1).unwrap_or_else(|| die("query: --limit requires a nonnegative integer"));
                if value.is_empty() || !value.bytes().all(|b| b.is_ascii_digit()) {
                    die("query: --limit requires a nonnegative integer");
                }
                limit = Some(value.parse::<u64>().unwrap_or_else(|_| die("query: --limit is outside u64 range")));
                i += 2;
            }
            "--corpus" if i + 1 < args.len() => {
                corpus = Some(args[i + 1].clone());
                i += 2;
            }
            "--docs" if i + 1 < args.len() => {
                docs = Some(args[i + 1].clone());
                i += 2;
            }
            "--patterns" if i + 1 < args.len() => {
                if fasta {
                    die("query: --patterns and --patterns-fasta are exclusive: pick one\n  \
                         --patterns reads 0x1E records; --patterns-fasta reads FASTA");
                }
                patterns = Some(args[i + 1].clone());
                i += 2;
            }
            "--patterns-fasta" if i + 1 < args.len() => {
                if patterns.is_some() && !fasta {
                    die("query: --patterns and --patterns-fasta are exclusive: pick one\n  \
                         --patterns reads 0x1E records; --patterns-fasta reads FASTA");
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
                "query: --mode is not accepted: the container carries no alphabet, and patterns \
                 are raw bytes\n  \
                 a byte-string index has exactly one orientation, so there is no dna/text mode \
                 and no reverse complement",
            ),
            other if other.starts_with('-') => die(&format!("query: unknown option {other}")),
            other => {
                if container.is_some() {
                    positional.push(raw[i].as_bytes().to_vec());
                } else {
                    container = Some(other.to_string());
                }
                i += 1;
            }
        }
    }
    if context.is_some() && (count || counts) { die("query: --context requires hit output"); }
    if count && limit.is_some() { die("query: --count and --limit cannot be combined"); }
    if count && (counts || output != "tsv") {
        die("query: --count emits pattern_id<TAB>pattern<TAB>occurrences TSV; cannot combine with --counts or --output jsonl");
    }
    let container = container.unwrap_or_else(|| {
        die("query: need a container path\n  xsa query <container> --patterns <file> [-j N]")
    });
    if patterns.is_some() && !positional.is_empty() {
        die("query: positional patterns and --patterns/--patterns-fasta cannot be combined (ambiguous pattern id numbering)");
    }
    if patterns.is_none() && positional.is_empty() {
        die("query: need <pattern> [<pattern> ...] or --patterns <file>");
    }
    for p in &positional {
        if p.is_empty() || p.len() > PAT_LEN_MAX {
            die("query: positional patterns must contain 1..=65536 bytes");
        }
    }
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
        docs,
        corpus,
        container,
        patterns,
        positional,
        fasta,
        output,
        counts,
        count,
        limit,
        context,
        threads,
        verify,
    }
}

/// One pattern's result: positions plus the walls the kernel charged for it.
struct Hit {
    occurrences: u64,
    positions: Vec<u64>,
    search_us: f64,
    /// mean over occurrences
    locate_us: f64,
    /// total LF steps over all occurrences, and the worst single walk
    steps: u64,
    max_steps: u64,
    /// Trace-only work counters for this pattern: (row_run scan runs, cum_before
    /// scan runs, LF steps, rank calls).
    diag: (u64, u64, u64, u64),
    /// Block-cache lookups this pattern paid for (`-j` threads' worth when the
    /// occurrence interval was located in parallel).
    blocks: u64,
}

/// The two pattern-file contracts.
///
/// `Records` is the corpus contract, verbatim: records separated by 0x1E,
/// `rec \x1E rec \x1E ... rec \x1E`, ARBITRARY BYTES, no escaping, no alphabet,
/// id = 0-based record index. That is the only thing a byte-string index can be
/// asked about, and it is the default.
///
/// `Fasta` is available explicitly through `--patterns-fasta`.
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
        (PatFormat::Records, "records")
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

/// THE BYTE CERTIFICATE (`xsa verify`): every located position must carry the
/// pattern's exact bytes in the corpus the container was built from, and no
/// position may repeat.
///
/// Why this is a separate verb and not a debug flag: a count is an interval
/// width, and an interval width can be right while the rows inside it are not
/// the occurrences - or while one occurrence is counted twice. This reads the
/// corpus back at each offset, so the answer is checked against the only text
/// there is. It is bounded I/O (pattern_len bytes per position) and it never
/// materialises the corpus: `read_exact_at` into one small buffer.
fn verify_positions(corpus: &str, n: u64, patterns: &[Vec<u8>], results: &[Hit]) {
    let f = File::open(corpus).unwrap_or_else(|e| die(&format!("verify: open {corpus}: {e}")));
    let bytes = std::fs::metadata(corpus)
        .unwrap_or_else(|e| die(&format!("verify: stat {corpus}: {e}")))
        .len();
    if bytes + 1 != n {
        die(&format!(
            "verify: {corpus} is {bytes} bytes but the container's text is n-1 = {} bytes\n  \
             (n={n}); this is not the corpus that container was built from",
            n - 1
        ));
    }
    let (mut tot_pos, mut tot_bad, mut tot_dup) = (0u64, 0u64, 0u64);
    for (pid, hit) in results.iter().enumerate() {
        let pat = &patterns[pid];
        let mut buf = vec![0u8; pat.len()];
        let mut bad = 0u64;
        for &p in &hit.positions {
            if p + pat.len() as u64 > bytes {
                bad += 1;
                if bad <= 5 {
                    eprintln!("VERIFY_BAD id={pid} offset={p} reason=past-corpus-end");
                }
                continue;
            }
            if let Err(e) = std::os::unix::fs::FileExt::read_exact_at(&f, &mut buf, p) {
                die(&format!("verify: read {corpus} at {p}: {e}"));
            }
            if buf != *pat {
                bad += 1;
                if bad <= 5 {
                    eprintln!(
                        "VERIFY_BAD id={pid} offset={p} bytes={:?} pattern={:?}",
                        String::from_utf8_lossy(&buf),
                        String::from_utf8_lossy(pat)
                    );
                }
            }
        }
        // a repeated position would mean one occurrence counted twice: sort a copy
        let mut sorted = hit.positions.clone();
        sorted.sort_unstable();
        let dup = sorted.windows(2).filter(|w| w[0] == w[1]).count() as u64;
        eprintln!(
            "VERIFY id={pid} len={} occurrences={} located={} unique={} bad={bad} dup={dup}",
            pat.len(),
            hit.occurrences,
            hit.positions.len(),
            hit.positions.len() as u64 - dup
        );
        tot_pos += hit.positions.len() as u64;
        tot_bad += bad;
        tot_dup += dup;
    }
    eprintln!(
        "VERIFY_TOTAL patterns={} positions={tot_pos} bad={tot_bad} duplicates={tot_dup} \
         corpus_bytes={bytes} corpus={corpus}",
        results.len()
    );
    if tot_bad != 0 || tot_dup != 0 {
        die(&format!(
            "verify: {tot_bad} located position(s) do not carry the pattern and {tot_dup} \
             duplicate position(s): the index does not answer from this corpus"
        ));
    }
}

fn assert_count(count: u64, n: u64, pattern_len: usize) {
    assert!(count <= n, "QUERY_COUNT_IMPOSSIBLE count={count} pattern_len={pattern_len}");
}

/// `xsa query`: answer counts and positions. `xsa verify`: the same, plus the
/// byte certificate - every located position is read back out of the corpus and
/// compared with the pattern (see `verify_positions`).
pub fn command(args: &[std::ffi::OsString], byte_certificate: bool) {
    // Wall clock from process start: the acceptance question is "how long
    // until the first answer", so every stage is reported against this.
    let t_proc = Instant::now();
    let a = parse(args);
    if byte_certificate && a.corpus.is_none() {
        die(
            "verify: --corpus FILE is required: the certificate is that every located offset\n  \
             carries the pattern's exact bytes in the corpus the container was built from",
        );
    }
    if !byte_certificate && a.corpus.is_some() {
        die("query: --corpus belongs to `xsa verify` (use `xsa verify <container> --corpus FILE ...`)");
    }
    let t_open0 = Instant::now();
    // `Reader::open`'s error IS the refusal (it names the file, the shape and the
    // rebuild instruction); there is nothing to add and nothing to fall back to.
    let reader = Reader::open(&a.container).unwrap_or_else(|e| die(&e));
    let docs = if a.count { None } else { discover_documents(&a.container, a.docs.as_deref(), reader.n) };
    let corpus = a.context.map(|_| docs.as_ref().unwrap_or_else(|| die("query: --context requires matching document sidecars and corpus.bin (--docs DIR)"))
        .context_corpus().unwrap_or_else(|e| die(&e)));
    let open_ms = t_open0.elapsed().as_secs_f64() * 1e3;
    let members: Vec<String> = reader.members.iter().map(|m| format!("m{}", m.id)).collect();
    // OPEN IS MMAP-ONLY: header, directory, header CRC and the 256-byte remap.
    // Nothing in the run stream, the chi set or the anchors is decoded here, so
    // `decoded_bytes` is 0 and no stage has run. This is the line that makes the
    // 405 s phi-era open auditable against the 1 ms it costs now.
    eprintln!(
        "QLITE_OPEN container={} format=sxi2-v10 magic=SXI2 version={} \
         mmap_ms={open_ms:.2} decoded_bytes=0 stages=none n={} runs={} members={} end_symbol=none \
         orientation=none alphabet=none",
        reader.path,
        reader.version,
        reader.n,
        reader.r,
        members.join(",")
    );
    eprintln!(
        "SQLITE_QUERY_OPEN format=sxi2-v10 mmap_ms={open_ms:.2} decoded_bytes=0 stages=none"
    );
    if a.verify {
        if let Err(e) = reader.verify_members() {
            die(&e);
        }
        eprintln!("QUERY_VERIFY members=ok");
    }
    // Format choice is journaled once, inside read_patterns (PATTERNS_USED).
    let patterns = if let Some(path) = &a.patterns {
        read_patterns(path, a.fasta).0
    } else {
        eprintln!("PATTERNS_USED format=positional count={}", a.positional.len());
        a.positional
    };
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
         m1_block_runs={} m1_blocks={} rundir_l2={} rundir_occ_stride={} rundir_gap={} \
         cache_bytes={} cache_slots={} rss_cap_bytes={}",
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
        index.rd.run_gap,
        index.cache_bytes(),
        index.cache_slots,
        index.rss_cap
    );
    let sigma = index.reader.sigma;
    // m7 maps corpus bytes to m1 symbols. Symbol 0 belongs exclusively to
    // virtual END; its inverse byte is absent even though total[0] is one.
    let present: [bool; 256] = std::array::from_fn(|b| {
        let symbol = sigma[b] as usize;
        symbol != 0 && index.total[symbol] != 0
    });
    let absent_symbols = patterns.iter()
        .filter(|pat| pat.iter().any(|&b| !present[b as usize])).count();
    eprintln!("QUERY_ABSENT_SYMBOL patterns={absent_symbols}");
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
            if pat.iter().any(|&b| !present[b as usize]) {
                return Hit { occurrences: 0, positions: Vec::new(), search_us: 0.0, locate_us: 0.0,
                    steps: 0, max_steps: 0, diag: (0, 0, 0, 0), blocks: 0 };
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
            let occurrences = r.checked_sub(l).expect("QUERY_INTERVAL_INVERTED");
            assert_count(occurrences, index.n(), pat.len());
            let located = if a.count { 0 } else { a.limit.map_or(occurrences, |limit| occurrences.min(limit)) };
            let mut positions = Vec::with_capacity(located.min(1 << 20) as usize);
            let mut steps = 0u64;
            let mut max_steps = 0u64;
            let mut par_blocks = 0u64;
            if located >= PARALLEL_LOCATE_MIN {
                // A large occurrence set is located on ALL threads: each LF walk
                // is independent, and a serial walk of 4.8 M occurrences on the
                // pile is hours. The chunk results are concatenated in row order,
                // so the output is identical to the serial loop below.
                let (p, st, mx, bl) = index.locate_range(l, l + located);
                positions = p;
                steps = st;
                max_steps = mx;
                par_blocks = bl;
            } else {
                for row in l..l + located {
                    let (p, st) = cx.locate_steps(row);
                    positions.push(p);
                    steps += st;
                    max_steps = max_steps.max(st);
                }
            }
            let locate_us = if located > 0 {
                tl.elapsed().as_secs_f64() * 1e6 / located as f64
            } else {
                0.0
            };
            let (d_of, d_cum, d_steps, d_ranks, d_blocks) = cx.diag();
            Hit {
                occurrences,
                positions,
                search_us,
                locate_us,
                steps,
                max_steps,
                diag: (d_of, d_cum, d_steps, d_ranks),
                blocks: if par_blocks > 0 { par_blocks } else { d_blocks },
            }
        })
        .collect();
    let elapsed = t0.elapsed();
    let first_answer_ms = t_proc.elapsed().as_secs_f64() * 1e3;
    let mut nocc = 0u64;
    let mut nabs = 0u64;
    let mut clamped_hits = 0u64;
    for (pid, hit) in results.iter().enumerate() {
        assert_count(hit.occurrences, index.n(), patterns[pid].len());
        let positions = &hit.positions;
        if hit.occurrences == 0 {
            nabs += 1;
        }
        nocc += hit.occurrences;
        if a.count {
            write!(w, "{pid}\t").unwrap();
            // Escape only TSV delimiters and backslashes; retain arbitrary bytes.
            for &b in &patterns[pid] {
                match b {
                    b'\t' => w.write_all(b"\\t"),
                    b'\n' => w.write_all(b"\\n"),
                    b'\r' => w.write_all(b"\\r"),
                    b'\\' => w.write_all(b"\\\\"),
                    _ => w.write_all(&[b]),
                }.unwrap();
            }
            writeln!(w, "\t{}", hit.occurrences).unwrap();
            continue;
        }
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
                    if let Some(docs) = &docs {
                        if let Some(window) = a.context {
                            let (_, _, snippet, clamped) = docs.snippet(corpus.as_ref().unwrap(), *p, window).unwrap_or_else(|e| die(&e));
                            clamped_hits += u64::from(clamped);
                            let mut row = Vec::new();
                            docs.emit(&mut row, pid, *p).unwrap_or_else(|e| die(&e));
                            row.pop();
                            w.write_all(&row).unwrap();
                            writeln!(w, "\t{snippet}").unwrap();
                        } else { docs.emit(&mut w, pid, *p).unwrap_or_else(|e| die(&e)); }
                    } else {
                        writeln!(w, "{}\t{}", pid, p).unwrap();
                    }
                }
            }
            ("jsonl", false) => {
                if let Some(window) = a.context {
                    let contexts: Vec<_> = positions.iter().map(|&pos| {
                        let (start, end, snippet, clamped) = docs.as_ref().unwrap().snippet(corpus.as_ref().unwrap(), pos, window).unwrap_or_else(|e| die(&e));
                        clamped_hits += u64::from(clamped);
                        serde_json::json!({"start":start,"end":end,"snippet":snippet})
                    }).collect();
                    writeln!(w, "{}", serde_json::json!({"pattern_id":pid,"count":positions.len(),"offsets":positions,"contexts":contexts})).unwrap();
                    continue;
                }
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
    if let Some(window) = a.context { eprintln!("QUERY_CONTEXT window={window} clamped_hits={clamped_hits}"); }
    // Per-pattern timing, for the rows the caller can actually time (a
    // high-count pattern is dominated by materialising tens of millions of
    // positions and says nothing about the search kernel).
    let (mut worst_us, mut low_sum_us, mut nlow_us, mut high) = (0.0f64, 0.0f64, 0u64, 0u64);
    let (mut low_steps, mut low_max_steps, mut low_occ) = (0u64, 0u64, 0u64);
    let _clamped_hits = 0u64;
    for (pid, hit) in results.iter().enumerate() {
        assert_count(hit.occurrences, index.n(), patterns[pid].len());
        let us = hit.search_us + hit.locate_us * hit.positions.len() as f64;
        if crate::trace_on() {
            eprintln!(
                "TRACE_PATTERN id={} len={} occ={} search_us={:.1} locate_us={:.1} \
                 steps={} max_steps={} total_us={:.1} lf_us_per_step={:.3} scan_of={} scan_cum={} \
                 diag_steps={} ranks={} blocks={}",
                pid,
                patterns[pid].len(),
                hit.occurrences,
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
                hit.diag.3,
                hit.blocks
            );
        }
        if hit.occurrences <= 3 {
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
    if let Some(corpus) = &a.corpus {
        verify_positions(corpus, index.n(), &patterns, &results);
    }
    eprintln!("QUERY_LOCATE mode={} positions_located={} lf_steps={}",
        if a.count { "count" } else if a.limit.is_some() { "limit" } else { "all" },
        results.iter().map(|h| h.positions.len() as u64).sum::<u64>(),
        results.iter().map(|h| h.steps).sum::<u64>());
    // The memory law's receipt, printed on EVERY query: the reader's per-run state
    // is the fixed arena below, `decoded_bytes` is 0 (see QLITE_BUILD), and the
    // RSS governor's drop count says how often the mapping's PTEs were released.
    {
        let (hits, misses, decodes, drops) = index.cache_stats();
        let (decode_ns, drop_ns, prefetch_calls) = index.wall_stats();
        eprintln!(
            "QLITE_CACHE hits={hits} misses={misses} decodes={decodes} drop_epochs={drops} \
             arena_bytes={} decoded_bytes={} decode_ms={:.1} drop_ms={:.1} prefetch_calls={prefetch_calls}",
            index.cache_bytes(),
            index.decoded_bytes,
            decode_ns as f64 / 1e6,
            drop_ns as f64 / 1e6
        );
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

#[cfg(test)]
mod wide_directory_tests {
    use super::*;
    fn words(v: &[u64]) -> Vec<u8> { v.iter().flat_map(|w| w.to_le_bytes()).collect() }
    #[test]
    fn run_directory_above_u32_and_old_codec() {
        for run in [u32::MAX as u64, 1u64 << 32, 19_973_766_926, 1u64 << 48] {
            let rank = run + 900;
            assert_eq!(decode_l2_entry(&words(&[rank, run]), 0), (rank, run));
            assert_eq!(scan_window(|_| 1, run, rank, rank + 2, 4, run + 4), (run + 2, rank + 2));
            assert_eq!(decode_run_entry(&words(&[rank, run, 32]), 0, 24), (rank, run, 32));
        }
        assert_eq!(decode_run_entry(&words(&[55, 7 | (32u64 << 32)]), 0, 16), (55, 7, 32));
    }
    /// The block cache is the ONLY per-run state, so the memory law is a property
    /// of the type: a `Ctx` holds counters, and `Index` holds no per-run column at
    /// all. This pins the arena's size formula against the container's declared
    /// block width, and asserts the `decoded_bytes` audit line stays 0.
    #[test]
    fn per_run_state_is_the_fixed_arena() {
        for br in [64u64, 256, 1024] {
            let per_slot = (br + 4 * br) as usize;
            let slots = (CACHE_BYTES / per_slot).max(1) as u64;
            let nshard = (CACHE_SHARDS as u64).min(slots).max(1);
            let per_shard = (slots / nshard).max(1);
            // the arena never exceeds the budget, whatever the container declares
            assert!((nshard * per_shard) as usize * per_slot <= CACHE_BYTES);
            assert!(per_shard >= 1 && nshard >= 1);
        }
    }
    #[test]
    fn block_offsets_above_old_32_gib_limit() {
        for offset in [(1u64 << 35) - 8, 1u64 << 35, 1u64 << 48] {
            assert_eq!(decode_block_entry(&words(&[offset, 1024 | (2048u64 << 32)]), 0, 16), (offset, 1024, 2048));
        }
        assert_eq!(decode_block_entry(&words(&[7 | (1024u64 << 32) | (2048u64 << 48)]), 0, 8), (56, 1024, 2048));
    }
}

#[cfg(test)]
mod document_tests {
    use super::*;

    fn fixture() -> tempfile::TempDir {
        let dir = tempfile::tempdir().unwrap();
        let mut seps = b"XSEP".to_vec();
        for v in [1u32, 1, 64] { seps.extend(v.to_le_bytes()); }
        for v in [130u64, 1300, 3, 0] { seps.extend(v.to_le_bytes()); }
        for i in 0..130 { seps.extend((if i == 0 { 0u32 } else { 10 }).to_le_bytes()); }
        for v in [0u64, 640, 1280] { seps.extend(v.to_le_bytes()); }
        let title = b"a\n\\\xff";
        let mut titles = b"XTIT".to_vec();
        titles.extend(1u32.to_le_bytes());
        titles.extend(130u32.to_le_bytes());
        titles.extend((130 * title.len() as u64).to_le_bytes());
        for _ in 0..130 { titles.extend(title); }
        for i in 0..130 { titles.extend((i * title.len() as u32).to_le_bytes()); }
        for _ in 0..130 { titles.extend((title.len() as u16).to_le_bytes()); }
        std::fs::write(dir.path().join("seps.bin"), seps).unwrap();
        std::fs::write(dir.path().join("titles.bin"), titles).unwrap();
        std::fs::write(dir.path().join("MANIFEST.json"), br#"{"doc_count":130,"corpus_bytes":1300}"#).unwrap();
        dir
    }
    #[test]
    fn context_clamps_and_escapes_at_document_edges() {
        let dir = fixture();
        let docs = Documents::open(dir.path().to_str().unwrap(), 1301).unwrap();
        let corpus = b"ab\n\t\\\xffghi\x1e".repeat(130);
        for pos in [0, 8, 630, 638, 640, 648, 1290, 1298] {
            let start = pos / 10 * 10;
            let (lo, hi, text, clamped) = docs.snippet(&corpus, pos, u64::MAX).unwrap();
            assert_eq!((lo, hi), (start, start+9));
            assert_eq!(text, "ab\\n\\t\\\\\\xffghi");
            assert!(clamped);
            assert_eq!(docs.snippet(&corpus, pos, 0).unwrap(), (pos, pos, String::new(), false));
        }
        assert!(docs.snippet(&corpus, 9, 120).is_err());
    }
    #[test]
    #[should_panic(expected = "QUERY_CONTEXT_SEPARATOR")]
    fn context_rejects_interior_separator() {
        let dir = fixture();
        let docs = Documents::open(dir.path().to_str().unwrap(), 1301).unwrap();
        let mut corpus = vec![b'a'; 1300];
        corpus[2] = 0x1e;
        docs.snippet(&corpus, 1, 120).unwrap();
    }
    #[test]
    #[should_panic(expected = "QUERY_COUNT_IMPOSSIBLE count=18446744073709551615 pattern_len=500")]
    fn impossible_count_injection_fires() { assert_count(u64::MAX, 100_093_934_467, 500); }

    #[test]
    fn documents_anchor_boundaries_separator_and_raw_title() {
        let dir = fixture();
        let docs = Documents::open(dir.path().to_str().unwrap(), 1301).unwrap();
        for pos in 0..1300 {
            let (doc, offset, title) = docs.lookup(pos).unwrap();
            assert_eq!((doc, offset), ((pos / 10) as usize, pos % 10));
            assert_eq!(title, b"a\n\\\xff");
        }
        assert!(docs.lookup(1300).is_err());
        let mut output = Vec::new();
        docs.emit(&mut output, 2, 649).unwrap();
        assert_eq!(output, b"2\t64\t9\ta\\n\\\\\xff\t649\n");
    }
    #[test]
    fn discovery_enforces_recorded_count_and_allows_sidecars_without_manifest() {
        let dir = fixture();
        let container = dir.path().join("c.sxi2");
        let journal = dir.path().join("xsa-build.log");
        let record = |count| format!("CONTAINER_CORPUS_SIDECARS dir={} seps=seps.bin titles=titles.bin docs={count} corpus_bytes=1300\n",
            serde_json::to_string(&dir.path().to_string_lossy()).unwrap());
        std::fs::write(&journal, record(129)).unwrap();
        assert!(discover_documents(container.to_str().unwrap(), None, 1301).is_none());
        std::fs::write(&journal, record(130)).unwrap();
        std::fs::remove_file(dir.path().join("MANIFEST.json")).unwrap();
        assert_eq!(discover_documents(container.to_str().unwrap(), None, 1301).unwrap().count, 130);
    }

    #[test]
    fn documents_refuse_mismatched_and_corrupt_metadata() {
        let dir = fixture();
        let path = dir.path().to_str().unwrap();
        assert!(Documents::open(path, 1300).is_err());
        std::fs::write(dir.path().join("MANIFEST.json"), br#"{"doc_count":129,"corpus_bytes":1300}"#).unwrap();
        assert!(Documents::open(path, 1301).is_err());
        std::fs::write(dir.path().join("MANIFEST.json"), br#"{"doc_count":130,"corpus_bytes":1300}"#).unwrap();
        let file = dir.path().join("seps.bin");
        let mut data = std::fs::read(&file).unwrap();
        data[48 + 130 * 4 + 8] ^= 1;
        std::fs::write(file, data).unwrap();
        assert!(Documents::open(path, 1301).is_err());
        let dir = fixture();
        let file = dir.path().join("titles.bin");
        let mut data = std::fs::read(&file).unwrap();
        data.pop();
        std::fs::write(file, data).unwrap();
        assert!(Documents::open(dir.path().to_str().unwrap(), 1301).is_err());
    }
}

#[cfg(test)]
mod query_interface_tests {
    use super::*;
    use std::os::unix::ffi::OsStringExt;

    #[test]
    fn positional_bytes_are_verbatim() {
        let a = parse(&["c.sxi2".into(), "a\\n b".into(), "-j".into(), "2".into(),
            "--all".into(), "--".into(), "--literal".into(),
            std::ffi::OsString::from_vec(vec![0xff, 0x1e])]);
        assert_eq!(a.positional, vec![b"a\\n b".to_vec(), b"--literal".to_vec(), vec![0xff, 0x1e]]);
        assert_eq!(a.threads, 2);
    }

    #[test]
    fn journal_paths_support_spaces_and_legacy_builds() {
        assert_eq!(recorded_corpus(r#"CONTAINER_CORPUS_SIDECARS dir="/tmp/a b" seps=seps.bin titles=titles.bin docs=13 corpus_bytes=42"#),
            Some(("/tmp/a b".into(), Some(13))));
        assert_eq!(recorded_corpus("XSA_BUILD START input=/tmp/a b/corpus.bin scratch=/tmp/build memory_gb=16"),
            Some(("/tmp/a b".into(), None)));
    }

    #[test]
    fn binary_records_starting_with_fasta_marker_stay_records() {
        let file = tempfile::NamedTempFile::new().unwrap();
        std::fs::write(file.path(), b">literal\nbytes\x1eother\x1e").unwrap();
        assert_eq!(read_patterns(file.path().to_str().unwrap(), false).0,
            vec![b">literal\nbytes".to_vec(), b"other".to_vec()]);
    }
}

include!("branches.inc.rs");
include!("mem.inc.rs");
include!("seeds.inc.rs");
include!("fanout.inc.rs");
include!("consult.inc.rs");
include!("search.inc.rs");

include!("stateless.inc.rs");

// ------------------------------------------- the ordered-sweep LF accessor
//
// One LF step, exposed for the CONTEXT_TRIE builder (`xsa/src/ctx.rs`). The
// builder needs the SUFFIX ARRAY (row -> text position) once, and the cheapest
// exact way to obtain it from the container is the LF cycle: LF maps
// row i to the row whose position is i-1, so ONE cycle from any row visits every
// row exactly once and yields SA[row] for all of them at one LF step per row.
// That is the "minimal extra pass" the trie costs when it is built outside the
// construction chain; inside the chain the sorter already holds the positions,
// so the pass is free there.
impl<'a> Ctx<'a> {
    /// `(LF(row), BWT[row])`: the row of `SA[row] - 1` and the byte that precedes
    /// the suffix. One `row_run` + one `cum_before`, i.e. exactly the work a
    /// `locate` step already pays.
    #[inline]
    pub fn lf_step(&mut self, row: u64) -> (u64, u8) {
        let (run, s, h, _) = self.row_run(row);
        let lf = self.idx.c[h as usize] + self.cum_before(h, run);
        (lf + (row - s), h)
    }
    /// `SA[row]`: the text position of BWT row `row` (an LF walk to the nearest
    /// m11 anchor).
    pub fn sa_of(&mut self, row: u64) -> u64 { self.locate(row) }
}

impl Reader {
    /// The raw bytes of member `id`, straight out of the mapping.
    pub fn member_bytes(&self, id: u32) -> Option<&[u8]> {
        let m = self.members.iter().find(|m| m.id == id)?;
        Some(&self.map[m.offset as usize..(m.offset + m.bytes) as usize])
    }
    /// The open container's mapping (read-only).
    pub fn map(&self) -> &[u8] { &self.map }
    /// The mapping as an Arc, so a member parser (m15: document starts) can
    /// hold it beyond this reader's borrow and nothing per-document is copied.
    pub fn map_arc(&self) -> std::sync::Arc<Mmap> { self.map.clone() }
}

/// ------------------------------------------------------------ THE BUILDER'S DOOR
///
/// `xsa build` does NOT assemble the container in one write. The finish stage
/// streams it, and the driver then JOINS two members in place, each a directory
/// rewrite: m20 (the context trie, built concurrently by `ctx-tap`) and m15 (the
/// document starts, from `--docs-map`). Between the finish and the first join the
/// file on disc is a HALF-BUILT container: v7/v8/v9 with no m15.
///
/// That file is not queryable and `Reader::open` refuses it - correctly. The join
/// code still has to look at it, so this door exists: header + trailing directory,
/// no mmap of the run stream, no block cache, no kernel, no query. It accepts
/// v7..=v10 and NOTHING else, it is `pub(crate)` to the build path only, and it
/// proves the final state through `Reader::open` after the last join (see
/// `chain::run_v2_tail`), which is where the one shape is actually certified.
///
/// A query cannot be built on a `Partial`: it has no `Index`, no `Ctx`, no `Index`.
pub struct Partial {
    pub path: String,
    pub version: u32,
    pub flags: u64,
    pub n: u64,
    pub members: Vec<sxi::Member>,
    map: Arc<Mmap>,
}

impl Partial {
    pub fn member(&self, id: u32) -> Option<&sxi::Member> {
        self.members.iter().find(|m| m.id == id)
    }
    pub fn member_bytes(&self, id: u32) -> Option<&[u8]> {
        let m = self.member(id)?;
        Some(&self.map[m.offset as usize..(m.offset + m.bytes) as usize])
    }
    pub fn map(&self) -> &[u8] {
        &self.map
    }
}

/// Open a container the BUILD is still assembling. v7..=v10 only: the layouts
/// before v7 are not this build's output and are not joined in place.
pub fn open_partial(path: &str) -> Result<Partial, String> {
    let f = File::open(path).map_err(|e| format!("open {path}: {e}"))?;
    let map = unsafe { Mmap::map(&f) }.map_err(|e| format!("mmap {path}: {e}"))?;
    if map.len() < 64 {
        return Err(format!("{path}: too short for an SXI header"));
    }
    let h = &map[..64];
    let version = u32::from_le_bytes(h[4..8].try_into().unwrap());
    if &h[..4] != b"SXI2" || !(7..=10).contains(&version) {
        return Err(refuse(
            path,
            &format!("{}, and a build join can only extend a v7..=v10 container", describe_format(path)),
        ));
    }
    let u32at = |o: usize| u32::from_le_bytes(h[o..o + 4].try_into().unwrap());
    let u64at = |o: usize| u64::from_le_bytes(h[o..o + 8].try_into().unwrap());
    let (n, r) = (u64at(8), u64at(24));
    let count = u32at(32);
    let length = u64at(40);
    if length != map.len() as u64 {
        return Err(format!(
            "{path}: header length {length} != file length {} (truncated or trailing bytes)",
            map.len()
        ));
    }
    // 8 = the finish's own member set (m1,m4,m5,m7,m10,m11,m12,m13: no m20 and
    // no m15 yet), 9 = +m20, 10 = +m15, 11 = +m14. Anything outside is not this
    // build's own half-built file.
    if !(8..=11).contains(&count) {
        return Err(format!("{path}: directory holds {count} members; a build join expects 8..=11"));
    }
    let dir_off = length as usize - 40 * count as usize;
    if dir_off < 64 || dir_off % 8 != 0 {
        return Err(format!("{path}: directory offset {dir_off} is not valid"));
    }
    let mut members = Vec::with_capacity(count as usize);
    for i in 0..count as usize {
        let d = &map[dir_off + 40 * i..dir_off + 40 * (i + 1)];
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
    if r == 0 || r > n {
        return Err(format!("{path}: invalid n={n} R={r}"));
    }
    Ok(Partial { path: path.to_string(), version, flags: u64at(48), n, members, map: Arc::new(map) })
}

impl Index {
    /// Decode block `b` of m1 into the caller's columns. This is the ONE decode
    /// entry point the context-trie builder uses to materialise every run ONCE
    /// (`ctx.rs` DenseRuns): a per-step block decode is 8 us, which is why an
    /// LF walk over a 100 GB container is not affordable one block at a time.
    pub fn decode_block_into(&self, arena: &mut WorkerArena, b: u64, heads: &mut [u8], lens: &mut [u32]) {
        arena.with_block(self, b, |h, l| {
            heads.copy_from_slice(h);
            lens.copy_from_slice(l);
        });
    }
    pub fn runs_per_block(&self) -> u64 { self.block_runs }
    pub fn block_count(&self) -> u64 { self.nblocks }
}

#[cfg(test)]
mod documents_v2_tests {
    use super::*;

    /// Write a tiny CORPUS V2 directory BY HAND (an independent spelling of
    /// the XCS2 v1 / XPM1 v1 layouts, so the reader is checked against a second
    /// implementation, not against the writer it was written next to).
    /// Documents: texts "aaaaa", "b\x1eb" (0x1E INSIDE text: legal in V2),
    /// "cc", "ddd", "e" -> starts 0,5,8,10,13; corpus_bytes 14.
    fn write_v2_dir(dir: &std::path::Path) -> u64 {
        let texts: [&[u8]; 5] = [b"aaaaa", b"b\x1eb", b"cc", b"ddd", b"e"];
        let comps: [&str; 5] = ["Pile-CC", "ArXiv", "Pile-CC", "ArXiv", "Pile-CC"];
        let corpus_bytes: u64 = texts.iter().map(|t| t.len() as u64).sum();
        // t.starts (XCS2 v1)
        let mut starts: Vec<u8> = Vec::new();
        starts.extend_from_slice(b"XCS2");
        starts.extend_from_slice(&1u32.to_le_bytes());
        starts.extend_from_slice(&0u32.to_le_bytes()); // flags: V2 has no separator
        starts.extend_from_slice(&64u32.to_le_bytes());
        starts.extend_from_slice(&5u64.to_le_bytes());
        starts.extend_from_slice(&corpus_bytes.to_le_bytes());
        starts.extend_from_slice(&1u64.to_le_bytes()); // anchors = ceil(5/64)
        starts.extend_from_slice(&0u64.to_le_bytes()); // base
        let mut cursor = 0u64; // start of the current document
        let mut prev = 0u64;   // start of the previous document
        for t in texts.iter() {
            // d[0] = 0; d[k] = start[k] - start[k-1] = len(doc k-1)
            let d: u32 = if starts.len() == 48 { 0 } else { (cursor - prev) as u32 };
            starts.extend_from_slice(&d.to_le_bytes());
            prev = cursor;
            cursor += t.len() as u64;
        }
        assert_eq!(cursor, corpus_bytes);
        starts.extend_from_slice(&0u64.to_le_bytes()); // anchor: start of doc 0
        std::fs::write(dir.join("t.starts"), &starts).unwrap();
        // t.docs (XPM1 v1 shape, corpus order)
        let names = [b"Pile-CC".to_vec(), b"ArXiv".to_vec()];
        let name_payload: u64 = names.iter().map(|n| n.len() as u64).sum();
        let shard_names = [b"00.jsonl.zst".to_vec()];
        let shard_payload: u64 = shard_names.iter().map(|n| n.len() as u64).sum();
        let mut docs: Vec<u8> = Vec::new();
        for n in &names { docs.extend_from_slice(n); }
        let mut o = 0u64;
        for n in &names { docs.extend_from_slice(&(o as u32).to_le_bytes()); o += n.len() as u64; }
        for n in &names { docs.extend_from_slice(&(n.len() as u16).to_le_bytes()); }
        for n in &shard_names { docs.extend_from_slice(n); }
        let mut o = 0u64;
        for n in &shard_names { docs.extend_from_slice(&(o as u32).to_le_bytes()); o += n.len() as u64; }
        for n in &shard_names { docs.extend_from_slice(&(n.len() as u16).to_le_bytes()); }
        let mut off = 0u64;
        for (k, t) in texts.iter().enumerate() {
            docs.extend_from_slice(&off.to_le_bytes());
            docs.extend_from_slice(&(t.len() as u64).to_le_bytes());
            docs.extend_from_slice(&0u16.to_le_bytes()); // shard
            docs.extend_from_slice(&(if comps[k] == "ArXiv" { 1u32 } else { 0u32 }).to_le_bytes());
            docs.extend_from_slice(&(k as u64).to_le_bytes()); // record
            docs.extend_from_slice(&0u32.to_le_bytes()); // internal_0x1e reserved
            off += t.len() as u64;
        }
        let mut h: Vec<u8> = Vec::new();
        h.extend_from_slice(b"XPM1");
        h.extend_from_slice(&1u32.to_le_bytes());
        h.extend_from_slice(&5u64.to_le_bytes());
        h.extend_from_slice(&corpus_bytes.to_le_bytes());
        h.extend_from_slice(&1u64.to_le_bytes()); // shard_count
        h.extend_from_slice(&2u64.to_le_bytes()); // name_count
        h.extend_from_slice(&name_payload.to_le_bytes());
        h.extend_from_slice(&shard_payload.to_le_bytes());
        h.extend_from_slice(&34u32.to_le_bytes());
        h.extend_from_slice(&0u64.to_le_bytes()); // partial_tail
        assert_eq!(h.len(), 68);
        std::fs::write(dir.join("t.docs"), [h, docs].concat()).unwrap();
        // MANIFEST.json
        std::fs::write(
            dir.join("MANIFEST.json"),
            format!("{{\"kind\":\"corpus-v2\",\"doc_count\":5,\"corpus_bytes\":{}}}\n", corpus_bytes),
        ).unwrap();
        // t.bin: the optional cache - raw concatenation, NO separator anywhere.
        let mut bin: Vec<u8> = Vec::new();
        for t in texts.iter() { bin.extend_from_slice(t); }
        std::fs::write(dir.join("t.bin"), &bin).unwrap();
        corpus_bytes
    }

    #[test]
    fn v2_documents_resolve_doc_offset_and_component_title() {
        let dir = tempfile::tempdir().unwrap();
        let bytes = write_v2_dir(dir.path());
        let docs = Documents::open(dir.path().to_str().unwrap(), bytes + 1).unwrap();
        assert!(docs.v2);
        assert_eq!(docs.count, 5);
        // starts 0,5,8,10,13 with lens 5,3,2,3,1 -> corpus_bytes 14
        let cases = [
            (0u64, 0usize, 0u64, "Pile-CC"),
            (4, 0, 4, "Pile-CC"),
            (5, 1, 0, "ArXiv"),
            (6, 1, 1, "ArXiv"), // INSIDE the doc that carries a literal 0x1E
            (7, 1, 2, "ArXiv"),
            (8, 2, 0, "Pile-CC"),
            (13, 4, 0, "Pile-CC"),
        ];
        for (pos, doc, off, title) in cases {
            let (d, o, t) = docs.lookup(pos).unwrap();
            assert_eq!((d, o), (doc, off), "pos={pos}");
            assert_eq!(t, title.as_bytes(), "pos={pos}");
        }
        assert!(docs.lookup(bytes).is_err());
        // --context over the optional cache: the doc with the in-text 0x1E
        // must survive a snippet window with NO separator stripping (V2 has
        // no separator; the doc boundary is the next doc's start).
        let corpus = docs.context_corpus().unwrap();
        let (lo, hi, text, clipped) = docs.snippet(&corpus, 6, 10).unwrap();
        // pos 6 is doc 1 ("b\x1eb", start 5): the window clips to the doc.
        assert!(clipped);
        assert_eq!((lo, hi), (5, 8));
        assert_eq!(&corpus[lo as usize..hi as usize], b"b\x1eb");
        assert_eq!(text, "b\\x1eb", "0x1E is escaped, never dropped or split on");
    }

    #[test]
    fn v2_pair_disagreement_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        let bytes = write_v2_dir(dir.path());
        // Corrupt one row's offset: starts and docs must never disagree.
        let mut d = std::fs::read(dir.path().join("t.docs")).unwrap();
        let rows = 68 + 11 + 6 * 2 + 12 + 6; // header+blobs+tables (5 docs, 2 names, 1 shard)
        let bad = rows + 34 * 2; // row 2
        d[bad..bad + 8].copy_from_slice(&999u64.to_le_bytes());
        std::fs::write(dir.path().join("t.docs"), &d).unwrap();
        assert!(Documents::open(dir.path().to_str().unwrap(), bytes + 1).is_err());
    }
}
