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

use crate::{die, sxi};
use memmap2::Mmap;
use rayon::prelude::*;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::sync::Arc;
use std::time::Instant;

/// Run-start sample stride (runs per level-1 entry).
const BS: u64 = 64;
/// Level-2 sample stride (runs per level-2 entry), a multiple of BS.
const L2: u64 = 65536;
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
        if !(5..=7).contains(&count) || hs != 64 + 40 * count as usize {
            return Err(format!("{path}: v6 directory must hold 5-7 members"));
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
    /// Total occurrences of each symbol.
    pub total: [u64; 256],
    heads: Vec<u8>,
    rlen: Vec<u32>,
    /// Count of `heads[i]` in runs strictly before run `i`.
    seen: Seen,
    /// run_start(j * BS) for j in 0..=R/BS
    blk: Vec<u64>,
    /// run_start(j * L2)
    blk2: Vec<u64>,
    anchors: Vec<u64>,
    stride: u64,
    pub build_ms: f64,
    /// Bytes of m1 actually decoded into the kernel (the run stream).
    pub decoded_bytes: u64,
    /// Per-stage build walls, in order.
    pub stages: Vec<(&'static str, f64)>,
}

/// Per-run cumulative own-symbol counts. u32 covers every container whose
/// most frequent symbol occurs < 2^32 times, which is all of them up to
/// n ~ 4e9 per symbol; the u64 form exists so a 1e10-byte pile with a
/// dominant symbol (e.g. one byte > 43% of the corpus) still answers exactly
/// instead of refusing.
enum Seen {
    U32(Vec<u32>),
    U64(Vec<u64>),
}

impl Seen {
    #[inline(always)]
    fn get(&self, i: usize) -> u64 {
        match self {
            Seen::U32(v) => v[i] as u64,
            Seen::U64(v) => v[i],
        }
    }
}

/// Decode member 1 (codec 101) into run symbols and run lengths.
fn decode_runs(
    reader: &Reader,
    threads: usize,
) -> Result<(Vec<u8>, Vec<u32>, [u64; 256], [u64; 256]), String> {
    let m = reader.member(1);
    if m.codec != 101 {
        return Err(format!("m1 codec {} is not 101", m.codec));
    }
    let r = reader.r;
    let data = reader.slice(m.offset, m.bytes);
    if data.len() < 2320 {
        return Err("m1 member too short for its header".into());
    }
    let u64at = |o: usize| u64::from_le_bytes(data[o..o + 8].try_into().unwrap());
    let mut c = [0u64; 256];
    for (i, v) in c.iter_mut().enumerate() {
        *v = u64at(8 * i);
    }
    let mut lens = [0u8; 256];
    lens.copy_from_slice(&data[2048..2304]);
    let hbits = u64at(2304);
    let lbits = u64at(2312);
    let hbytes = (hbits + 7) / 8;
    let lbytes = (lbits + 7) / 8;
    if m.bytes != 2320 + hbytes + lbytes {
        return Err(format!("m1 size {} != 2320+{hbytes}+{lbytes}", m.bytes));
    }
    if hbits < r || lbits < r {
        return Err("m1 bit counts shorter than the run count".into());
    }
    let head_bits = Bits::new(&data[2320..2320 + hbytes as usize], hbits);
    let len_bits = Bits::new(&data[2320 + hbytes as usize..], lbits);
    let huff = Huff::build(&lens)?;
    let _ = threads;

    let mut heads: Vec<u8> = vec![0; r as usize];
    let mut rlen: Vec<u32> = vec![0; r as usize];
    let (a, b) = rayon::join(
        || {
            let mut bits = head_bits;
            for h in heads.iter_mut() {
                *h = huff.decode(&mut bits);
            }
            if bits.pos != hbits {
                die(&format!(
                    "m1 head stream: consumed {} of {hbits} bits",
                    bits.pos
                ));
            }
        },
        || {
            let mut bits = len_bits;
            let mut sum = 0u64;
            let gamma = Gamma::build();
            for l in rlen.iter_mut() {
                let v = gamma.decode(&mut bits);
                if v == 0 {
                    die("m1 run length 0");
                }
                *l = v;
                sum = sum
                    .checked_add(v as u64)
                    .unwrap_or_else(|| die("m1 run length sum overflow"));
            }
            if bits.pos != lbits {
                die(&format!(
                    "m1 length stream: consumed {} of {lbits} bits",
                    bits.pos
                ));
            }
            sum
        },
    );
    if b != reader.n {
        return Err(format!("m1 run lengths sum to {b}, header n={}", reader.n));
    }
    // Its own pass: symbol totals weighted by run length, computed in
    // parallel. The C table is the cumulative version of exactly this array,
    // so agreeing with it is the strongest available proof that both bit
    // streams are aligned, complete and assigned to the right symbols.
    let chunks = threads.clamp(1, 4096);
    let chunk = (r as usize + chunks - 1) / chunks;
    let total = (0..((r as usize + chunk - 1) / chunk))
        .into_par_iter()
        .map(|ci| {
            let lo = ci * chunk;
            let hi = ((ci + 1) * chunk).min(r as usize);
            let mut acc = [0u64; 256];
            for i in lo..hi {
                acc[heads[i] as usize] += rlen[i] as u64;
            }
            acc
        })
        .reduce(|| [0u64; 256], |mut x, y| {
            for k in 0..256 {
                x[k] += y[k];
            }
            x
        });
    let mut acc = 0u64;
    for ch in 0..256 {
        if c[ch] != acc {
            return Err(format!("m1 C table entry {ch} is {}, expected {acc}", c[ch]));
        }
        acc += total[ch];
    }
    if acc != reader.n {
        return Err(format!("m1 symbol counts sum to {acc}, header n={}", reader.n));
    }
    Ok((heads, rlen, c, total))
}

impl Index {
    /// Build the query kernel from an already-mmap'd reader.
    ///
    /// This is the ONLY stage that touches run data, and its cost is
    /// irreducible for this container format: m1 is one sequential Huffman
    /// bit stream (run symbols) plus one sequential gamma bit stream (run
    /// lengths) with no per-run alignment and no restart directory, so run `i`
    /// cannot be reached without decoding runs 0..i. A query needs rank at
    /// arbitrary rows and an LF walk over arbitrary rows, so a cold query
    /// needs the whole run stream. What this kernel removes from the legacy
    /// path is everything else: no full-file CRC, no chi decode, no phi, no
    /// second decode, no 256-way `cruns` transpose, no per-run LF-start array
    /// of the same width as `n`.
    pub fn open(reader: Reader, threads: usize) -> Result<Index, String> {
        let t0 = Instant::now();
        let t_open = t0.elapsed();
        let r = reader.r;
        let (heads, rlen, c, total) = decode_runs(&reader, threads)?;
        let t_decode = t0.elapsed();
        let decode_bytes = reader.member(1).bytes;
        // seen[i] = occurrences of heads[i] in runs strictly before run i.
        // u32 is enough unless one symbol occurs more than 2^32 times
        // (n > 2^32 with a skewed alphabet); then the u64 form is used.
        let wide = total.iter().any(|&t| t > u32::MAX as u64);
        let chunks = threads.clamp(1, 4096);
        let chunk = (r as usize + chunks - 1) / chunks;
        let chunks = (r as usize + chunk - 1) / chunk;
        let counts: Vec<Box<[u64; 256]>> = (0..chunks)
            .into_par_iter()
            .map(|ci| {
                let lo = ci * chunk;
                let hi = ((ci + 1) * chunk).min(r as usize);
                let mut cnt = Box::new([0u64; 256]);
                for i in lo..hi {
                    cnt[heads[i] as usize] += rlen[i] as u64;
                }
                cnt
            })
            .collect();
        let mut base: Vec<Box<[u64; 256]>> = Vec::with_capacity(chunks);
        let mut acc = [0u64; 256];
        for cnt in &counts {
            let mut b = Box::new([0u64; 256]);
            for k in 0..256 {
                b[k] = acc[k];
                acc[k] += cnt[k];
            }
            base.push(b);
        }
        for ch in 0..256 {
            if acc[ch] != total[ch] {
                return Err(format!("run-length per-symbol total mismatch at {ch}"));
            }
        }
        let seen = if wide {
            let mut v = vec![0u64; r as usize];
            v.par_chunks_mut(chunk)
                .enumerate()
                .for_each(|(ci, out)| {
                    let mut run = base[ci].clone();
                    let lo = ci * chunk;
                    for (j, s) in out.iter_mut().enumerate() {
                        let ch = heads[lo + j] as usize;
                        *s = run[ch];
                        run[ch] += rlen[lo + j] as u64;
                    }
                });
            Seen::U64(v)
        } else {
            let mut v = vec![0u32; r as usize];
            v.par_chunks_mut(chunk)
                .enumerate()
                .for_each(|(ci, out)| {
                    let mut run = base[ci].clone();
                    let lo = ci * chunk;
                    for (j, s) in out.iter_mut().enumerate() {
                        let ch = heads[lo + j] as usize;
                        *s = run[ch] as u32;
                        run[ch] += rlen[lo + j] as u64;
                    }
                });
            Seen::U32(v)
        };
        drop(counts);
        drop(base);
        let t_rank = t0.elapsed();

        // Run-start samples.
        let nblk2 = (r / L2 + 1) as usize;
        let nblk = (r / BS + 1) as usize;
        let sums: Vec<u64> = (0..nblk2)
            .into_par_iter()
            .map(|j| {
                let lo = (j as u64 * L2) as usize;
                let hi = (((j as u64 + 1) * L2).min(r)) as usize;
                let mut s = 0u64;
                for i in lo..hi {
                    s += rlen[i] as u64;
                }
                s
            })
            .collect();
        let mut blk2: Vec<u64> = Vec::with_capacity(nblk2);
        let mut acc = 0u64;
        for &s in &sums {
            blk2.push(acc);
            acc += s;
        }
        if acc != reader.n {
            return Err(format!("run-start samples sum to {acc}, header n={}", reader.n));
        }
        let mut blk: Vec<u64> = vec![0; nblk];
        blk.par_chunks_mut(L2 as usize / BS as usize)
            .enumerate()
            .for_each(|(j, out)| {
                let mut s = blk2[j];
                let mut run = j as u64 * L2;
                for slot in out.iter_mut() {
                    *slot = s;
                    let end = (run + BS).min(r);
                    while run < end {
                        s += rlen[run as usize] as u64;
                        run += 1;
                    }
                }
            });
        let t_blk = t0.elapsed();

        // m11: tail SA of every 1024th run.
        let m = reader.member(11);
        let a = reader.slice(m.offset, m.bytes);
        if m.count != (r + 1023) / 1024 || m.bytes != 16 + 8 * m.count {
            return Err(format!("m11 anchor size {} / count {}", m.bytes, m.count));
        }
        let exponent = u64::from_le_bytes(a[0..8].try_into().unwrap());
        let stride = 1u64 << exponent;
        if stride != 1024 || u64::from_le_bytes(a[8..16].try_into().unwrap()) != m.count {
            return Err(format!(
                "m11 anchor header mismatch: stride {stride}, count field {}, directory {}",
                u64::from_le_bytes(a[8..16].try_into().unwrap()),
                m.count
            ));
        }
        let anchors: Vec<u64> = (0..m.count as usize)
            .map(|i| u64::from_le_bytes(a[16 + 8 * i..24 + 8 * i].try_into().unwrap()))
            .collect();
        for (i, &v) in anchors.iter().enumerate() {
            if v >= reader.n {
                return Err(format!("m11 anchor {i} value {v} outside [0,n)"));
            }
        }
        let t_end = t0.elapsed();
        let stages = vec![
            ("m1-runs", (t_decode - t_open).as_secs_f64() * 1e3),
            ("rank-dir", (t_rank - t_decode).as_secs_f64() * 1e3),
            ("run-starts", (t_blk - t_rank).as_secs_f64() * 1e3),
            ("m11-anchors", (t_end - t_blk).as_secs_f64() * 1e3),
        ];
        let build_ms = t_end.as_secs_f64() * 1e3;
        if crate::trace_on() {
            for (name, ms) in &stages {
                eprintln!("TRACE stage=lite.{name} ms={ms:.1}");
            }
            eprintln!("TRACE stage=lite.build-total ms={build_ms:.1}");
        }
        Ok(Index {
            reader,
            c,
            total,
            heads,
            rlen,
            seen,
            blk,
            blk2,
            anchors,
            stride,
            build_ms,
            decoded_bytes: decode_bytes,
            stages,
        })
    }
}

impl Index {
    #[inline]
    pub fn n(&self) -> u64 {
        self.reader.n
    }
    #[inline]
    pub fn r(&self) -> u64 {
        self.reader.r
    }
    /// Text position of run `run`'s first row (LF start).
    #[inline(always)]
    fn start(&self, run: u64) -> u64 {
        self.c[self.heads[run as usize] as usize] + self.seen.get(run as usize)
    }
    /// `(run, run_start)` of the run holding row `i`.
    #[inline]
    fn run_of(&self, i: u64) -> (u64, u64) {
        let j = self.blk2.partition_point(|&s| s <= i) - 1;
        let l1_lo = j as u64 * (L2 / BS);
        let l1_hi = (l1_lo + L2 / BS).min(self.blk.len() as u64);
        let k = l1_lo as usize + self.blk[l1_lo as usize..l1_hi as usize].partition_point(|&s| s <= i) - 1;
        let mut run = k as u64 * BS;
        let mut s = self.blk[k];
        while run + 1 < self.reader.r && s + self.rlen[run as usize] as u64 <= i {
            s += self.rlen[run as usize] as u64;
            run += 1;
        }
        (run, s)
    }
    /// Occurrences of `ch` in the BWT before row `i` (0-based within `ch`).
    #[inline]
    fn rank(&self, ch: u8, i: u64) -> u64 {
        if i == 0 {
            return 0;
        }
        if i >= self.reader.n {
            return self.total[ch as usize];
        }
        let (run, s) = self.run_of(i);
        if self.heads[run as usize] == ch {
            return self.seen.get(run as usize) + (i - s);
        }
        // Walk back to the nearest run of `ch`: ~|alphabet| steps, contiguous.
        let mut p = run;
        while p > 0 {
            p -= 1;
            if self.heads[p as usize] == ch {
                return self.seen.get(p as usize) + self.rlen[p as usize] as u64;
            }
        }
        0
    }
    /// One backward-search step.
    #[inline]
    fn step(&self, l: u64, r: u64, ch: u8) -> (u64, u64) {
        let base = self.c[ch as usize];
        (base + self.rank(ch, l), base + self.rank(ch, r))
    }
    /// Half-open BWT interval of the pattern, consumed left to right.
    pub fn search(&self, pat: &[u8]) -> (u64, u64) {
        let mut l = 0u64;
        let mut r = self.reader.n;
        for &ch in pat {
            let (nl, nr) = self.step(l, r, ch);
            l = nl;
            r = nr;
            if l >= r {
                return (l, r);
            }
        }
        (l, r)
    }
    /// Text position of BWT row `row`.
    ///
    /// m11 stores the *raw* SA coordinate of the tail row of runs 0, 1024,
    /// 2048, ... (bit6/xsa_finish.cpp: `if (idx % 1024 == 0) write(v.t)`), in
    /// the same coordinate system as the container's `n`. LF maps row i to the
    /// row whose SA is i-1, so walking LF `steps` times RAISES the text
    /// position by `steps`: `position = anchor + steps (mod n)`.
    ///
    /// Verified on t150m.sxi2: 574/574 oracle positions across all 100
    /// fixture patterns (mean 2650 steps, max 24437), see
    /// tools/binbwt/gate_qlite_150m.sh.
    pub fn locate(&self, row: u64) -> u64 {
        let mut pos = row;
        let mut steps = 0u64;
        loop {
            let (run, s) = self.run_of(pos);
            let end = s + self.rlen[run as usize] as u64;
            if pos == end - 1 && run % self.stride == 0 {
                let anchor = self.anchors[(run / self.stride) as usize];
                return (anchor + steps) % self.reader.n;
            }
            pos = self.start(run) + (pos - s);
            steps += 1;
            if steps >= self.reader.n {
                die("locate: LF walk has no anchored sample");
            }
        }
    }
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
        "QLITE_BUILD runs={} decoded_bytes={} stages=[{}] build_ms={:.1} n={} threads={} patterns={}",
        index.r(),
        index.decoded_bytes,
        stage_list.join(" "),
        index.build_ms,
        index.n(),
        a.threads,
        patterns.len()
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
            let ts = Instant::now();
            let (l, r) = index.search(&work);
            let search_us = ts.elapsed().as_secs_f64() * 1e6;
            let tl = Instant::now();
            let mut positions = Vec::with_capacity((r - l).min(1 << 20) as usize);
            for row in l..r {
                positions.push(index.locate(row));
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
    for (pid, hit) in results.iter().enumerate() {
        let us = hit.search_us + hit.locate_us * hit.positions.len() as f64;
        if crate::trace_on() {
            eprintln!(
                "TRACE_PATTERN id={} len={} occ={} search_us={:.1} locate_us={:.1} total_us={:.1}",
                pid,
                patterns[pid].len(),
                hit.positions.len(),
                hit.search_us,
                hit.locate_us,
                us
            );
        }
        if hit.positions.len() <= 3 {
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
         low_count_patterns={} low_count_mean_us={:.1} low_count_max_us={:.1} high_count_patterns={}",
        patterns.len(),
        nocc,
        nabs,
        elapsed.as_secs_f64() * 1e3,
        first_answer_ms,
        nlow_us,
        if nlow_us > 0 { low_sum_us / nlow_us as f64 } else { f64::NAN },
        worst_us,
        high
    );
}
