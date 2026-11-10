//! **m14: the LCP over runs** (member codec 117) - the accessor.
//!
//! WHY THIS MEMBER EXISTS. The suffix tree IS the LCP-interval tree: a node is a
//! distinct context, its children are contiguous in SA order, and the children of
//! a node of depth d are delimited by the ranks where LCP drops below d. The
//! finish already computes that column and used to throw it away. m14 persists it,
//! one value per BWT RUN, in run order:
//!
//! ```text
//! lcpRuns[i] = LCP[r_i] = lce(SA[r_i - 1], SA[r_i]),  r_i = sum(len[0..i-1])
//! ```
//!
//! i.e. the LEFT-neighbour LCP at run i's HEAD row (`lcpRuns[0] = 0` is the
//! array's conventional sentinel: row 0 has no left neighbour). r-space is the
//! point: R is ~3x smaller than n at the Pile (31,381,597,694 vs 100,093,934,467).
//!
//! WHAT IS **NOT** HERE: the LCP of the rows INSIDE a run. A run's interior rows
//! carry their own drops (an all-equal text is ONE run whose interior is the whole
//! tree), so this member delimits children at RUN granularity. The finish computes
//! that interior minimum for chi; it is not part of m14.
//!
//! ## The format (what this file parses, and the C++ writer emits)
//!
//! ```text
//! member m14, codec 117:
//!   [0,40)   head:  block_runs u64 | n u64 | payload_off u64 (=40) |
//!                   count u64 (0 = derived = the member count) |
//!                   enc_version u32 (=1) | codec u32 (=117)
//!   [40, ..) payload: one independently decodable block per `block_runs` runs
//!   [.., bytes) directory: 16 bytes per block = off u64 | span u32 | escapes u32
//! ```
//!
//! The directory is TRAILING (the v7 rule: a directory the writer cannot know
//! before the bytes exists cannot be a prefix), so `nblocks = ceil(R/block_runs)`
//! and it starts at `bytes - 16*nblocks`.
//!
//! A block is `mode u8` then:
//!
//! ```text
//! mode 0  every value is zero                    -> the block is 1 byte (padded)
//! mode 1  u8 array, value < 255 inline, 255 = escape
//! mode 2  width u8 (1..=32) then the values packed little-endian at that width,
//!         the all-ones field = escape
//! then    esc_count u32, then esc_count * { u16 value index, u64 value }
//! then    zero padding to an 8-byte multiple
//! ```
//!
//! Both schemes carry escaped values as full u64, so nothing is ever truncated.
//!
//! ## Random access (the point of the layout)
//!
//! One 16-byte directory entry (`dir[i/block_runs]`) + ONE bit slice (or one
//! byte) at a computed offset + a binary search in that block's escape list. Never
//! a scan over the block, never a walk over preceding blocks. `LCP_ACCESS` reports
//! the bytes touched per answer.

use super::{die, qlite};

/// Two independent implementations must agree on every field name, so the
/// constants the C++ writer uses are restated here and CHECKED at open rather
/// than assumed.
pub const PF_LCP_CODEC: u32 = 117;
pub const PF_LCP_ENC_VERSION: u32 = 1;
pub const PF_LCP_HEAD: u64 = 40;
/// Container header flag bit 7: "member m14 (LCP over runs) is present".
pub const PF_FLAG_LCP_RUNS: u64 = 128;
/// A block index is a u16 in the escape list; a wider block cannot address its
/// own escapes, so the reader refuses one by name.
const BLOCK_RUNS_MAX: u64 = 1 << 16;

fn need(ok: bool, msg: &str) -> Result<(), String> {
    if ok { Ok(()) } else { Err(format!("m14 (LCP over runs): {msg}")) }
}
fn u32_at(d: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(d[o..o + 4].try_into().unwrap())
}
fn u64_at(d: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(d[o..o + 8].try_into().unwrap())
}

/// One block's directory entry: where its bytes are, how many, how many escapes.
#[derive(Clone, Copy, Debug)]
pub struct BlockEntry {
    pub off: u64,
    pub span: u32,
    pub escapes: u32,
}

pub struct LcpRuns<'a> {
    map: &'a [u8],
    /// member id 14's bytes: [offset, offset+bytes)
    pub member_off: u64,
    pub member_bytes: u64,
    /// n of the text the LCP array is over (must equal the container's n).
    pub n: u64,
    /// runs (= the member's count).
    pub r: u64,
    pub block_runs: u64,
    pub nblocks: u64,
    /// absolute file offset of the payload and of the trailing directory.
    pub payload_off: u64,
    pub dir_off: u64,
    pub payload_bytes: u64,
    /// value counts per mode, filled by `audit` (0 before it runs).
    pub modes: [u64; 3],
}

impl<'a> LcpRuns<'a> {
    /// Parse m14 out of an opened (mmap'd) container. Nothing is scanned: the
    /// cost is one 40-byte head read and the directory's geometry.
    pub fn open(reader: &'a qlite::Reader) -> Result<LcpRuns<'a>, String> {
        let m = match reader.members.iter().find(|m| m.id == 14) {
            Some(m) => m,
            None => {
                return Err(format!(
                    "{}: container has no member m14 (LCP over runs){}: it was built with\n  \
                     --no-lcp-runs or by the v6/v7 legacy layout, so the tree's child\n  \
                     delimiters are not in it. Rebuild with `xsa build --tape` (SXI2 v8).",
                    reader.path,
                    if reader.flags & PF_FLAG_LCP_RUNS != 0 {
                        " although header flag bit 7 DECLARES it (corrupt header)"
                    } else {
                        ""
                    }
                ))
            }
        };
        need(reader.flags & PF_FLAG_LCP_RUNS != 0, "flag 128 (lcp-runs present) is not set in the container header; refusing to read a member the header does not declare")?;
        need(m.codec == PF_LCP_CODEC, &format!("member codec {} is not 117", m.codec))?;
        need(m.bytes >= PF_LCP_HEAD + 16, &format!("member is {} bytes, shorter than an empty directory", m.bytes))?;
        let map = reader.map_bytes();
        let end = m.offset + m.bytes;
        need(end <= map.len() as u64, "member runs past the end of the file")?;
        let h = &map[m.offset as usize..(m.offset + PF_LCP_HEAD) as usize];
        let block_runs = u64_at(h, 0);
        let n = u64_at(h, 8);
        let payload_off = u64_at(h, 16);
        let count = u64_at(h, 24);
        let enc_version = u32_at(h, 32);
        let codec = u32_at(h, 36);
        need(codec == PF_LCP_CODEC, &format!("head codec {codec} is not 117"))?;
        need(enc_version == PF_LCP_ENC_VERSION, &format!("head encoding version {enc_version} is not {PF_LCP_ENC_VERSION} (this reader decodes version 1 only)"))?;
        need(block_runs >= 1 && block_runs <= BLOCK_RUNS_MAX, &format!("block_runs {block_runs} outside 1..={BLOCK_RUNS_MAX}"))?;
        need(payload_off == PF_LCP_HEAD, &format!("payload_off {payload_off} is not {PF_LCP_HEAD} (the head is fixed and never patched)"))?;
        need(n == reader.n, &format!("head n {n} != container n {}", reader.n))?;
        need(count == 0 || count == reader.r, &format!("head count {count} is neither 0 (derived) nor the container's run count {}", reader.r))?;
        let r = reader.r;
        need(m.count == r, &format!("member count {} != container runs {r}", m.count))?;
        let nblocks = r.div_ceil(block_runs);
        need(m.bytes >= PF_LCP_HEAD + 16 * nblocks, &format!(
            "member is {} bytes but {nblocks} blocks need {PF_LCP_HEAD} + 16*{nblocks} = {} just for the directory",
            m.bytes, PF_LCP_HEAD + 16 * nblocks))?;
        let payload_bytes = m.bytes - PF_LCP_HEAD - 16 * nblocks;
        Ok(LcpRuns {
            map,
            member_off: m.offset,
            member_bytes: m.bytes,
            n,
            r,
            block_runs,
            nblocks,
            payload_off: m.offset + PF_LCP_HEAD,
            dir_off: end - 16 * nblocks,
            payload_bytes,
            modes: [0; 3],
        })
    }

    #[inline]
    fn entry(&self, block: u64) -> BlockEntry {
        debug_assert!(block < self.nblocks);
        let o = (self.dir_off + 16 * block) as usize;
        BlockEntry { off: u64_at(self.map, o), span: u32_at(self.map, o + 8), escapes: u32_at(self.map, o + 12) }
    }
    #[inline]
    fn block_slice(&self, e: BlockEntry) -> &'a [u8] {
        let a = (self.payload_off + e.off) as usize;
        &self.map[a..a + e.span as usize]
    }
    /// How many values block `block` holds (the last block is short).
    #[inline]
    pub fn block_vals(&self, block: u64) -> u64 {
        (self.r - block * self.block_runs).min(self.block_runs)
    }

    /// THE 16-BYTE DIRECTORY ENTRY OF RUN `run`.
    #[inline]
    pub fn entry_of(&self, run: u64) -> BlockEntry {
        self.entry(run / self.block_runs)
    }

    /// Decode value #`j` of one block - the random-access rule, and nothing else.
    ///
    /// `b` is the whole block (mode byte first), `n` its value count, `j` the
    /// index inside the block. One field read then a binary search; the bit
    /// extraction reads EXACTLY the bytes the field occupies (the writer's
    /// padding is never relied on).
    pub fn decode(b: &[u8], n: u64, j: u64) -> Result<u64, String> {
        need(j < n, &format!("value index {j} outside a block of {n}"))?;
        let mode = b[0];
        if mode == 0 {
            return Ok(0);
        }
        let (inline_val, escaped, esc_at) = match mode {
            1 => {
                need(b.len() >= 1 + n as usize, "u8 block is shorter than its value count")?;
                let v = u64::from(b[1 + j as usize]);
                (v, v == 255, 1 + n as usize)
            }
            2 => {
                need(b.len() >= 2, "packed block has no width byte")?;
                let w = u32::from(b[1]);
                need(w >= 1 && w <= 32, &format!("packed width {w} outside 1..=32"))?;
                let bit = j * u64::from(w);
                let at = 2 + (bit / 8) as usize;
                let sh = (bit % 8) as u32;
                let have = ((sh + w + 7) / 8) as usize;
                need(at + have <= b.len(), "packed field runs past the block")?;
                let mut acc = 0u128;
                for k in 0..have {
                    acc |= (b[at + k] as u128) << (8 * k);
                }
                let mask = (1u128 << w) - 1;
                let v = ((acc >> sh) & mask) as u64;
                (v, v == mask as u64, 2 + (n * u64::from(w)).div_ceil(8) as usize)
            }
            other => return Err(format!("m14 (LCP over runs): block mode {other} is not 0/1/2")),
        };
        if !escaped {
            return Ok(inline_val);
        }
        need(esc_at + 4 <= b.len(), "escape list is past the block")?;
        let count = u64::from(u32_at(b, esc_at));
        need(count <= n, &format!("escape count {count} exceeds the block's {n} values"))?;
        let list = esc_at + 4;
        need(list + 10 * count as usize <= b.len(), "escape list runs past the block")?;
        let (mut lo, mut hi) = (0u64, count);
        while lo < hi {
            let mid = (lo + hi) / 2;
            let at = list + 10 * mid as usize;
            let idx = u64::from(u16::from_le_bytes(b[at..at + 2].try_into().unwrap()));
            if idx == j {
                return Ok(u64_at(b, at + 2));
            }
            if idx < j {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        Err(format!("m14 (LCP over runs): value {j} of a block is marked escaped but has no entry in the block's escape list"))
    }

    /// `lcpRuns[run]` - the LCP at run `run`'s HEAD row. One directory entry, one
    /// field, one binary search.
    pub fn lcp(&self, run: u64) -> Result<u64, String> {
        need(run < self.r, &format!("run {run} outside [0,{})", self.r))?;
        let block = run / self.block_runs;
        let e = self.entry(block);
        need(e.span >= 1 && e.off + u64::from(e.span) <= self.payload_bytes, &format!(
            "block {block}'s directory entry (off={} span={}) does not lie inside the {}-byte payload",
            e.off, e.span, self.payload_bytes))?;
        Self::decode(self.block_slice(e), self.block_vals(block), run % self.block_runs)
    }

    /// Bytes of the member a single `lcp()` answer touches: one directory entry
    /// plus one block. This is the number the "few bytes of decode" claim means.
    pub fn bytes_per_answer(&self, run: u64) -> u64 {
        16 + u64::from(self.entry(run / self.block_runs).span)
    }

    /// THE STRUCTURAL AUDIT: the directory must tile the payload exactly, every
    /// span must be an 8-byte multiple, the escape counts must be printable, and
    /// every block's escape list must be sorted (a binary search over an unsorted
    /// list is a wrong answer that looks like a right one).
    ///
    /// This READS the directory and every block's escape list - it is the
    /// verification pass, not the open path (`decoded_bytes=0` stays true for
    /// `xsa query`). Returns (blocks checked, values addressed).
    pub fn audit(&mut self) -> Result<(u64, u64), String> {
        let mut at = 0u64;
        let mut vals = 0u64;
        self.modes = [0; 3];
        for b in 0..self.nblocks {
            let e = self.entry(b);
            need(e.off == at, &format!("block {b}'s payload offset {} != the tiled frontier {at}", e.off))?;
            need(e.span >= 1 && e.span % 8 == 0, &format!("block {b} spans {} bytes (must be a nonzero multiple of 8)", e.span))?;
            need(e.off + u64::from(e.span) <= self.payload_bytes, &format!("block {b} runs past the payload"))?;
            let n = self.block_vals(b);
            vals += n;
            let s = self.block_slice(e);
            let mode = s[0];
            need(mode <= 2, &format!("block {b}: mode {mode} is not 0/1/2"))?;
            self.modes[mode as usize] += 1;
            if mode != 0 {
                // the escape list follows the inline field; check it is sorted
                let esc_at = match mode {
                    1 => 1 + n as usize,
                    _ => {
                        let w = u32::from(s[1]);
                        need(w >= 1 && w <= 32, &format!("block {b}: width {w} outside 1..=32"))?;
                        need(s.len() >= 1 + 1 + (n * u64::from(w)).div_ceil(8) as usize, &format!("block {b}: packed array is truncated"))?;
                        2 + (n * u64::from(w)).div_ceil(8) as usize
                    }
                };
                need(esc_at + 4 <= s.len(), &format!("block {b}: escape count is past the block"))?;
                let count = u64::from(u32_at(s, esc_at));
                need(count <= n, &format!("block {b}: escape count {count} exceeds its {n} values"))?;
                need(count == u64::from(e.escapes), &format!("block {b}: directory says {} escapes, the block says {count}", e.escapes))?;
                let list = esc_at + 4;
                need(list + 10 * count as usize <= s.len(), &format!("block {b}: escape list runs past the block"))?;
                let mut prev = None;
                for k in 0..count as usize {
                    let idx = u64::from(u16::from_le_bytes(s[list + 10 * k..list + 10 * k + 2].try_into().unwrap()));
                    need(idx < n, &format!("block {b}: escape index {idx} outside its {n} values"))?;
                    if let Some(p) = prev {
                        need(idx > p, &format!("block {b}: escape indices are not strictly increasing ({p} then {idx})"))?;
                    }
                    prev = Some(idx);
                }
                // the escape field must not be reachable for an inline value: a
                // decoded all-ones field IS an escape, so a mode-1/2 block always
                // has at least one entry in its escape list.
                need(count > 0, &format!("block {b}: mode {mode} with zero escapes cannot exist (an all-ones field is an escape)"))?;
            }
            at += u64::from(e.span);
        }
        need(at == self.payload_bytes, &format!("the directory tiles {at} payload bytes but the member holds {}", self.payload_bytes))?;
        Ok((self.nblocks, vals))
    }
}

// ---------------------------------------------------------------- the corpus

/// The brute-force LCE oracle: the corpus IS the text (plus one VIRTUAL sentinel
/// symbol at position `filesize`, which is unique and minimal - bit6/xsa_finish.cpp
/// `CorpusText`). The container's text is `T[i] = REMAP[corpus[i]]` for i <
/// filesize and `T[filesize] = 0`, and REMAP is an ORDER-PRESERVING bijection on
/// the present bytes, so `T[i] == T[j] <=> corpus[i] == corpus[j]`: comparing raw
/// corpus bytes gives the same LCP as the pipeline's symbol space, with no remap
/// table, no sidecar and no second copy of the text.
pub struct Corpus {
    map: memmap2::Mmap,
    pub filesize: u64,
}
impl Corpus {
    pub fn open(path: &str) -> Result<Corpus, String> {
        let f = std::fs::File::open(path).map_err(|e| format!("open corpus {path}: {e}"))?;
        let len = f.metadata().map_err(|e| format!("stat corpus {path}: {e}"))?.len();
        if len == 0 {
            return Err(format!("corpus {path} is empty"));
        }
        let map = unsafe { memmap2::Mmap::map(&f) }.map_err(|e| format!("mmap corpus {path}: {e}"))?;
        Ok(Corpus { map, filesize: len })
    }
    /// `T[p]` in the container's coordinate system: the corpus byte, or the
    /// virtual sentinel (reported as `None`) at position `filesize`.
    #[inline]
    pub fn at(&self, p: u64) -> Option<u8> {
        if p < self.filesize { Some(self.map[p as usize]) } else { None }
    }
    /// THE BRUTE-FORCE LCP of the suffixes at text positions `p` and `q`: compare
    /// bytes until they differ or one suffix reaches the virtual sentinel (which
    /// is unequal to every corpus byte and cannot be shared by two positions).
    /// 8 bytes at a time, then the exact tail: this is a plain memcmp of the two
    /// suffixes, not an algorithm and not an index.
    pub fn lce(&self, p: u64, q: u64) -> u64 {
        if p == q {
            die(&format!("lce: identical positions {p} (a suffix has no LCP with itself)"));
        }
        let a0 = if p < self.filesize { p } else { return 0 };
        let b0 = if q < self.filesize { q } else { return 0 };
        let cap = (self.filesize - a0).min(self.filesize - b0);
        let a = &self.map[a0 as usize..(a0 + cap) as usize];
        let b = &self.map[b0 as usize..(b0 + cap) as usize];
        let mut i = 0usize;
        while i + 8 <= a.len() {
            let x = u64::from_le_bytes(a[i..i + 8].try_into().unwrap());
            let y = u64::from_le_bytes(b[i..i + 8].try_into().unwrap());
            if x != y {
                // the first differing byte inside this word
                let diff = (x ^ y).trailing_zeros() / 8;
                return (i as u64) + u64::from(diff);
            }
            i += 8;
        }
        while i < a.len() && a[i] == b[i] {
            i += 1;
        }
        i as u64
    }
}

// ---------------------------------------------------------------- the CLI

fn splitmix64(x: &mut u64) -> u64 {
    *x = x.wrapping_add(0x9e37_79b9_7f4a_7c15);
    let mut z = *x;
    z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    z ^ (z >> 31)
}

fn u64_arg(args: &[String], i: &mut usize, what: &str) -> u64 {
    if *i + 1 >= args.len() {
        die(&format!("{what} needs a value"));
    }
    *i += 1;
    args[*i].parse::<u64>().unwrap_or_else(|_| die(&format!("{what}: integer expected, got '{}'", args[*i])))
}

/// `xsa lcp <container> [--run I | --runs A:B | --sample N --seed S | --all] [--audit]`
///
/// Print `run<TAB>lcp` for each requested run. This is the accessor the whole
/// member exists for: a caller that knows WHICH run it wants pays one 16-byte
/// directory entry plus one block, and `LCP_ACCESS` says how many bytes that was.
pub fn command(args: &[String]) {
    let mut container: Option<String> = None;
    let mut runs: Vec<u64> = Vec::new();
    let mut range: Option<(u64, u64)> = None;
    let mut sample: Option<u64> = None;
    let mut seed = 1u64;
    let mut all = false;
    let mut audit = false;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--run" => runs.push(u64_arg(args, &mut i, "--run")),
            "--runs" => {
                let v = if i + 1 < args.len() { i += 1; args[i].clone() } else { die("--runs needs A:B") };
                let (a, b) = v.split_once(':').unwrap_or_else(|| die("--runs: expected A:B"));
                range = Some((
                    a.parse().unwrap_or_else(|_| die("--runs: A expected")),
                    b.parse().unwrap_or_else(|_| die("--runs: B expected")),
                ));
            }
            "--sample" => sample = Some(u64_arg(args, &mut i, "--sample")),
            "--seed" => seed = u64_arg(args, &mut i, "--seed"),
            "--all" => all = true,
            "--audit" => audit = true,
            other if other.starts_with("--") => die(&format!("lcp: unknown flag {other}")),
            other => {
                if container.is_some() {
                    die("lcp: one container");
                }
                container = Some(other.to_string());
            }
        }
        i += 1;
    }
    let path = container.unwrap_or_else(|| die("lcp: usage: xsa lcp <container> --run I | --runs A:B | --sample N --seed S | --all [--audit]"));
    let reader = qlite::Reader::open(&path).unwrap_or_else(|e| die(&e));
    let mut m14 = LcpRuns::open(&reader).unwrap_or_else(|e| die(&e));
    eprintln!(
        "LCP_OPEN container={} version={} member=m14 codec={} enc_version={} block_runs={} runs={} \
         n={} blocks={} payload_bytes={} dir_bytes={} payload_off={} random_access=block-directory",
        path,
        reader.version,
        PF_LCP_CODEC,
        PF_LCP_ENC_VERSION,
        m14.block_runs,
        m14.r,
        m14.n,
        m14.nblocks,
        m14.payload_bytes,
        16 * m14.nblocks,
        PF_LCP_HEAD
    );
    if audit {
        let (blocks, vals) = m14.audit().unwrap_or_else(|e| die(&e));
        eprintln!(
            "LCP_AUDIT blocks={blocks} values={vals} payload_bytes={} tiling=exact \
             escape_lists=sorted modes_zero={} modes_u8={} modes_packed={}",
            m14.payload_bytes, m14.modes[0], m14.modes[1], m14.modes[2]
        );
    } else if m14.r > 0 {
        // A cheap structural probe even without --audit: the first, last and a
        // middle block must at least address their own bytes.
        for b in [0u64, m14.nblocks / 2, m14.nblocks - 1] {
            let e = m14.entry(b);
            if e.span == 0 || e.off + u64::from(e.span) > m14.payload_bytes {
                die(&format!(
                    "m14: block {b} directory entry (off={} span={}) is outside the {} payload bytes",
                    e.off, e.span, m14.payload_bytes
                ));
            }
        }
    }
    let mut want: Vec<u64> = runs;
    if let Some((a, b)) = range {
        if b <= a || b > m14.r {
            die(&format!("lcp: --runs {a}:{b} is not a nonempty range inside [0,{})", m14.r));
        }
        want.extend(a..b);
    }
    if all {
        want.extend(0..m14.r);
    }
    if let Some(k) = sample {
        let mut st = seed ^ 0x5d5d_5d5d_5d5d_5d5d;
        for _ in 0..k {
            want.push(splitmix64(&mut st) % m14.r);
        }
    }
    if want.is_empty() {
        die("lcp: nothing requested (use --run/--runs/--sample/--all)");
    }
    let mut bytes = 0u64;
    let stdout = std::io::stdout();
    let mut w = std::io::BufWriter::with_capacity(1 << 20, stdout.lock());
    for run in &want {
        let v = m14.lcp(*run).unwrap_or_else(|e| die(&e));
        bytes += m14.bytes_per_answer(*run);
        use std::io::Write as _;
        writeln!(w, "{run}\t{v}").unwrap_or_else(|e| die(&e.to_string()));
    }
    use std::io::Write as _;
    w.flush().unwrap_or_else(|e| die(&e.to_string()));
    eprintln!(
        "LCP_ACCESS answers={} bytes_touched={} bytes_per_answer={:.1} dir_entry_bytes=16 \
         member_bytes={} runs={}",
        want.len(),
        bytes,
        bytes as f64 / want.len() as f64,
        m14.member_bytes,
        m14.r
    );
}

/// `xsa lcp-verify <container> --corpus FILE [--sample N] [--seed S] [--deep K]`
///
/// THE CORRECTNESS GATE. For every sampled run it reads the STORED value out of
/// m14 and compares it with a brute-force computation over the corpus:
///
///   stored[i]  = m14[i]                       (one directory entry + one block)
///   true[i]    = lce(SA[r_i - 1], SA[r_i])    (plain memcmp of two suffixes)
///
/// The two SA values come from the container's own locate (m11 anchors + an LF
/// walk inside one 1024-run stripe), which is certified separately - it is what
/// `xsa query --limit` and `xsa verify --corpus` already answer from. Nothing in
/// the LCP path is reused to compute the oracle.
pub fn verify_command(args: &[String]) {
    let mut container: Option<String> = None;
    let mut corpus_path: Option<String> = None;
    let mut sample = 50u64;
    let mut deep = 50u64;
    let mut seed = 1u64;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--corpus" => {
                if i + 1 >= args.len() {
                    die("--corpus needs a path");
                }
                i += 1;
                corpus_path = Some(args[i].clone());
            }
            "--sample" => sample = u64_arg(args, &mut i, "--sample"),
            "--deep" => deep = u64_arg(args, &mut i, "--deep"),
            "--seed" => seed = u64_arg(args, &mut i, "--seed"),
            other if other.starts_with("--") => die(&format!("lcp-verify: unknown flag {other}")),
            other => {
                if container.is_some() {
                    die("lcp-verify: one container");
                }
                container = Some(other.to_string());
            }
        }
        i += 1;
    }
    let path = container.unwrap_or_else(|| die("lcp-verify: usage: xsa lcp-verify <container> --corpus FILE [--sample N] [--deep K] [--seed S]"));
    let corpus_path = corpus_path.unwrap_or_else(|| die("lcp-verify: --corpus FILE is required: the oracle is the text"));
    let reader = qlite::Reader::open(&path).unwrap_or_else(|e| die(&e));
    let n = reader.n;
    let r = reader.r;
    eprintln!("LCP_VERIFY_OPEN container={} version={} n={n} runs={r}", path, reader.version);
    let index = qlite::Index::open(reader, 1).unwrap_or_else(|e| die(&e));
    let mut m14 = LcpRuns::open(&index.reader).unwrap_or_else(|e| die(&e));
    let (blocks, vals) = m14.audit().unwrap_or_else(|e| die(&e));
    eprintln!(
        "LCP_AUDIT blocks={blocks} values={vals} payload_bytes={} dir_bytes={} tiling=exact \
         escape_lists=sorted modes_zero={} modes_u8={} modes_packed={} member_bytes={}",
        m14.payload_bytes,
        16 * m14.nblocks,
        m14.modes[0],
        m14.modes[1],
        m14.modes[2],
        m14.member_bytes
    );
    let corpus = Corpus::open(&corpus_path).unwrap_or_else(|e| die(&e));
    eprintln!(
        "LCP_VERIFY_CORPUS path={corpus_path} bytes={} coordinate=T[i]=REMAP[corpus[i]] \
         sentinel_position={} order_preserving_remap_so_raw_bytes_compare_equally=1",
        corpus.filesize, corpus.filesize
    );
    if n != corpus.filesize + 1 {
        die(&format!(
            "lcp-verify: container n={n} is not the corpus's {} + 1 (the virtual sentinel); \
             this container was not built from this text",
            corpus.filesize
        ));
    }

    // ---- 1. THE CONVENTION: run 0 has no left neighbour, so its value is the
    // array's sentinel. This is the one entry the oracle cannot compute, so it is
    // asserted separately rather than skipped.
    let zero = m14.lcp(0).unwrap_or_else(|e| die(&e));
    if zero != 0 {
        die(&format!("lcp-verify: lcpRuns[0] = {zero}, expected the sentinel 0 (row 0 has no left neighbour)"));
    }
    eprintln!("LCP_VERIFY_SENTINEL run=0 stored={zero} expected=0 convention=head-row-has-no-left-neighbour");

    // ---- 2. CHEAPLY READ A LARGE RANDOM SET OF RUNS' STORED VALUES, then verify
    // the DEEPEST of them exactly: a uniform sample of 50 runs is dominated by
    // tiny values (most runs have LCP 0..3), which would never exercise an escape
    // or a wide field. The deepest-value runs are the adversarial half - in a
    // real text they are the deep contexts, i.e. exactly the nodes a tree walk
    // descends through.
    let mut st = seed ^ 0xa5a5_a5a5_a5a5_a5a5;
    let probe = sample.saturating_mul(200).max(2000);
    let mut cx = index.ctx();
    let mut seen: std::collections::BTreeMap<u64, (u64, u64)> = std::collections::BTreeMap::new(); // run -> (head row, stored)
    for _ in 0..probe {
        let row = splitmix64(&mut st) % n;
        let (run, s, _len) = cx.run_at(row);
        if run == 0 || seen.contains_key(&run) {
            continue;
        }
        let v = m14.lcp(run).unwrap_or_else(|e| die(&e));
        seen.insert(run, (s, v));
    }
    if seen.is_empty() {
        die("lcp-verify: probe found no runs");
    }
    let mut by_value: Vec<(u64, u64, u64)> = seen.iter().map(|(&run, &(s, v))| (v, run, s)).collect();
    by_value.sort_unstable();
    // the deepest `deep` runs, plus a uniform tail of the rest (so small values
    // are covered too), de-duplicated.
    let mut chosen: Vec<(u64, u64, u64)> = Vec::new();
    for x in by_value.iter().rev().take(deep as usize) {
        chosen.push(*x);
    }
    let stride = (by_value.len() / (sample as usize).max(1)).max(1);
    let mut k = 0usize;
    while k < by_value.len() && chosen.len() < deep as usize + sample as usize {
        chosen.push(by_value[k]);
        k += stride;
    }
    let mut chosen_runs: Vec<u64> = chosen.iter().map(|x| x.1).collect();
    chosen_runs.sort_unstable();
    chosen_runs.dedup();
    eprintln!(
        "LCP_VERIFY_SAMPLE probe_rows={probe} runs_probed={} chosen={} deep_target={deep} sample_target={sample} seed={seed}",
        seen.len(),
        chosen_runs.len()
    );

    // ---- 3. THE ORACLE. For each chosen run: head row r_i (m12), the row to its
    // left (r_i - 1), their text positions (locate: m11 anchor + LF walk), and the
    // true LCP by memcmp. Then compare with the stored value.
    let mut exact = 0u64;
    let mut mismatches = 0u64;
    let mut lf_steps = 0u64;
    let mut hist: Vec<u64> = Vec::new();
    for run in &chosen_runs {
        let (s, stored) = seen[run];
        let (p_prev, st1) = cx.locate_steps(s - 1);
        let (p_head, st2) = cx.locate_steps(s);
        lf_steps += st1 + st2;
        let truth = corpus.lce(p_prev, p_head);
        hist.push(stored);
        if truth == stored {
            exact += 1;
        } else {
            mismatches += 1;
            eprintln!(
                "LCP_MISMATCH run={run} head_row={s} stored={stored} true={truth} \
                 sa_prev={p_prev} sa_head={p_head} lf_steps_prev={st1} lf_steps_head={st2}"
            );
        }
        eprintln!(
            "LCP_CHECK run={run} head_row={s} stored={stored} true={truth} sa_prev={p_prev} \
             sa_head={p_head} lf_steps={} block={} block_bytes={} ok={}",
            st1 + st2,
            run / m14.block_runs,
            m14.bytes_per_answer(*run),
            u8::from(truth == stored)
        );
    }
    hist.sort_unstable();
    if mismatches != 0 {
        die(&format!("lcp-verify: {mismatches} of {} sampled runs answer a WRONG LCP", chosen_runs.len()));
    }
    eprintln!(
        "LCP_VERIFY n={} exact={exact} mismatches={mismatches} oracle=brute-force-lce-from-corpus \
         positions=locate(m11-anchor+LF-walk) lf_steps={lf_steps} lcp_min={} lcp_max={} lcp_median={} \
         blocks_probed={} member_bytes={}",
        chosen_runs.len(),
        hist.first().copied().unwrap_or(0),
        hist.last().copied().unwrap_or(0),
        hist.get(hist.len() / 2).copied().unwrap_or(0),
        m14.nblocks,
        m14.member_bytes
    );
}

#[cfg(test)]
mod block_tests {
    use super::LcpRuns;

    /// THE CROSS-IMPLEMENTATION GATE. `LcpRuns::decode` is a SECOND decoder of the
    /// writer's bytes, so proving it against a Rust re-encoder would prove nothing
    /// (a shared misunderstanding round-trips perfectly). This test reads the
    /// adversarial suite the C++ writer itself emitted:
    ///
    ///   LCP_BLOCK_DUMP=/tmp/blocks.bin <stages>/xsa_finish --selftest-lcp-runs
    ///   LCP_BLOCK_DUMP=/tmp/blocks.bin cargo test --release -- lcp
    ///
    /// File: "LCPBLK01" | u64 records | per record: u64 n, u64 span, span bytes, n*u64
    #[test]
    fn cxx_writer_blocks_decode_exactly() {
        let path = match std::env::var("LCP_BLOCK_DUMP") {
            Ok(p) => p,
            Err(_) => return, // the gate sets it; without it there is nothing to prove
        };
        let d = std::fs::read(&path).unwrap_or_else(|e| panic!("read {path}: {e}"));
        assert_eq!(&d[..8], b"LCPBLK01", "{path}: not a block dump");
        let be = |o: usize| u64::from_le_bytes(d[o..o + 8].try_into().unwrap());
        let records = be(8);
        assert!(records > 0, "{path}: zero records (halved dump?)");
        let mut at = 16usize;
        let mut values = 0u64;
        for r in 0..records {
            let n = be(at);
            let span = be(at + 8) as usize;
            at += 16;
            let blk = &d[at..at + span];
            at += span;
            let want = &d[at..at + 8 * n as usize];
            at += 8 * n as usize;
            assert_eq!(span % 8, 0, "record {r}: span {span} is not an 8-byte multiple");
            for j in 0..n {
                let expect = u64::from_le_bytes(want[8 * j as usize..8 * j as usize + 8].try_into().unwrap());
                let got = LcpRuns::decode(blk, n, j)
                    .unwrap_or_else(|e| panic!("record {r} value {j}/{n} (span {span}): {e}"));
                assert_eq!(got, expect, "record {r} value {j}/{n} (span {span}, mode {})", blk[0]);
                values += 1;
            }
        }
        assert_eq!(at, d.len(), "{path}: trailing bytes after {records} records");
        eprintln!("LCP_BLOCK_DUMP_OK path={path} records={records} values={values} exact=1");
    }
}
