//! DOCUMENT STARTS AS CONTAINER METADATA — member m15, SXI2 v10.
//!
//! THE PRINCIPLE (NOTE_FROM_EMENDER_DOCUMENT_BOUNDARIES.md): document
//! boundaries are METADATA, never inferred from content bytes. At vocabulary 256
//! no in-band delimiter can be safe by construction — the Pile's own 0x1E
//! framing collides with 0x1E inside document texts (78,715 documents over the
//! full 1.31 TB; 446 in the first 10 GB; 6,979 in the first 100 GB; 0 in the
//! flagship region — empirical uniqueness is region-local and decays with
//! corpus size). A delimiter is a serialization convenience; the index is the
//! authority for structure.
//!
//! This module puts the monotone array of document starts INTO the container as
//! member m15 (next free id; codec 15; container version bumps 9 -> 10, flag
//! bit 9 = 512). ABSENCE = the feature is OFF and every refusal names the
//! container version; OLD containers (v6..v9) stay readable unchanged.
//!
//! Encoding (ENCODING_NAME = "delta-varint", sampled absolutes every
//! SAMPLE_STRIDE docs):
//!
//!   header (48 B, little-endian):
//!     [ 0.. 8) b"XDOCST01"          member magic + member format version 1
//!     [ 8..12) u32 member_version = 1
//!     [12..16) u32 sample_stride (k; default 64)
//!     [16..24) u64 doc_count
//!     [24..32) u64 text_len          (== the container's n - 1)
//!     [32..40) u64 sample_count      (== ceil(doc_count / stride))
//!     [40..48) u64 gaps_bytes        (byte length of the varint stream)
//!   then sample_count u64: the ABSOLUTE start of every stride-th document
//!   then sample_count u32: the byte offset of that sample's first gap inside
//!        the varint stream
//!   then gaps_bytes LEB128 varints: gap[0] = start[0] (must be 0), gap[i] =
//!        start[i] - start[i-1] > 0
//!
//! Size on the Pile (mean gap 5,554 B -> 2-byte varints): ~2.2 B/doc plus
//! 12 B per 64 docs ≈ 2.4 B/doc ≈ 500 MB for 210.6 M docs over 1.31 TB
//! (~0.038 % of the corpus). A plain u64 array would be 1.9 GB; the sampled
//! form is reported next to it at gate time (gaps_bytes vs 8*doc_count).
//!
//! COST (stated, as required): `rank(p)` and `select(i)` are one binary search
//! over the sample table (O(log doc_count/stride)) plus a scan of at most
//! `stride` varint gaps; with the default stride 64 that is ~18 compares plus
//! <=64 single-byte-shift varint decodes per lookup — negligible next to the
//! query kernel's rank steps. Nothing per-document is materialised in RAM: the
//! sample tables are read straight out of the mapping cell by cell.
//!
//! The member is fully validated ONCE at load: every gap is decoded (fail-loud
//! on any non-canonical varint, non-monotone start, start >= text_len, count
//! mismatch, or a sample absolute that disagrees with the varint stream), so a
//! consult that answers from m15 has already checked every boundary it will
//! ever cut.

use crate::qlite;
use crate::sxi;
use crate::die;
use std::fs::File;
use std::path::Path;
use std::sync::Arc;

/// The document-start member id (next free after m14/m20).
pub const M15: u32 = 15;
/// Header flag bit declaring m15 (after m14's bit 7 = 128 and m20's bit 8 = 256).
pub const FLAG: u64 = 512;
/// The container version that carries m15.
pub const VERSION: u32 = 10;
pub const ENCODING_NAME: &str = "delta-varint";
pub const SAMPLE_STRIDE: u64 = 64;
const MAGIC: &[u8; 8] = b"XDOCST01";
const HEADER: usize = 48;
/// seps.bin (XSEP v1): 48 B header, then u32 deltas, then every-64th anchors.
const XSEP_HEADER: u64 = 48;
/// meta.bin (XPM1 v1) header size, and the per-document row size.
const XPM1_HEADER: usize = 68;
const XPM1_ROW: usize = 34;

fn need(ok: bool, msg: &str) {
    if !ok {
        die(&format!("DOCSTARTS: {msg}"));
    }
}
fn u64_at(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}
fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}

// ------------------------------------------------------------- the structure

enum Backing {
    Map(Arc<memmap2::Mmap>),
    Buf(Arc<Vec<u8>>),
}

impl Backing {
    fn bytes(&self) -> &[u8] {
        match self {
            Backing::Map(m) => &m[..],
            Backing::Buf(v) => &v[..],
        }
    }
}

enum Kind {
    /// m15: LEB128 delta-varint gaps + sampled absolutes.
    Member {
        samples_off: usize,
        block_offs_off: usize,
        gaps_off: usize,
        gaps_bytes: u64,
    },
    /// seps.bin XSEP v1: fixed u32 deltas + every-`stride`-th u64 anchors.
    /// Used only as a --docs source / oracle; never written by this module.
    Xsep {
        deltas_off: usize,
        anchors_off: usize,
    },
}

/// The authoritative document-start structure, with `rank`/`select`.
pub struct DocStarts {
    backing: Backing,
    kind: Kind,
    pub docs: u64,
    pub text_len: u64,
    pub stride: u64,
    pub samples: u64,
    /// the map's own declared corpus size (the XSEP header's corpus_bytes; for
    /// a container member this is the container's text). A superset map (a map
    /// of a longer stream than the container's text) is legal for RESOLUTION:
    /// starts are absolute offsets from corpus byte 0.
    pub corpus_bytes: u64,
}

impl DocStarts {
    /// From a container member: `None` = the container carries no m15 (the
    /// feature is OFF; the caller's refusal then names the version). A present
    /// but invalid member is a hard error, never a silent None.
    pub fn from_container(reader: &qlite::Reader) -> Option<DocStarts> {
        let m = reader.members.iter().find(|m| m.id == M15)?;
        Some(Self::parse(
            Backing::Map(reader.map_arc()),
            m.offset as usize,
            m.bytes as u64,
            reader.version,
            reader.n,
        ))
    }

    /// From an XSEP v1 `seps.bin` (the `--docs DIR` sidecar). The map must
    /// cover exactly this container's text.
    pub fn from_xsep(dir: &Path, text_len: u64) -> DocStarts {
        let path = dir.join("seps.bin");
        let file = File::open(&path).unwrap_or_else(|e| {
            die(&format!("DOCSTARTS: open {}: {e}", path.display()))
        });
        let map = unsafe {
            memmap2::Mmap::map(&file)
                .unwrap_or_else(|e| die(&format!("DOCSTARTS: mmap {}: {e}", path.display())))
        };
        Self::parse_xsep(Backing::Map(Arc::new(map)), &path.to_string_lossy(), text_len)
    }

    fn parse(backing: Backing, base: usize, bytes: u64, container_version: u32, n: u64) -> DocStarts {
        let b = backing.bytes();
        let bad = |what: &str| -> ! {
            die(&format!(
                "DOCSTARTS: container member m15 (SXI2 v{container_version}) is invalid: {what}"
            ))
        };
        if bytes < HEADER as u64 || base + bytes as usize > b.len() {
            bad("member is shorter than its 48-byte header or out of bounds");
        }
        let h = &b[base..base + HEADER];
        if h[..8] != *MAGIC {
            bad("magic is not XDOCST01");
        }
        let member_version = u32_at(h, 8);
        if member_version != 1 {
            bad(&format!("unsupported member version {member_version}"));
        }
        let stride = u64::from(u32_at(h, 12));
        let docs = u64_at(h, 16);
        let text_len = u64_at(h, 24);
        let samples = u64_at(h, 32);
        let gaps_bytes = u64_at(h, 40);
        if stride == 0 || stride > 4096 {
            bad(&format!("sample_stride {stride} is not 1..=4096"));
        }
        if docs == 0 {
            bad("doc_count is 0");
        }
        if text_len + 1 != n {
            bad(&format!(
                "text_len {text_len} does not match the container's n {n} (want n-1)"
            ));
        }
        let want_samples = docs.div_ceil(stride);
        if samples != want_samples {
            bad(&format!(
                "sample_count {samples} is not ceil(doc_count {docs} / stride {stride}) = {want_samples}"
            ));
        }
        let samples_off = base + HEADER;
        let block_offs_off = samples_off + 8 * samples as usize;
        let gaps_off = block_offs_off + 4 * samples as usize;
        let want_bytes = HEADER as u64 + 12 * samples + gaps_bytes;
        if bytes != want_bytes {
            bad(&format!(
                "member is {bytes} bytes, layout says {want_bytes} (48 + 12*{samples} + {gaps_bytes})"
            ));
        }
        let ds = DocStarts {
            backing,
            kind: Kind::Member {
                samples_off,
                block_offs_off,
                gaps_off,
                gaps_bytes,
            },
            docs,
            text_len,
            stride,
            samples,
            corpus_bytes: text_len,
        };
        // FULL VALIDATION, ONCE: every gap decoded, every sample cross-checked.
        // ~210 M varints at full-Pile scale, well under a second per million.
        let mut start = 0u64;
        let mut pos = 0u64;
        let mut block = 0u64;
        for i in 0..docs {
            if i % stride == 0 {
                if i > 0 {
                    block += 1;
                    if pos != ds.block_off(block) {
                        bad(&format!(
                            "sample {} declares gap-stream offset {} but the stream is at {pos}",
                            block, ds.block_off(block)
                        ));
                    }
                } else if pos != 0 {
                    bad("sample 0 does not start its gap stream at offset 0");
                }
            }
            let gap = match ds.next_gap(&mut pos) {
                Ok(g) => g,
                Err(_) => bad("the varint gap stream is truncated or non-canonical"),
            };
            if i == 0 {
                if start + gap != 0 {
                    bad("the first document does not start at text offset 0");
                }
            } else if gap == 0 {
                bad(&format!("document {i} has the same start as its predecessor"));
            }
            start += gap;
            // the sample check runs AFTER this document's gap, when `start` IS
            // sample j's absolute (sample j is document j*stride, decoded last)
            if i % stride == 0 {
                let sample = ds.sample_start(i / stride);
                if start != sample {
                    bad(&format!(
                        "sample {} (document {}) says start {sample}, the varint stream says {start}",
                        i / stride, i
                    ));
                }
            }
            if start >= text_len {
                bad(&format!(
                    "document {i} starts at {start}, at or past the text length {text_len}"
                ));
            }
        }
        if pos != gaps_bytes {
            bad(&format!(
                "the varint stream is {pos} bytes, the header declares {gaps_bytes}"
            ));
        }
        ds
    }

    fn parse_xsep(backing: Backing, path: &str, text_len: u64) -> DocStarts {
        let b = backing.bytes();
        let bad = |what: &str| -> ! {
            die(&format!("DOCSTARTS: seps.bin {path}: {what}"))
        };
        if b.len() < XSEP_HEADER as usize || &b[..4] != b"XSEP" {
            bad("not an XSEP file (48-byte header, b\"XSEP\")");
        }
        if u32_at(b, 4) != 1 {
            bad(&format!("unsupported XSEP version {}", u32_at(b, 4)));
        }
        let stride = u64::from(u32_at(b, 12));
        if stride == 0 || stride > 4096 {
            bad(&format!("anchor stride {stride} is not 1..=4096"));
        }
        let docs = u64_at(b, 16);
        let corpus_bytes = u64_at(b, 24);
        let anchors = u64_at(b, 32);
        if docs == 0 || docs > u32::MAX as u64 {
            bad(&format!("doc_count {docs} is not 1..=u32::MAX"));
        }
        if corpus_bytes < text_len {
            die(&format!(
                "DOCSTARTS: seps.bin {path} covers corpus_bytes={corpus_bytes}, less than the \
                 container's text {text_len} bytes; a window set can only be resolved by a \
                 map that COVERS the text - regenerate the map over this exact text \
                 (`xsa corpus --pile-map --limit-bytes {text_len}`)"
            ));
        }
        // a SUPERSET map is accepted: starts are absolute offsets from corpus byte
        // 0 of the same stream, so a map of the whole pile resolves a container built
        // over any head of it (the prefix embed took exactly the documents that START
        // inside the text). Doc ids whose start is at or past the text are refused by
        // the callers, never silently resolved.
        if anchors != docs.div_ceil(stride)
            || b.len() as u64 != XSEP_HEADER + 4 * docs + 8 * anchors
        {
            bad("counts or file length do not match the XSEP v1 layout");
        }
        let deltas_off = XSEP_HEADER as usize;
        let anchors_off = deltas_off + 4 * docs as usize;
        let ds = DocStarts {
            kind: Kind::Xsep {
                deltas_off,
                anchors_off,
            },
            docs,
            text_len,
            stride,
            samples: anchors,
            corpus_bytes,
            backing,
        };
        // Full validation, same teeth as the member: every delta decoded once.
        // The XSEP `base` field (offset 40) is the origin the first delta is
        // measured from; starts must be absolute from corpus byte 0.
        let b = ds.backing.bytes();
        let mut start = u64_at(b, 40);
        if start != 0 {
            bad("the map's base/first document does not start at corpus offset 0");
        }
        for i in 0..docs {
            let delta = u32_at(b, deltas_off + 4 * i as usize) as u64;
            if i > 0 && delta == 0 {
                bad(&format!("document {i} has the same start as its predecessor"));
            }
            start += delta;
            if start >= text_len {
                bad(&format!(
                    "document {i} starts at {start}, at or past the corpus length {text_len}"
                ));
            }
            if i % stride == 0 && start != ds.sample_start(i / stride) {
                bad(&format!(
                    "anchor {} says {}, the delta stream says {}",
                    i / stride,
                    ds.sample_start(i / stride),
                    start
                ));
            }
        }
        // The last document may be a head-cut tail (its text runs to the very
        // end of the corpus): only monotonicity and start < corpus length are
        // enforced, which the loop above did.
        ds
    }

    // ---- the two-level accessors

    /// Absolute start of sample `j` (document `j*stride`).
    fn sample_start(&self, j: u64) -> u64 {
        need(j < self.samples, "sample index out of range");
        match &self.kind {
            Kind::Member { samples_off, .. } => {
                u64_at(self.backing.bytes(), samples_off + 8 * j as usize)
            }
            Kind::Xsep { anchors_off, .. } => {
                u64_at(self.backing.bytes(), anchors_off + 8 * j as usize)
            }
        }
    }

    /// Byte offset (RELATIVE to the gap/delta stream's first byte) of the first
    /// gap of sample `j`'s block. `next_gap` adds the stream's base to `pos`,
    /// so this MUST be relative too — an absolute value here would be added
    /// twice and read past the member.
    fn block_off(&self, j: u64) -> u64 {
        match &self.kind {
            Kind::Member { block_offs_off, .. } => {
                u64::from(u32_at(self.backing.bytes(), block_offs_off + 4 * j as usize))
            }
            Kind::Xsep { .. } => 4 * j * self.stride,
        }
    }

    /// One gap at stream byte offset `pos`; advances `pos`. Fail-loud: a
    /// truncated or non-canonical varint is an error, never a guess.
    fn next_gap(&self, pos: &mut u64) -> Result<u64, ()> {
        let b = self.backing.bytes();
        match &self.kind {
            Kind::Member { gaps_off, gaps_bytes, .. } => {
                let limit = *gaps_off as u64 + gaps_bytes;
                let mut value = 0u64;
                let mut shift = 0u32;
                loop {
                    let at = *gaps_off as u64 + *pos;
                    if at >= limit {
                        return Err(());
                    }
                    let byte = b[at as usize];
                    let payload = (byte & 127) as u64;
                    if shift >= 64 || (shift >= 57 && payload > 1) {
                        return Err(());
                    }
                    value |= payload << shift;
                    *pos += 1;
                    if byte & 128 == 0 {
                        // canonical: the last byte must be nonzero unless it is the only one
                        if shift > 0 && payload == 0 {
                            return Err(());
                        }
                        return Ok(value);
                    }
                    shift += 7;
                }
            }
            Kind::Xsep { deltas_off, .. } => {
                let at = *deltas_off as u64 + *pos;
                *pos += 4;
                Ok(u64::from(u32_at(b, at as usize)))
            }
        }
    }

    /// The document containing text position `p`, and that document's start.
    /// `rank(p) -> docid`, `select(rank(p)) -> base`, `p - base -> depth`.
    pub fn rank_start(&self, p: u64) -> (u64, u64) {
        need(p < self.text_len, &format!("rank({p}) is at or past the text length"));
        // largest sample whose start is <= p
        let (mut lo, mut hi) = (0u64, self.samples);
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if self.sample_start(mid) <= p {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        let j = lo - 1;
        let mut doc = j * self.stride;
        let mut start = self.sample_start(j);
        let mut pos = self.block_off(j);
        // block j's stream begins with G[j*stride] (the gap that REBUILDS doc
        // j*stride from its predecessor); the sample already IS that start, so
        // the walk skips it and applies the gaps that walk forward from there.
        let _ = self.next_gap(&mut pos);
        while doc + 1 < self.docs {
            let gap = self.next_gap(&mut pos).unwrap_or_else(|_| {
                die("DOCSTARTS: the gap stream ended inside a rank scan (validated member?)")
            });
            let next = start.checked_add(gap).unwrap_or_else(|| {
                die("DOCSTARTS: document start overflow during a rank scan")
            });
            if next > p {
                break;
            }
            start = next;
            doc += 1;
        }
        (doc, start)
    }

    /// `select(i) -> start of document i`.
    pub fn select(&self, i: u64) -> u64 {
        need(i < self.docs, &format!("select({i}) is at or past doc_count {}", self.docs));
        let j = i / self.stride;
        let mut start = self.sample_start(j);
        let mut pos = self.block_off(j);
        // skip G[j*stride] (see rank_start): the sample is doc j*stride's start
        // and the walk applies G[j*stride+1 ..= i]
        let _ = self.next_gap(&mut pos);
        for _ in 0..(i % self.stride) {
            let gap = self.next_gap(&mut pos).unwrap_or_else(|_| {
                die("DOCSTARTS: the gap stream ended inside a select scan (validated member?)")
            });
            start = start
                .checked_add(gap)
                .unwrap_or_else(|| die("DOCSTARTS: document start overflow during a select scan"));
        }
        start
    }

    pub fn rank(&self, p: u64) -> u64 {
        self.rank_start(p).0
    }
}

// ------------------------------------------------------------------- encode

pub struct Encoded {
    pub bytes: Vec<u8>,
    pub docs: u64,
    pub stride: u64,
    pub gaps_bytes: u64,
    /// 8 * doc_count: the plain-u64-array size, reported next to ours.
    pub plain_u64_bytes: u64,
}

/// Encode a strictly increasing, 0-starting stream of document starts.
/// `expected_count` (Some) must match, or the source lied about its length.
pub fn encode(starts: &mut dyn Iterator<Item = u64>, text_len: u64, stride: u64, expected_count: Option<u64>) -> Encoded {
    need(stride >= 1, "--stride must be >= 1");
    let mut gaps: Vec<u8> = Vec::new();
    let mut samples: Vec<u64> = Vec::new();
    let mut block_offs: Vec<u32> = Vec::new();
    let mut prev = None::<u64>;
    let mut docs = 0u64;
    for s in starts {
        match prev {
            None => {
                if s != 0 {
                    die(&format!(
                        "DOCSTARTS: the first document starts at {s}, not 0; the member must \
                         cover the text from its first byte"
                    ));
                }
                block_offs.push(0);
            }
            Some(p) => {
                if s <= p {
                    die(&format!(
                        "DOCSTARTS: document starts are not strictly increasing at document {docs} \
                         ({p} -> {s})"
                    ));
                }
                if s >= text_len {
                    die(&format!(
                        "DOCSTARTS: document {docs} starts at {s}, at or past the text length \
                         {text_len}"
                    ));
                }
            }
        }
        if docs % stride == 0 {
            samples.push(s);
            if docs > 0 {
                block_offs.push(gaps.len() as u32);
            }
        }
        let gap = s - prev.unwrap_or(0);
        let mut v = gap;
        loop {
            let mut byte = (v & 127) as u8;
            v >>= 7;
            if v != 0 {
                byte |= 128;
            }
            gaps.push(byte);
            if v == 0 {
                break;
            }
        }
        prev = Some(s);
        docs += 1;
        if block_offs.len() > docs.div_ceil(stride) as usize {
            die("DOCSTARTS: sample bookkeeping overflow");
        }
    }
    if docs == 0 {
        die("DOCSTARTS: the source has no documents");
    }
    if let Some(want) = expected_count {
        if docs != want {
            die(&format!(
                "DOCSTARTS: the source declared {want} documents but yielded {docs}"
            ));
        }
    }
    // the last document must start inside the text (it may run to its very end)
    if let Some(p) = prev {
        if p >= text_len {
            die(&format!("DOCSTARTS: the last document starts at {p}, at or past the text"));
        }
    }
    let samples_count = samples.len() as u64;
    let gaps_bytes = gaps.len() as u64;
    let mut bytes = Vec::with_capacity(HEADER + gaps.len() + 12 * samples.len());
    bytes.extend_from_slice(MAGIC);
    bytes.extend_from_slice(&1u32.to_le_bytes());
    bytes.extend_from_slice(&(stride as u32).to_le_bytes());
    bytes.extend_from_slice(&docs.to_le_bytes());
    bytes.extend_from_slice(&text_len.to_le_bytes());
    bytes.extend_from_slice(&samples_count.to_le_bytes());
    bytes.extend_from_slice(&gaps_bytes.to_le_bytes());
    for s in &samples {
        bytes.extend_from_slice(&s.to_le_bytes());
    }
    for o in &block_offs {
        bytes.extend_from_slice(&o.to_le_bytes());
    }
    bytes.extend_from_slice(&gaps);
    let want = HEADER as u64 + 12 * samples_count + gaps_bytes;
    need(bytes.len() as u64 == want, "encoded member size mismatch");
    Encoded {
        bytes,
        docs,
        stride,
        gaps_bytes,
        plain_u64_bytes: 8 * docs,
    }
}

// ------------------------------------------------------- the starts sources

/// Where embeddable document starts come from.
pub enum Source {
    /// `--docs-map DIR`: an XSEP v1 seps.bin (validated while streaming).
    DocsMap(String),
    /// `--doc-starts FILE`: a raw little-endian u64 array.
    DocStartsFile(String),
}

/// A streaming, fully validated starts source. Prefix semantics: with
/// `limit = Some(text_len)` the stream stops at the first start >= text_len
/// (embedding a 100 GB map into a 10 GB container embeds exactly the documents
/// that begin inside the text).
pub struct StartsIter {
    kind: SourceKind,
    /// None = unlimited (the source is the whole text).
    limit: Option<u64>,
    state: SourceState,
    docs_declared: Option<u64>,
}

enum SourceKind {
    Xsep(Arc<memmap2::Mmap>),
    Raw(Arc<memmap2::Mmap>),
}

enum SourceState {
    NotStarted,
    Xsep { start: u64, i: u64, stride: u64, deltas_off: usize, anchors_off: usize },
    Raw { i: u64 },
    Done,
}

impl StartsIter {
    pub fn open(source: &Source, limit: Option<u64>) -> StartsIter {
        match source {
            Source::DocsMap(dir) => {
                let path = Path::new(dir).join("seps.bin");
                let file = File::open(&path)
                    .unwrap_or_else(|e| die(&format!("DOCSTARTS: open {}: {e}", path.display())));
                let map = Arc::new(unsafe {
                    memmap2::Mmap::map(&file)
                        .unwrap_or_else(|e| die(&format!("DOCSTARTS: mmap {}: {e}", path.display())))
                });
                let b = &map[..];
                let bad = |what: &str| -> ! {
                    die(&format!("DOCSTARTS: seps.bin {}: {what}", path.display()))
                };
                if b.len() < XSEP_HEADER as usize || &b[..4] != b"XSEP" {
                    bad("not an XSEP file");
                }
                if u32_at(b, 4) != 1 {
                    bad("unsupported XSEP version");
                }
                let stride = u64::from(u32_at(b, 12));
                let docs = u64_at(b, 16);
                let corpus_bytes = u64_at(b, 24);
                let anchors = u64_at(b, 32);
                if stride == 0 || docs == 0 || docs > u32::MAX as u64 {
                    bad("stride/doc_count out of range");
                }
                if anchors != docs.div_ceil(stride)
                    || b.len() as u64 != XSEP_HEADER + 4 * docs + 8 * anchors
                {
                    bad("counts or file length do not match the XSEP v1 layout");
                }
                if u64_at(b, 40) != 0 {
                    bad("the XSEP base field is not 0 (offsets must be absolute)");
                }
                match limit {
                    Some(text_len) if corpus_bytes < text_len => die(&format!(
                        "DOCSTARTS: seps.bin {} covers corpus_bytes={corpus_bytes}, less than the \
                         container's text {text_len}; embed needs a map covering the whole text",
                        path.display()
                    )),
                    _ => {}
                }
                StartsIter {
                    kind: SourceKind::Xsep(map),
                    limit,
                    state: SourceState::Xsep {
                        start: 0,
                        i: 0,
                        stride,
                        deltas_off: XSEP_HEADER as usize,
                        anchors_off: (XSEP_HEADER + 4 * docs) as usize,
                    },
                    docs_declared: Some(docs),
                }
            }
            Source::DocStartsFile(path) => {
                let file = File::open(path)
                    .unwrap_or_else(|e| die(&format!("DOCSTARTS: open {path}: {e}")));
                let map = Arc::new(unsafe {
                    memmap2::Mmap::map(&file)
                        .unwrap_or_else(|e| die(&format!("DOCSTARTS: mmap {path}: {e}")))
                });
                if map.len() % 8 != 0 {
                    die(&format!(
                        "DOCSTARTS: --doc-starts {path} is {} bytes, not a whole u64 array",
                        map.len()
                    ));
                }
                StartsIter {
                    kind: SourceKind::Raw(map),
                    limit,
                    state: SourceState::Raw { i: 0 },
                    docs_declared: None,
                }
            }
        }
    }
}

impl Iterator for StartsIter {
    type Item = u64;
    fn next(&mut self) -> Option<u64> {
        match &mut self.state {
            SourceState::Done => None,
            SourceState::Raw { i } => {
                let map = match &self.kind {
                    SourceKind::Raw(m) => m,
                    _ => die("DOCSTARTS: raw source state mismatch"),
                };
                let at = *i;
                if at >= (map.len() / 8) as u64 {
                    self.state = SourceState::Done;
                    return None;
                }
                *i += 1;
                let start = u64_at(map, 8 * at as usize);
                if let Some(limit) = self.limit {
                    if start >= limit {
                        self.state = SourceState::Done;
                        return None;
                    }
                }
                Some(start)
            }
            SourceState::Xsep { start, i, stride, deltas_off, anchors_off } => {
                let map = match &self.kind {
                    SourceKind::Xsep(m) => m,
                    _ => die("DOCSTARTS: xsep source state mismatch"),
                };
                let docs = self.docs_declared.unwrap_or(0);
                let at = *i;
                if at >= docs {
                    self.state = SourceState::Done;
                    return None;
                }
                let delta = u32_at(map, *deltas_off + 4 * at as usize) as u64;
                if at > 0 && delta == 0 {
                    die(&format!(
                        "DOCSTARTS: seps.bin document {at} has the same start as its predecessor"
                    ));
                }
                *start += delta;
                *i += 1;
                if at % *stride == 0 && *start != u64_at(map, *anchors_off + 8 * (at / *stride) as usize) {
                    die(&format!(
                        "DOCSTARTS: seps.bin anchor {} disagrees with the delta stream at document {at}",
                        at / *stride
                    ));
                }
                if let Some(limit) = self.limit {
                    if *start >= limit {
                        // a PREFIX embed stops at the text end; the document
                        // that would straddle it is not a document of this text
                        self.state = SourceState::Done;
                        return None;
                    }
                }
                Some(*start)
            }
            SourceState::NotStarted => die("DOCSTARTS: source not initialised"),
        }
    }
}

// -------------------------------------------------------------------- embed

pub struct EmbedStats {
    pub docs: u64,
    pub member_bytes: u64,
    pub gaps_bytes: u64,
    pub plain_u64_bytes: u64,
    pub stride: u64,
    pub text_len: u64,
    pub corpus_bytes: u64,
    pub in_place: bool,
    pub seconds: f64,
}

/// Append m15 to a container. Versions 7..=9 (trailing directory) append IN
/// PLACE (no container byte is copied); version 6 (head directory) REQUIRES
/// `out` and streams a copy to it in the v7+ layout. Both end as SXI2 v10 with
/// flag bit 9 set. Version 10+ with m15 already present is refused.
pub fn embed(
    path: &str,
    source: &Source,
    stride: u64,
    out: Option<&str>,
) -> EmbedStats {
    let t0 = std::time::Instant::now();
    let reader = qlite::Reader::open(path).unwrap_or_else(|e| die(&format!("DOCSTARTS: {e}")));
    if reader.members.iter().any(|m| m.id == M15) {
        die(&format!(
            "DOCSTARTS: {path} is SXI2 v{} and already carries member m15 (document starts); \
             refusing to append a second one",
            reader.version
        ));
    }
    let text_len = reader.n - 1;
    let mut starts = StartsIter::open(source, Some(text_len));
    let expected_docs = starts.docs_declared;
    let encoded = encode(&mut starts, text_len, stride, expected_docs);
    // The map's declared corpus size, for the journal's coverage line.
    let corpus_bytes = match source {
        Source::DocsMap(dir) => {
            let p = Path::new(dir).join("seps.bin");
            let b = std::fs::read(p).unwrap_or_else(|e| die(&format!("DOCSTARTS: {e}")));
            u64_at(&b, 24)
        }
        // a raw starts file has no separate corpus notion: the starts are
        // this text's, by construction of the caller's file
        Source::DocStartsFile(_) => text_len,
    };
    let s = if reader.version >= 7 {
        need(
            out.is_none(),
            "--out is only needed for a v6 container (head directory); this one has a trailing \
             directory and is appended in place",
        );
        append_in_place(&reader, &encoded, path);
        EmbedStats {
            docs: encoded.docs,
            member_bytes: encoded.bytes.len() as u64,
            gaps_bytes: encoded.gaps_bytes,
            plain_u64_bytes: encoded.plain_u64_bytes,
            stride: encoded.stride,
            text_len,
            corpus_bytes,
            in_place: true,
            seconds: t0.elapsed().as_secs_f64(),
        }
    } else if reader.version == 6 {
        let out = out.unwrap_or_else(|| {
            die(&format!(
                "DOCSTARTS: {path} is SXI2 v6 (head member directory); appending in place would \
                 rewrite the whole file - pass --out NEW.sxi2 for the copy route"
            ))
        });
        append_copy(&reader, &encoded, out);
        EmbedStats {
            docs: encoded.docs,
            member_bytes: encoded.bytes.len() as u64,
            gaps_bytes: encoded.gaps_bytes,
            plain_u64_bytes: encoded.plain_u64_bytes,
            stride: encoded.stride,
            text_len,
            corpus_bytes,
            in_place: false,
            seconds: t0.elapsed().as_secs_f64(),
        }
    } else {
        die(&format!(
            "DOCSTARTS: {path} is SXI2 v{}, not a lite container (want 6..=9)",
            reader.version
        ))
    };
    // THE MEMBER MUST BE READABLE FROM THE PUBLISHED CONTAINER: reopen, load
    // through the full validator, and sample it. A payload that lands wrong is
    // a wrong product, not a wash.
    let published = if s.in_place { path.to_string() } else { out.unwrap().to_string() };
    let check = qlite::Reader::open(&published).unwrap_or_else(|e| die(&format!("DOCSTARTS: reopen {published}: {e}")));
    if check.version != VERSION || check.flags & FLAG == 0 {
        die(&format!(
            "DOCSTARTS: {published} reopens as SXI2 v{} flags={:#x}, want v{VERSION} with bit {FLAG} set",
            check.version, check.flags
        ));
    }
    let ds = DocStarts::from_container(&check)
        .unwrap_or_else(|| die("DOCSTARTS: the published container has no m15 after the append"));
    need(ds.docs == s.docs, "published member doc_count disagrees with the encode");
    // one round trip through rank/select at the text's ends
    if s.docs > 0 {
        let mid = ds.text_len / 2;
        let (doc, start) = ds.rank_start(mid);
        need(ds.select(doc) == start, "published member select(rank) disagrees at the midpoint");
        need(ds.select(0) == 0, "published member select(0) is not 0");
    }
    s
}

/// In-place append for the trailing-directory layout (v7+), after the m20
/// appender's own pattern: truncate to the members' end, write m15, rewrite
/// the directory with one more entry, patch the header (version 10, flag bit).
fn append_in_place(reader: &qlite::Reader, encoded: &Encoded, path: &str) {
    use std::os::unix::fs::FileExt;
    let count = reader.members.len();
    let length = std::fs::metadata(path).unwrap().len();
    let dir_start = length - 40 * count as u64;
    let mut last = 0u64;
    for m in &reader.members {
        last = last.max(m.offset + m.bytes);
    }
    let at = (last + 7) / 8 * 8;
    need(
        dir_start >= at,
        &format!("DOCSTARTS: {path} has no room for m15 (directory at {dir_start}, members end at {at})"),
    );
    let mut entry = [0u8; 40];
    entry[0..4].copy_from_slice(&M15.to_le_bytes());
    entry[4..8].copy_from_slice(&M15.to_le_bytes()); // codec == id, like m14/m20
    entry[8..16].copy_from_slice(&at.to_le_bytes());
    entry[16..24].copy_from_slice(&(encoded.bytes.len() as u64).to_le_bytes());
    entry[24..32].copy_from_slice(&encoded.docs.to_le_bytes());
    entry[32..36].copy_from_slice(&sxi::crc32_of(&encoded.bytes).to_le_bytes());
    let mut entries: Vec<[u8; 40]> = reader.map()[dir_start as usize..]
        .chunks_exact(40)
        .map(|c| c.try_into().unwrap())
        .collect();
    entries.push(entry);
    entries.sort_unstable_by_key(|e| u32::from_le_bytes(e[0..4].try_into().unwrap()));
    let mut dir = Vec::with_capacity(40 * entries.len());
    for e in &entries {
        dir.extend_from_slice(e);
    }
    let f = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(path)
        .unwrap_or_else(|e| die(&format!("DOCSTARTS: open {path} for the append: {e}")));
    f.set_len(at).unwrap_or_else(|e| die(&format!("DOCSTARTS: truncate {path}: {e}")));
    f.write_all_at(&encoded.bytes, at)
        .unwrap_or_else(|e| die(&format!("DOCSTARTS: write m15: {e}")));
    let new_len = at + encoded.bytes.len() as u64 + dir.len() as u64;
    f.write_all_at(&dir, at + encoded.bytes.len() as u64)
        .unwrap_or_else(|e| die(&format!("DOCSTARTS: write directory: {e}")));
    let mut h = [0u8; 64];
    h.copy_from_slice(&reader.map()[..64]);
    h[4..8].copy_from_slice(&VERSION.to_le_bytes());
    h[32..36].copy_from_slice(&(entries.len() as u32).to_le_bytes());
    h[36..40].copy_from_slice(&((64 + 40 * entries.len()) as u32).to_le_bytes());
    h[40..48].copy_from_slice(&new_len.to_le_bytes());
    let flags = u64::from_le_bytes(h[48..56].try_into().unwrap()) | FLAG;
    h[48..56].copy_from_slice(&flags.to_le_bytes());
    h[56..60].fill(0);
    let mut hc = h.to_vec();
    hc.extend_from_slice(&dir);
    let crc = sxi::crc32_of(&hc);
    h[56..60].copy_from_slice(&crc.to_le_bytes());
    f.write_all_at(&h, 0).unwrap_or_else(|e| die(&format!("DOCSTARTS: patch header: {e}")));
    f.sync_all().unwrap_or_else(|e| die(&format!("DOCSTARTS: fsync {path}: {e}")));
}

/// The copy route for a v6 container (head directory): stream every member to a
/// NEW file in the trailing-directory layout, then m15, then the directory.
/// No byte of the source is modified.
fn append_copy(reader: &qlite::Reader, encoded: &Encoded, out: &str) {
    use std::io::{BufWriter, Write};
    use std::os::unix::fs::FileExt;
    need(
        std::fs::symlink_metadata(out).is_err(),
        &format!("DOCSTARTS: output {out} exists; refusing to overwrite"),
    );
    let mut spans: Vec<(u64, u64, sxi::Member)> =
        reader.members.iter().map(|m| (m.offset, m.bytes, m.clone())).collect();
    spans.sort_unstable_by_key(|(o, _, _)| *o);
    let mut crcb = vec![0u8; 1 << 20];
    let tmp = format!("{out}.partial");
    let wfile = std::fs::OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&tmp)
        .unwrap_or_else(|e| die(&format!("DOCSTARTS: create {tmp}: {e}")));
    let mut w = BufWriter::with_capacity(1 << 20, wfile);
    let mut members: Vec<(u32, u32, u64, u64, u64, u32)> = Vec::new(); // id, codec, off, bytes, count, crc
    let mut wpos = 64u64;
    let mut off = 64u64;
    for (soff, bytes, m) in &spans {
        off = (off + 7) / 8 * 8;
        if off != wpos {
            w.write_all(&vec![0u8; (off - wpos) as usize])
                .unwrap_or_else(|e| die(&format!("DOCSTARTS: pad: {e}")));
            wpos = off;
        }
        let mut left = *bytes as usize;
        let mut at = *soff as usize;
        let mut v = !0u32;
        while left > 0 {
            let z = left.min(crcb.len());
            crcb[..z].copy_from_slice(&reader.map()[at..at + z]);
            v = sxi::crc_step(v, &crcb[..z]);
            w.write_all(&crcb[..z]).unwrap_or_else(|e| die(&format!("DOCSTARTS: copy member m{}: {e}", m.id)));
            at += z;
            left -= z;
            wpos += z as u64;
        }
        // member bytes are copied verbatim: their CRCs are re-derived AND
        // compared, so a copy error can never publish.
        if !v != m.checksum {
            die(&format!(
                "DOCSTARTS: member m{} CRC changed during the copy (source checksum corruption?)",
                m.id
            ));
        }
        members.push((m.id, m.codec, off, *bytes, m.count, m.checksum));
        off += bytes;
    }
    off = (off + 7) / 8 * 8;
    if off != wpos {
        w.write_all(&vec![0u8; (off - wpos) as usize])
            .unwrap_or_else(|e| die(&format!("DOCSTARTS: pad: {e}")));
    }
    w.write_all(&encoded.bytes).unwrap_or_else(|e| die(&format!("DOCSTARTS: write m15: {e}")));
    members.push((M15, M15, off, encoded.bytes.len() as u64, encoded.docs, sxi::crc32_of(&encoded.bytes)));
    members.sort_unstable_by_key(|m| m.0);
    let mut dir = Vec::with_capacity(40 * members.len());
    for &(id, codec, moff, bytes, count, crc) in &members {
        dir.extend_from_slice(&id.to_le_bytes());
        dir.extend_from_slice(&codec.to_le_bytes());
        dir.extend_from_slice(&moff.to_le_bytes());
        dir.extend_from_slice(&bytes.to_le_bytes());
        dir.extend_from_slice(&count.to_le_bytes());
        dir.extend_from_slice(&crc.to_le_bytes());
    }
    w.write_all(&dir).unwrap_or_else(|e| die(&format!("DOCSTARTS: write directory: {e}")));
    w.flush().unwrap_or_else(|e| die(&format!("DOCSTARTS: flush: {e}")));
    let new_len = off + encoded.bytes.len() as u64 + dir.len() as u64;
    // header: copy the source's 64 bytes, patch version/count/hs/length/flags
    let mut h = [0u8; 64];
    h.copy_from_slice(&reader.map()[..64]);
    h[4..8].copy_from_slice(&VERSION.to_le_bytes());
    h[32..36].copy_from_slice(&(members.len() as u32).to_le_bytes());
    h[36..40].copy_from_slice(&((64 + 40 * members.len()) as u32).to_le_bytes());
    h[40..48].copy_from_slice(&new_len.to_le_bytes());
    let flags = u64::from_le_bytes(h[48..56].try_into().unwrap()) | FLAG;
    h[48..56].copy_from_slice(&flags.to_le_bytes());
    h[56..60].fill(0);
    let mut hc = h.to_vec();
    hc.extend_from_slice(&dir);
    let crc = sxi::crc32_of(&hc);
    h[56..60].copy_from_slice(&crc.to_le_bytes());
    w.get_ref().write_all_at(&h, 0).unwrap_or_else(|e| die(&format!("DOCSTARTS: patch header: {e}")));
    w.get_ref().sync_all().unwrap_or_else(|e| die(&format!("DOCSTARTS: fsync {tmp}: {e}")));
    drop(w);
    std::fs::rename(&tmp, out).unwrap_or_else(|e| die(&format!("DOCSTARTS: rename {tmp} -> {out}: {e}")));
}

/// One journal line, exactly the shape the consumer's D2.5 asked for.
pub fn journal_line(s: &EmbedStats) -> String {
    format!(
        "DOCSTARTS docs={} bytes={} ratio_vs_n={:.9} encoding={} sample_stride={} \
         gaps_bytes={} plain_u64_bytes={} gaps_vs_plain={:.4} text_len={} map_corpus_bytes={} \
         in_place={} append_s={:.1}",
        s.docs,
        s.member_bytes,
        s.member_bytes as f64 / s.text_len.max(1) as f64,
        ENCODING_NAME,
        s.stride,
        s.gaps_bytes,
        s.plain_u64_bytes,
        s.gaps_bytes as f64 / s.plain_u64_bytes.max(1) as f64,
        s.text_len,
        s.corpus_bytes,
        u8::from(s.in_place),
        s.seconds,
    )
}

// ------------------------------------------------------------------ verify

fn splitmix64(state: &mut u64) -> u64 {
    *state = state.wrapping_add(0x9E37_79B9_7F4A_7C15);
    let mut z = *state;
    z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    z ^ (z >> 31)
}

/// The G1 instrument: rank/select parity of the container member against the
/// map's seps.bin, with the internal-0x1E documents deliberately included.
///
/// The seps.bin oracle here is a THIRD, independent decoder of the XSEP format
/// (anchor bisect + delta scan, written separately from `DocStarts`'s
/// `Kind::Xsep` path and from the container member's varint path); the gate
/// additionally cross-checks both against `gates/docstarts_oracle.c`.
struct XsepOracle {
    map: Arc<memmap2::Mmap>,
    docs: u64,
    stride: u64,
    deltas_off: usize,
    anchors_off: usize,
    corpus_bytes: u64,
}

impl XsepOracle {
    fn open(dir: &str) -> XsepOracle {
        let path = Path::new(dir).join("seps.bin");
        let file = File::open(&path).unwrap_or_else(|e| die(&format!("DOCSTARTS: open {}: {e}", path.display())));
        let map = Arc::new(unsafe {
            memmap2::Mmap::map(&file)
                .unwrap_or_else(|e| die(&format!("DOCSTARTS: mmap {}: {e}", path.display())))
        });
        let b = &map[..];
        need(b.len() >= 48 && &b[..4] == b"XSEP" && u32_at(b, 4) == 1, "oracle: not an XSEP v1 file");
        let docs = u64_at(b, 16);
        let stride = u64::from(u32_at(b, 12));
        let anchors = u64_at(b, 32);
        need(anchors == docs.div_ceil(stride), "oracle: anchor count");
        XsepOracle {
            deltas_off: 48,
            anchors_off: 48 + 4 * docs as usize,
            docs,
            stride,
            corpus_bytes: u64_at(b, 24),
            map,
        }
    }
    fn start_of(&self, i: u64) -> u64 {
        need(i < self.docs, "oracle: doc id out of range");
        let j = i / self.stride;
        let mut start = u64_at(&self.map, self.anchors_off + 8 * j as usize);
        let mut d = j * self.stride;
        while d < i {
            start += u32_at(&self.map, self.deltas_off + 4 * d as usize) as u64;
            d += 1;
        }
        start
    }
    /// (docid, start) containing p, by anchor bisect + forward delta scan.
    fn locate(&self, p: u64) -> (u64, u64) {
        need(p < self.corpus_bytes, "oracle: position out of range");
        let blocks = self.docs.div_ceil(self.stride);
        let (mut lo, mut hi) = (0u64, blocks);
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if u64_at(&self.map, self.anchors_off + 8 * mid as usize) <= p {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        let block = lo - 1;
        let mut doc = block * self.stride;
        let mut start = u64_at(&self.map, self.anchors_off + 8 * block as usize);
        while doc + 1 < self.docs {
            let next = start + u32_at(&self.map, self.deltas_off + 4 * (doc + 1) as usize) as u64;
            if next > p {
                break;
            }
            start = next;
            doc += 1;
        }
        (doc, start)
    }
}

fn verify_command(args: &[String]) {
    let mut container: Option<String> = None;
    let mut map: Option<String> = None;
    let mut samples: u64 = 10_000;
    let mut range_start: u64 = 0;
    let mut range_end: Option<u64> = None;
    let mut seed: u64 = 0x5DEECE66D;
    let mut i = 0usize;
    while i < args.len() {
        macro_rules! val {
            ($name:expr) => {{
                i += 1;
                match args.get(i) {
                    Some(v) => v.clone(),
                    None => die(&format!("DOCSTARTS: {} needs a value", $name)),
                }
            }};
        }
        match args[i].as_str() {
            "--docs-map" => map = Some(val!("--docs-map")),
            "--samples" => {
                let v = val!("--samples");
                samples = v.parse().unwrap_or_else(|_| die("DOCSTARTS: --samples needs a count"));
            }
            "--range-start" => {
                let v = val!("--range-start");
                range_start = v.parse().unwrap_or_else(|_| die("DOCSTARTS: --range-start needs an offset"));
            }
            "--range-end" => {
                let v = val!("--range-end");
                range_end = Some(v.parse().unwrap_or_else(|_| die("DOCSTARTS: --range-end needs an offset")));
            }
            "--seed" => {
                let v = val!("--seed");
                seed = v.parse().unwrap_or_else(|_| die("DOCSTARTS: --seed needs a number"));
            }
            other => {
                if other.starts_with('-') {
                    die(&format!("DOCSTARTS verify: unknown option '{other}'"));
                }
                if container.replace(other.to_string()).is_some() {
                    die("DOCSTARTS verify: one container only");
                }
            }
        }
        i += 1;
    }
    let container = container.unwrap_or_else(|| die("DOCSTARTS verify: need a container"));
    let map = map.unwrap_or_else(|| die("DOCSTARTS verify: need --docs-map DIR (the seps.bin oracle)"));
    let reader = qlite::Reader::open(&container).unwrap_or_else(|e| die(&format!("DOCSTARTS: {e}")));
    let ds = DocStarts::from_container(&reader).unwrap_or_else(|| {
        die(&format!(
            "DOCSTARTS: {container} is SXI2 v{} and carries no member m15 (document starts); \
             this is the feature-OFF arm - build with `xsa build --docs-map DIR` or embed with \
             `xsa docstarts embed`",
            reader.version
        ))
    });
    let oracle = XsepOracle::open(&map);
    // An explicit --range-end is honoured up to the text end; the DEFAULT range
    // is a 10 GB window from --range-start (the gate driver can then sweep the
    // whole text with several invocations, or cover it in one with --range-end).
    let end = match range_end {
        Some(e) => e.min(ds.text_len),
        None => ds.text_len.min(range_start + 10_000_000_000),
    };
    if samples == 0 || range_start >= end {
        die(&format!(
            "DOCSTARTS verify: empty range [{range_start}, {end}) or --samples 0"
        ));
    }
    // ---- random positions
    let mut state = seed;
    let mut disagreements = 0u64;
    let mut positions: Vec<u64> = Vec::with_capacity(samples as usize);
    for _ in 0..samples {
        positions.push(range_start + splitmix64(&mut state) % (end - range_start));
    }
    // ---- positions inside documents that contain an internal 0x1E (meta.bin)
    let mut internal_positions: Vec<u64> = Vec::new();
    let meta = Path::new(&map).join("meta.bin");
    if let Ok(data) = std::fs::read(&meta) {
        need(data.len() >= XPM1_HEADER, "meta.bin is shorter than its 68-byte header");
        need(&data[..4] == b"XPM1", "meta.bin is not an XPM1 file");
        let docs = u64_at(&data, 8);
        let corpus_bytes = u64_at(&data, 16);
        let names = u64_at(&data, 32);
        let name_payload = u64_at(&data, 40);
        let shard_payload = u64_at(&data, 48);
        need(corpus_bytes == oracle.corpus_bytes, "meta.bin corpus_bytes disagrees with seps.bin");
        let rows_off = XPM1_HEADER
            + name_payload as usize
            + 6 * names as usize
            + shard_payload as usize;
        need(u32_at(&data, 56) as usize == XPM1_ROW, "meta.bin row size is not 34");
        need(rows_off + XPM1_ROW * docs as usize <= data.len(), "meta.bin rows do not fit");
        for d in 0..docs as usize {
            let row = rows_off + XPM1_ROW * d;
            let off = u64_at(&data, row);
            let len = u64_at(&data, row + 8);
            let internal = u32_at(&data, row + 32);
            if internal == 0 || off >= end {
                continue;
            }
            // a position INSIDE the document, deliberately ambiguous to a
            // 0x1E scanner: the document's own bytes contain the separator
            let p = off + (len / 2).min(len.saturating_sub(1)).max(1);
            if p < end && p < ds.text_len {
                internal_positions.push(p);
            }
        }
    }
    let internal_used = internal_positions.len() as u64;
    positions.extend(internal_positions.iter().copied());
    // ---- rank/depth parity at every position, both decoders
    let mut rank_checks = 0u64;
    for &p in &positions {
        let (doc, start) = ds.rank_start(p);
        let depth = p - start;
        let (odoc, ostart) = oracle.locate(p);
        let mut ok = doc == odoc && start == ostart;
        // select must close the loop on BOTH decoders
        if ok && ds.select(doc) != start {
            ok = false;
        }
        if ok && oracle.start_of(odoc) != ostart {
            ok = false;
        }
        // the containing interval must hold p
        if ok {
            let next = if doc + 1 < ds.docs { ds.select(doc + 1) } else { ds.text_len };
            if !(start <= p && p < next) {
                ok = false;
            }
        }
        if !ok {
            disagreements += 1;
            if disagreements <= 10 {
                eprintln!(
                    "DOCSTARTS_PARITY_MISMATCH pos={p} member=({doc},{start},{depth}) \
                     oracle=({odoc},{ostart},{})",
                    p - ostart
                );
            }
        }
        rank_checks += 1;
    }
    // ---- select parity at random doc ids
    let mut select_checks = 0u64;
    for _ in 0..samples {
        let d = splitmix64(&mut state) % ds.docs;
        let s = ds.select(d);
        let o = oracle.start_of(d);
        if s != o {
            disagreements += 1;
            if disagreements <= 10 {
                eprintln!("DOCSTARTS_PARITY_MISMATCH select docid={d} member={s} oracle={o}");
            }
        }
        select_checks += 1;
    }
    let covered = ds.docs;
    println!(
        "DOCSTARTS docs={} bytes={} ratio_vs_n={:.9} encoding={} sample_stride={}",
        covered,
        {
            let m = reader.member(M15);
            m.bytes
        },
        {
            let m = reader.member(M15);
            m.bytes as f64 / ds.text_len.max(1) as f64
        },
        ENCODING_NAME,
        ds.stride,
    );
    println!(
        "DOCSTARTS_PARITY positions={} internal_1e_positions={} select_checks={} \
disagreements={} disagreements_tol=0 \
method=\"member rank/select (delta-varint+sampled-absolutes) vs seps.bin XSEP-v1 \
anchor-bisect+delta-scan; both cross-checked against gates/docstarts_oracle.c\" \
range=[{},{}) text_len={} docs_member={} docs_map={}",
        rank_checks,
        internal_used,
        select_checks,
        disagreements,
        range_start,
        end,
        ds.text_len,
        covered,
        oracle.docs,
    );
    if disagreements != 0 {
        die(&format!(
            "DOCSTARTS_PARITY result=FAIL disagreements={disagreements} (tolerance 0)"
        ));
    }
    println!("DOCSTARTS_PARITY result=PASS");
}

/// `xsa docstarts probe <container> --positions FILE`: member rank/select for a
/// list of positions (u64 LE, one per 8 bytes), printed in the same format as
/// the C oracle so a gate can diff the two outputs byte for byte.
fn probe_command(args: &[String]) {
    let mut container: Option<String> = None;
    let mut positions: Option<String> = None;
    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--positions" => {
                i += 1;
                positions = Some(args.get(i).cloned().unwrap_or_else(|| die("DOCSTARTS: --positions needs a file")));
            }
            other => {
                if other.starts_with('-') {
                    die(&format!("DOCSTARTS probe: unknown option '{other}'"));
                }
                if container.replace(other.to_string()).is_some() {
                    die("DOCSTARTS probe: one container only");
                }
            }
        }
        i += 1;
    }
    let container = container.unwrap_or_else(|| die("DOCSTARTS probe: need a container"));
    let positions = positions.unwrap_or_else(|| die("DOCSTARTS probe: need --positions FILE"));
    let reader = qlite::Reader::open(&container).unwrap_or_else(|e| die(&format!("DOCSTARTS: {e}")));
    let ds = DocStarts::from_container(&reader).unwrap_or_else(|| {
        die(&format!(
            "DOCSTARTS: {container} is SXI2 v{} and carries no member m15 (document starts)",
            reader.version
        ))
    });
    let data = std::fs::read(&positions).unwrap_or_else(|e| die(&format!("DOCSTARTS: {e}")));
    need(data.len() % 8 == 0, "--positions file is not a whole u64 array");
    for chunk in data.chunks_exact(8) {
        let p = u64::from_le_bytes(chunk.try_into().unwrap());
        let (doc, start) = ds.rank_start(p);
        println!("PROBE pos={p} docid={doc} start={start} depth={}", p - start);
    }
}

fn info_command(container: &str) {
    let reader = qlite::Reader::open(container).unwrap_or_else(|e| die(&format!("DOCSTARTS: {e}")));
    match DocStarts::from_container(&reader) {
        Some(ds) => {
            let m = reader.member(M15);
            println!(
                "DOCSTARTS container={container} version={} present=true docs={} bytes={} \
                 ratio_vs_n={:.9} encoding={} sample_stride={} text_len={} plain_u64_bytes={}",
                reader.version,
                ds.docs,
                m.bytes,
                m.bytes as f64 / ds.text_len.max(1) as f64,
                ENCODING_NAME,
                ds.stride,
                ds.text_len,
                8 * ds.docs,
            );
        }
        None => {
            println!(
                "DOCSTARTS container={container} version={} present=false \
                 reason=no-member-m15 (the feature is OFF; build with `xsa build --docs-map DIR` \
                 or embed with `xsa docstarts embed`)",
                reader.version
            );
        }
    }
}

fn embed_command(args: &[String]) {
    let mut container: Option<String> = None;
    let mut map: Option<String> = None;
    let mut starts_file: Option<String> = None;
    let mut out: Option<String> = None;
    let stride = SAMPLE_STRIDE;
    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--docs-map" => {
                i += 1;
                map = Some(args.get(i).cloned().unwrap_or_else(|| die("DOCSTARTS: --docs-map needs a directory")));
            }
            "--doc-starts" => {
                i += 1;
                starts_file = Some(args.get(i).cloned().unwrap_or_else(|| die("DOCSTARTS: --doc-starts needs a file")));
            }
            "--out" => {
                i += 1;
                out = Some(args.get(i).cloned().unwrap_or_else(|| die("DOCSTARTS: --out needs a file")));
            }
            other => {
                if other.starts_with('-') {
                    die(&format!("DOCSTARTS embed: unknown option '{other}'"));
                }
                if container.replace(other.to_string()).is_some() {
                    die("DOCSTARTS embed: one container only");
                }
            }
        }
        i += 1;
    }
    let container = container.unwrap_or_else(|| die("DOCSTARTS embed: need a container"));
    let source = match (map, starts_file) {
        (Some(d), None) => Source::DocsMap(d),
        (None, Some(f)) => Source::DocStartsFile(f),
        (Some(_), Some(_)) => die("DOCSTARTS embed: --docs-map and --doc-starts are mutually exclusive"),
        (None, None) => die("DOCSTARTS embed: need --docs-map DIR or --doc-starts FILE"),
    };
    let stats = embed(&container, &source, stride, out.as_deref());
    println!("{}", journal_line(&stats));
}

pub fn command(args: &[String]) {
    match args.first().map(|s| s.as_str()) {
        Some("embed") => embed_command(&args[1..]),
        Some("verify") => verify_command(&args[1..]),
        Some("probe") => probe_command(&args[1..]),
        Some("info") => {
            if args.len() != 2 {
                die("usage: xsa docstarts info <container>");
            }
            info_command(&args[1]);
        }
        _ => die(&format!(
            "usage: xsa docstarts embed|verify|probe|info ...\n  \
             xsa docstarts embed <container> (--docs-map DIR | --doc-starts FILE) [--out FILE]\n  \
             xsa docstarts verify <container> --docs-map DIR [--samples N] [--range-start B]\n        \
             [--range-end B] [--seed S]\n  \
             xsa docstarts probe <container> --positions FILE\n  \
             xsa docstarts info <container>\n  \
             got: {args:?}"
        )),
    }
}

// -------------------------------------------------------------------- tests

#[cfg(test)]
mod tests {
    use super::*;

    fn synthetic_starts(n: u64, seed: u64) -> Vec<u64> {
        let mut state = seed;
        let mut v = Vec::new();
        let mut at = 0u64;
        for _ in 0..n {
            v.push(at);
            at += 1 + (splitmix64(&mut state) % 9973);
        }
        v
    }

    /// Parse an encoded member as if it were m15 of a container whose text is
    /// `text_len` bytes (n = text_len + 1).
    fn member_backing(bytes: Vec<u8>, text_len: u64) -> DocStarts {
        let b = Arc::new(bytes);
        let n = b.len() as u64;
        DocStarts::parse(Backing::Buf(b), 0, n, VERSION, text_len + 1)
    }

    fn brute(ds: &DocStarts, starts: &[u64], text_len: u64) {
        for (i, &s) in starts.iter().enumerate() {
            assert_eq!(ds.select(i as u64), s, "select({i})");
        }
        // rank/depth at every position of a dense prefix and random midpoints
        let mut state = 0xC0FFEEu64;
        let mut probes: Vec<u64> = (0..text_len.min(5000)).collect();
        for _ in 0..2000 {
            probes.push(splitmix64(&mut state) % text_len);
        }
        for p in probes {
            let (doc, start) = ds.rank_start(p);
            assert_eq!(start, starts[doc as usize], "rank_start({p}).start");
            assert!(p >= start, "rank_start({p}) below its document start");
            let next = starts.get(doc as usize + 1).copied().unwrap_or(text_len);
            assert!(p < next, "rank_start({p}) is inside document {doc} but past its end");
            assert_eq!(ds.depth3(p), (doc, start, p - start));
        }
    }

    #[test]
    fn encode_load_roundtrip_stride_64() {
        let starts = synthetic_starts(10_000, 42);
        let text_len = starts.last().unwrap() + 500;
        let mut it = starts.iter().copied();
        let enc = encode(&mut it, text_len, 64, Some(10_000));
        assert_eq!(enc.docs, 10_000);
        let ds = member_backing(enc.bytes.clone(), text_len);
        assert_eq!((ds.docs, ds.text_len, ds.stride), (10_000, text_len, 64));
        brute(&ds, &starts, text_len);
        // the member is far smaller than the plain u64 array at these gaps:
        // 2-byte varints + 12 B per 64 docs ≈ 2.19 B/doc vs 8 B/doc (ratio ~0.27)
        assert!(enc.bytes.len() as u64 * 3 < 8 * 10_000, "sampled varint must beat u64[] at mean gap ~5k");
    }

    #[test]
    fn encode_load_roundtrip_odd_stride_and_one_doc() {
        let starts = synthetic_starts(999, 7);
        let text_len = starts.last().unwrap() + 17;
        let mut it = starts.iter().copied();
        let enc = encode(&mut it, text_len, 7, Some(999));
        let ds = member_backing(enc.bytes, text_len);
        brute(&ds, &starts, text_len);

        // ONE document: samples == 1, rank(anything) == 0
        let mut it2 = std::iter::once(0u64);
        let enc2 = encode(&mut it2, 100, 64, Some(1));
        let ds2 = member_backing(enc2.bytes, 100);
        assert_eq!(ds2.docs, 1);
        assert_eq!(ds2.rank_start(99), (0, 0));
        assert_eq!(ds2.select(0), 0);
    }

    /// `die` exits the process, so refusal tests run their failing case in a
    /// CHILD of this same test binary (the env var selects it) and assert on the
    /// exit status and the stderr text. catch_unwind would pass silently because
    /// `exit` does not unwind.
    fn expect_die(case: &'static str, run: impl FnOnce()) {
        if std::env::var("XSA_DOCSTARTS_DIE_CASE").as_deref() == Ok(case) {
            run();
            eprintln!("DIE_CASE_RETURNED case={case}");
            std::process::exit(2); // the refusal did NOT happen: fail loud in the child too
        }
        let exe = std::env::current_exe().unwrap();
        let out = std::process::Command::new(exe)
            .arg("--exact")
            .arg("docstarts::tests::refusals")
            .arg("--nocapture")
            .env("XSA_DOCSTARTS_DIE_CASE", case)
            .output()
            .unwrap();
        let err = String::from_utf8_lossy(&out.stderr);
        assert_eq!(
            out.status.code(),
            Some(1),
            "case {case}: expected exit 1 (a refusal), got {:?}; stderr:\n{err}",
            out.status
        );
    }

    #[test]
    fn refusals() {
        expect_die("encode_nonzero_first", || {
            let mut it = std::iter::once(5u64);
            encode(&mut it, 100, 64, Some(1));
        });
        expect_die("encode_unsorted", || {
            let bad = [0u64, 10, 10, 30];
            let mut it = bad.iter().copied();
            encode(&mut it, 100, 64, Some(4));
        });
        expect_die("member_lying_header", || {
            let starts = synthetic_starts(100, 3);
            let text_len = starts.last().unwrap() + 9;
            let mut it = starts.iter().copied();
            let mut enc = encode(&mut it, text_len, 64, Some(100));
            enc.bytes[16..24].copy_from_slice(&99u64.to_le_bytes());
            let _ = member_backing(enc.bytes, text_len);
        });
    }

    #[test]
    fn xsep_load_and_parity_with_member() {
        // hand-build an XSEP v1 seps.bin and compare DocStarts over it with an
        // identically-sourced member, position by position.
        let starts = synthetic_starts(5000, 99);
        let text_len = starts.last().unwrap() + 321;
        let docs = starts.len() as u64;
        let stride = 64u64;
        let anchors = docs.div_ceil(stride);
        let mut seps = Vec::new();
        seps.extend_from_slice(b"XSEP");
        seps.extend_from_slice(&1u32.to_le_bytes());
        seps.extend_from_slice(&1u32.to_le_bytes());
        seps.extend_from_slice(&(stride as u32).to_le_bytes());
        seps.extend_from_slice(&docs.to_le_bytes());
        seps.extend_from_slice(&text_len.to_le_bytes());
        seps.extend_from_slice(&anchors.to_le_bytes());
        seps.extend_from_slice(&0u64.to_le_bytes());
        let mut prev = 0u64;
        for &s in &starts {
            seps.extend_from_slice(&((s - prev) as u32).to_le_bytes());
            prev = s;
        }
        for (j, &s) in starts.iter().enumerate().filter(|(i, _)| i % stride as usize == 0) {
            let _ = j;
            seps.extend_from_slice(&s.to_le_bytes());
        }
        let dir = std::env::temp_dir().join(format!("docstarts-xsep-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("seps.bin"), &seps).unwrap();
        let from_xsep = DocStarts::from_xsep(&dir, text_len);
        let mut it = starts.iter().copied();
        let enc = encode(&mut it, text_len, 64, Some(docs));
        let from_member = member_backing(enc.bytes, text_len);
        assert_eq!(from_xsep.docs, from_member.docs);
        let mut state = 12345u64;
        for _ in 0..5000 {
            let p = splitmix64(&mut state) % text_len;
            assert_eq!(from_xsep.rank_start(p), from_member.rank_start(p), "parity at {p}");
        }
        for d in 0..docs {
            assert_eq!(from_xsep.select(d), from_member.select(d));
        }
        let _ = std::fs::remove_dir_all(&dir);
    }
}

// A tiny helper used by the tests; kept here to avoid touching other modules.
impl DocStarts {
    #[cfg(test)]
    fn depth3(&self, p: u64) -> (u64, u64, u64) {
        let (doc, start) = self.rank_start(p);
        (doc, start, p - start)
    }
}
