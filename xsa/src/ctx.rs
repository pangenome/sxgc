//! m20 CONTEXT_TRIE: the top of the suffix tree, materialised.
//!
//! WHY THIS MEMBER. MEMORY_OPS.md lists the operations the index has to answer
//! (`extend`, `relax`, `project`) and the ones it cannot answer today. Every one
//! of them is a walk on a *context node*: a distinct string, its occurrences, and
//! the bytes that follow it. The container can find the SA interval of a context
//! by backward search (that is `seek`/`xsa query`), but it cannot enumerate what
//! comes NEXT: children of a context are delimited by where the LCP drops, and the
//! lite container has no LCP column and no per-row SA column. Enumerating one wide
//! node by scanning its interval is hopeless (a depth-1 context has billions of
//! occurrences; VIRTUAL_TREE.md prices that scan at 46.95 TB/s of LCP traffic at
//! the 150k/s target). CONTEXT_DEPTH.md says all the information is in the first
//! ~16-32 bytes (H(next|ctx) = 3.89 bits at depth 1, 1.29 at 8, 0.50 at 16, 0 at
//! 32), so the part of the tree that must be materialised is SHALLOW, and shallow
//! means small: the node store below is 0.1-0.5 % of n.
//!
//! HOW IT IS BUILT (the ordered sweep, exactly as the finish already streams).
//! Suffixes arrive in sorted order. A single pass keeps a stack of the current
//! path (<= D entries) and, for each suffix, descends from the longest common
//! prefix with the previous one. A node's occurrence count is its ROW SPAN -
//! `row_at_close - row_at_open` - so no per-suffix counter increments are needed
//! at all: the pass is O(1) amortised per suffix and its state is O(D) plus the
//! node store. Children of a node appear in ascending byte order for free, which
//! is both the branch histogram and the lookup order.
//!
//! WHERE THE ORDERED STREAM COMES FROM. Inside `xsa build` the band producer holds
//! (row, SA value, text) and can feed this builder directly. Outside it - which is
//! how this member is produced today - the ordered stream is reconstructed from
//! the container itself: LF maps row i to the row of `SA[i]-1`, so ONE cycle of LF
//! visits every row exactly once and yields the whole suffix array at one LF step
//! per row, in parallel (see `sa_reconstruct`). That is the "minimal extra pass":
//! n LF steps, 5 bytes per row of transient RAM, no scratch written, and the
//! corpus text is read only through a read-only mmap.
//!
//! WHAT IS STORED. For every distinct context of length 1..D whose occurrence
//! count is >= MIN_SUPPORT: its depth, its occurrence count, its exact next-byte
//! histogram (every child byte, END included, with the exact count - pruned
//! children keep their counts, so the histogram is exact even where the child node
//! is not materialised), the child node id when the child itself clears
//! MIN_SUPPORT, and a document summary. The root (depth 0) is always present, so
//! `branch-of ""` is the byte histogram of the whole corpus.
//!
//! DOCUMENT SUMMARY. Counts of documents are exact while a node contains <= 8
//! distinct documents (an inline sorted id list: the useful case for `project`,
//! and the one `select` needs to weight by document frequency); beyond that the
//! node carries the exact top-1 document id (the source-leakage answer) plus a
//! 64-register HLL estimate of the distinct document count (~13 % standard error,
//! stated, never presented as exact). A document is a run of bytes delimited by
//! 0x1E, counted in the ORIGINAL corpus bytes.

use std::io::{BufWriter, Write};

use memmap2::Mmap;

use crate::{die, qlite, sxi};

pub const TRIE_CODEC: u32 = 20;
/// Member format version. 1 = the first draft (fixed 16-byte node and edge slots,
/// 128-byte head, no stream source); 2 = packed columns, 192-byte head, stream
/// source journaled; 3 = the boundary_aware head field (the no-cross law: an
/// occurrence whose continuation position is a document start is reclassified to
/// the END branch at BUILD time, so the member's histogram can never conflate a
/// crossing continuation with content). A reader must REFUSE an unknown version BY
/// NUMBER, never by an offset that happens not to be in range.
pub const TRIE_VERSION: u64 = 3;
/// Header flag value: "member m20 (context trie) is present".
pub const FLAG_TRIE: u64 = 256;
/// Head bytes of the member.
// ------------------------------------------------------- the boundary oracle
//
// THE NO-CROSS LAW AT BUILD TIME (NOTE_FROM_EMENDER_DOCUMENT_BOUNDARIES.md D2,
// the owner's semantic requirement): a match or branch count must never span a
// document boundary, as the DEFAULT. For an occurrence of a context at position
// `pos` with the context's last byte at `pos + len - 1`, the continuation
// position is `q = pos + len`, and the EXACT predicate implemented here is:
//
//   q == corpus_len                      -> the structural END (at most one)
//   select(rank(q)) == q (q < corpus_len) -> the END branch ("this context ends
//                                           a document"); implemented as
//                                           "the next document starts at q"
//   otherwise                            -> the content byte corpus[q]
//
// and the occurrence itself is a CROSSING match (excluded entirely) iff
// rank(pos) != rank(pos + len - 1), i.e. iff a document start lies inside
// (pos, pos + len - 1]. The builder enforces both BY CONSTRUCTION: every suffix
// fed to it is CUT AT ITS DOCUMENT'S END - `avail = next_start(pos) - pos`
// bytes - so (a) a context deeper than the cut never enters the trie (crossing
// occurrences vanish from every node's row span), and (b) a context exactly the
// cut length lands on the existing END branch. Node counts therefore become
// non-crossing occurrence counts, and the END edge count becomes the exact
// number of occurrences that end a document. m15 makes the predicate O(1)-ish
// per suffix (one rank/select), which is why the build can afford the law.

/// A document-starts table the builder can consult, owned by the build process.
pub enum OwnedStarts {
    /// No structure: the member is built boundary-UNAWARE and SAYS SO
    /// (`boundary_aware=0` in its head), and the query side refuses
    /// boundary-sensitive answers against it.
    None,
    /// member m15 or an XSEP `seps.bin` sidecar (the pile-map shape).
    Table(crate::docstarts::DocStarts),
    /// A plain little-endian u64 starts array (the `.starts` fixtures shape) or
    /// a decoded CORPUS V2 `XCS2` table: starts[0] == 0, strictly increasing.
    Plain { starts: Vec<u64>, text_len: u64 },
}

impl OwnedStarts {
    /// (container document id, bytes from `pos` to the END of pos's document).
    /// `None` = no structure, or `pos` is the virtual END row (no bytes left).
    /// The next document's start is capped at `text_len`: a SUPERSET map (the
    /// 1.31 TB map over a 10 GB text) names documents that start past the text,
    /// and the in-text cut for the last document is the text end itself.
    pub fn avail(&self, pos: u64, text_len: u64) -> Option<(u32, u64)> {
        match self {
            OwnedStarts::None => None,
            OwnedStarts::Table(ds) => {
                if pos >= text_len {
                    return None;
                }
                let (doc, _base) = ds.rank_start(pos);
                let next = if doc + 1 < ds.docs {
                    ds.select(doc + 1).min(text_len)
                } else {
                    text_len
                };
                Some((doc as u32, next - pos))
            }
            OwnedStarts::Plain { starts, text_len } => {
                if pos >= *text_len {
                    return None;
                }
                // largest start <= pos
                let doc = starts.partition_point(|&s| s <= pos) as u64 - 1;
                let next = starts
                    .get(doc as usize + 1)
                    .copied()
                    .unwrap_or(u64::MAX)
                    .min(*text_len);
                Some((doc as u32, next - pos))
            }
        }
    }

    pub fn is_some(&self) -> bool {
        !matches!(self, OwnedStarts::None)
    }

    /// rank_start for the oracle: the document containing `p` and its start.
    /// `p` must be inside the text.
    pub fn rank_start(&self, p: u64) -> (u64, u64) {
        match self {
            OwnedStarts::None => die("ctx: rank_start on an absent starts table"),
            OwnedStarts::Table(ds) => ds.rank_start(p),
            OwnedStarts::Plain { starts, .. } => {
                let doc = starts.partition_point(|&s| s <= p) as u64 - 1;
                (doc, starts[doc as usize])
            }
        }
    }

    /// THE EXACT END-BRANCH PREDICATE: is `q` a document start?
    /// select(rank(q)) == q.
    pub fn is_start(&self, q: u64) -> bool {
        match self {
            OwnedStarts::None => false,
            OwnedStarts::Table(ds) => q < ds.text_len && ds.rank_start(q).1 == q,
            OwnedStarts::Plain { starts, text_len } => {
                if q >= *text_len {
                    return false;
                }
                let doc = starts.partition_point(|&s| s <= q) as u64 - 1;
                starts[doc as usize] == q
            }
        }
    }

    pub fn docs(&self) -> u64 {
        match self {
            OwnedStarts::None => 0,
            OwnedStarts::Table(ds) => ds.docs,
            OwnedStarts::Plain { starts, .. } => starts.len() as u64,
        }
    }

    /// Load a starts table from a path: a DIRECTORY holding `seps.bin` (the
    /// `xsa corpus --pile-map` sidecar), a CORPUS V2 `PREFIX.starts` (XCS2 v1),
    /// or a raw little-endian u64 array (the `xsa docstarts embed --doc-starts`
    /// input shape). Anything else is a hard error: boundaries are metadata and
    /// a guess that does not exist is not a guess.
    pub fn load(path: &str, text_len: u64) -> OwnedStarts {
        let p = std::path::Path::new(path);
        if p.is_dir() {
            return OwnedStarts::Table(crate::docstarts::DocStarts::from_xsep(p, text_len));
        }
        let bytes = std::fs::read(p).unwrap_or_else(|e| die(&format!("ctx: read starts table {path}: {e}")));
        if bytes.len() >= 8 {
            if &bytes[..4] == b"XCS2" {
                let (_docs, _corpus_bytes, starts) = crate::corpusv2::read_starts(p);
                return OwnedStarts::validate_plain(starts, text_len, path);
            }
            if &bytes[..4] == b"XSEP" {
                die(&format!(
                    "ctx: {path} is an XSEP separators file: pass the map DIRECTORY that holds it"
                ));
            }
        }
        if bytes.len() % 8 != 0 {
            die(&format!(
                "ctx: starts table {path} is {} bytes, not a whole u64 array, XCS2 or a directory holding seps.bin",
                bytes.len()
            ));
        }
        let starts: Vec<u64> = bytes
            .chunks_exact(8)
            .map(|c| u64::from_le_bytes(c.try_into().unwrap()))
            .collect();
        OwnedStarts::validate_plain(starts, text_len, path)
    }

    fn validate_plain(starts: Vec<u64>, text_len: u64, path: &str) -> OwnedStarts {
        need(!starts.is_empty(), &format!("starts table {path} is empty"));
        need(starts[0] == 0, &format!("starts table {path} does not start at 0"));
        for i in 1..starts.len() {
            need(starts[i] > starts[i - 1] && starts[i] < text_len,
                &format!("starts table {path} is not strictly increasing inside the text at entry {i}"));
        }
        OwnedStarts::Plain { starts, text_len }
    }
}

/// Child code of "the text ends here". Real bytes are stored as 1+byte, so the
/// u16 child code sorts exactly like the lexicographic order (END first).
/// In a boundary-aware member this same code is the whole END branch: "the
/// context ends a document (or the text)".
pub const END: u16 = 0;
const NO_NODE: u32 = u32::MAX;
const DOC_SMALL: usize = 8;

fn need(ok: bool, msg: &str) {
    if !ok {
        die(&format!("CONTEXT_TRIE: {msg}"));
    }
}

fn put(b: &mut [u8], v: u64, w: usize) {
    b[..w].copy_from_slice(&v.to_le_bytes()[..w]);
}
fn get(b: &[u8], w: usize) -> u64 {
    let mut x = [0u8; 8];
    x[..w].copy_from_slice(&b[..w]);
    u64::from_le_bytes(x)
}

// ---------------------------------------------------------------- bit packing
//
// A block of 256 u64 counts, encoded with the cheaper of two exact schemes, so a
// Pile node count of 4.9e9 and a fixture's count of 3 are the same format:
//
//   mode 0  every value is zero                                  -> 1 byte
//   mode 1  u8 array, value < 255 inline, 255 = escape
//   mode 2  w-bit packed array (1..=32), the all-ones field = escape
//   then    u32 escape count, then esc * { u16 index, u64 value } (10 B each,
//           sorted by index, so a lookup BINARY SEARCHES them)
//   then    zero padding to an 8-byte multiple
//
// Nothing is ever truncated: an escaped value keeps all 64 bits. The choice is
// made per block from the width histogram, so the reader never needs the writer's
// reasons (mode and width are in the block).
pub const PACK_BLOCK: usize = 256;
const PACK_DIR: usize = 40;

fn bits_of(v: u64) -> u32 {
    if v == 0 { 0 } else { 64 - v.leading_zeros() }
}

fn encode_block(v: &[u64], o: &mut Vec<u8>) -> (u32, u32) {
    need(!v.is_empty() && v.len() <= PACK_BLOCK, "pack block size");
    let start = o.len();
    let mut hist = [0usize; 65];
    let mut maxb = 0u32;
    for &x in v {
        let b = bits_of(x);
        hist[b as usize] += 1;
        maxb = maxb.max(b);
    }
    if maxb == 0 {
        o.push(0);
        while (o.len() - start) % 8 != 0 { o.push(0); }
        return (0, 0);
    }
    const WIDTHS: [u32; 14] = [1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 20, 24, 32];
    let esc1: usize = v.iter().filter(|&&x| x >= 255).count();
    let mut best_mode = 1u8;
    let mut best_w = 0u32;
    let mut best_bytes = 1 + v.len() + 4 + 10 * esc1;
    for &w in &WIDTHS {
        let above: usize = v.iter().filter(|&&x| bits_of(x) > w).count();
        let bytes = 1 + 1 + (v.len() * w as usize + 7) / 8 + 4 + 10 * above;
        if bytes < best_bytes {
            best_bytes = bytes;
            best_mode = 2;
            best_w = w;
        }
    }
    let limit: u64 = if best_mode == 2 { (1u64 << best_w) - 1 } else { 255 };
    let esc: usize = v.iter().filter(|&&x| x >= limit).count();
    o.push(best_mode);
    if best_mode == 1 {
        for &x in v {
            o.push(if x >= limit { 255 } else { x as u8 });
        }
    } else {
        o.push(best_w as u8);
        let at = o.len();
        let packed = (v.len() * best_w as usize + 7) / 8;
        o.resize(at + packed, 0);
        let mut acc = 0u64;
        let mut have = 0u32;
        let mut oi = 0usize;
        for &x in v {
            acc |= (if x >= limit { limit } else { x }) << have;
            have += best_w;
            while have >= 8 {
                o[at + oi] = acc as u8;
                acc >>= 8;
                have -= 8;
                oi += 1;
            }
        }
        if have > 0 {
            o[at + oi] = acc as u8;
            oi += 1;
        }
        need(oi == packed, "packed block length");
    }
    o.extend_from_slice(&(esc as u32).to_le_bytes());
    for (i, &x) in v.iter().enumerate() {
        if x < limit { continue; }
        need(i < 65536, "pack escape index");
        o.extend_from_slice(&(i as u16).to_le_bytes());
        o.extend_from_slice(&x.to_le_bytes());
    }
    while (o.len() - start) % 8 != 0 { o.push(0); }
    (best_mode as u32, best_w)
}

fn decode_block_value(b: &[u8], n: usize, j: usize) -> u64 {
    need(j < n, "pack index");
    let mode = b[0];
    if mode == 0 {
        return 0;
    }
    let (inline, escaped, esc_at) = if mode == 1 {
        let x = b[1 + j] as u64;
        (x, x == 255, 1 + n)
    } else {
        need(mode == 2, "pack block mode");
        let w = b[1] as u32;
        need((1..=32).contains(&w), "pack block width");
        let bit = j as u64 * w as u64;
        let p = 2 + (bit / 8) as usize;
        let sh = (bit % 8) as u32;
        let mut acc = 0u64;
        for i in 0..5 {
            acc |= (b[p + i] as u64) << (8 * i);
        }
        let mask = (1u64 << w) - 1;
        let x = (acc >> sh) & mask;
        (x, x == mask, 2 + (n * w as usize + 7) / 8)
    };
    if !escaped {
        return inline;
    }
    let count = get(&b[esc_at..], 4) as usize;
    need(count <= n, "pack escape count");
    let list = &b[esc_at + 4..];
    let (mut lo, mut hi) = (0usize, count);
    while lo < hi {
        let mid = (lo + hi) / 2;
        let e = &list[10 * mid..];
        let idx = get(e, 2) as usize;
        if idx == j {
            return get(&e[2..], 8);
        }
        if idx < j { lo = mid + 1 } else { hi = mid }
    }
    die("CONTEXT_TRIE: pack escape list has no entry for an escaped value");
}

/// A block-packed u64 array: append values, then read any index in O(1).
pub struct Packed {
    pub buf: Vec<u8>,
    dir: Vec<(u64, u32)>, // (payload offset, span)
    block: Vec<u64>,
    count: u64,
}

impl Packed {
    pub fn new() -> Self {
        Self { buf: Vec::new(), dir: Vec::new(), block: Vec::with_capacity(PACK_BLOCK), count: 0 }
    }
    pub fn push(&mut self, v: u64) {
        self.block.push(v);
        self.count += 1;
        if self.block.len() == PACK_BLOCK {
            self.flush();
        }
    }
    fn flush(&mut self) {
        if self.block.is_empty() {
            return;
        }
        let off = self.buf.len() as u64;
        let (mode, w) = encode_block(&self.block, &mut self.buf);
        let span = (self.buf.len() as u64 - off) as u32;
        self.dir.push((off, span));
        let _ = (mode, w);
        self.block.clear();
    }
    pub fn len(&self) -> u64 {
        self.count
    }
    /// Serialised bytes so far (payload only, before the directory).
    pub fn bytes(&self) -> u64 {
        self.buf.len() as u64
    }
    /// Serialise: 40-byte head, the block directory, then the block payloads.
    pub fn finish(mut self) -> Vec<u8> {
        self.flush();
        let blocks = self.dir.len();
        let mut out = vec![0u8; PACK_DIR + 16 * blocks];
        put(&mut out[0..], self.count, 8);
        put(&mut out[8..], PACK_BLOCK as u64, 8);
        put(&mut out[16..], blocks as u64, 8);
        put(&mut out[24..], PACK_DIR as u64, 8);
        for (i, &(off, span)) in self.dir.iter().enumerate() {
            put(&mut out[PACK_DIR + 16 * i..], off, 8);
            put(&mut out[PACK_DIR + 16 * i + 8..], span as u64, 4);
        }
        out.extend_from_slice(&self.buf);
        out
    }
}

/// A block-packed u64 array inside the member.
pub struct PackedView<'a> {
    d: &'a [u8],
}
impl<'a> PackedView<'a> {
    pub fn new(d: &'a [u8], count: u64) -> Self {
        need(d.len() >= PACK_DIR, "packed array head");
        need(get(&d[0..], 8) == count && get(&d[8..], 8) == PACK_BLOCK as u64, "packed array count/block");
        need(get(&d[24..], 8) == PACK_DIR as u64, "packed array directory offset");
        let blocks = get(&d[16..], 8);
        need((blocks * 16) as usize <= d.len() - PACK_DIR, "packed array directory size");
        let v = Self { d };
        need(v.blocks() == blocks, "packed array block count");
        v
    }
    fn blocks(&self) -> u64 {
        get(&self.d[16..], 8)
    }
    pub fn get(&self, i: u64) -> u64 {
        let total = get(&self.d[0..], 8);
        need(i < total, "packed array index");
        let b = (i / PACK_BLOCK as u64) as usize;
        need((b as u64) < self.blocks(), "packed array block index");
        let o = PACK_DIR + 16 * b;
        let off = get(&self.d[o..], 8) as usize;
        let span = get(&self.d[o + 8..], 4) as usize;
        need(span >= 1, "packed block span");
        let base = PACK_DIR + 16 * self.blocks() as usize + off;
        need(base + span <= self.d.len(), "packed block bounds");
        let body = &self.d[base..base + span];
        // the LAST block may hold fewer than PACK_BLOCK values
        let in_block = (total - (b as u64) * PACK_BLOCK as u64).min(PACK_BLOCK as u64) as usize;
        let j = i as usize % PACK_BLOCK;
        need(j < in_block, "packed block value index");
        decode_block_value(body, in_block, j)
    }
}

// ------------------------------------------------------------- document access

/// Distinct-document estimator: 64 registers, 4 bits each, k=1 (the rank of the
/// first set bit of the remaining 58 hash bits). Standard error 1.04/sqrt(64) =
/// 13 %. Used only above DOC_SMALL distinct documents; below that the count is
/// EXACT because the ids are stored.
#[derive(Clone, Copy)]
struct Hll {
    reg: [u8; 64],
}
impl Hll {
    fn new() -> Self {
        Self { reg: [0; 64] }
    }
    fn add_hash(&mut self, h: u64) {
        let i = (h & 63) as usize;
        let rest = h >> 6;
        let r = if rest == 0 { 58 } else { rest.trailing_zeros() as u8 + 1 };
        if r > self.reg[i] {
            self.reg[i] = r;
        }
    }
    fn estimate(&self) -> u64 {
        let m = 64f64;
        let mut sum = 0f64;
        let mut zeros = 0;
        for &r in self.reg.iter() {
            sum += 2f64.powi(-(r as i32));
            if r == 0 {
                zeros += 1;
            }
        }
        let mut e = 0.7213 / (1.0 + 1.079 / m) * m * m / sum;
        if e <= 2.5 * m && zeros > 0 {
            e = m * (m / zeros as f64).ln();
        }
        // an HLL below DOC_SMALL is never used (the exact list covers it), but a
        // monotone floor keeps the reported count >= the exact list size.
        (e.round() as u64).max(1)
    }
}

fn doc_hash(doc: u64) -> u64 {
    // SplitMix64 finaliser: a fixed, documented, non-cryptographic mix.
    let mut z = doc.wrapping_add(0x9e3779b97f4a7c15);
    z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
    z ^ (z >> 31)
}

#[derive(Clone)]
struct DocAcc {
    small: [u32; DOC_SMALL],
    n: usize,
    /// Misra-Gries with 2 counters: a heavy document id survives with a count of
    /// at least half its true frequency. This is a CANDIDATE, and it is labelled
    /// as one; it answers "which document does this context keep coming from"
    /// (the leakage question) without a per-document counter per node.
    mg: [u32; 2],
    mgc: [u32; 2],
    hll: Hll,
    over: bool,
}

impl DocAcc {
    fn new() -> Self {
        Self { small: [0; DOC_SMALL], n: 0, mg: [NO_NODE; 2], mgc: [0; 2], hll: Hll::new(), over: false }
    }
    #[inline]
    fn add(&mut self, doc: u32) {
        // exact while the node holds <= DOC_SMALL distinct documents
        if !self.over {
            if self.n < DOC_SMALL {
                if !self.small[..self.n].contains(&doc) {
                    self.small[self.n] = doc;
                    self.n += 1;
                }
                return;
            }
            if self.small[..self.n].contains(&doc) {
                return;
            }
            for i in 0..self.n {
                self.hll.add_hash(doc_hash(self.small[i] as u64));
            }
            self.over = true;
        }
        self.hll.add_hash(doc_hash(doc as u64));
        if self.mg[0] == doc {
            self.mgc[0] += 1;
        } else if self.mg[1] == doc {
            self.mgc[1] += 1;
        } else if self.mgc[0] == 0 {
            self.mg[0] = doc;
            self.mgc[0] = 1;
        } else if self.mgc[1] == 0 {
            self.mg[1] = doc;
            self.mgc[1] = 1;
        } else {
            self.mgc[0] -= 1;
            self.mgc[1] -= 1;
        }
    }
    /// The heavy-document CANDIDATE (exact when the node stayed exact).
    fn heavy(&self) -> u32 {
        if !self.over {
            // exact: the most frequent of the <= 8 documents needs a count, and the
            // first one is not it. For an exact node the LIST is the answer, so the
            // candidate is the last id added (stable, documented).
            return if self.n == 0 { NO_NODE } else { self.small[0] };
        }
        if self.mgc[1] > self.mgc[0] { self.mg[1] } else { self.mg[0] }
    }
    fn n_docs(&self) -> u64 {
        if self.over { self.hll.estimate() } else { self.n as u64 }
    }
}

// ------------------------------------------------------------------- the builder
//
// THE RECORD COST (m20 format 2). A node is 4 fixed bytes (depth u8, child count
// u16, flags u8) plus three block-packed u64 columns; an edge is three block-packed
// u64 columns and NO fixed bytes at all. Everything that used to be a fixed 16-byte
// slot is now a delta or a packed value, because the measured store was
// dominated by fixed-width waste: at 150 MB / depth 16 / min_support 10 the old
// layout cost 112 B per node and 19 B per edge, and the EDGES were 3/4 of it.
//
//   node  depth u8 | nchild u16 | flags u8
//         first_edge delta            (children of a node are adjacent: 0 or 1)
//         occurrence count            (packed; the top of the tree is 1e9-ish)
//   edge  child code 0..256           (packed, 9 bits; END = 0 so the code order
//                                      IS the lexicographic order)
//         child node id as parent - child (post-order: children are emitted
//                                      immediately before the parent, so the
//                                      delta is the size of the child's subtree)
//         child occurrence count     (packed)
//
// flags: bit0 = this node carries a document summary, bit1 = the summary's id list
// is exact.
struct Level {
    /// the row that opened this level (a debug anchor; counts no longer come
    /// from row spans - see `count`)
    start: u64,
    /// THE NO-CROSS OCCURRENCE COUNT of this node's context: incremented by every
    /// row whose DOCUMENT holds the whole context (avail >= depth). A row whose
    /// context would cross its boundary stops at the shallower levels, so a
    /// crossing occurrence is never counted anywhere.
    count: u64,
    children: Vec<Child>,
    docs: DocAcc,
}

struct Child {
    code: u16,
    start: u64,
    count: u64,
    node: u32,
}

pub struct Builder {
    depth: usize,
    min_support: u64,
    docs_min_support: u64,
    root: Vec<Child>,
    root_docs: DocAcc,
    stack: Vec<Level>,
    row: u64,
    prev: Vec<u8>,
    nfixed: Vec<u8>,
    nedge: Packed,
    ncount: Packed,
    ecode: Packed,
    echild: Packed,
    ecount: Packed,
    dndocs: Packed,
    dtop: Packed,
    dfirst: Packed,
    dids: Packed,
    prev_edge_end: u64,
    prev_first_edge: u64,
    nnodes: u64,
    ndoc_nodes: u64,
    /// HOW THE ORDERED STREAM WAS OBTAINED: 0 = reconstructed from the container by
    /// the LF cycle (the post-hoc path: 5 B per POSITION of transient RAM), 1 = fed
    /// by the construction chain, where the producer already holds (row, SA, text)
    /// and the stream is free. A container must say which one produced it.
    source: u8,
    /// transient bytes the stream cost (the u40 array plus the dense run columns)
    source_mem: u64,
    /// THE NO-CROSS LAW, JOURNALED IN THE MEMBER HEAD: 1 = every suffix fed to
    /// this builder was CUT AT ITS DOCUMENT'S END (a starts table was present at
    /// build time), so node counts are non-crossing occurrence counts and the
    /// END edge is "this context ends a document (or the text)". 0 = built
    /// without a starts table: the histogram conflates crossing continuations
    /// with content, and the query side REFUSES boundary-sensitive answers
    /// against such a member rather than returning them silently.
    boundary_aware: u8,
}

const HEAD: usize = 192;
const TRIE_SECTIONS: usize = 10;

impl Builder {
    pub fn new(depth: usize, min_support: u64, docs_min_support: u64) -> Self {
        need((1..=64).contains(&depth), "--depth must be 1..=64");
        need(min_support >= 1, "--min-support must be >= 1");
        Self {
            depth,
            min_support,
            docs_min_support,
            root: Vec::new(),
            root_docs: DocAcc::new(),
            stack: Vec::new(),
            row: 0,
            prev: Vec::new(),
            nfixed: Vec::new(),
            nedge: Packed::new(),
            ncount: Packed::new(),
            ecode: Packed::new(),
            echild: Packed::new(),
            ecount: Packed::new(),
            dndocs: Packed::new(),
            dtop: Packed::new(),
            dfirst: Packed::new(),
            dids: Packed::new(),
            prev_edge_end: 0,
            prev_first_edge: 0,
            nnodes: 0,
            ndoc_nodes: 0,
            source: 0,
            source_mem: 0,
            boundary_aware: 0,
        }
    }

    /// Declare that the ordered stream was boundary-cut (see `OwnedStarts`):
    /// every suffix fed from now on must already be truncated at its document's
    /// end. Journaled in the member head as boundary_aware=1.
    pub fn set_boundary_aware(&mut self) {
        self.boundary_aware = 1;
    }

    /// Declare where the ordered stream came from (journaled in the member).
    pub fn set_source(&mut self, mode: u8, mem: u64) {
        self.source = mode;
        self.source_mem = mem;
    }

    /// One suffix, in sorted order. `p` holds the suffix's FULL prefix bytes
    /// (up to depth+1 of them are read: the stream's sorted-order contract is
    /// untouched, so node spans stay contiguous). `rem` is the suffix's length
    /// to the corpus end. `avail` is the no-cross oracle's answer for this row:
    /// the bytes from the suffix's start to the END of its document
    /// (`select(rank(pos)+1) - pos`, or the text end); `u64::MAX` = no structure
    /// (the legacy conflated build). `doc` is the document the suffix starts in
    /// (container doc id when boundary-aware, 0x1E span id otherwise).
    ///
    /// THE LAW, PER ROW: the row is a NON-CROSSING occurrence of every context
    /// of length d <= min(top, avail) - those levels take its count, its document
    /// and its branch - and contributes NOTHING past its boundary. The branch at
    /// the cut depth is the END branch ("this context ends a document"), never
    /// the next document's first byte; at the corpus end (rem <= depth) it is the
    /// one structural END.
    pub fn feed(&mut self, p: &[u8], rem: u64, avail: u64, doc: u32) {
        let m = (rem.min(self.depth as u64 + 1)) as usize;
        need(p.len() >= m, "builder: prefix shorter than the suffix's own length");
        need(avail >= 1 || rem == 0, "builder: avail is zero for a real suffix");
        let cap = m.min(self.prev.len());
        let mut lcp = 0usize;
        while lcp < cap && p[lcp] == self.prev[lcp] {
            lcp += 1;
        }
        let keep = lcp.min(self.depth);
        let top = m.min(self.depth);
        need(keep <= top, "builder: keep above top");
        while self.stack.len() > keep {
            self.close_one();
        }
        need(self.stack.len() == keep, "builder: stack/prefix invariant");
        for d in (keep + 1)..=top {
            let code = 1 + p[d - 1] as u16;
            let parent = if d == 1 { &mut self.root } else { &mut self.stack[d - 2].children };
            need(
                parent.last().map_or(true, |c| c.code < code),
                "builder: children are not in ascending byte order (the suffix stream is not sorted)",
            );
            parent.push(Child { code, start: self.row, count: 0, node: NO_NODE });
            self.stack.push(Level { start: self.row, count: 0, children: Vec::new(), docs: DocAcc::new() });
        }
        // THE CUT DEPTH: the deepest context of this row that lies wholly inside
        // its document. Every level up to here is a NON-CROSSING occurrence.
        let bd = (top as u64).min(avail) as usize;
        for lv in self.stack.iter_mut().take(bd) {
            lv.count += 1;
            lv.docs.add(doc);
        }
        self.root_docs.add(doc);
        let parent = if bd == 0 { &mut self.root } else { &mut self.stack[bd - 1].children };
        if top < m && (top as u64) < avail {
            // an in-document continuation: the content byte at pos+top (inside
            // this row's document by the test above)
            let code = 1 + p[top] as u16;
            match parent.last_mut() {
                Some(c) if c.code == code => c.count += 1,
                Some(c) => {
                    need(c.code < code, "builder: children are not in ascending byte order");
                    parent.push(Child { code, start: self.row, count: 1, node: NO_NODE });
                }
                None => parent.push(Child { code, start: self.row, count: 1, node: NO_NODE }),
            }
        } else {
            // THE END BRANCH: either the suffix ends the corpus (rem <= depth:
            // the one structural END) or the context of depth bd ends this row's
            // DOCUMENT (the continuation byte - the next document's first byte, or
            // the separator - is reclassified here, never counted as content).
            // Several occurrences of one context can end documents, so an
            // existing END child ACCUMULATES.
            match parent.first_mut() {
                Some(c) if c.code == END => c.count += 1,
                _ => parent.insert(0, Child { code: END, start: self.row, count: 1, node: NO_NODE }),
            }
        }
        self.prev.clear();
        self.prev.extend_from_slice(&p[..m]);
        self.row += 1;
    }

    fn close_one(&mut self) {
        let lv = self.stack.pop().expect("builder: close on empty stack");
        let depth = self.stack.len() + 1;
        let count = lv.count;
        let parent = if depth == 1 { &mut self.root } else { &mut self.stack[depth - 2].children };
        let last = parent.last_mut().expect("builder: closing a node whose parent has no child");
        need(last.count == 0 && last.node == NO_NODE, "builder: child already closed");
        if count == 0 {
            // EVERY occurrence of this context crossed a boundary: it is not a
            // match anywhere in the corpus. The zero-count edge is dropped - a
            // branch that never fired is noise the brute-force oracle does not
            // have (and a reader must not be offered a content byte that no
            // in-document occurrence ever took).
            parent.pop();
            return;
        }
        last.count = count;
        if count < self.min_support {
            return;
        }
        let id = self.emit_node(depth, count, &lv.children, &lv.docs);
        let parent = if depth == 1 { &mut self.root } else { &mut self.stack[depth - 2].children };
        parent.last_mut().expect("builder: parent child vanished").node = id;
    }

    fn emit_node(&mut self, depth: usize, span: u64, children: &[Child], docs: &DocAcc) -> u32 {
        need(children.len() <= u16::MAX as usize, "node has more than 65535 children");
        let me = self.nnodes as u32; // the id this node is about to get
        let first_edge = self.prev_edge_end;
        // first_edge is monotone in post-order (edges are appended in emission
        // order), so the column stores the delta from the previous node and stays
        // at 1 byte per 256 nodes for the leaf-heavy middle of the tree.
        self.nedge.push(first_edge - self.prev_first_edge);
        self.prev_first_edge = first_edge;
        for c in children {
            self.ecode.push(c.code as u64);
            self.echild.push(if c.node == NO_NODE { 0 } else { (me - c.node) as u64 });
            self.ecount.push(c.count);
        }
        let (flags, ndocs, top, first, nids) = self.doc_summary(span, docs);
        self.dndocs.push(ndocs);
        self.dtop.push(top as u64);
        self.dfirst.push(first);
        if nids > 0 {
            self.ndoc_nodes += 1;
        }
        let mut f = [0u8; 4];
        put(&mut f[0..], depth as u64, 1);
        put(&mut f[1..], children.len() as u64, 2);
        put(&mut f[3..], flags as u64, 1);
        self.nfixed.extend_from_slice(&f);
        self.ncount.push(span);
        let id = self.nnodes as u32;
        self.nnodes += 1;
        self.prev_edge_end += children.len() as u64;
        id
    }

    /// Document summary of a node: (flags, distinct estimate, heavy-doc candidate,
    /// exact-id-list index, list length).
    fn doc_summary(&mut self, span: u64, d: &DocAcc) -> (u8, u64, u32, u64, u64) {
        if d.n == 0 && !d.over {
            return (0, 0, NO_NODE, u64::MAX, 0);
        }
        if span < self.docs_min_support {
            return (0, 0, NO_NODE, u64::MAX, 0);
        }
        let exact = !d.over;
        let flags = 1u8 | if exact { 2 } else { 0 };
        let top = d.heavy();
        if exact {
            let first = self.dids.len();
            for i in 0..d.n {
                self.dids.push(d.small[i] as u64);
            }
            (flags, d.n as u64, top, first, d.n as u64)
        } else {
            (flags, d.hll.estimate(), top, u64::MAX, 0)
        }
    }

    /// Close the root and serialise the member payload.
    pub fn finish(mut self, n: u64, corpus_len: u64) -> (Vec<u8>, [u64; TRIE_SECTIONS]) {
        while !self.stack.is_empty() {
            self.close_one();
        }
        let root_children = std::mem::take(&mut self.root);
        let root_docs = std::mem::replace(&mut self.root_docs, DocAcc::new());
        let root = self.emit_node(0, n, &root_children, &root_docs);
        let nodes = self.nnodes;
        let edges = self.prev_edge_end;
        need(self.ecode.len() == edges && self.ecount.len() == edges && self.echild.len() == edges,
            "edge column lengths");
        let secs: [Vec<u8>; TRIE_SECTIONS] = [
            std::mem::take(&mut self.nfixed),
            std::mem::replace(&mut self.nedge, Packed::new()).finish(),
            std::mem::replace(&mut self.ncount, Packed::new()).finish(),
            std::mem::replace(&mut self.ecode, Packed::new()).finish(),
            std::mem::replace(&mut self.echild, Packed::new()).finish(),
            std::mem::replace(&mut self.ecount, Packed::new()).finish(),
            std::mem::replace(&mut self.dndocs, Packed::new()).finish(),
            std::mem::replace(&mut self.dtop, Packed::new()).finish(),
            std::mem::replace(&mut self.dfirst, Packed::new()).finish(),
            std::mem::replace(&mut self.dids, Packed::new()).finish(),
        ];
        let mut offs = [0u64; TRIE_SECTIONS];
        let mut at = HEAD as u64;
        for (i, s) in secs.iter().enumerate() {
            offs[i] = at;
            at += ((s.len() as u64 + 7) / 8) * 8;
        }
        let mut out = vec![0u8; at as usize];
        for (i, s) in secs.iter().enumerate() {
            out[offs[i] as usize..offs[i] as usize + s.len()].copy_from_slice(s);
        }
        put(&mut out[0..], TRIE_VERSION, 8);
        put(&mut out[8..], self.depth as u64, 8);
        put(&mut out[16..], self.min_support, 8);
        put(&mut out[24..], n, 8);
        put(&mut out[32..], corpus_len, 8);
        put(&mut out[40..], nodes, 8);
        put(&mut out[48..], edges, 8);
        put(&mut out[56..], self.docs_min_support, 8);
        for i in 0..TRIE_SECTIONS {
            put(&mut out[64 + 8 * i..], offs[i], 8);
        }
        put(&mut out[144..], root as u64, 8);
        put(&mut out[152..], self.source as u64, 8);
        put(&mut out[160..], self.source_mem, 8);
        put(&mut out[168..], self.boundary_aware as u64, 8);
        let cols = secs.iter().map(|s| s.len() as u64).collect::<Vec<_>>();
        let mut c = [0u64; TRIE_SECTIONS];
        c.copy_from_slice(&cols);
        (out, c)
    }

    /// Column sizes, for the build's own budget line: what each field costs.
    pub fn column_bytes(&self) -> [u64; TRIE_SECTIONS] {
        [
            self.nfixed.len() as u64,
            self.nedge.bytes(),
            self.ncount.bytes(),
            self.ecode.bytes(),
            self.echild.bytes(),
            self.ecount.bytes(),
            self.dndocs.bytes(),
            self.dtop.bytes(),
            self.dfirst.bytes(),
            self.dids.bytes(),
        ]
    }
}

// -------------------------------------------------------------------- the reader

pub struct Trie<'a> {
    d: &'a [u8],
    pub depth: u64,
    pub min_support: u64,
    pub docs_min_support: u64,
    pub n: u64,
    pub corpus_len: u64,
    pub nodes: u64,
    pub edges: u64,
    pub source: u64,
    pub source_mem: u64,
    /// 1 = every fed row was classified against its document's end: node counts are
    /// NON-CROSSING occurrence counts and the END edge is "this context ends a
    /// document (or the text)". 0 (also every member format < 3) = built without
    /// a starts table; the histogram conflates crossing continuations with
    /// content and boundary-sensitive queries against it are REFUSED.
    pub boundary_aware: u64,
    root: u32,
    offs: [usize; TRIE_SECTIONS],
    // the first-edge absolute-offset prefix sums, built once at the first random
    // access; OnceLock (not OnceCell) because consult workers share one &Trie
    // across threads
    starts: std::sync::OnceLock<Vec<u32>>,
    nedge: PackedView<'a>,
    ncount: PackedView<'a>,
    ecode: PackedView<'a>,
    echild: PackedView<'a>,
    ecount: PackedView<'a>,
    dndocs: PackedView<'a>,
    dtop: PackedView<'a>,
    dfirst: PackedView<'a>,
    dids: PackedView<'a>,
}

pub struct ChildView {
    pub code: u16,
    pub count: u64,
    pub node: Option<u32>,
}
impl ChildView {
    pub fn byte(&self) -> Option<u8> {
        if self.code == END { None } else { Some((self.code - 1) as u8) }
    }
    pub fn label(&self) -> String {
        match self.byte() {
            Some(b) => format!("0x{:02x}", b),
            None => "END".to_string(),
        }
    }
}

pub struct Stat {
    pub depth: u64,
    pub count: u64,
    pub ndocs: u64,
    pub exact_docs: bool,
    pub top_doc: Option<u32>,
    pub children: Vec<ChildView>,
}

impl<'a> Trie<'a> {
    pub fn load(d: &'a [u8]) -> Self {
        need(d.len() >= 8, "member shorter than its version field");
        let ver = get(&d[0..], 8);
        let hint = if ver == 1 { " (version 1 is the first-draft layout)" } else { "" };
        need(ver >= 1 && ver <= TRIE_VERSION,
            &format!("m20 member format version {ver} is not supported: this build reads 1..={TRIE_VERSION}{hint}"));
        need(d.len() >= HEAD, &format!("m20 member is {} bytes, shorter than its {HEAD}-byte head", d.len()));
        let depth = get(&d[8..], 8);
        let min_support = get(&d[16..], 8);
        let n = get(&d[24..], 8);
        let corpus_len = get(&d[32..], 8);
        let nodes = get(&d[40..], 8);
        let edges = get(&d[48..], 8);
        let docs_min_support = get(&d[56..], 8);
        need(depth >= 1 && depth <= 64 && min_support >= 1 && n >= 1, "member head values");
        let mut offs = [0usize; TRIE_SECTIONS];
        for i in 0..TRIE_SECTIONS {
            offs[i] = get(&d[64 + 8 * i..], 8) as usize;
            // THE LAYOUT DISCRIMINATOR: version 1's first draft had a 128-byte head
            // and fixed 16-byte node/edge slots, so its first section offset is 128.
            // A 192-byte head is the packed layout. Refusing on the offset alone
            // (as an older reader did) says nothing; this says which layout it is.
            need(offs[i] >= HEAD && offs[i] <= d.len(),
                &format!("m20 member section {i} offset {} is outside [{HEAD},{}]: this member has the \
                         128-byte first-draft head, which this build does not read (member version {ver})",
                    offs[i], d.len()));
        }
        let root = get(&d[144..], 8) as u32;
        need((root as u64) < nodes.max(1), "root node id");
        need(HEAD as u64 + nodes * 4 <= d.len() as u64, "node column bounds");
        Self {
            d,
            depth,
            min_support,
            docs_min_support,
            n,
            corpus_len,
            nodes,
            edges,
            source: get(&d[152..], 8),
            source_mem: get(&d[160..], 8),
            boundary_aware: if ver >= 3 { get(&d[168..], 8) } else { 0 },
            root,
            offs,
            starts: std::sync::OnceLock::new(),
            nedge: PackedView::new(&d[offs[1]..], nodes),
            ncount: PackedView::new(&d[offs[2]..], nodes),
            ecode: PackedView::new(&d[offs[3]..], edges),
            echild: PackedView::new(&d[offs[4]..], edges),
            ecount: PackedView::new(&d[offs[5]..], edges),
            dndocs: PackedView::new(&d[offs[6]..], nodes),
            dtop: PackedView::new(&d[offs[7]..], nodes),
            dfirst: PackedView::new(&d[offs[8]..], nodes),
            dids: PackedView::new(&d[offs[9]..], get(&d[offs[9]..], 8)),
        }
    }

    /// (depth, child count, flags)
    fn node(&self, i: u32) -> (u8, usize, u8) {
        need((i as u64) < self.nodes, "node id");
        let o = self.offs[0] + i as usize * 4;
        let b = &self.d[o..o + 4];
        (b[0], get(&b[1..], 2) as usize, b[3])
    }

    /// First edge of a node. The stored column is a delta (a node's children start
    /// where the previous node's children ended); the absolute offsets are summed
    /// ONCE into a transient array at the first random access, never per lookup.
    fn first_edge(&self, i: u32) -> u32 {
        if self.starts.get().is_none() {
            let mut v = Vec::with_capacity(self.nodes as usize + 1);
            let mut at = 0u64;
            for k in 0..self.nodes {
                at += self.nedge.get(k);
                v.push(at as u32);
            }
            v.push(at as u32);
            let last = (self.nodes - 1) as u32;
            let nch = self.node(last).1 as u64;
            need(at + nch == self.edges,
                &format!("edge offset column ends at {at} + {nch} children, edge count is {}", self.edges));
            let _ = self.starts.set(v);
        }
        self.starts.get().unwrap()[i as usize]
    }

    fn edge_at(&self, node: u32, k: usize) -> ChildView {
        let i = self.first_edge(node) + k as u32;
        let code = self.ecode.get(i as u64) as u16;
        let d = self.echild.get(i as u64);
        ChildView {
            code,
            count: self.ecount.get(i as u64),
            node: if d == 0 { None } else { Some(node - d as u32) },
        }
    }

    pub fn children(&self, node: u32) -> Vec<ChildView> {
        let (_, nch, _) = self.node(node);
        (0..nch).map(|k| self.edge_at(node, k)).collect()
    }

    pub fn count(&self, node: u32) -> u64 {
        self.ncount.get(node as u64)
    }

    pub fn node_depth(&self, node: u32) -> u64 {
        self.node(node).0 as u64
    }

    /// (exact?, distinct documents, heavy-doc candidate)
    pub fn docs(&self, node: u32) -> (bool, u64, Option<u32>) {
        let (_, _, flags) = self.node(node);
        if flags & 1 == 0 {
            return (false, 0, None);
        }
        let ndocs = self.dndocs.get(node as u64);
        let top = self.dtop.get(node as u64) as u32;
        (flags & 2 != 0, ndocs, if top == NO_NODE { None } else { Some(top) })
    }

    /// The exact document id list, when the node kept one.
    pub fn doc_ids(&self, node: u32) -> Vec<u32> {
        let (exact, ndocs, _) = self.docs(node);
        if !exact {
            return Vec::new();
        }
        let first = self.dfirst.get(node as u64);
        (0..ndocs).map(|k| self.dids.get(first + k) as u32).collect()
    }

    pub fn root(&self) -> u32 {
        self.root
    }

    pub fn find(&self, pat: &[u8]) -> Option<u32> {
        if pat.len() as u64 > self.depth {
            return None;
        }
        let mut node = self.root;
        for &b in pat {
            let (_, nch, _) = self.node(node);
            let want = 1 + b as u16;
            let mut lo = 0usize;
            let mut hi = nch;
            let mut found = None;
            while lo < hi {
                let mid = (lo + hi) / 2;
                let e = self.edge_at(node, mid);
                if e.code == want {
                    found = e.node;
                    break;
                }
                if e.code < want { lo = mid + 1 } else { hi = mid }
            }
            node = found?;
        }
        Some(node)
    }

    /// THE NORMAL PATH: descend from the root as far as this context exists and
    /// stop at the deepest node that does. Returns (node, matched prefix length).
    /// This is `relax` in MEMORY_OPS.md: descend while the context is supported,
    /// back off when it is not. A miss is NOT an error and is NOT scanned for: the
    /// measured price of a locate is ~40 ms/position on the 10 GB container, so the
    /// tail is answered by the caller from `seek` (backward search) or not at all.
    pub fn descend(&self, pat: &[u8]) -> (u32, usize) {
        let mut node = self.root;
        let mut depth = 0usize;
        for &b in pat.iter().take(self.depth as usize) {
            let (_, nch, _) = self.node(node);
            let want = 1 + b as u16;
            let mut lo = 0usize;
            let mut hi = nch;
            let mut found = None;
            while lo < hi {
                let mid = (lo + hi) / 2;
                let e = self.edge_at(node, mid);
                if e.code == want {
                    found = e.node;
                    break;
                }
                if e.code < want { lo = mid + 1 } else { hi = mid }
            }
            match found {
                Some(n) => {
                    node = n;
                    depth += 1;
                }
                None => break,
            }
        }
        (node, depth)
    }

    pub fn stat(&self, node: u32) -> Stat {
        let (exact, ndocs, top) = self.docs(node);
        Stat {
            depth: self.node_depth(node),
            count: self.count(node),
            ndocs,
            exact_docs: exact,
            top_doc: top,
            children: self.children(node),
        }
    }
}

// --------------------------------------------------------------- SA from the LF cycle
//
// The ordered stream the builder consumes is the suffix array itself. A lite
// container does not store it, but LF does: LF maps row i to the row of SA[i]-1,
// so following LF from any row visits EVERY row exactly once (the text is
// primitive: the END symbol is unique) and yields SA[row] for all of them at one
// LF step per row. `K` segments of that cycle can run in parallel because each
// start row is an arbitrary row on the same cycle: segment k walks n/K steps from
// its own start. Cost: n LF steps + n u40 writes; no scratch, no text copy.
const SA_ITEM: usize = 5; // u40: n <= 2^40 = 1.1e12
const SA_UNSET: u64 = (1u64 << 40) - 1;

fn sa_put(sa: &mut [u8], i: u64, v: u64) {
    let o = i as usize * SA_ITEM;
    sa[o..o + 5].copy_from_slice(&v.to_le_bytes()[..5]);
}
fn sa_get(sa: &[u8], i: u64) -> u64 {
    let o = i as usize * SA_ITEM;
    let mut x = [0u8; 8];
    x[..5].copy_from_slice(&sa[o..o + 5]);
    u64::from_le_bytes(x)
}

/// The every-run columns: BWT char and run length per run, plus the per-char
/// occurrence table sampled at block boundaries. The LF walk is a random access per
/// step; through the query kernel that is a block decode (8 us measured, and the
/// LRU does not help on a random walk), so the builder decodes m1 ONCE into flat
/// columns and makes a step two cache-shaped scans instead. This is build-time
/// memory: 5 B/run + 256 * 8 B per 1024 runs, freed when the sweep is done.
pub struct Dense {
    pub heads: Vec<u8>,
    pub lens: Vec<u32>,
    br: u64,
    block_first: Vec<u64>,
    occ: Vec<u64>,
    c: [u64; 256],
    pub r: u64,
    pub n: u64,
}

impl Dense {
    pub fn build(idx: &qlite::Index, threads: usize) -> Dense {
        let t0 = std::time::Instant::now();
        let r = idx.reader.r;
        let n = idx.reader.n;
        let br = idx.runs_per_block();
        let nb = idx.block_count();
        need(br > 0 && nb > 0 && nb * br >= r, "m1 block geometry");
        let mut heads = vec![0u8; r as usize];
        let mut lens = vec![0u32; r as usize];
        // raw pointers as `usize` so the closure is plainly Send; the threads touch
        // disjoint run ranges (block-strided), which the block geometry guarantees.
        let hp = heads.as_mut_ptr() as usize;
        let lp = lens.as_mut_ptr() as usize;
        let nt = threads.max(1) as u64;
        std::thread::scope(|sc| {
            let mut hs = Vec::new();
            for t in 0..nt {
                hs.push(sc.spawn(move || {
                    let mut arena = qlite::WorkerArena::new(idx).expect("arena");
                    let mut b = t;
                    while b < nb {
                        let base = (b * br) as usize;
                        let want = ((r - b * br).min(br)) as usize;
                        unsafe {
                            let h = std::slice::from_raw_parts_mut((hp as *mut u8).add(base), want);
                            let l = std::slice::from_raw_parts_mut((lp as *mut u32).add(base), want);
                            idx.decode_block_into(&mut arena, b, h, l);
                        }
                        b += nt;
                    }
                }));
            }
            for h in hs {
                h.join().expect("dense decode thread panicked");
            }
        });
        // block row starts and the per-char occurrence table at every block edge
        let mut block_first = Vec::with_capacity(nb as usize + 1);
        let mut occ = vec![0u64; (nb as usize + 1) * 256];
        let mut cum = [0u64; 256];
        let mut row = 0u64;
        block_first.push(0);
        for b in 0..nb {
            let base = b * br;
            let end = (base + br).min(r);
            let mut len_sum = 0u64;
            for i in base..end {
                let l = u64::from(lens[i as usize]);
                cum[heads[i as usize] as usize] += l;
                len_sum += l;
            }
            // a block's row span is the SUM OF ITS RUN LENGTHS, not its run count
            row += len_sum;
            block_first.push(row);
            let o = (b as usize + 1) * 256;
            occ[o..o + 256].copy_from_slice(&cum);
        }
        need(row == n, "dense runs do not sum to n");
        crate::trace_since("ctx.dense-runs", t0);
        Dense { heads, lens, br, block_first, occ, c: idx.c, r, n }
    }

    fn block_of(&self, row: u64) -> u64 {
        let v = &self.block_first;
        let (mut lo, mut hi) = (0usize, v.len() - 1);
        while hi - lo > 1 {
            let mid = (lo + hi) / 2;
            if v[mid] <= row {
                lo = mid
            } else {
                hi = mid
            }
        }
        lo as u64
    }

    /// LF(row) = C[h] + occ_h(row), computed from the flat columns.
    #[inline]
    pub fn lf_step(&self, row: u64) -> u64 {
        let b = self.block_of(row);
        let base = (b * self.br) as usize;
        let end = (((b + 1) * self.br).min(self.r)) as usize;
        let hs = &self.heads[base..end];
        let ls = &self.lens[base..end];
        let mut s = self.block_first[b as usize];
        let mut run = 0usize;
        while s + u64::from(ls[run]) <= row {
            s += u64::from(ls[run]);
            run += 1;
        }
        let h = hs[run] as usize;
        let mut occ = self.occ[(b as usize + 1) * 256 + h];
        // the block edge sample is the count BEFORE block b+1; walk back from the
        // block's own start instead, which is the same table one block earlier
        occ = self.occ[b as usize * 256 + h];
        for i in 0..run {
            if hs[i] as usize == h {
                occ += u64::from(ls[i]);
            }
        }
        let _ = end;
        self.c[h] + occ + (row - s)
    }

    pub fn bytes(&self) -> u64 {
        self.heads.len() as u64
            + 4 * self.lens.len() as u64
            + 8 * self.block_first.len() as u64
            + 8 * self.occ.len() as u64
    }
}

/// The K breakpoints that partition the LF cycle.
///
/// THE ARCS MUST BE BALANCED, and "rows i*n/K" does NOT balance them: the cycle
/// order is by position, and K rows chosen evenly in ROW order land at arbitrary
/// positions, so one thread can be handed 90 % of the cycle while the rest idle
/// (measured: 4 of 32 threads still walking after 20 minutes at 10 GB). The fix is
/// to locate 4*K candidate rows, order them by position (which IS cycle order) and
/// keep the candidate closest to each ideal grid point j*n/K, so every arc is
/// within a factor of ~2 of n/K.
fn sa_breakpoints(idx: &qlite::Index, threads: usize) -> Vec<(u64, u64)> {
    let n = idx.reader.n;
    let k = (threads.max(1) as u64).min(n).min(64);
    let cand = (4 * k).min(n);
    let rows: Vec<u64> = (0..cand).map(|i| i * n / cand).collect();
    let mut all: Vec<(u64, u64)> = rows
        .iter()
        .map(|&r| {
            let mut ctx = idx.ctx();
            (r, ctx.sa_of(r))
        })
        .collect();
    let mut seen = std::collections::HashSet::new();
    for &(_, p) in all.iter() {
        need(seen.insert(p), "SA reconstruction: two candidate breakpoints at one position");
    }
    // cycle order = descending position
    all.sort_by(|a, b| b.1.cmp(&a.1));
    // keep the candidate closest to each ideal cycle position j*n/k
    let mut brk: Vec<(u64, u64)> = Vec::with_capacity(k as usize);
    let mut used = vec![false; all.len()];
    for j in 0..k {
        // ideal cycle index j*n/k == position n-1 - j*n/k (mod n)
        let want = (n - 1 + n - (j * n / k) % n) % n;
        let mut best = usize::MAX;
        let mut best_d = u64::MAX;
        for (i, &(_, p)) in all.iter().enumerate() {
            if used[i] {
                continue;
            }
            let d = if p > want { p - want } else { want - p };
            let d = d.min(n - d);
            if d < best_d {
                best_d = d;
                best = i;
            }
        }
        need(best != usize::MAX, "SA reconstruction: no candidate for a breakpoint");
        used[best] = true;
        brk.push(all[best]);
    }
    brk.sort_by(|a, b| b.1.cmp(&a.1));
    brk.dedup_by_key(|x| x.1);
    need(brk.len() >= 2, "SA reconstruction: fewer than two distinct breakpoints");
    brk
}

/// Walk the LF cycle from the breakpoints and fill `sa[row] = SA[row]`.
fn sa_walk<F>(n: u64, brk: Vec<(u64, u64)>, threads: usize, lf: F) -> (Vec<u8>, u64)
where
    F: Fn(u64) -> u64 + Sync,
{
    let mut sa = vec![0xFFu8; (n as usize) * SA_ITEM];
    let plan: Vec<(u64, u64, u64)> = (0..brk.len())
        .map(|j| (brk[j].0, brk[j].1, brk[(j + 1) % brk.len()].1))
        .collect();
    // The segments write disjoint rows; the two checks after the join (n steps
    // total, no row left at the sentinel) are what prove it.
    let sh = sa.as_mut_ptr() as usize;
    let results: Vec<u64> = std::thread::scope(|sc| {
        let mut hs = Vec::new();
        let per = (plan.len() as u64).div_ceil(threads.max(1) as u64).max(1) as usize;
        for chunk in plan.chunks(per) {
            let s = sh;
            let lf = &lf;
            hs.push(sc.spawn(move || {
                let mut steps = 0u64;
                for &(row0, pos0, stop) in chunk {
                    let mut row = row0;
                    let mut pos = pos0;
                    while pos != stop {
                        unsafe {
                            let o = (row as usize) * SA_ITEM;
                            std::ptr::copy_nonoverlapping(pos.to_le_bytes().as_ptr(), (s as *mut u8).add(o), 5);
                        }
                        row = lf(row);
                        pos = if pos == 0 { n - 1 } else { pos - 1 };
                        steps += 1;
                        need(steps <= n, "SA reconstruction: cycle did not reach its breakpoint");
                    }
                }
                steps
            }));
        }
        hs.into_iter().map(|h| h.join().expect("SA reconstruction thread panicked")).collect()
    });
    let steps_total: u64 = results.iter().sum();
    need(steps_total == n, &format!("SA reconstruction walked {steps_total} rows, expected n={n}"));
    let mut unset = 0u64;
    for i in 0..n {
        if sa_get(&sa, i) >= n {
            unset += 1;
        }
    }
    need(unset == 0, &format!("SA reconstruction left {unset} of {n} rows unset"));
    (sa, steps_total)
}

/// Reconstruct the suffix array through the query kernel (one block decode per
/// step - correct, and only affordable at fixture scale).
pub fn sa_reconstruct(idx: &qlite::Index, threads: usize) -> (Vec<u8>, u64) {
    let t0 = std::time::Instant::now();
    let n = idx.reader.n;
    need(n < SA_UNSET, "n does not fit the u40 suffix array");
    let brk = sa_breakpoints(idx, threads);
    let slots = std::sync::Arc::new(std::sync::Mutex::new(
        (0..64).map(|_| idx.ctx()).collect::<Vec<_>>(),
    ));
    let _ = &slots;
    // one Ctx per thread, made inside the walk: the kernel's Ctx borrows the index
    let sa = {
        // The dense accessor is not used here, so re-run the walk with a per-thread
        // kernel context.
        let mut out = vec![0xFFu8; (n as usize) * SA_ITEM];
        let plan: Vec<(u64, u64, u64)> = (0..brk.len())
            .map(|j| (brk[j].0, brk[j].1, brk[(j + 1) % brk.len()].1))
            .collect();
        let sh = out.as_mut_ptr() as usize;
        std::thread::scope(|sc| {
            let mut hs = Vec::new();
            for &(row0, pos0, stop) in plan.iter() {
                let s = sh;
                hs.push(sc.spawn(move || {
                    let mut cx = idx.ctx();
                    let mut row = row0;
                    let mut pos = pos0;
                    let mut steps = 0u64;
                    while pos != stop {
                        unsafe {
                            let o = (row as usize) * SA_ITEM;
                            std::ptr::copy_nonoverlapping(pos.to_le_bytes().as_ptr(), (s as *mut u8).add(o), 5);
                        }
                        let (nr, _) = cx.lf_step(row);
                        row = nr;
                        pos = if pos == 0 { n - 1 } else { pos - 1 };
                        steps += 1;
                        need(steps <= n, "SA reconstruction: cycle did not reach its breakpoint");
                    }
                    steps
                }));
            }
            for h in hs {
                h.join().expect("SA reconstruction thread panicked");
            }
        });
        out
    };
    let mut unset = 0u64;
    for i in 0..n {
        if sa_get(&sa, i) >= n {
            unset += 1;
        }
    }
    need(unset == 0, &format!("SA reconstruction left {unset} of {n} rows unset"));
    crate::trace_since("ctx.sa-reconstruct", t0);
    (sa, n)
}

// ------------------------------------------------------------------ doc ranks

/// Documents are runs of bytes delimited by 0x1E, counted on the ORIGINAL corpus
/// bytes (the container's remap is order preserving, so the SA order is the same
/// order, but the remap may move 0x1E - so 0x1E is counted on the corpus, never on
/// the remapped text). One u32 every `DOC_STRIDE` bytes; the sweep pays a bounded
/// SWAR tail scan per row, so the stride is what the pass costs (256 B = 32 words).
const DOC_STRIDE: usize = 256;

fn count_1e(w: u64) -> u32 {
    // SWAR: bytes equal to 0x1E
    let x = w ^ 0x1e1e_1e1e_1e1e_1e1e;
    ((x.wrapping_sub(0x0101_0101_0101_0101)) & !x & 0x8080_8080_8080_8080).count_ones() / 8
}

pub fn doc_ranks(text: &[u8]) -> Vec<u32> {
    doc_ranks_stride(text, DOC_STRIDE)
}

/// The same table at any stride. The value at block b is the number of 0x1E bytes
/// before byte b*stride, so `doc_of_stride` is EXACT for every stride; the stride
/// only trades RAM (4 B per stride) against the bounded tail scan (stride/8 words)
/// that every row pays. The post-hoc sweep uses DOC_STRIDE=256 to keep the table
/// at n/64; the in-build tap reads a random position per row anyway, so it spends
/// more table and less tail.
pub fn doc_ranks_stride(text: &[u8], stride: usize) -> Vec<u32> {
    let blocks = text.len() / stride + 1;
    let mut out = Vec::with_capacity(blocks);
    let mut acc = 0u32;
    let mut at = 0usize;
    out.push(0);
    while at + 8 <= text.len() {
        let mut w = [0u8; 8];
        w.copy_from_slice(&text[at..at + 8]);
        acc += count_1e(u64::from_le_bytes(w));
        at += 8;
        if at % stride == 0 {
            out.push(acc);
        }
    }
    while at < text.len() {
        if text[at] == 0x1e {
            acc += 1;
        }
        at += 1;
        if at % stride == 0 {
            out.push(acc);
        }
    }
    while out.len() < blocks {
        out.push(acc);
    }
    out
}

/// The document id of a text position: the number of 0x1E bytes before it, i.e.
/// the 0x1E span index. This is NOT the shard record count: MEMORY_OPS.md measures
/// 6,979 internal 0x1E bytes in the first 100 GB of the Pile, so a span split is
/// 0.043 % finer than a document. Stated, never hidden.
///
/// The reference block is DOC_STRIDE bytes, so the tail scan is bounded by that;
/// it counts 8 bytes at a time (the sweep does this once per row, so a byte loop
/// here would be the whole cost of the pass).
#[inline]
fn doc_of(rank: &[u32], text: &[u8], pos: u64) -> u32 {
    doc_of_stride(rank, text, pos, DOC_STRIDE)
}

#[inline]
fn doc_of_stride(rank: &[u32], text: &[u8], pos: u64, stride: usize) -> u32 {
    let p = pos as usize;
    let b = p / stride;
    let mut d = rank[b];
    let mut i = b * stride;
    while i + 8 <= p {
        let mut w = [0u8; 8];
        w.copy_from_slice(&text[i..i + 8]);
        d += count_1e(u64::from_le_bytes(w));
        i += 8;
    }
    while i < p {
        if text[i] == 0x1e {
            d += 1;
        }
        i += 1;
    }
    d
}

// ------------------------------------------------------------------- the build

pub struct BuildArgs {
    pub sxi: String,
    pub text: String,
    pub out: String,
    pub depth: usize,
    pub min_support: u64,
    pub threads: usize,
    pub docs: bool,
    /// stop after this many rows (a fixture-scale check of the pass itself)
    pub limit_rows: Option<u64>,
    /// decoded m1 block arena (MB): the LF walk is random over runs, so the arena
    /// IS the speed of the reconstruction. 0 = the query default (64 MB).
    pub cache_mb: u64,
    /// keep the reconstructed suffix array here and reuse it (the walk is the cost)
    pub sa_cache: String,
    /// store the document summary only for nodes with at least this many
    /// occurrences (below it `project` answers from seek + the document map)
    pub docs_min_support: u64,
    /// decode every run once before the LF walk (build-time memory, 5 B/run)
    pub dense: bool,
}

/// `XSA_CTX_SA_PROBE=<steps>`: time a single-threaded LF walk and report its
/// rate, so the price of the ordered stream is measured, not guessed.
fn sa_probe(idx: &qlite::Index, steps: u64, stateless: bool) {
    let n = idx.reader.n;
    let mut row = 0u64;
    let (mut pos, mut got) = (0u64, 0u64);
    let t0 = std::time::Instant::now();
    if stateless {
        let arena = std::cell::RefCell::new(qlite::WorkerArena::new(idx).expect("arena"));
        let mut cx = idx.worker_ctx(&arena);
        pos = cx.sa_of(row);
        for _ in 0..steps {
            let (nr, _) = cx.lf_step(row);
            row = nr;
            pos = if pos == 0 { n - 1 } else { pos - 1 };
            got += 1;
        }
        let w = t0.elapsed().as_secs_f64();
        eprintln!("SA_PROBE stateless steps={got} wall={w:.3}s ns_per_step={:.1} decodes={}",
            w * 1e9 / got as f64, arena.borrow().decodes);
    } else {
        let mut cx = idx.ctx();
        pos = cx.sa_of(row);
        for _ in 0..steps {
            let (nr, _) = cx.lf_step(row);
            row = nr;
            pos = if pos == 0 { n - 1 } else { pos - 1 };
            got += 1;
        }
        let w = t0.elapsed().as_secs_f64();
        eprintln!("SA_PROBE shared steps={got} wall={w:.3}s ns_per_step={:.1}", w * 1e9 / got as f64);
    }
}

/// The ordered stream: the suffix array, from the cache if it is there.
pub fn load_or_build_sa(
    idx: &qlite::Index,
    threads: usize,
    cache: &str,
    dense_on: bool,
) -> (Vec<u8>, u64, u8, u64) {
    let n = idx.reader.n;
    if !cache.is_empty() {
        if let Ok(f) = std::fs::File::open(cache) {
            let len = f.metadata().unwrap().len();
            need(len == n * SA_ITEM as u64,
                &format!("SA cache {cache} is {len} bytes, expected n*5 = {}", n * SA_ITEM as u64));
            let map = unsafe { Mmap::map(&f) }.unwrap_or_else(|e| die(&format!("mmap {cache}: {e}")));
            eprintln!("SA_CACHE_REUSED path={cache} rows={n}");
            return (map.to_vec(), 0, 2, 0);
        }
    }
    let t0 = std::time::Instant::now();
    let mut mode = 0u8;
    let mut dense_mem = 0u64;
    let (sa, steps) = if dense_on && idx.reader.r > 2_000_000 {
        mode = 1;
        // build-time dense columns: one decode pass, then two cache-shaped scans
        // per LF step instead of a block decode per step
        let dense = Dense::build(idx, threads);
        dense_mem = dense.bytes();
        eprintln!("DENSE_RUNS runs={} bytes={} wall={:.1}s",
            dense.r, dense.bytes(), t0.elapsed().as_secs_f64());
        let brk = sa_breakpoints(idx, threads);
        let t1 = std::time::Instant::now();
        let (sa, steps) = sa_walk(n, brk, threads, |row| dense.lf_step(row));
        eprintln!("SA_WALK rows={n} steps={steps} wall={:.1}s dense=1", t1.elapsed().as_secs_f64());
        (sa, steps)
    } else {
        sa_reconstruct(idx, threads)
    };
    if !cache.is_empty() {
        let tmp = format!("{cache}.partial");
        std::fs::write(&tmp, &sa).unwrap_or_else(|e| die(&format!("write {tmp}: {e}")));
        std::fs::rename(&tmp, cache).unwrap_or_else(|e| die(&format!("rename {tmp}: {e}")));
        eprintln!("SA_CACHE_WRITTEN path={cache} rows={n} bytes={}", sa.len());
    }
    (sa, steps, mode, dense_mem)
}

/// One ordered sweep -> one member payload.
///
/// `starts` is the boundary oracle: when present every suffix is CUT at its
/// document's end before feeding (the no-cross law, by construction), the doc
/// summary's ids are CONTAINER document ids, and the member records
/// boundary_aware=1. When absent the build is the legacy conflated one and the
/// member records boundary_aware=0.
#[allow(clippy::too_many_arguments)]
pub fn sweep(
    sa: &[u8],
    text: &[u8],
    ranks: &[u32],
    starts: &OwnedStarts,
    corpus_len: u64,
    n: u64,
    rows: u64,
    depth: usize,
    min_support: u64,
    docs: bool,
    docs_min_support: u64,
    source: (u8, u64),
) -> (Vec<u8>, u64, [u64; TRIE_SECTIONS]) {
    let mut b = Builder::new(depth, min_support, docs_min_support);
    b.set_source(source.0, source.1);
    let aware = starts.is_some();
    if aware {
        b.set_boundary_aware();
    }
    let dmax = depth as u64 + 1;
    let mut buf = [0u8; 128];
    let mut reads = 0u64;
    for row in 0..rows {
        let pos = sa_get(sa, row);
        need(pos <= corpus_len, "suffix array position");
        let rem = corpus_len - pos;
        // the FULL prefix: the stream's sorted-order contract is untouched; the
        // law is applied per row inside the builder (the cut is classification,
        // never a stream edit)
        let m = rem.min(dmax) as usize;
        if m > 0 {
            let p = pos as usize;
            buf[..m].copy_from_slice(&text[p..p + m]);
            reads += 1;
        }
        let (avail, doc) = match starts.avail(pos, corpus_len) {
            Some((doc, avail)) => (avail, doc),
            None => (
                u64::MAX,
                if !aware && docs && m > 0 && !ranks.is_empty() {
                    doc_of(ranks, text, pos)
                } else {
                    0
                },
            ),
        };
        b.feed(&buf, rem, avail, doc);
    }
    let (payload, cols) = b.finish(n, corpus_len);
    (payload, reads, cols)
}

// ---------------------------------------------------------------- the band tap
//
// WHY THIS EXISTS. The post-hoc path (`ctx-build`) reconstructs the suffix array
// out of the container - one LF cycle, 5 B of transient RAM per row, n LF steps
// (47 GB and 1,192 s at 10 GB) - and then sweeps it. Inside `xsa build` the
// ordered stream ALREADY EXISTS: the sorter's band slices, concatenated in band
// order, ARE the suffix array, and each row carries its position. This tap reads
// that stream instead of replaying it: no LF cycle, no u40 column, no cache.
//
// THE ORDERED STREAM, NAMED. `scatter_sort --tape-bands` publishes band_<i>.sa,
// 8 B per row: low 48 bits the suffix position, top 8 bits the carried BWT
// symbol. Band indices ascend in rank order, so concat(band_0 .. band_k) is
// SA[0..row]. Its normal consumer is `emit-tape` (cross_lcp_merge), which unlinks
// each band as it absorbs it. Under TRIE_TAP=1 the emitter does NOT unlink and
// does not stamp `.consumed`; it leaves the slice and its already-existing
// `.opened` marker. This tap then walks the bands in ASCENDING INDEX (subject to
// the emitter having opened them, so no slice is removed out from under it) and
// for each one: read, feed, unlink, stamp `.consumed`. Unlink-while-open is safe:
// the emitter read the payload into its own buffer before it stamped `.opened`,
// and an open descriptor keeps the inode alive.
//
// WHY NOT FEED THE SORTER DIRECTLY. The builder is a SEQUENTIAL stack machine
// (row i's state depends on row i-1), and the sorter's bands are produced by P
// workers in arbitrary order. The band stream is the first place where the rows
// are both in rank order and out of the sorters' hands, so it is where the feed
// goes. What the tap does NOT do is reconstruct anything: the alternative it
// replaces is a full LF cycle plus a 5 B/row column in RAM.

/// Where the ordered stream came from, as the member records it (see `Builder`).
pub const SOURCE_IN_BUILD: u8 = 3;

pub struct TapArgs {
    /// the sorter's --tape-bands directory
    pub bands: String,
    /// the raw corpus: the only copy of the text, read through mmap
    pub corpus: String,
    /// where the m20 payload is written
    pub out: String,
    pub depth: usize,
    pub min_support: u64,
    pub docs_min_support: u64,
    pub threads: usize,
    /// n = corpus_len + 1 (the container's row count, END row included)
    pub n: u64,
    /// MEASUREMENT ONLY: a u40 suffix array (the post-hoc `--sa-cache` format) to
    /// feed from instead of the band stream. It exercises exactly the same
    /// per-row path (prefix read + builder) without a sorter, so the feed's own
    /// cost can be priced and optimised without a 55-minute build.
    pub sa: String,
    /// MEASUREMENT ONLY: stop after this many rows (0 = all).
    pub rows: u64,
    /// THE BOUNDARY ORACLE: a document-starts table (a `--docs-map` directory
    /// holding seps.bin, a CORPUS V2 `PREFIX.starts`, or a raw u64 `.starts`
    /// file). Present = the trie is built boundary-aware (no-cross, by
    /// construction). Absent = boundary_aware=0 in the member head.
    pub doc_starts: String,
}

/// Rows per prefetch chunk: the corpus reads are random, so they are issued by a
/// worker pool and consumed in order by the single-threaded builder.
const TAP_CHUNK: usize = 1 << 16;

fn exists(p: &str) -> bool {
    std::path::Path::new(p).exists()
}

/// Wait until the emitter has opened band `band`. `false` = there is no such
/// band and none will appear (the sorter published its final band count).
fn tap_wait_opened(dir: &str, band: u64) -> bool {
    let p = format!("{dir}/band_{band}.opened");
    let interrupted = format!("{dir}/producer.interrupted");
    loop {
        if exists(&p) {
            return true;
        }
        if exists(&interrupted) {
            eprintln!("TRIE_TAP_INTERRUPTED band={band} (the producer signalled an interruption)");
            std::process::exit(75);
        }
        if exists(&format!("{dir}/end.ready")) {
            if let Ok(s) = std::fs::read_to_string(format!("{dir}/end")) {
                if let Ok(count) = s.trim().parse::<u64>() {
                    if band >= count {
                        return false;
                    }
                }
            }
        }
        std::thread::sleep(std::time::Duration::from_millis(2));
    }
}

/// Feed one band's positions (in rank order) to the builder: the corpus reads are
/// issued by `threads` workers per chunk, the builder consumes the chunk in order.
/// In a boundary-aware build each suffix is CUT AT ITS DOCUMENT'S END here (the
/// `starts` oracle), so crossing contexts never enter deep nodes and a context
/// that ends a document lands on the END branch. Returns (rows fed, rows whose
/// prefix was read).
#[allow(clippy::too_many_arguments)]
fn tap_feed_band(
    b: &mut Builder,
    rows: &[u64],
    text: &[u8],
    ranks: &[u32],
    starts: &OwnedStarts,
    corpus_len: u64,
    depth: usize,
    docs: bool,
    threads: usize,
    doc_stride: usize,
    pfx: &mut Vec<u8>,
    docs_buf: &mut Vec<u32>,
    ms: &mut Vec<u8>,
    rems: &mut Vec<u64>,
) -> (u64, u64) {
    let w = depth + 1;
    let mut fed = 0u64;
    let mut reads = 0u64;
    for chunk in rows.chunks(TAP_CHUNK) {
        let c = chunk.len();
        pfx.clear();
        pfx.resize(c * w, 0);
        docs_buf.clear();
        docs_buf.resize(c, 0);
        ms.clear();
        ms.resize(c, 0);
        rems.clear();
        rems.resize(c, 0);
        // THE PREFIX READ, IN PARALLEL. One bounded random read per row from the
        // corpus mmap - the same read the post-hoc sweep makes, and the reason
        // the sweep cannot be merely multi-threaded (the STATE is sequential,
        // the reads are not). In a boundary-aware build the same worker pays the
        // rank/select that cuts the suffix at its document's end, so the oracle
        // costs nothing on the sequential path.
        let nthreads = threads.clamp(1, 64).min(c.max(1));
        let step = (c + nthreads - 1) / nthreads.max(1);
        let mut rest_rows: &[u64] = chunk;
        let mut rest_pfx: &mut [u8] = pfx.as_mut_slice();
        let mut rest_docs: &mut [u32] = docs_buf.as_mut_slice();
        let mut rest_ms: &mut [u8] = ms.as_mut_slice();
        let mut rest_rems: &mut [u64] = rems.as_mut_slice();
        std::thread::scope(|sc| {
            let mut hs = Vec::new();
            while !rest_rows.is_empty() {
                let take = rest_rows.len().min(step);
                let (rows_p, rows_rest) = rest_rows.split_at(take);
                let (pfx_p, pfx_rest) = rest_pfx.split_at_mut(take * w);
                let (d_p, d_rest) = rest_docs.split_at_mut(take);
                let (m_p, m_rest) = rest_ms.split_at_mut(take);
                let (r_p, r_rest) = rest_rems.split_at_mut(take);
                hs.push(sc.spawn(move || {
                    for (j, &pos) in rows_p.iter().enumerate() {
                        need(pos <= corpus_len, "TRIE_TAP_POSITION suffix position above the corpus");
                        let rem_full = corpus_len - pos;
                        // avail = bytes from pos to the END of pos's document
                        // (u64::MAX = no structure); the prefix read stays the
                        // FULL depth+1 bytes because the law is applied per row
                        // inside the builder, never by editing the stream
                        let (avail, doc) = match starts.avail(pos, corpus_len) {
                            Some((doc, avail)) => (avail, doc),
                            None => (
                                u64::MAX,
                                if docs && !ranks.is_empty() {
                                    doc_of_stride(ranks, text, pos, doc_stride)
                                } else {
                                    0
                                },
                            ),
                        };
                        r_p[j] = avail;
                        let m = (rem_full.min(w as u64)) as usize;
                        m_p[j] = m as u8;
                        if m > 0 {
                            let p = pos as usize;
                            pfx_p[j * w..j * w + m].copy_from_slice(&text[p..p + m]);
                            d_p[j] = doc;
                        }
                    }
                }));
                rest_rows = rows_rest;
                rest_pfx = pfx_rest;
                rest_docs = d_rest;
                rest_ms = m_rest;
                rest_rems = r_rest;
            }
            for h in hs {
                h.join().expect("TRIE_TAP worker panicked");
            }
        });
        // THE FEED, IN ORDER. This is the only sequential part, and the reason
        // the member is cheap: O(1) amortised per row, state O(depth).
        for j in 0..c {
            let pos = chunk[j];
            let rem = corpus_len - pos;
            let m = ms[j] as usize;
            if m > 0 {
                reads += 1;
            }
            b.feed(&pfx[j * w..j * w + m], rem, rems[j], docs_buf[j]);
        }
        fed += c as u64;
    }
    (fed, reads)
}

/// The whole tap: the band stream -> one m20 payload.
pub fn tap(a: &TapArgs) {
    let t_all = std::time::Instant::now();
    let tfile = std::fs::File::open(&a.corpus)
        .unwrap_or_else(|e| die(&format!("ctx-tap: open {}: {e}", a.corpus)));
    let flen = tfile.metadata().unwrap().len();
    need(flen + 1 == a.n,
        &format!("ctx-tap: corpus {} is {flen} bytes but n={} was declared: a lite container's n is the corpus length + 1 (one END row)", a.corpus, a.n));
    let text = unsafe { Mmap::map(&tfile) }.unwrap_or_else(|e| die(&format!("ctx-tap: mmap {}: {e}", a.corpus)));
    let corpus_len = flen;
    // THE BOUNDARY ORACLE (--doc-starts), loaded BEFORE any row is fed because it
    // decides what the member IS: present = boundary-aware (every suffix cut at
    // its document's end, so no crossing context ever enters a deep node and the
    // END edge is "this context ends a document"); absent = the legacy
    // conflated build, and the member head SAYS boundary_aware=0.
    let starts = if a.doc_starts.is_empty() {
        OwnedStarts::None
    } else {
        OwnedStarts::load(&a.doc_starts, corpus_len)
    };
    let t_doc = std::time::Instant::now();
    // The doc summary's ids: container document ids from the oracle when the
    // build is boundary-aware (a real document, not a 0x1E span); the legacy
    // per-stride 0x1E span table otherwise. 4 B per `stride` bytes of corpus:
    // 32 B stride below 2^31 rows, 64 B below 2^36, 256 B (the post-hoc
    // figure) above. The aware arm builds NO 0x1E table at all.
    let doc_stride = if a.n <= (1 << 31) {
        32
    } else if a.n <= (1 << 36) {
        64
    } else {
        256
    };
    let ranks = if starts.is_some() {
        Vec::new()
    } else {
        doc_ranks_stride(&text, doc_stride)
    };
    let doc_wall = t_doc.elapsed().as_secs_f64();
    let mut b = Builder::new(a.depth, a.min_support, a.docs_min_support);
    b.set_source(SOURCE_IN_BUILD, 0);
    if starts.is_some() {
        b.set_boundary_aware();
    }
    let mut pfx: Vec<u8> = Vec::new();
    let mut docs_buf: Vec<u32> = Vec::new();
    let mut ms: Vec<u8> = Vec::new();
    let mut rems: Vec<u64> = Vec::new();
    if !a.sa.is_empty() {
        // THE MEASUREMENT ARM: the same rows, from a u40 suffix array instead of
        // the band stream. Same prefix reads, same builder, same member - so the
        // per-row cost can be attributed without a build around it.
        let f = std::fs::File::open(&a.sa).unwrap_or_else(|e| die(&format!("ctx-tap: open {}: {e}", a.sa)));
        let sa = unsafe { Mmap::map(&f) }.unwrap_or_else(|e| die(&format!("ctx-tap: mmap {}: {e}", a.sa)));
        let rows = if a.rows == 0 { a.n } else { a.rows.min(a.n) };
        need(sa.len() as u64 >= rows * SA_ITEM as u64, "ctx-tap: the SA file is shorter than --rows");
        let mut pfx: Vec<u8> = Vec::new();
        let mut docs_buf: Vec<u32> = Vec::new();
        let mut ms: Vec<u8> = Vec::new();
        let t0 = std::time::Instant::now();
        let mut row = 0u64;
        let mut reads = 0u64;
        while row < rows {
            let c = (rows - row).min(TAP_CHUNK as u64) as usize;
            let pos: Vec<u64> = (0..c).map(|j| sa_get(&sa, row + j as u64)).collect();
            let (f2, r) = tap_feed_band(&mut b, &pos, &text, &ranks, &starts, corpus_len, a.depth, true, a.threads, doc_stride, &mut pfx, &mut docs_buf, &mut ms, &mut rems);
            need(f2 == c as u64, "ctx-tap: short feed");
            reads += r;
            row += c as u64;
        }
        let w = t0.elapsed().as_secs_f64();
        eprintln!("TRIE_TAP_SA_ROWS rows={row} n={} wall={w:.1}s ns_per_row={:.1} prefix_reads={reads} threads={} doc_stride={doc_stride}",
            a.n, w * 1e9 / row.max(1) as f64, a.threads);
        if a.rows != 0 && a.rows < a.n {
            return;
        }
        let (payload, _cols) = b.finish(a.n, corpus_len);
        let tmp = format!("{}.partial", a.out);
        std::fs::write(&tmp, &payload).unwrap_or_else(|e| die(&format!("ctx-tap: write {tmp}: {e}")));
        std::fs::rename(&tmp, &a.out).unwrap_or_else(|e| die(&format!("ctx-tap: rename {tmp}: {e}")));
        eprintln!("CONTEXT_TRIE_TAP_SA out={} bytes={}", a.out, payload.len());
        return;
    }
    let mut band = 0u64;
    let mut fed = 0u64;
    let mut reads = 0u64;
    let mut band_bytes = 0u64;
    let t_feed = std::time::Instant::now();
    let mut wait_s = 0f64;
    let mut bandread_s = 0f64;
    let mut work_s = 0f64;
    loop {
        let tw = std::time::Instant::now();
        let more = tap_wait_opened(&a.bands, band);
        wait_s += tw.elapsed().as_secs_f64();
        if !more { break; }
        let path = format!("{}/band_{band}.sa", a.bands);
        let tr = std::time::Instant::now();
        let bytes = std::fs::read(&path).unwrap_or_else(|e| die(&format!("ctx-tap: read {path}: {e}")));
        need(bytes.len() % 8 == 0, &format!("ctx-tap: {path} is {} bytes, not a multiple of 8", bytes.len()));
        band_bytes += bytes.len() as u64;
        let rows: Vec<u64> = bytes
            .chunks_exact(8)
            .map(|c| {
                let w = u64::from_le_bytes(c.try_into().unwrap());
                need(w >> 56 == 0, "ctx-tap: invalid packed band row (top byte must be zero)");
                w & ((1u64 << 48) - 1)
            })
            .collect();
        bandread_s += tr.elapsed().as_secs_f64();
        let tf = std::time::Instant::now();
        let (f, r) = tap_feed_band(&mut b, &rows, &text, &ranks, &starts, corpus_len, a.depth, true, a.threads, doc_stride, &mut pfx, &mut docs_buf, &mut ms, &mut rems);
        work_s += tf.elapsed().as_secs_f64();
        fed += f;
        reads += r;
        eprintln!("TRIE_TAP_BAND band={band} rows={f} total_rows={fed}");
        std::fs::remove_file(&path).unwrap_or_else(|e| die(&format!("ctx-tap: unlink {path}: {e}")));
        std::fs::write(format!("{}/band_{band}.consumed", a.bands), b"trie-tap\n")
            .unwrap_or_else(|e| die(&format!("ctx-tap: stamp consumed band {band}: {e}")));
        band += 1;
    }
    let feed_wall = t_feed.elapsed().as_secs_f64();
    need(band > 0, "ctx-tap: the band stream was empty");
    need(fed == a.n,
        &format!("TRIE_TAP_COVERAGE rows_fed={fed} != n={}: the band stream is not the whole suffix array", a.n));
    let (payload, cols) = b.finish(a.n, corpus_len);
    let tmp = format!("{}.partial", a.out);
    std::fs::write(&tmp, &payload).unwrap_or_else(|e| die(&format!("ctx-tap: write {tmp}: {e}")));
    std::fs::rename(&tmp, &a.out).unwrap_or_else(|e| die(&format!("ctx-tap: rename {tmp}: {e}")));
    let trie = Trie::load(&payload);
    let ratio = payload.len() as f64 / a.n as f64;
    let build_s = t_all.elapsed().as_secs_f64();
    eprintln!(
        "CONTEXT_TRIE_DEPTH depth={} min_support={} nodes={} edges={} bytes={} ratio_vs_n={:.6} source=in-build build_s={build_s:.1}",
        trie.depth, trie.min_support, trie.nodes, trie.edges, payload.len(), ratio
    );
    eprintln!(
        "CONTEXT_TRIE_BOUNDARY aware={} doc_starts={} docs_in_table={} law=no_match_may_cross_a_document_boundary \
         note=\"aware=1: every fed row was classified against its document's end, so node counts are \
         non-crossing occurrence counts, a crossing occurrence is counted nowhere, and the END \
         edge is 'this context ends a document (or the text)'; aware=0: the histogram conflates \
         crossing continuations with content and boundary-sensitive queries against it are \
         refused\"",
        trie.boundary_aware,
        if a.doc_starts.is_empty() { "-" } else { &a.doc_starts },
        starts.docs()
    );
    eprintln!(
        "CONTEXT_TRIE_COLUMNS node_fixed={} node_edge_delta={} node_count={} edge_code={} edge_child={} \
         edge_count={} doc_ndocs={} doc_top={} doc_first={} doc_ids={} head={HEAD}",
        cols[0], cols[1], cols[2], cols[3], cols[4], cols[5], cols[6], cols[7], cols[8], cols[9]
    );
    eprintln!(
        "CONTEXT_TRIE_TAP bands={band} rows={fed} n={} band_bytes={band_bytes} prefix_reads={reads} \
         wall={feed_wall:.1}s work_s={work_s:.1}s band_read_s={bandread_s:.1}s wait_s={wait_s:.1}s \
         doc_wall={doc_wall:.1}s doc_stride={doc_stride} doc_table_bytes={} rss_gb={:.3} threads={} source=in-build \
         note=\"work_s is the tap's OWN work (prefix reads + feed); the rest is waiting for the sorter; \
         the ordered stream is the sorter's band slices (8 B/row, already written); no LF cycle, no SA column\"",
        a.n, ranks.len() * 4, peak_rss_gb(), a.threads
    );
    eprintln!("CONTEXT_TRIE_BUDGET ratio_vs_n={ratio:.6} target<=0.05 pass={}", u8::from(ratio <= 0.05));
}

/// This process's peak RSS in GiB (VmHWM), the honest number for a phase.
pub fn peak_rss_gb() -> f64 {
    let s = std::fs::read_to_string("/proc/self/status").unwrap_or_default();
    for line in s.lines() {
        if let Some(v) = line.strip_prefix("VmHWM:") {
            if let Some(kb) = v.split_whitespace().next() {
                return kb.parse::<u64>().unwrap_or(0) as f64 / (1u64 << 20) as f64;
            }
        }
    }
    0.0
}

/// THE TAP'S OWN ORACLE (the pyramid's bottom rung, no container, no sorter).
///
/// For adversarial tiny texts: take the brute-force suffix order, write it as the
/// sorter writes it (8 B rows, split into bands of awkward sizes, in rank order),
/// run the tap over those bands, and require the payload to be BYTE-IDENTICAL to
/// the payload the post-hoc `sweep` produces from the same order. The sweep path is
/// itself gated against a brute-force trie by `ctx-selftest`, so equality here
/// closes the loop: the band stream is a faithful stand-in for the suffix array.
pub fn tap_selftest() {
    let texts: Vec<Vec<u8>> = vec![
        b"a".to_vec(),
        b"aaaaaa".to_vec(),
        b"mississippi".to_vec(),
        b"\x1eabc\x1edef\x1eabc\x1e".to_vec(),
        b"the quick brown fox jumps over the lazy dog the quick brown".to_vec(),
        b"\x00\x00\x00\x00\x01\x00\x01\x01".to_vec(),
    ];
    let mut cases = 0u64;
    for t in &texts {
        for &(depth, k) in &[(8usize, 1u64), (4, 2), (16, 1), (8, 1000)] {
            let n = t.len() as u64 + 1;
            let order = suffix_order(t);
            let dir = std::env::temp_dir().join(format!("xsa-tap-{}-{cases}", std::process::id()));
            let _ = std::fs::remove_dir_all(&dir);
            std::fs::create_dir_all(&dir).unwrap();
            let corpus = dir.join("corpus.bin");
            std::fs::write(&corpus, t).unwrap();
            // the sorter's band slices: 8 B per row, low 48 = position, top 8 = sym
            let bands = dir.join("bands");
            std::fs::create_dir_all(&bands).unwrap();
            let mut at = 0usize;
            let mut b = 0u64;
            let mut sizes = [1usize, 3, 7, 100, 5].iter().cycle();
            while at < order.len() {
                let take = (*sizes.next().unwrap()).min(order.len() - at);
                let mut bytes = Vec::with_capacity(take * 8);
                for &p in &order[at..at + take] {
                    let sym = if p > 0 { t[p as usize - 1] } else { 0u8 };
                    let w = (p & ((1u64 << 48) - 1)) | ((sym as u64) << 48);
                    bytes.extend_from_slice(&w.to_le_bytes());
                }
                std::fs::write(bands.join(format!("band_{b}.sa")), &bytes).unwrap();
                std::fs::write(bands.join(format!("band_{b}.opened")), b"\n").unwrap();
                at += take;
                b += 1;
            }
            std::fs::write(bands.join("end"), format!("{b}\n")).unwrap();
            std::fs::write(bands.join("end.ready"), b"\n").unwrap();
            let out = dir.join("m20.bin");
            tap(&TapArgs {
                bands: bands.to_string_lossy().to_string(),
                corpus: corpus.to_string_lossy().to_string(),
                out: out.to_string_lossy().to_string(),
                depth,
                min_support: k,
                docs_min_support: 1,
                threads: 2,
                n,
                sa: String::new(),
                rows: 0,
                doc_starts: String::new(),
            });
            // the post-hoc sweep over the SAME order, same parameters, same source
            let mut sa = vec![0u8; order.len() * SA_ITEM];
            for (i, &p) in order.iter().enumerate() {
                sa_put(&mut sa, i as u64, p);
            }
            let ranks = doc_ranks(t);
            let none = OwnedStarts::None;
            let (want, _, _) = sweep(&sa, t, &ranks, &none, t.len() as u64, n, n, depth, k, true, 1,
                (SOURCE_IN_BUILD, 0));
            let got = std::fs::read(&out).unwrap();
            need(got == want,
                &format!("TAP_SELFTEST: the band stream and the suffix array built DIFFERENT members (text len {}, depth {depth}, k {k}, {} vs {} bytes)",
                    t.len(), got.len(), want.len()));
            let trie = Trie::load(&got);
            eprintln!("TAP_SELFTEST text_len={} depth={depth} min_support={k} bands={b} nodes={} edges={} bytes={} identity=1",
                t.len(), trie.nodes, trie.edges, got.len());
            let _ = std::fs::remove_dir_all(&dir);
            cases += 1;
        }
    }
    // THE BOUNDARY-AWARE ARM: the same texts with METADATA starts (positions the
    // content does not suggest), the tap reading a raw u64 `.starts` file and the
    // sweep reading the same table in memory. The members must be byte-identical
    // AND say boundary_aware=1, and a spot oracle checks END counts that the
    // unaware arm could never produce (several occurrences of one context ending
    // documents).
    let mut cut_cases = 0u64;
    for t in &texts {
        let text_len = t.len() as u64;
        let n = text_len + 1;
        if text_len < 6 {
            continue;
        }
        let starts_table: Vec<u64> = {
            let mut s = vec![0u64, text_len / 3, (2 * text_len) / 3];
            s.retain(|&x| x == 0 || (x > 0 && x < text_len));
            s.dedup();
            if s.len() != 3 {
                continue;
            }
            s
        };
        let order = suffix_order(t);
        for &(depth, k) in &[(8usize, 1u64), (4, 1), (5, 2)] {
            // a FRESH band directory per case: the tap consumes (unlinks) the
            // sorter's slices as it absorbs them, by design
            let dir = std::env::temp_dir().join(format!("xsa-tapcut-{cut_cases}-{}-{depth}", std::process::id()));
            let _ = std::fs::remove_dir_all(&dir);
            std::fs::create_dir_all(&dir).unwrap();
            let corpus = dir.join("corpus.bin");
            std::fs::write(&corpus, t).unwrap();
            let starts_path = dir.join("doc.starts");
            std::fs::write(&starts_path, starts_table.iter().flat_map(|&v| v.to_le_bytes()).collect::<Vec<u8>>()).unwrap();
            let bands = dir.join("bands");
            std::fs::create_dir_all(&bands).unwrap();
            let mut at = 0usize;
            let mut bno = 0u64;
            let mut sizes = [2usize, 11, 5].iter().cycle();
            while at < order.len() {
                let take = (*sizes.next().unwrap()).min(order.len() - at);
                let mut bytes = Vec::with_capacity(take * 8);
                for &p in &order[at..at + take] {
                    let sym = if p > 0 { t[p as usize - 1] } else { 0u8 };
                    let w = (p & ((1u64 << 48) - 1)) | ((sym as u64) << 48);
                    bytes.extend_from_slice(&w.to_le_bytes());
                }
                std::fs::write(bands.join(format!("band_{bno}.sa")), &bytes).unwrap();
                std::fs::write(bands.join(format!("band_{bno}.opened")), b"\n").unwrap();
                at += take;
                bno += 1;
            }
            std::fs::write(bands.join("end"), format!("{bno}\n")).unwrap();
            std::fs::write(bands.join("end.ready"), b"\n").unwrap();
            let out = dir.join("m20.bin");
            tap(&TapArgs {
                bands: bands.to_string_lossy().to_string(),
                corpus: corpus.to_string_lossy().to_string(),
                out: out.to_string_lossy().to_string(),
                depth,
                min_support: k,
                docs_min_support: 1,
                threads: 2,
                n,
                sa: String::new(),
                rows: 0,
                doc_starts: starts_path.to_string_lossy().to_string(),
            });
            let mut sa = vec![0u8; order.len() * SA_ITEM];
            for (i, &p) in order.iter().enumerate() {
                sa_put(&mut sa, i as u64, p);
            }
            let oracle = OwnedStarts::Plain { starts: starts_table.clone(), text_len };
            let (want, _, _) = sweep(&sa, t, &[], &oracle, text_len, n, n, depth, k, true, 1,
                (SOURCE_IN_BUILD, 0));
            let got = std::fs::read(&out).unwrap();
            need(got == want,
                &format!("TAP_SELFTEST(cut): the band stream and the suffix array built DIFFERENT boundary-aware members (text len {}, depth {depth}, k {k})",
                    t.len()));
            let trie = Trie::load(&got);
            need(trie.boundary_aware == 1,
                "TAP_SELFTEST(cut): the aware tap did not journal boundary_aware=1");
            // spot oracle: at least one context must END a document on these
            // fixtures (repeated patterns + mid-pattern starts), and its END edge
            // must be exactly the count of doc-ending occurrences.
            let want = reference_cut(t, &starts_table, depth, k);
            let mut ends_checked = 0;
            for (c, (_count, hist)) in want.iter() {
                let Some(end_count) = hist.iter().find(|x| x.0 == END).map(|x| x.1) else {
                    continue;
                };
                let node = trie.find(c).unwrap_or_else(|| die(&format!(
                    "TAP_SELFTEST(cut): END-carrying context {:?} missing from the member", c)));
                let st = trie.stat(node);
                let got_end = st.children.iter().find(|x| x.code == END).map(|x| x.count)
                    .unwrap_or_else(|| die(&format!(
                        "TAP_SELFTEST(cut): context {:?} lost its END branch in the member", c)));
                need(got_end == end_count,
                    &format!("TAP_SELFTEST(cut): END count for {:?}: want {end_count} got {got_end}", c));
                ends_checked += 1;
            }
            eprintln!("TAP_SELFTEST cut text_len={} depth={depth} min_support={k} bands={bno} nodes={} edges={} bytes={} identity=1 boundary_aware=1 end_branch_contexts={ends_checked}",
                t.len(), trie.nodes, trie.edges, got.len());
            let _ = std::fs::remove_dir_all(&dir);
        }
        cut_cases += 1;
    }
    println!("TAP_SELFTEST_DONE cases={cases} cut_cases={cut_cases} note=\"band stream == suffix array, member bytes identical; the cut arm proves the same for the boundary-aware build\"");
}

pub fn build(a: &BuildArgs) {
    let t_all = std::time::Instant::now();
    let reader = qlite::Reader::open(&a.sxi).unwrap_or_else(|e| die(&format!("ctx-build: {e}")));
    let idx = if a.cache_mb > 0 {
        qlite::Index::open_with_cache(reader, a.threads, (a.cache_mb << 20) as usize)
    } else {
        qlite::Index::open(reader, a.threads)
    }
    .unwrap_or_else(|e| die(&format!("ctx-build: {e}")));
    let n = idx.reader.n;
    let tfile = std::fs::File::open(&a.text).unwrap_or_else(|e| die(&format!("open {}: {e}", a.text)));
    let flen = tfile.metadata().unwrap().len();
    need(flen + 1 == n,
        &format!("corpus {} is {flen} bytes but the container has n={n} rows: a lite container's n is the corpus length + 1 (one END row)", a.text));
    let text = unsafe { Mmap::map(&tfile) }.unwrap_or_else(|e| die(&format!("mmap {}: {e}", a.text)));
    let corpus_len = flen;
    let rows = a.limit_rows.unwrap_or(n).min(n);
    // A container that ALREADY carries m20 must be refused BY NAME: write_container
    // copies every old member verbatim and then appends m20, so the output would
    // carry two members with the same id and every reader would silently pick one.
    need(idx.reader.member_bytes(TRIE_CODEC).is_none(),
        &format!("ctx-build: {} already carries m20 (the context trie). Build from the container WITHOUT it \
                  (the --no-trie arm), or read the existing member with `xsa trie-info`", a.sxi));

    if let Ok(v) = std::env::var("XSA_CTX_SA_PROBE") {
        let steps: u64 = v.parse().unwrap_or_else(|_| die("XSA_CTX_SA_PROBE: integer"));
        sa_probe(&idx, steps, std::env::var("XSA_CTX_SA_STATELESS").is_ok());
        return;
    }
    // 1. the ordered stream: the suffix array, from one parallel LF cycle (or from
    //    the cache a previous run wrote - the walk is the expensive part, the sweep
    //    is cheap, and the curve needs many sweeps of one stream).
    let t_sa = std::time::Instant::now();
    let (sa, sa_steps, sa_mode, dense_mem) = load_or_build_sa(&idx, a.threads, &a.sa_cache, a.dense);
    let sa_wall = t_sa.elapsed().as_secs_f64();

    // 2. document ranks (original bytes) — built ONLY for the boundary-unaware
    //    arm; the aware arm takes container document ids from member m15.
    let t_doc = std::time::Instant::now();
    // THE NO-CROSS LAW, POST-HOC ARM: the container's own member m15 (when present)
    // is the boundary oracle, so a `ctx-build` over a v10 container produces the
    // same boundary-aware member the in-build tap produced from the same map.
    let starts = match crate::docstarts::DocStarts::from_container(&idx.reader) {
        Some(ds) => {
            need(ds.text_len + 1 == n,
                "ctx-build: member m15 text_len does not match the container's n");
            eprintln!("CONTEXT_TRIE_BOUNDARY aware=1 source=member-m15 docs={}", ds.docs);
            OwnedStarts::Table(ds)
        }
        None => {
            eprintln!(
                "CONTEXT_TRIE_BOUNDARY aware=0 source=absent \
                 note=\"no member m15: the trie is built boundary-unaware and boundary-sensitive \
                 queries against it are refused; embed the starts (xsa build --docs-map DIR, or \
                 xsa docstarts embed) and rebuild to make the no-cross law structural\""
            );
            OwnedStarts::None
        }
    };
    let ranks = if a.docs && !starts.is_some() { doc_ranks(&text) } else { Vec::new() };
    let doc_wall = t_doc.elapsed().as_secs_f64();

    // 3. the sweep
    let t_sweep = std::time::Instant::now();
    let stream_mem = sa.len() as u64 + dense_mem;
    let (payload, reads, cols) = sweep(&sa, &text, &ranks, &starts, corpus_len, n, rows, a.depth, a.min_support, a.docs, a.docs_min_support, (sa_mode, stream_mem));
    let sweep_wall = t_sweep.elapsed().as_secs_f64();

    // 4. write the container: the old members, then m20, then a v9 directory.
    let (bytes, member_off) = write_container(&a.sxi, &a.out, &payload, n, &idx.reader);
    // the BUDGET is the member, not the container: m20 on top of an existing index
    let ratio = member_off as f64 / n as f64;
    let trie = Trie::load(&payload);
    eprintln!(
        "CONTEXT_TRIE depth={} min_support={} nodes={} edges={} bytes={} ratio_vs_n={:.6} docs_summary=exact<=8+hll13pct",
        trie.depth, trie.min_support, trie.nodes, trie.edges, member_off, ratio
    );
    let bn = member_off as f64 / trie.nodes.max(1) as f64;
    let be = member_off as f64 / trie.edges.max(1) as f64;
    eprintln!(
        "CONTEXT_TRIE_COLUMNS node_fixed={} node_edge_delta={} node_count={} edge_code={} edge_child={} \
         edge_count={} doc_ndocs={} doc_top={} doc_first={} doc_ids={} head={HEAD} bytes_per_node={bn:.1} bytes_per_edge={be:.1}",
        cols[0], cols[1], cols[2], cols[3], cols[4], cols[5], cols[6], cols[7], cols[8], cols[9]
    );
    eprintln!(
        "CONTEXT_TRIE_SOURCE mode={} rows={} n={} sa_bytes={} dense_bytes={} stream_mem_gb={:.1} note=\"0=kernel-lf 1=dense-lf 2=sa-cache 3=in-build\"",
        match sa_mode { 0 => "kernel-lf", 1 => "dense-lf", 2 => "sa-cache-reuse", _ => "in-build" },
        sa_steps, n, sa.len(), dense_mem, stream_mem as f64 / (1u64 << 30) as f64
    );
    let pass = ratio <= 0.05;
    eprintln!("CONTEXT_TRIE_BUDGET ratio_vs_n={ratio:.6} target<=0.05 pass={}", u8::from(pass));
    eprintln!(
        "CONTEXT_TRIE_BUILD rows={rows} sa_steps={sa_steps} sa_wall={sa_wall:.1}s doc_wall={doc_wall:.1}s \
         sweep_wall={sweep_wall:.1}s prefix_reads={reads} total_wall={:.1}s threads={} container_out={bytes} member_off={member_off}",
        t_all.elapsed().as_secs_f64(),
        a.threads
    );
}

/// Write `out` = the input container's members verbatim, plus m20, with a v9
/// header and a trailing directory. Old containers stay readable: this only ADDS a
/// member and a version.
fn write_container(inp: &str, out: &str, payload: &[u8], n: u64, reader: &qlite::Reader) -> (u64, u64) {
    use std::os::unix::fs::FileExt;
    let src = std::fs::File::open(inp).unwrap_or_else(|e| die(&format!("open {inp}: {e}")));
    let map = reader.map();
    match std::fs::symlink_metadata(out) {
        Ok(_) => die(&format!("ctx-build: output exists: {out}")),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
        Err(e) => die(&format!("ctx-build: inspect {out}: {e}")),
    }
    let tmp = format!("{out}.partial");
    let f = std::fs::File::create(&tmp).unwrap_or_else(|e| die(&format!("create {tmp}: {e}")));
    let mut w = BufWriter::with_capacity(1 << 22, &f);
    let mut members: Vec<(u64, u64, u64, u32, u64, u64)> = Vec::new(); // id, codec, off, crc, bytes, count
    let mut off = 64u64;
    let mut wpos = 64u64;
    let mut hdr = [0u8; 64];
    hdr[..4].copy_from_slice(b"SXI2");
    let old = &map[..64];
    hdr[4..16].copy_from_slice(&old[4..16]); // version (patched below), n, k(lo)
    hdr[16..24].copy_from_slice(&old[16..24]);
    hdr[24..32].copy_from_slice(&old[24..32]); // r
    hdr[48..56].copy_from_slice(&old[48..56]);
    let flags = u64::from_le_bytes(old[48..56].try_into().unwrap()) | FLAG_TRIE;
    hdr[48..56].copy_from_slice(&flags.to_le_bytes());
    let old_count = u32::from_le_bytes(old[32..36].try_into().unwrap()) as usize;
    let count = old_count + 1;
    hdr[32..36].copy_from_slice(&(count as u32).to_le_bytes());
    hdr[36..40].copy_from_slice(&((64 + 40 * count) as u32).to_le_bytes());
    w.write_all(&hdr).unwrap();
    // copy every old member in PHYSICAL order, keeping its id and codec
    let mut spans: Vec<(u64, u64, u32, u32, u64)> = reader
        .members
        .iter()
        .map(|m| (m.offset, m.bytes, m.id, m.codec, m.count))
        .collect();
    spans.sort_unstable();
    let mut crcs: Vec<u32> = Vec::new();
    let mut crcb = vec![0u8; 1 << 20];
    for &(soff, bytes, id, codec, cnt) in &spans {
        off = (off + 7) / 8 * 8;
        if off != wpos {
            w.write_all(&vec![0u8; (off - wpos) as usize]).unwrap();
            wpos = off;
        }
        let mut v = !0u32;
        let mut left = bytes as usize;
        let mut at = soff as usize;
        while left > 0 {
            let z = left.min(crcb.len());
            crcb[..z].copy_from_slice(&map[at..at + z]);
            v = sxi::crc_step(v, &crcb[..z]);
            w.write_all(&crcb[..z]).unwrap();
            at += z;
            left -= z;
            wpos += z as u64;
        }
        crcs.push(!v);
        members.push((id as u64, codec as u64, off, !v, bytes, cnt));
        off += bytes;
    }
    // m20
    off = (off + 7) / 8 * 8;
    if off != wpos {
        w.write_all(&vec![0u8; (off - wpos) as usize]).unwrap();
        wpos = off;
    }
    w.write_all(payload).unwrap();
    wpos += payload.len() as u64;
    let member_off = payload.len() as u64;
    let crc = sxi::crc32_of(payload);
    members.push((TRIE_CODEC as u64, TRIE_CODEC as u64, off, crc, member_off, n));
    off += member_off;
    // trailing directory, sorted by id
    members.sort_unstable_by_key(|m| m.0);
    let mut dir = Vec::with_capacity(40 * members.len());
    for &(id, codec, moff, mcrc, mbytes, mcount) in &members {
        let mut d = [0u8; 40];
        put(&mut d[0..], id as u64, 4);
        put(&mut d[4..], codec as u64, 4);
        put(&mut d[8..], moff, 8);
        put(&mut d[16..], mbytes, 8);
        put(&mut d[24..], mcount, 8);
        put(&mut d[32..], mcrc as u64, 4);
        dir.extend_from_slice(&d);
    }
    w.write_all(&dir).unwrap();
    let length = off + dir.len() as u64;
    w.flush().unwrap();
    drop(w);
    // patch the header: version, length, CRC
    let mut h2 = [0u8; 64];
    h2.copy_from_slice(&hdr);
    h2[4..8].copy_from_slice(&9u32.to_le_bytes());
    h2[40..48].copy_from_slice(&length.to_le_bytes());
    h2[56..60].fill(0);
    let mut hc = vec![0u8; 64];
    hc.copy_from_slice(&h2);
    hc.extend_from_slice(&dir);
    let crc = sxi::crc32_of(&hc);
    h2[56..60].copy_from_slice(&crc.to_le_bytes());
    f.write_all_at(&h2, 0).unwrap();
    f.sync_all().unwrap();
    drop(f);
    std::fs::rename(&tmp, out).unwrap_or_else(|e| die(&format!("rename {tmp} -> {out}: {e}")));
    let _ = src;
    (length, member_off)
}

/// Append the m20 payload to a container that is ALREADY WRITTEN, IN PLACE.
///
/// The finish phase streams the container; the trie is built concurrently by a
/// separate phase (see `tap`). The cheap join is therefore an in-place append:
/// drop the trailing member directory, write m20 at the end of the last member,
/// write the directory back with one more entry, and patch the header (count,
/// directory offset, length, FLAG_TRIE, version 9). No container byte is copied.
///
/// This is the same member and the same directory a `ctx-build` produces; only
/// the route to the payload differs, which the member itself records (source).
pub fn append_m20(path: &str, payload: &[u8], n: u64) -> u64 {
    use std::os::unix::fs::FileExt;
    let reader = qlite::Reader::open(path).unwrap_or_else(|e| die(&format!("trie append: {e}")));
    need(reader.member_bytes(TRIE_CODEC).is_none(),
        &format!("trie append: {path} already carries m20"));
    need(reader.members.iter().all(|m| m.id != 15),
        &format!("trie append: {path} carries member m15 (document starts; SXI2 v{}); appending m20 \n             here would rewrite the version as 9 and strand m15 - build with --trie and \n             --docs-map in one `xsa build`, or embed m15 after m20",
            reader.version));
    let count = reader.members.len();
    let length = std::fs::metadata(path).unwrap().len();
    let dir_start = length - 40 * count as u64;
    let mut last = 0u64;
    for m in &reader.members {
        last = last.max(m.offset + m.bytes);
    }
    let at = (last + 7) / 8 * 8;
    need(dir_start >= at, &format!("trie append: {path} has no room for m20 (dir at {dir_start}, members end at {at})"));
    // the old directory, verbatim
    let mut dir: Vec<u8> = Vec::with_capacity(40 * (count + 1));
    dir.extend_from_slice(&reader.map()[dir_start as usize..]);
    // the new entry: id, codec, offset, bytes, count, crc (40 B, sorted by id)
    let mut entry = [0u8; 40];
    put(&mut entry[0..], TRIE_CODEC as u64, 4);
    put(&mut entry[4..], TRIE_CODEC as u64, 4);
    put(&mut entry[8..], at, 8);
    put(&mut entry[16..], payload.len() as u64, 8);
    put(&mut entry[24..], n, 8);
    put(&mut entry[32..], sxi::crc32_of(payload) as u64, 4);
    // keep the directory sorted by member id (it is how every reader binary-searches)
    let mut entries: Vec<[u8; 40]> = dir.chunks_exact(40).map(|c| c.try_into().unwrap()).collect();
    entries.push(entry);
    entries.sort_unstable_by_key(|e| u32::from_le_bytes(e[0..4].try_into().unwrap()));
    dir.clear();
    for e in &entries {
        dir.extend_from_slice(e);
    }
    let f = std::fs::OpenOptions::new().read(true).write(true).open(path)
        .unwrap_or_else(|e| die(&format!("trie append: open {path}: {e}")));
    f.set_len(at).unwrap_or_else(|e| die(&format!("trie append: truncate {path}: {e}")));
    f.write_all_at(payload, at).unwrap_or_else(|e| die(&format!("trie append: write m20: {e}")));
    let new_len = at + payload.len() as u64 + dir.len() as u64;
    f.write_all_at(&dir, at + payload.len() as u64)
        .unwrap_or_else(|e| die(&format!("trie append: write directory: {e}")));
    // patch the header
    let old = reader.map()[..64].to_vec();
    let mut h = [0u8; 64];
    h.copy_from_slice(&old);
    put(&mut h[4..], 9, 4);
    put(&mut h[32..], entries.len() as u64, 4);
    put(&mut h[36..], (64 + 40 * entries.len()) as u64, 4);
    put(&mut h[40..], new_len, 8);
    let old_flags = u64::from_le_bytes(old[48..56].try_into().unwrap());
    put(&mut h[48..], old_flags | FLAG_TRIE, 8);
    h[56..60].fill(0);
    let mut hc = h.to_vec();
    hc.extend_from_slice(&dir);
    let crc = sxi::crc32_of(&hc);
    put(&mut h[56..], crc as u64, 4);
    f.write_all_at(&h, 0).unwrap_or_else(|e| die(&format!("trie append: patch header: {e}")));
    f.sync_all().unwrap();
    drop(f);
    drop(reader);
    payload.len() as u64
}

/// The m20 member of a container, for a gate that compares two routes' bytes.
///
/// `sha256_noprov` is the digest with the 16 provenance bytes (the ordered-stream
/// source and its memory figure, head offsets 152..168) ZEROED. That field is the
/// member saying HOW it was built, so two routes that agree on every byte of the
/// tree will still differ there by design; comparing the masked digest proves the
/// tree itself is identical, and the gate separately proves that the ONLY
/// differing bytes are those 16.
pub fn trie_member_report(path: &str, out: Option<&str>) {
    use sha2::{Digest, Sha256};
    let reader = open_reader(path);
    let m = reader.member_bytes(TRIE_CODEC).unwrap_or_else(|| die(&format!("{path}: no m20 member")));
    let full = format!("{:x}", Sha256::digest(m));
    let mut masked = m.to_vec();
    let (po, pl) = (152usize, 16usize);
    let source = get(&m[po..], 8);
    let source_mem = get(&m[po + 8..], 8);
    masked[po..po + pl].fill(0);
    let np = format!("{:x}", Sha256::digest(&masked));
    if let Some(o) = out {
        std::fs::write(o, m).unwrap_or_else(|e| die(&format!("write {o}: {e}")));
    }
    println!("TRIE_MEMBER path={path} bytes={} sha256={full} sha256_noprov={np} prov_offset={po} prov_len={pl} source={source} source_mem={source_mem} boundary_aware={}",
        m.len(), get(&m[168..], 8));
}

// ----------------------------------------------------------------- the commands

fn parse_context(s: &str) -> Vec<u8> {
    if let Some(h) = s.strip_prefix("hex:") {
        need(h.len() % 2 == 0, "hex: context needs an even number of digits");
        let mut v = Vec::with_capacity(h.len() / 2);
        for i in (0..h.len()).step_by(2) {
            v.push(u8::from_str_radix(&h[i..i + 2], 16).unwrap_or_else(|_| die("hex: bad digit")));
        }
        return v;
    }
    let b = s.as_bytes();
    let mut v = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] != b'\\' || i + 1 == b.len() {
            v.push(b[i]);
            i += 1;
            continue;
        }
        i += 1;
        match b[i] {
            b'n' => { v.push(b'\n'); i += 1; }
            b't' => { v.push(b'\t'); i += 1; }
            b'r' => { v.push(b'\r'); i += 1; }
            b'0' => { v.push(0); i += 1; }
            b'\\' => { v.push(b'\\'); i += 1; }
            b'x' => {
                need(i + 2 < b.len(), "\\x needs two hex digits");
                let h = std::str::from_utf8(&b[i + 1..i + 3]).unwrap_or_else(|_| die("bad \\x escape"));
                v.push(u8::from_str_radix(h, 16).unwrap_or_else(|_| die("bad \\x escape")));
                i += 3;
            }
            c => die(&format!("unknown escape \\{} (use hex:... for raw bytes)", c as char)),
        }
    }
    v
}

fn load_trie<'a>(reader: &'a qlite::Reader) -> Trie<'a> {
    let m = reader.member_bytes(TRIE_CODEC).unwrap_or_else(|| {
        die("this container has no context trie (member m20); build one with `xsa ctx-build`")
    });
    Trie::load(m)
}

fn open_reader(path: &str) -> qlite::Reader {
    qlite::Reader::open(path).unwrap_or_else(|e| die(&format!("{e}")))
}

/// THE QUERY-SIDE OF THE NO-CROSS LAW. A boundary-UNAWARE m20 conflates
/// cross-boundary continuations with content (an occurrence that ends a
/// document counts the next document's first byte as a content branch), so a
/// boundary-sensitive answer against it is REFUSED rather than returned
/// silently; `--cross-doc` is the explicit opt-in to the conflated answer. A
/// boundary-AWARE member cannot represent the conflated answer at all, so
/// `--cross-doc` on one is refused too - never a silent semantic switch.
fn trie_boundary_gate(trie: &Trie, cross_doc: bool, what: &str) {
    if trie.boundary_aware == 0 && !cross_doc {
        die(&format!(
            "CONTEXT_TRIE: refusing {what}: this container's m20 is boundary-UNAWARE \
             (boundary_aware=0: built without a document-starts table), so the answer would \
             CONFLATE cross-boundary continuations with content - an occurrence that ends a \
             document counts the next document's first byte as a content branch. Opt in \
             explicitly with --cross-doc, or rebuild the trie against a starts table \
             (`xsa build --docs-map DIR`, or `xsa ctx-build` over a container that carries \
             member m15) so the no-cross law holds by construction."
        ));
    }
    if trie.boundary_aware == 1 && cross_doc {
        die(
            "CONTEXT_TRIE: --cross-doc on a boundary-AWARE trie: the member cannot represent \
             crossing continuations (they were reclassified to the END branch at build time); \
             query it without --cross-doc",
        );
    }
}

pub fn command(cmd: &str, args: &[String]) {
    match cmd {
        "ctx-build" => {
            let mut a = BuildArgs {
                sxi: String::new(),
                text: String::new(),
                out: String::new(),
                depth: 8,
                min_support: 1_000,
                threads: 16,
                docs: true,
                limit_rows: None,
                cache_mb: 0,
                sa_cache: String::new(),
                docs_min_support: 1000,
                dense: true,
            };
            let mut i = 0;
            while i < args.len() {
                let f = args[i].as_str();
                let mut val = |what: &str| -> String {
                    i += 1;
                    args.get(i).cloned().unwrap_or_else(|| die(&format!("{what} needs a value")))
                };
                match f {
                    "--sxi" => a.sxi = val("--sxi"),
                    "--text" => a.text = val("--text"),
                    "-o" | "--out" => a.out = val("--out"),
                    "--depth" => a.depth = val("--depth").parse().unwrap_or_else(|_| die("--depth: integer")),
                    "--min-support" => {
                        a.min_support = val("--min-support").parse().unwrap_or_else(|_| die("--min-support: integer"))
                    }
                    "--threads" => a.threads = val("--threads").parse().unwrap_or_else(|_| die("--threads: integer")),
                    "--cache-mb" => a.cache_mb = val("--cache-mb").parse().unwrap_or_else(|_| die("--cache-mb: integer")),
                    "--sa-cache" => a.sa_cache = val("--sa-cache"),
                    "--docs-min-support" => {
                        a.docs_min_support = val("--docs-min-support").parse().unwrap_or_else(|_| die("--docs-min-support: integer"))
                    }
                    "--rows" => {
                        a.limit_rows = Some(val("--rows").parse().unwrap_or_else(|_| die("--rows: integer")))
                    }
                    "--no-docs" => a.docs = false,
                    "--dense" => a.dense = true,
                    "--no-dense" => a.dense = false,
                    other => die(&format!("ctx-build: unknown option {other}")),
                }
                i += 1;
            }
            need(!a.sxi.is_empty() && !a.text.is_empty() && !a.out.is_empty(),
                "usage: xsa ctx-build --sxi <in.sxi2> --text <corpus> --out <out.sxi2> \
                 [--depth D] [--min-support K] [--threads N] [--no-docs] [--rows N]");
            build(&a);
        }
        "branch-of" if args.iter().any(|a| a == "--batch") => batch("branch-of", args),
        "relax" if args.iter().any(|a| a == "--batch") => batch("relax", args),
        "branch-of" => {
            let pos: Vec<&String> = args.iter().filter(|a| !a.starts_with("--")).collect();
            let cross_doc = args.iter().any(|a| a == "--cross-doc");
            need(pos.len() == 2, "usage: xsa branch-of <container> <context> [--cross-doc]   (context: text with \\xHH escapes, or hex:...)");
            let reader = open_reader(pos[0]);
            let pat = parse_context(pos[1]);
            let trie = load_trie(&reader);
            trie_boundary_gate(&trie, cross_doc, "the branch histogram");
            match trie.find(&pat) {
                None => println!("absent depth={} context_len={}", trie.depth, pat.len()),
                Some(node) => {
                    let st = trie.stat(node);
                    println!("present count={} depth={} children={}", st.count, st.depth, st.children.len());
                    for c in &st.children {
                        println!("child {} count={} node={}", c.label(), c.count,
                            c.node.map_or("-".to_string(), |n| n.to_string()));
                    }
                }
            }
        }
        "trie-stat" => {
            let pos: Vec<&String> = args.iter().filter(|a| !a.starts_with("--")).collect();
            let cross_doc = args.iter().any(|a| a == "--cross-doc");
            need(pos.len() == 2, "usage: xsa trie-stat <container> <context> [--cross-doc]");
            let reader = open_reader(pos[0]);
            let pat = parse_context(pos[1]);
            let trie = load_trie(&reader);
            trie_boundary_gate(&trie, cross_doc, "the occurrence count (non-crossing vs conflated)");
            match trie.find(&pat) {
                None => println!("absent depth={} context_len={}", trie.depth, pat.len()),
                Some(node) => {
                    let st = trie.stat(node);
                    println!("present count={} depth={} documents={} docs_exact={} top_doc={} children={}",
                        st.count, st.depth, st.ndocs, u8::from(st.exact_docs),
                        st.top_doc.map_or("-".to_string(), |d| d.to_string()), st.children.len());
                }
            }
        }
        "relax" => {
            let pos: Vec<&String> = args.iter().filter(|a| !a.starts_with("--")).collect();
            let cross_doc = args.iter().any(|a| a == "--cross-doc");
            need(pos.len() == 2, "usage: xsa relax <container> <context> [--cross-doc]   (descend as far as the trie supports it)");
            let reader = open_reader(pos[0]);
            let pat = parse_context(pos[1]);
            let trie = load_trie(&reader);
            trie_boundary_gate(&trie, cross_doc, "the relaxed descent (counts and branches)");
            let (node, reached) = trie.descend(&pat);
            let st = trie.stat(node);
            println!("reached_depth={reached} asked={} count={} documents={} children={}",
                pat.len(), st.count, st.ndocs, st.children.len());
            for c in &st.children {
                println!("child {} count={} node={}", c.label(), c.count,
                    c.node.map_or("-".to_string(), |n| n.to_string()));
            }
        }
        "relax-stat" => relax_stat(args),
        "trie-bench" => trie_bench(args),
        "trie-info" => {
            need(args.len() == 1, "usage: xsa trie-info <container>");
            let reader = open_reader(&args[0]);
            let trie = load_trie(&reader);
            let m = reader.member_bytes(TRIE_CODEC).unwrap();
            println!("CONTEXT_TRIE depth={} min_support={} nodes={} edges={} bytes={} ratio_vs_n={:.6} n={}",
                trie.depth, trie.min_support, trie.nodes, trie.edges, m.len(),
                m.len() as f64 / trie.n as f64, trie.n);
        }
        "ctx-curve" => curve(args),
        "ctx-verify" => verify(args),
        "ctx-tap" => {
            let mut a = TapArgs {
                bands: String::new(),
                corpus: String::new(),
                out: String::new(),
                depth: 8,
                min_support: 1_000,
                docs_min_support: 1000,
                threads: 8,
                n: 0,
                sa: String::new(),
                rows: 0,
                doc_starts: String::new(),
            };
            let mut i = 0;
            while i < args.len() {
                let f = args[i].as_str();
                let mut val = |what: &str| -> String {
                    i += 1;
                    args.get(i).cloned().unwrap_or_else(|| die(&format!("{what} needs a value")))
                };
                match f {
                    "--bands" => a.bands = val("--bands"),
                    "--corpus" => a.corpus = val("--corpus"),
                    "-o" | "--out" => a.out = val("--out"),
                    "--depth" => a.depth = val("--depth").parse().unwrap_or_else(|_| die("--depth: integer")),
                    "--min-support" => a.min_support = val("--min-support").parse().unwrap_or_else(|_| die("--min-support: integer")),
                    "--docs-min-support" => a.docs_min_support = val("--docs-min-support").parse().unwrap_or_else(|_| die("--docs-min-support: integer")),
                    "--threads" => a.threads = val("--threads").parse().unwrap_or_else(|_| die("--threads: integer")),
                    "--n" => a.n = val("--n").parse().unwrap_or_else(|_| die("--n: integer")),
                    "--doc-starts" => a.doc_starts = val("--doc-starts"),
                    "--sa" => a.sa = val("--sa"),
                    "--rows" => a.rows = val("--rows").parse().unwrap_or_else(|_| die("--rows: integer")),
                    other => die(&format!("ctx-tap: unknown option {other}")),
                }
                i += 1;
            }
            need((!a.bands.is_empty() || !a.sa.is_empty()) && !a.corpus.is_empty() && !a.out.is_empty() && a.n > 0,
                "usage: xsa ctx-tap --bands <dir> --corpus <file> --out <payload> --n <rows> \
                 [--depth D] [--min-support K] [--docs-min-support M] [--threads N] \
                 [--doc-starts DIR|FILE] (a starts table: boundary-aware by construction)");
            tap(&a);
        }
        "trie-member" => {
            let mut container: Option<String> = None;
            let mut out: Option<String> = None;
            let mut i = 0;
            while i < args.len() {
                match args[i].as_str() {
                    "-o" | "--out" => {
                        i += 1;
                        out = Some(args.get(i).cloned().unwrap_or_else(|| die("--out needs a path")));
                    }
                    _ => container = Some(args[i].clone()),
                }
                i += 1;
            }
            let c = container.unwrap_or_else(|| die("usage: xsa trie-member <container> [-o <raw m20>]"));
            trie_member_report(&c, out.as_deref());
        }
        "ctx-selftest" => selftest(),
        "ctx-tap-selftest" => tap_selftest(),
        other => die(&format!("ctx: unknown command {other}")),
    }
}

// ------------------------------------------------------ the brute-force oracle
//
// THE EXACTNESS GATE. For a context sampled from the corpus, the trie's
// children-and-counts must equal a next-byte histogram computed by SCANNING THE
// CORPUS at every occurrence of that context - never from the suffix array, never
// from the trie, never from the container. This is the whole point: a tree that
// agrees with itself proves nothing.

fn verify(args: &[String]) {
    let mut sxi = String::new();
    let mut text = String::new();
    let mut samples = 50usize;
    let mut seed = 20261110u64;
    let mut given: Vec<String> = Vec::new();
    let mut sample_nodes = 0usize;
    let mut cross_doc = false;
    let mut i = 0;
    while i < args.len() {
        let f = args[i].as_str();
        let mut val = |what: &str| -> String {
            i += 1;
            match args.get(i) { Some(v) => v.clone(), None => die(&format!("{what} needs a value")) }
        };
        match f {
            "--sxi" => sxi = val("--sxi"),
            "--text" => text = val("--text"),
            "--samples" => samples = val("--samples").parse().unwrap_or_else(|_| die("--samples: integer")),
            "--seed" => seed = val("--seed").parse().unwrap_or_else(|_| die("--seed: integer")),
            "--context" => given.push(val("--context")),
            "--sample-nodes" => sample_nodes = val("--sample-nodes").parse().unwrap_or_else(|_| die("--sample-nodes")),
            "--cross-doc" => cross_doc = true,
            other => die(&format!("ctx-verify: unknown option {other}")),
        }
        i += 1;
    }
    need(!sxi.is_empty() && !text.is_empty(), "usage: xsa ctx-verify --sxi X --text C [--samples 50] [--seed S] [--context CTX]... [--cross-doc]");
    let reader = open_reader(&sxi);
    let trie = load_trie(&reader);
    trie_boundary_gate(&trie, cross_doc, "the exactness gate's brute histogram");
    // THE LAW, VERIFIED WITH THE SAME ORACLE THE BUILDER USED: a boundary-aware
    // member must be checked against a boundary-aware corpus scan (the container
    // carries m15 by construction - an aware m20 without m15 is an inconsistent
    // member and is refused), and an unaware member against the legacy conflated
    // scan (only under --cross-doc, per the gate above).
    let starts = if trie.boundary_aware == 1 {
        match crate::docstarts::DocStarts::from_container(&reader) {
            Some(ds) => {
                need(ds.text_len + 1 == trie.n, "ctx-verify: member m15 text_len disagrees with n");
                OwnedStarts::Table(ds)
            }
            None => die(
                "ctx-verify: the m20 member says boundary_aware=1 but the container carries no \
                 member m15: an aware trie without its starts table cannot be verified (or trusted)",
            ),
        }
    } else {
        OwnedStarts::None
    };
    let aware = trie.boundary_aware == 1;
    let tfile = std::fs::File::open(&text).unwrap_or_else(|e| die(&format!("open {text}: {e}")));
    let tlen = tfile.metadata().unwrap().len();
    need(tlen + 1 == trie.n, "corpus length + 1 != the trie's n");
    let corpus = unsafe { Mmap::map(&tfile) }.unwrap_or_else(|e| die(&format!("mmap {text}: {e}")));
    let clen = tlen as usize;

    // sample real contexts: S uniform positions, the D-byte context at each
    let d = trie.depth as usize;
    let mut st = seed | 1;
    let mut ctxs: Vec<(usize, Vec<u8>)> = Vec::new();
    let have_given = !given.is_empty();
    for g in given.clone() {
        let c = parse_context(&g);
        let p = find_once(&corpus, &c).unwrap_or_else(|| die(&format!("--context not present in the corpus")));
        ctxs.push((p, c));
    }
    while ctxs.len() < samples && !have_given && sample_nodes == 0 {
        st ^= st << 13;
        st ^= st >> 7;
        st ^= st << 17;
        let p = (st % (clen as u64 - d as u64 - 1)) as usize;
        let c = corpus[p..p + d].to_vec();
        if ctxs.iter().any(|(q, _)| *q == p) {
            continue;
        }
        ctxs.push((p, c));
    }
    // contexts that the TRIE ITSELF claims to have: sampled from the node store by
    // reconstructing each node's path from the parent links. These are the ones
    // whose child histograms can actually disagree - a random 16-byte context is
    // usually below support, which tests the absence flag, not the counts.
    if sample_nodes > 0 {
        let nodes = trie.nodes as usize;
        let mut parent = vec![u32::MAX; nodes];
        let mut pbyte = vec![0u16; nodes];
        for n in 0..nodes as u32 {
            let (_, nch, _) = trie.node(n);
            let first = trie.first_edge(n);
            for k in 0..nch as u32 {
                if let Some(ch) = trie.edge_at(n, k as usize).node {
                    parent[ch as usize] = n;
                    pbyte[ch as usize] = trie.edge_at(n, k as usize).code;
                }
                let _ = first;
            }
        }
        let mut cand: Vec<u32> = (0..nodes as u32).filter(|&n| trie.node_depth(n) >= 2).collect();
        need(!cand.is_empty(), "no node with depth >= 2 to sample");
        let stride = (cand.len() / sample_nodes).max(1);
        cand.sort_unstable();
        for n in cand.iter().step_by(stride).take(sample_nodes) {
            let mut path = Vec::new();
            let mut cur = *n;
            while cur != trie.root() {
                let par = parent[cur as usize];
                need(par != u32::MAX, "node without a parent");
                path.push((pbyte[cur as usize] - 1) as u8);
                cur = par;
            }
            path.reverse();
            need(!path.is_empty(), "empty node path");
            ctxs.push((0usize, path));
        }
    }
    let ctxs: Vec<Vec<u8>> = ctxs.into_iter().map(|(_, c)| c).collect();

    // ONE pass over the corpus collecting every context's occurrences: candidates
    // are gated on their first 4 bytes, then verified byte for byte.
    let mut hist: Vec<Vec<(u16, u64)>> = vec![Vec::new(); ctxs.len()];
    let mut totals = vec![0u64; ctxs.len()];
    {
        use std::collections::HashMap;
        let mut key: HashMap<u32, Vec<usize>> = HashMap::new();
        let mut short: Vec<usize> = Vec::new();
        for (i, c) in ctxs.iter().enumerate() {
            if c.len() >= 4 {
                let k = u32::from_le_bytes([c[0], c[1], c[2], c[3]]);
                key.entry(k).or_default().push(i);
            } else {
                short.push(i);
            }
        }
        let mut bump = |i: usize, sym: u16| {
            totals[i] += 1;
            let h = &mut hist[i];
            match h.iter_mut().find(|e| e.0 == sym) {
                Some(e) => e.1 += 1,
                None => h.push((sym, 1)),
            }
        };
        // classify one verified occurrence at `p` of context `c` under the law
        let mut classify = |p: usize, c: &[u8]| -> Option<u16> {
            let q = p + c.len();
            if q == clen {
                return Some(END);
            }
            if aware {
                // crossing match: rank(p) != rank(q-1) - a document starts inside
                // (p, q-1] - the occurrence is NOT a match at all
                if starts.rank_start(q as u64 - 1).1 > p as u64 {
                    return None;
                }
                // the END branch: q is a document start
                if starts.is_start(q as u64) {
                    return Some(END);
                }
            }
            Some(1 + corpus[q] as u16)
        };
        let mut p = 0usize;
        while p + 4 <= clen {
            let k = u32::from_le_bytes([corpus[p], corpus[p + 1], corpus[p + 2], corpus[p + 3]]);
            if let Some(ids) = key.get(&k) {
                for &id in ids {
                    let c = &ctxs[id];
                    if p + c.len() <= clen && &corpus[p..p + c.len()] == &c[..] {
                        if let Some(sym) = classify(p, c) {
                            bump(id, sym);
                        }
                    }
                }
            }
            p += 1;
        }
        for id in short {
            let c = &ctxs[id];
            let mut p = 0usize;
            while p + c.len() <= clen {
                if &corpus[p..p + c.len()] == &c[..] {
                    if let Some(sym) = classify(p, c) {
                        bump(id, sym);
                    }
                }
                p += 1;
            }
        }
    }
    let mut exact = 0usize;
    let mut mism = 0usize;
    let mut below = 0usize;
    for (i, c) in ctxs.iter().enumerate() {
        hist[i].sort_unstable();
        let want: Vec<(u16, u64)> = hist[i].clone();
        let got = trie.find(c);
        let ok = match got {
            None => {
                below += 1;
                // absent must be EXPLAINED: either the support is under MIN_SUPPORT
                // or the context is deeper than D (it cannot be, here) - never a
                // silent hole.
                totals[i] < trie.min_support
            }
            Some(node) => {
                let st = trie.stat(node);
                let g: Vec<(u16, u64)> = st.children.iter().map(|x| (x.code, x.count)).collect();
                g == want && st.count == totals[i]
            }
        };
        if ok {
            exact += 1;
        } else {
            mism += 1;
            println!("TRIE_VERIFY_MISMATCH context=hex:{} occurrences={} support={} node={}",
                hex(c), totals[i], trie.min_support, got.is_some());
            println!("  brute next-byte histogram: {}", fmt_hist(&want));
            match got {
                None => println!("  trie: absent (support {} vs min_support {})", totals[i], trie.min_support),
                Some(node) => {
                    let st = trie.stat(node);
                    let g: Vec<(u16, u64)> = st.children.iter().map(|x| (x.code, x.count)).collect();
                    println!("  trie next-byte histogram: {} count_field={}", fmt_hist(&g), st.count);
                }
            }
        }
    }
    println!("TRIE_VERIFY n={} exact={} mismatches={} below_min_support={}", ctxs.len(), exact, mism, below);
    if mism > 0 {
        die(&format!("CONTEXT_TRIE: {mism} of {} contexts disagree with the brute-force histogram", ctxs.len()));
    }
}

/// THE PARAMETER CURVE: how big is the top of the tree, really, as a function of
/// depth D and minimum support K. One ordered stream, one sweep per cell, exact
/// serialised bytes per cell (the payload the writer would emit, not an estimate).
fn curve(args: &[String]) {
    let mut sxi = String::new();
    let mut text = String::new();
    let mut cache = String::new();
    let mut depths: Vec<usize> = vec![4, 8, 16];
    let mut supports: Vec<u64> = vec![10, 100, 1000, 10000];
    let mut docs = true;
    let mut docs_min_support = 1000u64;
    let mut threads = 24usize;
    let mut cache_mb = 0u64;
    let mut dense = true;
    let mut i = 0;
    while i < args.len() {
        let f = args[i].as_str();
        let mut val = |what: &str| -> String {
            i += 1;
            match args.get(i) { Some(v) => v.clone(), None => die(&format!("{what} needs a value")) }
        };
        match f {
            "--sxi" => sxi = val("--sxi"),
            "--text" => text = val("--text"),
            "--sa-cache" => cache = val("--sa-cache"),
            "--depths" => depths = val("--depths").split(',').map(|x| x.parse().unwrap_or_else(|_| die("--depths"))).collect(),
            "--supports" => supports = val("--supports").split(',').map(|x| x.parse().unwrap_or_else(|_| die("--supports"))).collect(),
            "--threads" => threads = val("--threads").parse().unwrap_or_else(|_| die("--threads")),
            "--cache-mb" => cache_mb = val("--cache-mb").parse().unwrap_or_else(|_| die("--cache-mb")),
            "--no-docs" => docs = false,
            "--no-dense" => dense = false,
            "--docs-min-support" => docs_min_support = val("--docs-min-support").parse().unwrap_or_else(|_| die("--docs-min-support")),
            other => die(&format!("ctx-curve: unknown option {other}")),
        }
        i += 1;
    }
    let reader = qlite::Reader::open(&sxi).unwrap_or_else(|e| die(&format!("ctx-curve: {e}")));
    let idx = if cache_mb > 0 {
        qlite::Index::open_with_cache(reader, threads, (cache_mb << 20) as usize)
    } else {
        qlite::Index::open(reader, threads)
    }
    .unwrap_or_else(|e| die(&format!("ctx-curve: {e}")));
    let n = idx.reader.n;
    let tfile = std::fs::File::open(&text).unwrap_or_else(|e| die(&format!("open {text}: {e}")));
    let flen = tfile.metadata().unwrap().len();
    need(flen + 1 == n, "corpus length + 1 != n");
    let corpus = unsafe { Mmap::map(&tfile) }.unwrap_or_else(|e| die(&format!("mmap {text}: {e}")));
    let ranks = if docs { doc_ranks(&corpus) } else { Vec::new() };
    let (sa, _, _, _) = load_or_build_sa(&idx, threads, &cache, dense);
    println!("CONTEXT_TRIE_CURVE n={n} corpus_bytes={flen} docs_summary={}", if docs { "exact<=8+hll13pct" } else { "none" });
    for &d in &depths {
        for &k in &supports {
            let t0 = std::time::Instant::now();
            let (payload, _, cols) = sweep(&sa, &corpus, &ranks, &OwnedStarts::None, flen, n, n, d, k, docs, docs_min_support, (0, sa.len() as u64));
            let trie = Trie::load(&payload);
            let ratio = payload.len() as f64 / n as f64;
            let per_node = payload.len() as f64 / trie.nodes.max(1) as f64;
            println!(
                "CONTEXT_TRIE_CELL depth={d} min_support={k} nodes={} edges={} bytes={} ratio_vs_n={ratio:.6} bytes_per_node={per_node:.1} sweep_wall={:.1}s",
                trie.nodes, trie.edges, payload.len(), t0.elapsed().as_secs_f64()
            );
        }
    }
}

/// THE DESIGN JUSTIFICATION: over N random positions, how deep does the descent
/// actually get, and how much support does the answer carry. The context of a
/// position is the `--ctx-len` bytes that precede it (the CONTEXT_DEPTH.md
/// orientation: Q = corpus[p-L:p], excluding corpus[p]).
fn relax_stat(args: &[String]) {
    let mut sxi = String::new();
    let mut text = String::new();
    let mut samples = 1000usize;
    let mut ctx_len = 16usize;
    let mut seed = 20261110u64;
    let mut cross_doc = false;
    let mut i = 0;
    while i < args.len() {
        let f = args[i].as_str();
        let mut val = |what: &str| -> String {
            i += 1;
            match args.get(i) { Some(v) => v.clone(), None => die(&format!("{what} needs a value")) }
        };
        match f {
            "--sxi" => sxi = val("--sxi"),
            "--text" => text = val("--text"),
            "--samples" => samples = val("--samples").parse().unwrap_or_else(|_| die("--samples")),
            "--ctx-len" => ctx_len = val("--ctx-len").parse().unwrap_or_else(|_| die("--ctx-len")),
            "--seed" => seed = val("--seed").parse().unwrap_or_else(|_| die("--seed")),
            "--cross-doc" => cross_doc = true,
            other => die(&format!("relax-stat: unknown option {other}")),
        }
        i += 1;
    }
    let reader = open_reader(&sxi);
    let trie = load_trie(&reader);
    trie_boundary_gate(&trie, cross_doc, "relax-stat's support statistics");
    let tf = std::fs::File::open(&text).unwrap_or_else(|e| die(&format!("open {text}: {e}")));
    let corpus = unsafe { Mmap::map(&tf) }.unwrap_or_else(|e| die(&format!("mmap {text}: {e}")));
    let clen = corpus.len();
    let mut hist = vec![0u64; trie.depth as usize + 1];
    let mut counts: Vec<u64> = Vec::new();
    let mut st = seed | 1;
    let mut us = Vec::new();
    for _ in 0..samples {
        st ^= st << 13;
        st ^= st >> 7;
        st ^= st << 17;
        let p = ctx_len + (st % (clen as u64 - ctx_len as u64 - 1)) as usize;
        let ctx = &corpus[p - ctx_len..p];
        let t0 = std::time::Instant::now();
        let (node, reached) = trie.descend(ctx);
        us.push(t0.elapsed().as_nanos() as f64 / 1000.0);
        hist[reached] += 1;
        counts.push(trie.count(node));
    }
    counts.sort_unstable();
    us.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let pct = |v: &Vec<f64>, q: f64| v[((v.len() as f64 - 1.0) * q) as usize];
    print!("RELAX_STAT samples={samples} ctx_len={ctx_len} depth={} reached_hist=", trie.depth);
    for (d, &c) in hist.iter().enumerate() {
        print!("{d}:{c}{}", if d + 1 < hist.len() { " " } else { "\n" });
    }
    println!("RELAX_SUPPORT median_count={} p10_count={} us_p50={:.2} us_p99={:.2} us_max={:.2}",
        counts[counts.len() / 2], counts[counts.len() / 10],
        pct(&us, 0.50), pct(&us, 0.99), us[us.len() - 1]);
}

/// THE BATCH GATE: one container open, then N contexts in-process.
///
/// A per-invocation timing measures process start plus container open (~50 ms),
/// which is 1,000x the operation, so it cannot verify a <= 50 us target at all. This
/// mode loads the container ONCE and times only the descent plus the answer. It
/// opens NO corpus: `corpus_touched=0` is structural (there is no text argument in
/// this mode, so there is nothing to touch), not an assumption.
///
///   xsa branch-of <container> --batch <contexts.bin>   0x1E-separated contexts
///   xsa relax     <container> --batch <contexts.bin>
fn batch(op: &str, args: &[String]) {
    let mut sxi = String::new();
    let mut path = String::new();
    let mut cross_doc = false;
    let mut i = 0;
    while i < args.len() {
        let f = args[i].as_str();
        match f {
            "--batch" => {
                i += 1;
                path = match args.get(i) { Some(v) => v.clone(), None => die("--batch needs a file") };
            }
            "--cross-doc" => cross_doc = true,
            other if !other.starts_with("--") && sxi.is_empty() => sxi = other.to_string(),
            other => die(&format!("{op} --batch: unknown argument {other} (it takes a container, --batch FILE and optionally --cross-doc; no corpus)")),
        }
        i += 1;
    }
    need(!sxi.is_empty() && !path.is_empty(),
        &format!("usage: xsa {op} <container> --batch <contexts.bin> [--cross-doc]"));
    let reader = open_reader(&sxi);
    let trie = load_trie(&reader);
    trie_boundary_gate(&trie, cross_doc, "the batch answers (counts and branch histograms)");
    let data = std::fs::read(&path).unwrap_or_else(|e| die(&format!("read {path}: {e}")));
    let mut ctxs: Vec<&[u8]> = data.split(|&b| b == 0x1e).collect();
    while ctxs.last().map_or(false, |v| v.is_empty()) {
        ctxs.pop();
    }
    need(!ctxs.is_empty(), "no contexts in the batch file");
    // WARM THE MEMBER: the trie is a memory-mapped member, so the first touch of a
    // page is a fault (measured as a 258 us p99 before this line). That is an open
    // cost, not a query cost, so it is paid here and reported separately.
    let tw = std::time::Instant::now();
    let mut warm = 0u64;
    let m = reader.member_bytes(TRIE_CODEC).unwrap();
    let mut at = 0usize;
    while at < m.len() {
        warm = warm.wrapping_add(m[at] as u64);
        at += 4096;
    }
    let warm_ms = tw.elapsed().as_secs_f64() * 1000.0;
    std::hint::black_box(warm);
    // one untimed descent so the reader's own first-touch work (the edge-offset
    // prefix array) is not billed to the first context
    let _ = trie.stat(trie.descend(ctxs[0]).0);
    let mut us = Vec::with_capacity(ctxs.len());
    let mut answered = 0usize;
    let mut below = 0usize;
    let mut hist = vec![0u64; trie.depth as usize + 1];
    let mut depth_sum = 0f64;
    for c in ctxs.iter() {
        let t0 = std::time::Instant::now();
        if op == "branch-of" {
            // ONE descent, and "answered" means the context itself is a node (the
            // full asked prefix was reached inside the depth cap)
            let (node, reached) = trie.descend(c);
            let want = c.len().min(trie.depth as usize);
            if reached == want {
                let _ = trie.stat(node);
                answered += 1;
            } else {
                below += 1;
            }
            hist[reached] += 1;
            depth_sum += reached as f64;
        } else {
            let (node, reached) = trie.descend(c);
            let _ = trie.stat(node);
            answered += 1;
            if reached < c.len().min(trie.depth as usize) {
                below += 1;
            }
            hist[reached] += 1;
            depth_sum += reached as f64;
        }
        us.push(t0.elapsed().as_nanos() as f64 / 1000.0);
    }
    us.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let pct = |v: &Vec<f64>, q: f64| v[((v.len() as f64 - 1.0) * q) as usize];
    // TWO DIFFERENT MEMORY NUMBERS, and neither is a guess:
    //   rss_gb    = THIS process's peak resident set (VmHWM), i.e. what the run cost;
    //   stream_gb = what the MEMBER RECORDS its ordered stream cost when it was built
    //               (the u40 suffix array plus the dense run columns: a build-time
    //               figure carried in the container, NOT this process's RSS).
    // They were conflated in one `mem_gb` field that read 69.25 for a 348 MB process.
    let rss_kb = std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|t| {
            t.lines()
                .find(|l| l.starts_with("VmHWM:"))
                .and_then(|l| l.split_whitespace().nth(1).map(|v| v.parse::<f64>().ok()))
                .flatten()
        })
        .unwrap_or(0.0);
    println!("BATCH_BENCH op={op} n={} p50_us={:.2} p90_us={:.2} p99_us={:.2} max_us={:.2} \
              answered={answered} below_support={below} corpus_touched=0 \
              rss_gb={:.3} member_gb={:.3} build_stream_gb={:.2}",
        ctxs.len(), pct(&us, 0.50), pct(&us, 0.90), pct(&us, 0.99), us[us.len() - 1],
        rss_kb / (1024.0 * 1024.0),
        m.len() as f64 / (1u64 << 30) as f64,
        trie.source_mem as f64 / (1u64 << 30) as f64);
    // ONE LINE PER SETTING so the coverage/size/latency tradeoff can be quoted:
    // answered is the trie's coverage on this sample, ratio_vs_n is what it costs.
    println!("COVERAGE depth={} ms={} answered={answered} below_support={below} p50_us={:.2} p99_us={:.2} ratio_vs_n={:.6}",
        trie.depth, trie.min_support, pct(&us, 0.50), pct(&us, 0.99),
        m.len() as f64 / trie.n as f64);
    let h: Vec<String> = hist.iter().enumerate().map(|(d, &c)| format!("{d}:{c}")).collect();
    println!("BATCH_DEPTH reached_depth_hist={} mean_depth={:.3} asked_depth={} trie_depth={} warm_ms={warm_ms:.1}",
        h.join(","), depth_sum / ctxs.len() as f64, ctxs[0].len(), trie.depth);
}

/// THE LATENCY GATE: 1,000 random contexts, time the two operations the consumer
/// calls. The trie is a member of the container, reached through the mmap: NO corpus
/// byte and no run is touched by a lookup (only the sampling of the contexts reads
/// the text, and that is outside the timed region).
fn trie_bench(args: &[String]) {
    let mut sxi = String::new();
    let mut text = String::new();
    let mut samples = 1000usize;
    let mut ctx_len = 16usize;
    let mut seed = 20261110u64;
    let mut i = 0;
    while i < args.len() {
        let f = args[i].as_str();
        let mut val = |what: &str| -> String {
            i += 1;
            match args.get(i) { Some(v) => v.clone(), None => die(&format!("{what} needs a value")) }
        };
        match f {
            "--sxi" => sxi = val("--sxi"),
            "--text" => text = val("--text"),
            "--samples" => samples = val("--samples").parse().unwrap_or_else(|_| die("--samples")),
            "--ctx-len" => ctx_len = val("--ctx-len").parse().unwrap_or_else(|_| die("--ctx-len")),
            "--seed" => seed = val("--seed").parse().unwrap_or_else(|_| die("--seed")),
            other => die(&format!("trie-bench: unknown option {other}")),
        }
        i += 1;
    }
    let reader = open_reader(&sxi);
    let trie = load_trie(&reader);
    let tf = std::fs::File::open(&text).unwrap_or_else(|e| die(&format!("open {text}: {e}")));
    let corpus = unsafe { Mmap::map(&tf) }.unwrap_or_else(|e| die(&format!("mmap {text}: {e}")));
    let clen = corpus.len();
    let mut st = seed | 1;
    let mut ctxs: Vec<&[u8]> = Vec::with_capacity(samples);
    for _ in 0..samples {
        st ^= st << 13;
        st ^= st >> 7;
        st ^= st << 17;
        let p = ctx_len + (st % (clen as u64 - ctx_len as u64 - 1)) as usize;
        ctxs.push(&corpus[p - ctx_len..p]);
    }
    let mut tb = Vec::with_capacity(samples);
    let mut ts = Vec::with_capacity(samples);
    let mut found = 0usize;
    for c in ctxs.iter() {
        let t0 = std::time::Instant::now();
        let (node, reached) = trie.descend(c);
        tb.push(t0.elapsed().as_nanos() as f64 / 1000.0);
        let t1 = std::time::Instant::now();
        let _ = trie.stat(node);
        ts.push(t1.elapsed().as_nanos() as f64 / 1000.0);
        if reached == ctx_len {
            found += 1;
        }
    }
    for v in [&mut tb, &mut ts] {
        v.sort_by(|a, b| a.partial_cmp(b).unwrap());
    }
    let pct = |v: &Vec<f64>, q: f64| v[((v.len() as f64 - 1.0) * q) as usize];
    println!("TRIE_BENCH samples={} ctx_len={} depth={} exact_depth_reached={}               branch_of_us_p50={:.2} p90={:.2} p99={:.2} max={:.2}               trie_stat_us_p50={:.2} p99={:.2} max={:.2}",
        samples, ctx_len, trie.depth, found,
        pct(&tb, 0.50), pct(&tb, 0.90), pct(&tb, 0.99), tb[tb.len() - 1],
        pct(&ts, 0.50), pct(&ts, 0.99), ts[ts.len() - 1]);
}

fn hex(v: &[u8]) -> String {
    v.iter().map(|b| format!("{b:02x}")).collect()
}
fn fmt_hist(h: &[(u16, u64)]) -> String {
    h.iter()
        .map(|&(c, n)| {
            let lab = if c == END { "END".to_string() } else { format!("0x{:02x}", c - 1) };
            format!("{lab}:{n}")
        })
        .collect::<Vec<_>>()
        .join(" ")
}

fn find_once(corpus: &[u8], pat: &[u8]) -> Option<usize> {
    if pat.is_empty() || pat.len() > corpus.len() {
        return None;
    }
    let mut p = 0usize;
    while p + pat.len() <= corpus.len() {
        if &corpus[p..p + pat.len()] == pat {
            return Some(p);
        }
        p += 1;
    }
    None
}

// -------------------------------------------------------------- the tiny oracle
//
// The verification pyramid's first tier: a brute-force trie over a tiny corpus,
// compared NODE BY NODE with the streamed builder, on adversarial texts. Nothing
// here touches a container: the builder is fed the suffixes in sorted order (which
// is what the ordered sweep is) and the reference counts are computed by scanning
// the text at every position.
fn suffix_order(t: &[u8]) -> Vec<u64> {
    let mut v: Vec<u64> = (0..=t.len() as u64).collect();
    v.sort_by(|&a, &b| t[a as usize..].cmp(&t[b as usize..]));
    v
}

fn reference(t: &[u8], depth: usize, k: u64) -> std::collections::BTreeMap<Vec<u8>, (u64, Vec<(u16, u64)>)> {
    let mut m: std::collections::BTreeMap<Vec<u8>, (u64, Vec<(u16, u64)>)> = Default::default();
    for d in 1..=depth {
        for p in 0..=t.len() {
            if p + d > t.len() {
                continue;
            }
            let c = t[p..p + d].to_vec();
            let sym = if p + d == t.len() { END } else { 1 + t[p + d] as u16 };
            let e = m.entry(c).or_insert((0, Vec::new()));
            e.0 += 1;
            match e.1.iter_mut().find(|x| x.0 == sym) {
                Some(x) => x.1 += 1,
                None => e.1.push((sym, 1)),
            }
        }
    }
    m.retain(|_, v| {
        v.1.sort_unstable();
        v.0 >= k
    });
    m
}

/// The BOUNDARY-AWARE brute-force oracle (the no-cross law, no builder code):
/// every occurrence is classified against its document's end exactly as the
/// builder's feed does, so the comparison checks the LAW and not just the
/// implementation.
/// `starts` is a strictly increasing, 0-starting table of document starts.
fn reference_cut(
    t: &[u8],
    starts: &[u64],
    depth: usize,
    k: u64,
) -> std::collections::BTreeMap<Vec<u8>, (u64, Vec<(u16, u64)>)> {
    let n = t.len() as u64;
    let mut m: std::collections::BTreeMap<Vec<u8>, (u64, Vec<(u16, u64)>)> = Default::default();
    for d in 1..=depth {
        for p in 0..=t.len() {
            let p = p as u64;
            // the document containing p: its end is the next start (or the text end)
            let doc = starts.partition_point(|&s| s <= p) as u64 - 1;
            let next = starts
                .get(doc as usize + 1)
                .copied()
                .unwrap_or(u64::MAX)
                .min(n);
            let avail = next - p;
            if d as u64 > avail {
                continue; // the context would CROSS the boundary: not an occurrence
            }
            let c = t[p as usize..p as usize + d].to_vec();
            let q = p + d as u64;
            let sym = if q == n {
                END
            } else if starts.binary_search(&q).is_ok() {
                END // q is a document start: "this context ends a document"
            } else {
                1 + t[q as usize] as u16
            };
            let e = m.entry(c).or_insert((0, Vec::new()));
            e.0 += 1;
            match e.1.iter_mut().find(|x| x.0 == sym) {
                Some(x) => x.1 += 1,
                None => e.1.push((sym, 1)),
            }
        }
    }
    m.retain(|_, v| {
        v.1.sort_unstable();
        v.0 >= k
    });
    m
}

pub fn selftest() {
    // --- the packed array codec: adversarial values, exact round trip
    let mut cases = 0u64;
    let mut values = 0u64;
    for &len in &[1usize, 2, 3, 17, 255, 256, 257, 1023, 4096] {
        let mut suites: Vec<Vec<u64>> = Vec::new();
        suites.push(vec![0; len]);
        suites.push(vec![1; len]);
        let mut bite = vec![0u64; len];
        bite[len / 2] = 1_000_000;
        suites.push(bite);
        suites.push((0..len as u64).map(|i| i).collect());
        suites.push((0..len as u64).map(|i| [0u64, 1, 254, 255, 256, 65535, 65536, 1 << 40][i as usize % 8]).collect());
        suites.push((0..len as u64).map(|i| (i * 2654435761) % 7).collect());
        let mut st = 0x9e3779b97f4a7c15u64 ^ len as u64;
        suites.push((0..len).map(|_| { st ^= st << 13; st ^= st >> 7; st ^= st << 17; st & ((1u64 << (st % 63 + 1)) - 1) }).collect());
        for v in suites {
            let mut p = Packed::new();
            for &x in v.iter() {
                p.push(x);
            }
            let buf = p.finish();
            let view = PackedView::new(&buf, v.len() as u64);
            for (i, &x) in v.iter().enumerate() {
                let got = view.get(i as u64);
                need(got == x, &format!("pack round trip len={len} i={i} want={x} got={got}"));
                values += 1;
            }
            cases += 1;
        }
    }
    // --- the builder against a brute-force trie, node by node
    let mut texts: Vec<Vec<u8>> = vec![
        b"".to_vec(),
        b"a".to_vec(),
        b"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa".to_vec(),
        b"abababababababababababab".to_vec(),
        b"abcabcabcabcabcabcabcabcabc".to_vec(),
        b"the quick brown fox jumps over the lazy dog".to_vec(),
        b"\x1e\x1e\x1e".to_vec(),
        b"aaaa\x1ebbbb\x1eaaaa\x1ebbbb\x1e".to_vec(),
        b"ab\x00\xff\x00\xffab\x00\xff".to_vec(),
        b"mississippi mississippi missouri".to_vec(),
    ];
    {
        let mut st = 12345u64;
        for len in [37usize, 64, 129, 300] {
            let t: Vec<u8> = (0..len)
                .map(|_| {
                    st ^= st << 13;
                    st ^= st >> 7;
                    st ^= st << 17;
                    b"abc \x1e"[(st % 5) as usize]
                })
                .collect();
            texts.push(t);
        }
    }
    let mut contexts = 0u64;
    for t in &texts {
        for &depth in &[1usize, 2, 3, 5, 8, 32] {
            for &k in &[1u64, 2, 3, 1000] {
                let order = suffix_order(t);
                let mut b = Builder::new(depth, k, 0);
                let mut buf = vec![0u8; depth + 1];
                for &p in &order {
                    let p = p as usize;
                    let rem = (t.len() - p) as u64;
                    let m = (rem.min(depth as u64 + 1)) as usize;
                    buf[..m].copy_from_slice(&t[p..p + m]);
                    b.feed(&buf, rem, u64::MAX, 0);
                }
                let (payload, _) = b.finish(t.len() as u64 + 1, t.len() as u64);
                let trie = Trie::load(&payload);
                need(trie.count(trie.root()) == t.len() as u64 + 1, "root count must be n");
                let want = reference(t, depth, k);
                for (c, (count, hist)) in want.iter() {
                    let node = match trie.find(c) {
                        Some(n) => n,
                        None => {
                            let r = trie.root();
                            let st = trie.stat(r);
                            let kids: Vec<String> = st.children.iter()
                                .map(|x| format!("{}({},{:?})", x.label(), x.count, x.node)).collect();
                            die(&format!("selftest: context {:?} (len {}) missing; root count={} kids={:?} nodes={} t={:?} d={depth} k={k}",
                                c, c.len(), st.count, kids, trie.nodes, String::from_utf8_lossy(t)));
                        }
                    };
                    let st = trie.stat(node);
                    let got: Vec<(u16, u64)> = st.children.iter().map(|x| (x.code, x.count)).collect();
                    need(st.count == *count && &got == hist,
                        &format!("selftest: context {:?} count want={count} got={} hist want={:?} got={:?} (len={} depth={depth} k={k})", c, st.count, hist, got, t.len()));
                    need(st.depth as usize == c.len(), "selftest: node depth");
                    contexts += 1;
                }
                // every stored node must be a key of the reference
                for i in 0..trie.nodes as u32 {
                    let d = trie.node_depth(i);
                    if d == 0 {
                        continue;
                    }
                    need(d as u64 <= depth as u64, "selftest: node deeper than D");
                }
            }
        }
    }
    // --- the builder against the BOUNDARY-AWARE brute-force oracle: the no-cross
    // law, node by node. The starts tables are METADATA placed at bytes the
    // content does not suggest (including inside repeated patterns, so many
    // contexts END documents and their continuations must land on the END branch,
    // never on the next document's first byte).
    let mut cut_contexts = 0u64;
    let mut cut_nodes = 0u64;
    for t in &texts {
        let n = t.len() as u64;
        if n == 0 {
            continue;
        }
        let mut tables: Vec<Vec<u64>> = vec![vec![0u64]];
        for &(a, b) in &[(1u64, 2u64), (1, 3), (3, 4), (2, 5)] {
            // 0 is the mandatory first start; the interior starts must be
            // strictly inside the text (0 < s < n) and distinct from 0
            let mut s = vec![0u64, (n * a).div_ceil(a + b), (n * b).div_ceil(a + b)];
            s.retain(|&x| x == 0 || (x > 0 && x < n));
            s.dedup();
            if s.len() == 3 {
                tables.push(s);
            }
        }
        for table in &tables {
            let oracle = OwnedStarts::Plain { starts: table.clone(), text_len: n };
            for &depth in &[1usize, 2, 3, 5, 8, 32] {
                for &k in &[1u64, 2, 3, 1000] {
                    let order = suffix_order(t);
                    let mut b = Builder::new(depth, k, 0);
                    b.set_boundary_aware();
                    let mut buf = vec![0u8; depth + 1];
                    for &p in &order {
                        let rem_full = n - p;
                        // avail: bytes to the END of p's document (u64::MAX for
                        // the virtual END row); the prefix stays FULL - the law is
                        // classification, never a stream edit
                        let avail = if p >= n {
                            u64::MAX
                        } else {
                            let doc = table.partition_point(|&s| s <= p) as u64 - 1;
                            table
                                .get(doc as usize + 1)
                                .copied()
                                .unwrap_or(u64::MAX)
                                .min(n)
                                - p
                        };
                        let m = (rem_full.min(depth as u64 + 1)) as usize;
                        buf[..m].copy_from_slice(&t[p as usize..p as usize + m]);
                        let doc = if p >= n { 0 } else { table.partition_point(|&s| s <= p) - 1 };
                        b.feed(&buf, rem_full, avail, doc as u32);
                    }
                    let (payload, _) = b.finish(n + 1, n);
                    let trie = Trie::load(&payload);
                    need(trie.boundary_aware == 1, "selftest: aware build did not journal boundary_aware");
                    need(trie.count(trie.root()) == n + 1, "aware root count must be n");
                    let want = reference_cut(t, table, depth, k);
                    for (c, (count, hist)) in want.iter() {
                        let node = trie
                            .find(c)
                            .unwrap_or_else(|| die(&format!(
                                "selftest(cut): context {:?} missing (len {} t={:?} starts={:?} d={depth} k={k})",
                                c, c.len(), String::from_utf8_lossy(t), table, )));
                        let st = trie.stat(node);
                        let got: Vec<(u16, u64)> = st.children.iter().map(|x| (x.code, x.count)).collect();
                        need(st.count == *count && &got == hist,
                            &format!(
                                "selftest(cut): context {:?} count want={count} got={} hist want={:?} got={:?} \
                                 (len={} depth={depth} k={k} starts={:?} t={:?})",
                                c, st.count, hist, got, t.len(), table, String::from_utf8_lossy(t)));
                        cut_contexts += 1;
                    }
                    cut_nodes += trie.nodes;
                }
            }
        }
    }
    println!("CTX_SELFTEST pack_cases={cases} pack_values={values} texts={} contexts={} cut_contexts={} cut_nodes={}",
        texts.len(), contexts, cut_contexts, cut_nodes);
}
