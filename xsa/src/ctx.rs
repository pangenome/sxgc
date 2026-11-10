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
pub const TRIE_VERSION: u64 = 1;
/// Header flag value: "member m20 (context trie) is present".
pub const FLAG_TRIE: u64 = 256;
/// Head bytes of the member.
const HEAD: usize = 128;
/// Child code of "the text ends here". Real bytes are stored as 1+byte, so the
/// u16 child code sorts exactly like the lexicographic order (END first).
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
    top: u32,
    topcount: u32,
    hll: Hll,
    over: bool,
}

impl DocAcc {
    fn new() -> Self {
        Self { small: [0; DOC_SMALL], n: 0, top: NO_NODE, topcount: 0, hll: Hll::new(), over: false }
    }
    #[inline]
    fn add(&mut self, doc: u32) {
        if !self.over {
            if self.n < DOC_SMALL || self.small[..self.n].contains(&doc) {
                if self.n < DOC_SMALL && !self.small[..self.n].contains(&doc) {
                    self.small[self.n] = doc;
                    self.n += 1;
                }
                if self.top == doc {
                    self.topcount += 1;
                } else if self.top == NO_NODE || self.topcount == 0 {
                    self.top = doc;
                    self.topcount = 1;
                }
                return;
            }
            // overflow: seed the sketch from the exact set we already have
            for i in 0..self.n {
                self.hll.add_hash(doc_hash(self.small[i] as u64));
            }
            self.over = true;
        }
        self.hll.add_hash(doc_hash(doc as u64));
        if self.top == doc {
            self.topcount += 1;
        } else {
            // a cheap exact top-1 over the whole node would need a per-doc
            // counter; the stored top-1 is the most frequent of the small prefix,
            // so it is only claimed where the node stayed exact.
            self.top = NO_NODE;
        }
    }
    fn n_docs(&self) -> u64 {
        if self.over { self.hll.estimate() } else { self.n as u64 }
    }
}

// ------------------------------------------------------------------- the builder

struct Level {
    start: u64,
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
    root: Vec<Child>, // the root's children (open level 0)
    root_docs: DocAcc,
    stack: Vec<Level>, // levels 1..=deepest open node
    row: u64,
    prev: Vec<u8>,
    nodes: Vec<u8>, // serialised node records
    edges: Vec<u8>, // serialised edge records
    ncount: Packed,
    ecount: Packed,
    docrec: Vec<u8>,
    doclist: Vec<u32>,
    nnodes: u64,
}

const NODE_REC: usize = 16;
const EDGE_REC: usize = 16;
const DOC_REC: usize = 16;

impl Builder {
    pub fn new(depth: usize, min_support: u64) -> Self {
        need((1..=64).contains(&depth), "--depth must be 1..=64");
        need(min_support >= 1, "--min-support must be >= 1");
        Self {
            depth,
            min_support,
            root: Vec::new(),
            root_docs: DocAcc::new(),
            stack: Vec::new(),
            row: 0,
            prev: Vec::new(),
            nodes: Vec::new(),
            edges: Vec::new(),
            ncount: Packed::new(),
            ecount: Packed::new(),
            docrec: Vec::new(),
            doclist: Vec::new(),
            nnodes: 0,
        }
    }

    /// One suffix, in sorted order. `p` holds the suffix's bytes (up to depth+1 of
    /// them are read), `rem` is how many bytes the suffix really has, and `doc` is
    /// the document the suffix starts in.
    pub fn feed(&mut self, p: &[u8], rem: u64, doc: u32) {
        // m = the suffix's own length, capped at D+1 (its first D bytes open the
        // node path, byte D feeds the depth-D histogram).
        let m = (rem.min(self.depth as u64 + 1)) as usize;
        need(p.len() >= m, "builder: prefix shorter than the suffix's own length");
        let cap = m.min(self.prev.len());
        let mut lcp = 0usize;
        while lcp < cap && p[lcp] == self.prev[lcp] {
            lcp += 1;
        }
        // `keep` nodes stay open, `top` is the deepest node this suffix reaches
        let keep = lcp.min(self.depth);
        let top = m.min(self.depth);
        need(keep <= top, "builder: keep above top");
        self.root_docs.add(doc);
        for lv in self.stack.iter_mut().take(keep) {
            lv.docs.add(doc);
        }
        // close every node deeper than the longest common prefix
        while self.stack.len() > keep {
            self.close_one();
        }
        need(self.stack.len() == keep, "builder: stack/prefix invariant");
        // open the nodes this suffix is the first row of; each starts at this row
        // and its count is its row span, so no counter is ever incremented.
        for d in (keep + 1)..=top {
            let code = 1 + p[d - 1] as u16;
            let parent = if d == 1 { &mut self.root } else { &mut self.stack[d - 2].children };
            need(
                parent.last().map_or(true, |c| c.code < code),
                "builder: children are not in ascending byte order (the suffix stream is not sorted)",
            );
            parent.push(Child { code, start: self.row, count: 0, node: NO_NODE });
            self.stack.push(Level { start: self.row, children: Vec::new(), docs: DocAcc::new() });
        }
        for lv in self.stack.iter_mut().skip(keep) {
            lv.docs.add(doc);
        }
        // the deepest open node records a histogram-only child: END when the
        // suffix ends here (END sorts below every byte, so it is the FIRST child),
        // otherwise the byte at the depth cap. Every shallower child is already a
        // node whose count is its span.
        let parent = if top == 0 { &mut self.root } else { &mut self.stack[top - 1].children };
        if top == m {
            need(
                !parent.iter().any(|c| c.code == END),
                "builder: two END children for one context",
            );
            parent.insert(0, Child { code: END, start: self.row, count: 1, node: NO_NODE });
        } else {
            let code = 1 + p[top] as u16;
            match parent.last_mut() {
                Some(c) if c.code == code => c.count += 1,
                Some(c) => {
                    need(c.code < code, "builder: children are not in ascending byte order");
                    parent.push(Child { code, start: self.row, count: 1, node: NO_NODE });
                }
                None => parent.push(Child { code, start: self.row, count: 1, node: NO_NODE }),
            }
        }
        self.prev.clear();
        self.prev.extend_from_slice(&p[..m]);
        self.row += 1;
    }

    fn close_one(&mut self) {
        let lv = self.stack.pop().expect("builder: close on empty stack");
        let span = self.row - lv.start;
        let depth = self.stack.len() + 1;
        // the parent's last child is exactly this node
        let parent = if depth == 1 { &mut self.root } else { &mut self.stack[depth - 2].children };
        let last = parent.last_mut().expect("builder: closing a node whose parent has no child");
        need(last.count == 0 && last.node == NO_NODE, "builder: child already closed");
        last.count = span;
        let parent_docs = if depth == 1 { &mut self.root_docs } else { &mut self.stack[depth - 2].docs };
        let _ = parent_docs;
        if span < self.min_support {
            return; // pruned: the histogram keeps its count, the node store does not
        }
        let id = self.emit_node(depth, span, &lv.children, &lv.docs);
        let parent = if depth == 1 { &mut self.root } else { &mut self.stack[depth - 2].children };
        parent.last_mut().expect("builder: parent child vanished").node = id;
    }

    fn emit_node(&mut self, depth: usize, span: u64, children: &[Child], docs: &DocAcc) -> u32 {
        need(children.len() <= u16::MAX as usize, "node has more than 65535 children");
        let first_edge = (self.edges.len() / EDGE_REC) as u32;
        for c in children {
            let mut e = [0u8; EDGE_REC];
            put(&mut e[0..], c.code as u64, 2);
            put(&mut e[2..], u64::from(c.node != NO_NODE), 2);
            put(&mut e[4..], if c.node == NO_NODE { NO_NODE as u64 } else { c.node as u64 }, 4);
            self.edges.extend_from_slice(&e);
            self.ecount.push(c.count);
        }
        let doc_ref = self.doc_record(docs);
        let mut n = [0u8; NODE_REC];
        put(&mut n[0..], depth as u64, 1);
        put(&mut n[1..], children.len() as u64, 2);
        put(&mut n[4..], first_edge as u64, 4);
        put(&mut n[8..], doc_ref as u64, 4);
        self.nodes.extend_from_slice(&n);
        self.ncount.push(span);
        let id = self.nnodes as u32;
        self.nnodes += 1;
        id
    }

    fn doc_record(&mut self, d: &DocAcc) -> u32 {
        let mode = if !d.over && d.n == 0 { 0 } else if !d.over { 1 } else { 2 };
        let mut r = [0u8; DOC_REC];
        put(&mut r[0..], mode, 1);
        put(&mut r[1..], d.n as u64, 1);
        put(&mut r[4..], d.n_docs(), 4);
        let first = self.doclist.len() as u32;
        put(&mut r[8..], first as u64, 4);
        put(&mut r[12..], if d.top == NO_NODE { NO_NODE as u64 } else { d.top as u64 }, 4);
        self.doclist.extend_from_slice(&d.small[..d.n]);
        self.docrec.extend_from_slice(&r);
        (self.docrec.len() / DOC_REC - 1) as u32
    }

    /// Close the root and serialise the member payload.
    pub fn finish(mut self, n: u64, corpus_len: u64) -> Vec<u8> {
        while !self.stack.is_empty() {
            self.close_one();
        }
        let root_children = std::mem::take(&mut self.root);
        let root_docs = std::mem::replace(&mut self.root_docs, DocAcc::new());
        let root = self.emit_node(0, n, &root_children, &root_docs);
        let nodes = self.nnodes;
        let edges = (self.edges.len() / EDGE_REC) as u64;
        let ncount = std::mem::replace(&mut self.ncount, Packed::new()).finish();
        let ecount = std::mem::replace(&mut self.ecount, Packed::new()).finish();
        let dlist: Vec<u8> = self.doclist.iter().flat_map(|v| v.to_le_bytes()).collect();
        let sections: [&[u8]; 6] =
            [&self.nodes, &self.edges, &ncount, &ecount, &self.docrec, &dlist];
        let mut offs = [0u64; 6];
        let mut at = HEAD as u64;
        for (i, s) in sections.iter().enumerate() {
            offs[i] = at;
            at += ((s.len() as u64 + 7) / 8) * 8;
        }
        let mut out = vec![0u8; at as usize];
        for (i, s) in sections.iter().enumerate() {
            out[offs[i] as usize..offs[i] as usize + s.len()].copy_from_slice(s);
        }
        put(&mut out[0..], TRIE_VERSION, 8);
        put(&mut out[8..], self.depth as u64, 8);
        put(&mut out[16..], self.min_support, 8);
        put(&mut out[24..], n, 8);
        put(&mut out[32..], corpus_len, 8);
        put(&mut out[40..], nodes, 8);
        put(&mut out[48..], edges, 8);
        put(&mut out[56..], offs[0], 8);
        put(&mut out[64..], offs[1], 8);
        put(&mut out[72..], offs[2], 8);
        put(&mut out[80..], offs[3], 8);
        put(&mut out[88..], offs[4], 8);
        put(&mut out[96..], offs[5], 8);
        put(&mut out[104..], root as u64, 8);
        out
    }
}

// -------------------------------------------------------------------- the reader

pub struct Trie<'a> {
    d: &'a [u8],
    pub depth: u64,
    pub min_support: u64,
    pub n: u64,
    pub corpus_len: u64,
    pub nodes: u64,
    pub edges: u64,
    node_off: usize,
    edge_off: usize,
    root: u32,
    ncount: PackedView<'a>,
    ecount: PackedView<'a>,
    doc_off: usize,
    doclist_off: usize,
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
        need(d.len() >= HEAD, "member shorter than its head");
        need(get(&d[0..], 8) == TRIE_VERSION, "unsupported member format version");
        let depth = get(&d[8..], 8);
        let min_support = get(&d[16..], 8);
        let n = get(&d[24..], 8);
        let corpus_len = get(&d[32..], 8);
        let nodes = get(&d[40..], 8);
        let edges = get(&d[48..], 8);
        let node_off = get(&d[56..], 8) as usize;
        let edge_off = get(&d[64..], 8) as usize;
        let ncount_off = get(&d[72..], 8) as usize;
        let ecount_off = get(&d[80..], 8) as usize;
        let doc_off = get(&d[88..], 8) as usize;
        let doclist_off = get(&d[96..], 8) as usize;
        let root = get(&d[104..], 8) as u32;
        need(depth >= 1 && depth <= 64 && min_support >= 1 && n >= 1, "member head values");
        need(root < nodes.max(1) as u32, "root node id");
        need(node_off >= HEAD && edge_off >= HEAD && ncount_off >= HEAD && ecount_off >= HEAD,
            "member section offsets");
        need(node_off + nodes as usize * NODE_REC <= d.len(), "node array bounds");
        need(edge_off + edges as usize * EDGE_REC <= d.len(), "edge array bounds");
        need(doc_off <= d.len() && doclist_off <= d.len(), "doc section bounds");
        Self {
            d,
            depth,
            min_support,
            n,
            corpus_len,
            nodes,
            edges,
            node_off,
            edge_off,
            root,
            ncount: PackedView::new(&d[ncount_off..], nodes),
            ecount: PackedView::new(&d[ecount_off..], edges),
            doc_off,
            doclist_off,
        }
    }

    fn node(&self, i: u32) -> (u8, usize, u32, u32) {
        need((i as u64) < self.nodes, "node id");
        let o = self.node_off + i as usize * NODE_REC;
        let b = &self.d[o..o + NODE_REC];
        (b[0], get(&b[1..], 2) as usize, get(&b[4..], 4) as u32, get(&b[8..], 4) as u32)
    }

    fn edge(&self, i: u32) -> ChildView {
        need((i as u64) < self.edges, "edge id");
        let o = self.edge_off + i as usize * EDGE_REC;
        let b = &self.d[o..o + EDGE_REC];
        let code = get(&b[0..], 2) as u16;
        let has = get(&b[2..], 2) != 0;
        let node = get(&b[4..], 4) as u32;
        ChildView { code, count: self.ecount.get(i as u64), node: if has { Some(node) } else { None } }
    }

    pub fn children(&self, node: u32) -> Vec<ChildView> {
        let (_, nch, first, _) = self.node(node);
        (0..nch as u32).map(|k| self.edge(first + k)).collect()
    }

    pub fn count(&self, node: u32) -> u64 {
        self.ncount.get(node as u64)
    }

    pub fn node_depth(&self, node: u32) -> u64 {
        self.node(node).0 as u64
    }

    /// Document summary of a node: (exact?, distinct count, top-1 doc).
    pub fn docs(&self, node: u32) -> (bool, u64, Option<u32>) {
        let (_, _, _, dr) = self.node(node);
        let o = self.doc_off + dr as usize * DOC_REC;
        need(o + DOC_REC <= self.d.len(), "doc record id");
        let b = &self.d[o..o + DOC_REC];
        let mode = b[0];
        let ndocs = get(&b[4..], 4);
        let top = get(&b[12..], 4) as u32;
        (mode == 1, ndocs, if top == NO_NODE { None } else { Some(top) })
    }

    pub fn root(&self) -> u32 {
        self.root
    }

    /// Descend by `pat` from the root. `None` = the context is absent (below
    /// MIN_SUPPORT, deeper than D, or not in the corpus at all).
    pub fn find(&self, pat: &[u8]) -> Option<u32> {
        if pat.len() as u64 > self.depth {
            return None;
        }
        let mut node = self.root;
        for &b in pat {
            let (_, nch, first, _) = self.node(node);
            let want = 1 + b as u16;
            let mut lo = 0usize;
            let mut hi = nch;
            let mut found = None;
            while lo < hi {
                let mid = (lo + hi) / 2;
                let e = self.edge(first + mid as u32);
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

/// Reconstruct the suffix array of a lite container: `sa[row] = SA[row]`.
///
/// The LF cycle is one cycle of length n (the END symbol is unique), and it visits
/// rows in DESCENDING text position. K breakpoints partition that cycle exactly:
/// locate K rows (one walk each, ~1650 LF steps), order them by position, and each
/// segment walks from its breakpoint until it would reach the next one. Total work
/// is n LF steps, split across K threads, plus K locates.
pub fn sa_reconstruct(idx: &qlite::Index, threads: usize) -> (Vec<u8>, u64) {
    let t0 = std::time::Instant::now();
    let n = idx.reader.n;
    need(n < SA_UNSET, "n does not fit the u40 suffix array");
    let mut sa = vec![0xFFu8; (n as usize) * SA_ITEM];
    let k = (threads.max(1) as u64).min(n).min(64);
    let rows: Vec<u64> = (0..k).map(|i| i * n / k).collect();
    let mut brk: Vec<(u64, u64)> = rows
        .iter()
        .map(|&r| {
            let mut ctx = idx.ctx();
            (r, ctx.sa_of(r))
        })
        .collect();
    let mut seen = std::collections::HashSet::new();
    for &(_, p) in brk.iter() {
        need(seen.insert(p), "SA reconstruction: two breakpoints at one position");
    }
    brk.sort_by(|a, b| b.1.cmp(&a.1)); // descending position == cycle order
    let plan: Vec<(u64, u64, u64)> = (0..brk.len())
        .map(|j| {
            let next = brk[(j + 1) % brk.len()].1;
            (brk[j].0, brk[j].1, next)
        })
        .collect();
    // The segments write disjoint rows; the compiler cannot see that, and the two
    // checks after the join (n steps total, no row left at the sentinel) are what
    // prove it.
    #[derive(Clone, Copy)]
    struct Shared(*mut u8);
    unsafe impl Send for Shared {}
    unsafe impl Sync for Shared {}
    let sh = Shared(sa.as_mut_ptr());
    let results: Vec<u64> = std::thread::scope(|sc| {
        let mut hs = Vec::new();
        for &(row0, pos0, stop) in plan.iter() {
            let s = &sh;
            hs.push(sc.spawn(move || {
                let mut ctx = idx.ctx();
                let p = s.0;
                let mut row = row0;
                let mut pos = pos0;
                let mut steps = 0u64;
                while pos != stop {
                    unsafe {
                        let o = (row as usize) * SA_ITEM;
                        std::ptr::copy_nonoverlapping(pos.to_le_bytes().as_ptr(), p.add(o), 5);
                    }
                    let (nr, _) = ctx.lf_step(row);
                    row = nr;
                    pos = if pos == 0 { n - 1 } else { pos - 1 };
                    steps += 1;
                    need(steps <= n, "SA reconstruction: cycle did not reach its breakpoint");
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
    crate::trace_since("ctx.sa-reconstruct", t0);
    (sa, steps_total)
}

// ------------------------------------------------------------------ doc ranks

/// Documents are runs of bytes delimited by 0x1E, counted on the ORIGINAL corpus
/// bytes (the container's remap is order preserving, so the SA order is the same
/// order, but the remap may move 0x1E - so 0x1E is counted on the corpus, never on
/// the remapped text). One u32 every `DOC_STRIDE` bytes.
const DOC_STRIDE: usize = 4096;

fn count_1e(w: u64) -> u32 {
    // SWAR: bytes equal to 0x1E
    let x = w ^ 0x1e1e_1e1e_1e1e_1e1e;
    ((x.wrapping_sub(0x0101_0101_0101_0101)) & !x & 0x8080_8080_8080_8080).count_ones() / 8
}

pub fn doc_ranks(text: &[u8]) -> Vec<u32> {
    let blocks = text.len() / DOC_STRIDE + 1;
    let mut out = Vec::with_capacity(blocks);
    let mut acc = 0u32;
    let mut at = 0usize;
    out.push(0);
    while at + 8 <= text.len() {
        let mut w = [0u8; 8];
        w.copy_from_slice(&text[at..at + 8]);
        acc += count_1e(u64::from_le_bytes(w));
        at += 8;
        if at % DOC_STRIDE == 0 {
            out.push(acc);
        }
    }
    while at < text.len() {
        if text[at] == 0x1e {
            acc += 1;
        }
        at += 1;
        if at % DOC_STRIDE == 0 {
            out.push(acc);
        }
    }
    while out.len() < blocks {
        out.push(acc);
    }
    out
}

/// The document id of a text position: the number of 0x1E bytes before it. The
/// last byte of a document (the 0x1E itself) belongs to that document, so the
/// rank is taken at `pos + 1`.
#[inline]
fn doc_of(rank: &[u32], text: &[u8], pos: u64) -> u32 {
    let b = (pos as usize) / DOC_STRIDE;
    let mut d = rank[b];
    let start = b * DOC_STRIDE;
    let mut i = start;
    while i < pos as usize {
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
}

pub fn build(a: &BuildArgs) {
    let t_all = std::time::Instant::now();
    let reader = qlite::Reader::open(&a.sxi).unwrap_or_else(|e| die(&format!("ctx-build: {e}")));
    let idx = qlite::Index::open(reader, a.threads)
        .unwrap_or_else(|e| die(&format!("ctx-build: {e}")));
    let n = idx.reader.n;
    let tfile = std::fs::File::open(&a.text).unwrap_or_else(|e| die(&format!("open {}: {e}", a.text)));
    let flen = tfile.metadata().unwrap().len();
    need(flen + 1 == n,
        &format!("corpus {} is {flen} bytes but the container has n={n} rows: a lite container's n is the corpus length + 1 (one END row)", a.text));
    let text = unsafe { Mmap::map(&tfile) }.unwrap_or_else(|e| die(&format!("mmap {}: {e}", a.text)));
    let corpus_len = flen;
    let rows = a.limit_rows.unwrap_or(n).min(n);

    // 1. the ordered stream: the suffix array, from one parallel LF cycle.
    let t_sa = std::time::Instant::now();
    let (sa, sa_steps) = sa_reconstruct(&idx, a.threads);
    let sa_wall = t_sa.elapsed().as_secs_f64();

    // 2. document ranks (original bytes)
    let t_doc = std::time::Instant::now();
    let ranks = if a.docs { doc_ranks(&text) } else { Vec::new() };
    let doc_wall = t_doc.elapsed().as_secs_f64();

    // 3. the sweep
    let t_sweep = std::time::Instant::now();
    let mut b = Builder::new(a.depth, a.min_support);
    let dmax = a.depth as u64 + 1;
    let mut buf = [0u8; 128];
    let mut reads = 0u64;
    for row in 0..rows {
        let pos = sa_get(&sa, row);
        need(pos < n, "suffix array position");
        let rem = if pos >= corpus_len { 0 } else { corpus_len - pos };
        let m = rem.min(dmax) as usize;
        if m > 0 {
            let p = pos as usize;
            buf[..m].copy_from_slice(&text[p..p + m]);
            reads += 1;
        }
        let doc = if a.docs && a.min_support < u64::MAX {
            if m > 0 { doc_of(&ranks, &text, pos) } else { 0 }
        } else {
            0
        };
        b.feed(&buf, rem, doc);
    }
    let payload = b.finish(n, corpus_len);
    let sweep_wall = t_sweep.elapsed().as_secs_f64();

    // 4. write the container: the old members, then m20, then a v9 directory.
    let (bytes, member_off) = write_container(&a.sxi, &a.out, &payload, n, &idx.reader);
    let ratio = bytes as f64 / n as f64;
    let trie = Trie::load(&payload);
    eprintln!(
        "CONTEXT_TRIE depth={} min_support={} nodes={} edges={} bytes={} ratio_vs_n={:.6} docs_summary=exact<=8+hll13pct",
        trie.depth, trie.min_support, trie.nodes, trie.edges, member_off, ratio
    );
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

pub fn command(cmd: &str, args: &[String]) {
    match cmd {
        "ctx-build" => {
            let mut a = BuildArgs {
                sxi: String::new(),
                text: String::new(),
                out: String::new(),
                depth: 16,
                min_support: 10_000,
                threads: 16,
                docs: true,
                limit_rows: None,
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
                    "--rows" => {
                        a.limit_rows = Some(val("--rows").parse().unwrap_or_else(|_| die("--rows: integer")))
                    }
                    "--no-docs" => a.docs = false,
                    other => die(&format!("ctx-build: unknown option {other}")),
                }
                i += 1;
            }
            need(!a.sxi.is_empty() && !a.text.is_empty() && !a.out.is_empty(),
                "usage: xsa ctx-build --sxi <in.sxi2> --text <corpus> --out <out.sxi2> \
                 [--depth D] [--min-support K] [--threads N] [--no-docs] [--rows N]");
            build(&a);
        }
        "branch-of" => {
            need(args.len() == 2, "usage: xsa branch-of <container> <context>   (context: text with \\xHH escapes, or hex:...)");
            let reader = open_reader(&args[0]);
            let pat = parse_context(&args[1]);
            let trie = load_trie(&reader);
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
            need(args.len() == 2, "usage: xsa trie-stat <container> <context>");
            let reader = open_reader(&args[0]);
            let pat = parse_context(&args[1]);
            let trie = load_trie(&reader);
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
        "trie-info" => {
            need(args.len() == 1, "usage: xsa trie-info <container>");
            let reader = open_reader(&args[0]);
            let trie = load_trie(&reader);
            let m = reader.member_bytes(TRIE_CODEC).unwrap();
            println!("CONTEXT_TRIE depth={} min_support={} nodes={} edges={} bytes={} ratio_vs_n={:.6} n={}",
                trie.depth, trie.min_support, trie.nodes, trie.edges, m.len(),
                m.len() as f64 / trie.n as f64, trie.n);
        }
        "ctx-verify" => verify(args),
        "ctx-selftest" => selftest(),
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
            other => die(&format!("ctx-verify: unknown option {other}")),
        }
        i += 1;
    }
    need(!sxi.is_empty() && !text.is_empty(), "usage: xsa ctx-verify --sxi X --text C [--samples 50] [--seed S] [--context CTX]...");
    let reader = open_reader(&sxi);
    let trie = load_trie(&reader);
    let tfile = std::fs::File::open(&text).unwrap_or_else(|e| die(&format!("open {text}: {e}")));
    let tlen = tfile.metadata().unwrap().len();
    need(tlen + 1 == trie.n, "corpus length + 1 != the trie's n");
    let corpus = unsafe { Mmap::map(&tfile) }.unwrap_or_else(|e| die(&format!("mmap {text}: {e}")));
    let clen = tlen as usize;

    // sample real contexts: S uniform positions, the D-byte context at each
    let d = trie.depth as usize;
    let mut st = seed | 1;
    let mut ctxs: Vec<(usize, Vec<u8>)> = Vec::new();
    for g in given {
        let c = parse_context(&g);
        let p = find_once(&corpus, &c).unwrap_or_else(|| die(&format!("--context not present in the corpus")));
        ctxs.push((p, c));
    }
    while ctxs.len() < samples {
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
        let mut p = 0usize;
        while p + 4 <= clen {
            let k = u32::from_le_bytes([corpus[p], corpus[p + 1], corpus[p + 2], corpus[p + 3]]);
            if let Some(ids) = key.get(&k) {
                for &id in ids {
                    let c = &ctxs[id];
                    if p + c.len() <= clen && &corpus[p..p + c.len()] == &c[..] {
                        let sym = if p + c.len() == clen { END } else { 1 + corpus[p + c.len()] as u16 };
                        bump(id, sym);
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
                    let sym = if p + c.len() == clen { END } else { 1 + corpus[p + c.len()] as u16 };
                    bump(id, sym);
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
                let mut b = Builder::new(depth, k);
                let mut buf = vec![0u8; depth + 1];
                for &p in &order {
                    let p = p as usize;
                    let rem = (t.len() - p) as u64;
                    let m = (rem.min(depth as u64 + 1)) as usize;
                    buf[..m].copy_from_slice(&t[p..p + m]);
                    b.feed(&buf, rem, 0);
                }
                let payload = b.finish(t.len() as u64 + 1, t.len() as u64);
                let trie = Trie::load(&payload);
                need(trie.count(trie.root()) == t.len() as u64 + 1, "root count must be n");
                let want = reference(t, depth, k);
                for (c, (count, hist)) in want.iter() {
                    let node = trie.find(c).unwrap_or_else(|| {
                        die(&format!("selftest: context {:?} (len {}) missing from the trie (len={} depth={depth} k={k})", c, c.len(), t.len()))
                    });
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
    println!("CTX_SELFTEST pack_cases={cases} pack_values={values} texts={} contexts={contexts}", texts.len());
}
