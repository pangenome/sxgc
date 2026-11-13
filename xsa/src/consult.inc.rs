// ============================================================================
// `xsa consult` — BOUNDED-MEMORY STREAMING EXTRACTION (cache-build mode).
//
// Included into `qlite` so it shares the one query kernel: the immutable
// `Index` (fixed decoded-block arena, RSS governor) and `branch_result`, the
// same function `xsa branches --serve` answers with. There is no second query
// path here and there is no per-position state that outlives a position:
//
//   windows (doc, base, start, len, left) -> corpus bytes (windowed mmap)
//     -> one `branch_result` per position (ONE query, ONE context length,
//        NO backoff; empty or absent context -> empty histogram)
//     -> append-only row chunks, fsync'd, journaled, resumable.
//
// WHAT MAKES THE MEMORY BOUNDED (all three are structural, not tuning):
//
//  1. NO ACCUMULATION. A row is encoded into the batch buffer that is already
//     in flight and written out by the writer thread; nothing holds a row, a
//     context or a count past the batch that carried it. Consecutive positions
//     are not cached, deduplicated or remembered, so there is no object whose
//     size grows with positions consumed (a 490 B/position accumulation like
//     the emender service's ~400 GB RSS has nothing to attach to here).
//  2. A BOUNDED IN-FLIGHT WINDOW. Requests are at most `jobs * 8` batches of
//     `BATCH_POSITIONS` positions, each behind a semaphore permit; the producer
//     therefore BLOCKS (backpressure) instead of reading ahead. The journal
//     records every block as CONSULT_BACKPRESSURE state=pause|resume.
//  3. THE RSS GOVERNOR + THROTTLE. The container mapping is read-only and the
//     reader's own arena is fixed (`CACHE_BYTES`); the only thing that can grow
//     is the resident set of the container mapping's directories, which the
//     governor bleeds off in windows (MADV_DONTNEED) and which the producer
//     throttles on (CONSULT_RSS + pause/resume) against `--rss-gb`.
//
// DURABILITY: rows are appended, then `fdatasync`'d, THEN the commit record is
// appended to the journal and fsync'd. A crash can therefore leave an
// uncommitted row tail, never a committed byte that is not on disk. `--resume`
// truncates the output back to the last commit's byte length, VERIFIES that
// prefix against the commit's sha256, and continues at the commit's whole-window
// cursor. A torn journal tail (no trailing newline / unparsable) is discarded,
// never trusted.
// ============================================================================

use sha2::{Digest as _, Sha256};
use std::io::{Read as _, Seek as _};

/// Row-layout file magic and version; the byte layout is documented in
/// CONSULT.md and is what the training-side reader is written against.
const CONSULT_MAGIC: &[u8; 8] = b"XSACONS1";
/// Row-layout file version. v3 = the no-cross law: `eof` counts every
/// occurrence that ends a DOCUMENT as well as the one structural END (the
/// context that is a suffix of the text), and flag bit 4 (16) marks a row whose
/// histogram is the boundary-UNAWARE conflated answer (see CONSULT.md §5).
const CONSULT_VERSION: u32 = 3;
/// Opt-in row-layout version (`--rows-v4`): every row is preceded by a 1-byte
/// KIND, and a FORCED row (the width-1 / one-step-verification answers: one
/// continuation byte, no locate) is encoded WITHOUT its 9-byte branch pair —
/// the pair is (kind-1 `cont`, width−eof) by construction. The DEFAULT output
/// stays v3 and byte-identical to today (downstream experiments are hash-bound
/// to the row bytes); v4 is the compact opt-in the χ-proportional consult
/// makes possible (see CONSULT.md §LAYERING).
const CONSULT_VERSION_V4: u32 = 4;
const ROW_KIND_FULL: u8 = 0;
const ROW_KIND_FORCED: u8 = 1;
/// Binary `--windows` batch magic (the first 32-byte record is a header).
const WIN_MAGIC: &[u8; 8] = b"XSWIN001";
const WIN_RECORD_BYTES: usize = 32;
/// The ONE context length this command queries: the last min(1024, available)
/// bytes before the position. No backoff, ever.
const CTX_MAX: u32 = 1024;
/// Fixed part of a row record: nsym u32, ctxlen u16, flags u16, doc u64,
/// off u64, width u64, eof u64.
const ROW_FIXED_BYTES: usize = 40;
/// v2 tail of a row record: docid u64, base u64, depth_in_doc u64 (see
/// CONSULT.md §3 - the reply answers "inside which document" alongside
/// "what continues this").
const ROW_TAIL_BYTES: usize = 24;
/// A window whose `base` is this (JSONL: field absent; binary: the record's
/// u64 is all-ones) resolves its base from the document structure (container
/// member m15 or `--docs DIR`). REFUSED if no structure is available - a base
/// is never inferred from content bytes.
const BASE_ABSENT: u64 = u64::MAX;
/// The row's `docid` field when the position's document is NOT a container
/// document (no structure, or the position is beyond the container's text -
/// the flagship case: train/eval documents live beyond the spine).
const DOCID_ABSENT: u64 = u64::MAX;
/// `--doc-restrict`: a context whose occurrence interval is wider than this is
/// REFUSED rather than silently counted unrestricted. Exact document-restricted
/// counting enumerates the interval's occurrences (one LF walk each), so the
/// cap is the cost knob; the default is sized for long-spine cache builds.
const DOC_RESTRICT_CAP_DEFAULT: u64 = 1024;
/// THE NO-CROSS DEFAULT CAP: exact boundary-aware counting ENUMERATES the
/// occurrence interval (one locate per occurrence, then an m15 rank/select per
/// classification), so the cap is the cost knob for contexts wider than the
/// boundary-aware trie's depth. Beyond the cap the row falls back to the
/// conflated rank-arithmetic answer, MARKED boundary_exact=0 in the row and
/// journaled (--boundary-strict makes the fallback a refusal instead). 8192
/// covers the 9..24-byte ramp contexts that exceed the old 1024 default.
const BOUNDARY_CAP_DEFAULT: u64 = 8192;
/// Sliding corpus mapping width. The corpus is read through a WINDOWED mmap of
/// this size and nothing else: a 100 GB corpus never becomes this process's
/// resident set, and a retired window is MADV_DONTNEED'd before it is dropped.
const CONSULT_CORPUS_WINDOW: u64 = 64 << 20;
/// Positions per work batch. One batch is one message and one buffer, so this
/// is the granularity at which in-flight memory is accounted.
const BATCH_POSITIONS: usize = 64;
/// A binary window record whose `left` field is this means "use --left".
const LEFT_USE_DEFAULT: u32 = 0xFFFF_FFFF;
/// THE SEPARATOR-FRAMING LAW (see CONSULT.md §LAYERING): on a corpus whose
/// every document start d >= 1 is immediately preceded by one 0x1E byte (the
/// Pile's own framing — verified EXHAUSTIVELY at open, never assumed), a
/// context that contains NO 0x1E byte cannot touch a document boundary:
/// a CROSSING occurrence would need a doc start d inside (pos, at-1], and
/// text[d-1] = 0x1E would then have to equal C[d-1-pos]; an END-branch
/// occurrence (at a doc start) would need C[ctxlen-1] = 0x1E. With C
/// 0x1E-free both are impossible, so EVERY occurrence is either in-document
/// content or the one structural text end — exactly the raw backward-search
/// classification. The no-cross histogram IS the raw histogram, zero locate,
/// at ANY width (still capped so the marked fallback's bytes stay what they
/// are today).
const SEP_FRAMING: u8 = 0x1E;

fn gb(n: u64) -> f64 {
    n as f64 / (1u64 << 30) as f64
}

// ------------------------------------------------------- the χ layer ledger

/// One answer layer of the χ-proportional consult (the journal's CONSULT_LAYERS
/// line and G4's per-layer cost table). The layers are ordered by the decision
/// cascade in the worker: EMPTY and ABSENT answer without touching the index;
/// TRIE answers from the member; WIDTH1, TFORCED, FORCED and RAWFORK are the
/// ZERO-LOCATE raw layers (the χ-proportional mass); ENUM is the exact
/// m15-crossing-filtered enumeration; MARKED and CONFLATED are the conflated
/// fallbacks (marked in the row); RESTRICT is the --doc-restrict arm.
const CHI_LAYERS: usize = 11;
const CHI_LAYER_NAMES: [&str; CHI_LAYERS] = [
    "empty", "trie", "absent", "width1", "tforced", "forced", "rawfork", "enum", "marked",
    "conflated", "restrict",
];
const CHI_EMPTY: usize = 0;
const CHI_TRIE: usize = 1;
const CHI_ABSENT: usize = 2;
const CHI_WIDTH1: usize = 3;
const CHI_TFORCED: usize = 4;
const CHI_FORCED: usize = 5;
const CHI_RAWFORK: usize = 6;
const CHI_ENUM: usize = 7;
const CHI_MARKED: usize = 8;
const CHI_CONFLATED: usize = 9;
const CHI_RESTRICT: usize = 10;
/// Log2 nanosecond buckets per layer (exact cdf p50, O(1) memory per worker:
/// the memory law forbids anything that grows with positions consumed).
const CHI_HIST_BUCKETS: usize = 40;

/// Per-worker layer ledger: rows, nanoseconds and a log2-ns histogram.
struct ChiLedger {
    rows: [u64; CHI_LAYERS],
    nanos: [u64; CHI_LAYERS],
    hist: [u64; CHI_LAYERS * CHI_HIST_BUCKETS],
}

impl ChiLedger {
    fn new() -> ChiLedger {
        ChiLedger {
            rows: [0; CHI_LAYERS],
            nanos: [0; CHI_LAYERS],
            hist: [0; CHI_LAYERS * CHI_HIST_BUCKETS],
        }
    }
    /// Charge one answered row to a layer.
    #[inline]
    fn charge(&mut self, layer: usize, ns: u64) {
        self.rows[layer] += 1;
        self.nanos[layer] += ns;
        // bucket k holds ns in [2^k, 2^(k+1)); bucket 0 also holds ns < 1.
        let b = (64 - ns.leading_zeros() as usize).min(CHI_HIST_BUCKETS - 1);
        self.hist[layer * CHI_HIST_BUCKETS + b] += 1;
    }
    /// The p50 of the observed per-row costs, from the log2 histogram's cdf
    /// (the geometric mid of the bucket the median falls into).
    fn p50_ns(&self, layer: usize) -> f64 {
        let n = self.rows[layer];
        if n == 0 {
            return 0.0;
        }
        let half = n / 2;
        let mut acc = 0u64;
        for b in 0..CHI_HIST_BUCKETS {
            acc += self.hist[layer * CHI_HIST_BUCKETS + b];
            if acc > half {
                if b == 0 {
                    return 0.5;
                }
                let lo = (1u64 << (b - 1)) as f64;
                let hi = if b >= 63 { f64::INFINITY } else { (1u64 << b) as f64 };
                return (lo * hi).sqrt();
            }
        }
        0.0
    }
}

/// Verify THE SEPARATOR-FRAMING LAW for this container+corpus pair: every
/// document start d with 1 <= start(d) < text_len must have corpus[start(d)-1]
/// == 0x1E. EXHAUSTIVE (a sample cannot authorize the zero-locate raw layers:
/// one unframed boundary anywhere would let a crossing occurrence hide inside
/// a forced row), parallel over `threads` handles, one 1-byte pread per document
/// start. Returns (docs checked, violations, first violation's doc start,
/// seconds). The caller journals the result and DISABLES the raw layers for the
/// whole run when the law does not hold (exactness is never traded for speed).
fn verify_sep_framing(
    corpus_path: &str,
    ds: &crate::docstarts::DocStarts,
    text_len: u64,
    threads: usize,
) -> (u64, u64, u64, f64) {
    use std::os::unix::fs::FileExt as _;
    let started = Instant::now();
    // doc ids whose start is a real in-text boundary: 1 <= start < text_len
    // (start == text_len is inert: `at == text_len` is classified eof BEFORE
    // the doc-start test, and no occurrence can reach past the text end).
    let per = (ds.docs + threads as u64 - 1) / threads.max(1) as u64;
    let mut checked = 0u64;
    let mut violations = 0u64;
    let mut first_bad = 0u64;
    std::thread::scope(|scope| {
        let mut handles = Vec::with_capacity(threads);
        for t in 0..threads {
            let lo = t as u64 * per;
            let hi = ((t as u64 + 1) * per).min(ds.docs);
            if lo >= hi {
                continue;
            }
            let f = std::fs::File::open(corpus_path)
                .unwrap_or_else(|e| die(&format!("consult: open corpus {corpus_path}: {e}")));
            handles.push(scope.spawn(move || {
                let mut buf = [0u8; 1];
                let mut n = 0u64;
                let mut bad = 0u64;
                let mut first = 0u64;
                for d in lo..hi {
                    if d == 0 {
                        continue;
                    }
                    let s = ds.select(d);
                    if s >= text_len {
                        continue; // a superset map's tail: not this container's text
                    }
                    f.read_exact_at(&mut buf, s - 1)
                        .unwrap_or_else(|e| die(&format!("consult: framing read at {}: {e}", s - 1)));
                    n += 1;
                    if buf[0] != SEP_FRAMING {
                        bad += 1;
                        if first == 0 {
                            first = s;
                        }
                    }
                }
                (n, bad, first)
            }));
        }
        for h in handles {
            let (n, bad, first) =
                h.join().unwrap_or_else(|_| die("consult: framing verify died"));
            checked += n;
            violations += bad;
            if first_bad == 0 {
                first_bad = first;
            }
        }
    });
    (checked, violations, first_bad, started.elapsed().as_secs_f64())
}

/// The width of `context || c` for ONE candidate byte, by the same pure rank
/// arithmetic `extension_counts` uses per candidate: start at c's own interval
/// (the C table answers rank(c,0) and rank(c,n) for free) and prepend the
/// context right to left. Zero locate, zero corpus read.
fn extension_width(cx: &mut Ctx<'_>, sym: u8, work: &[u8]) -> u64 {
    let base = cx.idx.c[sym as usize];
    let mut a = base;
    let mut z = base + cx.idx.total[sym as usize];
    for &p in work {
        if a == z {
            return 0; // this byte is absent after this context prefix
        }
        let pb = cx.idx.c[p as usize];
        a = pb + cx.rank(p, a);
        z = pb + cx.rank(p, z);
    }
    z - a
}

/// This process's resident set in bytes (`/proc/self/statm` field 2).
fn consult_resident_bytes() -> Option<u64> {
    let s = std::fs::read_to_string("/proc/self/statm").ok()?;
    let mut it = s.split_whitespace();
    let _size = it.next()?;
    let resident: u64 = it.next()?.parse().ok()?;
    Some(resident * 4096)
}

/// This process's PEAK resident set in bytes (VmHWM), so the memory claim is
/// the kernel's number and not a sampling of ours.
fn consult_peak_rss_bytes() -> Option<u64> {
    let s = std::fs::read_to_string("/proc/self/status").ok()?;
    for line in s.lines() {
        if let Some(rest) = line.strip_prefix("VmHWM:") {
            let kb: u64 = rest.split_whitespace().next()?.parse().ok()?;
            return Some(kb * 1024);
        }
    }
    None
}

// ------------------------------------------------------------- the journal

struct ConsultJournal {
    f: File,
}

impl ConsultJournal {
    fn open_append(path: &str) -> ConsultJournal {
        let f = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(path)
            .unwrap_or_else(|e| die(&format!("consult: open journal {}: {e}", path)));
        ConsultJournal { f }
    }
    /// One journal record, durably on disk before it is believed. O_APPEND is
    /// per-write; records are small, so a record is never interleaved.
    fn line(&mut self, s: &str) {
        let mut b = Vec::with_capacity(s.len() + 1);
        b.extend_from_slice(s.as_bytes());
        b.push(b'\n');
        self.f
            .write_all(&b)
            .unwrap_or_else(|e| die(&format!("consult: journal write: {e}")));
        self.f
            .sync_data()
            .unwrap_or_else(|e| die(&format!("consult: journal fsync: {e}")));
    }
}

/// The last complete commit record found in a journal.
#[derive(Debug, Clone)]
struct ConsultCommit {
    seq: u64,
    windows_done: u64,
    windows_bytes_done: u64,
    positions_done: u64,
    output_bytes: u64,
    rows_sha256: String,
}

fn journal_field<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    line.split_whitespace()
        .find_map(|kv| kv.strip_prefix(key).and_then(|r| r.strip_prefix('=')))
}

/// Parse the LAST commit record of a journal.
///
/// "Complete" means: the line ends with a newline (so the writer's fsync'd
/// record arrived whole), it starts with CONSULT_COMMIT, and every required
/// field parses. A torn tail - a partial line without a newline - is DISCARDED
/// and reported; it is never used as a cursor. A COMPLETE but unparsable
/// commit record is a hard error, not a discard.
fn read_last_commit(path: &str) -> Option<(ConsultCommit, u64, u64)> {
    let data =
        std::fs::read(path).unwrap_or_else(|e| die(&format!("consult: read journal {}: {e}", path)));
    let mut last: Option<ConsultCommit> = None;
    let mut lines = 0u64;
    let mut discarded = 0u64;
    // split_inclusive keeps the terminator, which is how "complete" is decided.
    for raw in data.split_inclusive(|&b| b == b'\n') {
        if raw.last() != Some(&b'\n') {
            discarded += 1;
            continue;
        }
        let line = String::from_utf8_lossy(&raw[..raw.len() - 1]).into_owned();
        lines += 1;
        if !line.starts_with("CONSULT_COMMIT ") {
            continue;
        }
        let parse = (|| {
            Some(ConsultCommit {
                seq: journal_field(&line, "seq")?.parse().ok()?,
                windows_done: journal_field(&line, "windows_done")?.parse().ok()?,
                windows_bytes_done: journal_field(&line, "windows_bytes_done")?.parse().ok()?,
                positions_done: journal_field(&line, "positions_done")?.parse().ok()?,
                output_bytes: journal_field(&line, "output_bytes")?.parse().ok()?,
                rows_sha256: journal_field(&line, "rows_sha256")?.to_string(),
            })
        })();
        match parse {
            Some(c) => {
                if let Some(prev) = &last {
                    if c.seq <= prev.seq {
                        die(&format!(
                            "consult: journal {path} is not append-monotone: commit seq {} follows {}",
                            c.seq, prev.seq
                        ));
                    }
                    if c.positions_done < prev.positions_done || c.output_bytes < prev.output_bytes {
                        die(&format!(
                            "consult: journal {path} commit seq {} moves backwards (positions {} -> \
                             {}, output_bytes {} -> {})",
                            c.seq, prev.positions_done, c.positions_done, prev.output_bytes,
                            c.output_bytes
                        ));
                    }
                }
                last = Some(c);
            }
            None => die(&format!(
                "consult: journal {path} has a COMPLETE but unparsable commit record:\n  {line}"
            )),
        }
    }
    last.map(|c| (c, lines, discarded))
}

// ------------------------------------------------------- the window source

/// One window of positions: `len` consecutive document-relative positions
/// starting at `start`, inside the document whose first corpus byte is at
/// `base`. The position's own byte is at corpus offset `base + start + i`.
///
/// `base` may be `BASE_ABSENT`: the window then resolves its base from the
/// document structure by `doc` (container m15 or `--docs DIR`) - D1, the
/// caller who knows the boundaries may also state them explicitly, and a
/// missing base with no structure available is REFUSED, never scanned for.
/// `whole_doc` marks a window that gave neither base nor len: it resolves to
/// every position of the document `doc`.
#[derive(Debug, Clone, Copy)]
struct ConsultWindow {
    doc: u64,
    base: u64,
    start: u64,
    len: u32,
    left: u32,
    whole_doc: bool,
}

struct WindowSource {
    path: String,
    file: File,
    len: u64,
    buf: Vec<u8>,
    /// File offset of `buf[0]`.
    buf_off: u64,
    /// Consume cursor inside `buf`.
    i: usize,
    /// Valid bytes in `buf`.
    filled: usize,
    binary: bool,
}

impl WindowSource {
    fn open(path: &str, binary: bool) -> WindowSource {
        let file =
            File::open(path).unwrap_or_else(|e| die(&format!("consult: open windows {path}: {e}")));
        let len = file
            .metadata()
            .unwrap_or_else(|e| die(&format!("consult: stat windows {path}: {e}")))
            .len();
        let mut s = WindowSource {
            path: path.to_string(),
            file,
            len,
            buf: vec![0u8; 1 << 20],
            buf_off: 0,
            i: 0,
            filled: 0,
            binary,
        };
        if binary {
            if len < WIN_RECORD_BYTES as u64 || len % WIN_RECORD_BYTES as u64 != 0 {
                die(&format!(
                    "consult: binary windows {path}: size {len} is not a non-zero multiple of the \
                     {WIN_RECORD_BYTES}-byte record (the first record is an XSWIN001 header)"
                ));
            }
            s.fill();
            if &s.buf[..8] != WIN_MAGIC {
                die(&format!(
                    "consult: binary windows {path}: the first record does not start with XSWIN001 \
                     (got {:?})",
                    String::from_utf8_lossy(&s.buf[..8])
                ));
            }
            s.i += WIN_RECORD_BYTES;
        }
        s
    }

    /// The byte offset the NEXT window record starts at.
    fn cursor(&self) -> u64 {
        self.buf_off + self.i as u64
    }

    /// Position the source at a record boundary (used by --resume).
    fn seek_to(&mut self, off: u64) {
        if off > self.len {
            die(&format!(
                "consult: --resume cursor {off} is past the end of {} ({})",
                self.path, self.len
            ));
        }
        if self.binary && off % WIN_RECORD_BYTES as u64 != 0 {
            die(&format!(
                "consult: --resume cursor {off} is not a {WIN_RECORD_BYTES}-byte record boundary in \
                 the binary window file"
            ));
        }
        let mut f = self
            .file
            .try_clone()
            .unwrap_or_else(|e| die(&format!("consult: {e}")));
        f.seek(std::io::SeekFrom::Start(off))
            .unwrap_or_else(|e| die(&format!("consult: seek windows: {e}")));
        self.file = f;
        self.buf_off = off;
        self.i = 0;
        self.filled = 0;
    }

    /// Read more bytes into the buffer, compacting first. False at EOF.
    fn fill(&mut self) -> bool {
        if self.i > 0 {
            self.buf.copy_within(self.i..self.filled, 0);
            self.buf_off += self.i as u64;
            self.filled -= self.i;
            self.i = 0;
        }
        if self.filled == self.buf.len() {
            self.buf.resize(self.buf.len() * 2, 0);
        }
        let n = self
            .file
            .read(&mut self.buf[self.filled..])
            .unwrap_or_else(|e| die(&format!("consult: read windows {}: {e}", self.path)));
        self.filled += n;
        n > 0
    }

    fn next(&mut self) -> Option<ConsultWindow> {
        loop {
            if self.binary {
                if self.filled - self.i >= WIN_RECORD_BYTES {
                    let r = &self.buf[self.i..self.i + WIN_RECORD_BYTES];
                    let u = |o: usize| u64::from_le_bytes(r[o..o + 8].try_into().unwrap());
                    let w = ConsultWindow {
                        doc: u(0),
                        base: u(8),
                        start: u(16),
                        len: u32::from_le_bytes(r[24..28].try_into().unwrap()),
                        left: u32::from_le_bytes(r[28..32].try_into().unwrap()),
                        // a binary record always carries len; base == all-ones
                        // is the resolve-me sentinel
                        whole_doc: false,
                    };
                    self.i += WIN_RECORD_BYTES;
                    return Some(w);
                }
                if !self.fill() {
                    if self.filled - self.i == 0 {
                        return None;
                    }
                    die(&format!(
                        "consult: binary windows {}: {} trailing bytes are not a whole record",
                        self.path,
                        self.filled - self.i
                    ));
                }
                continue;
            }
            // JSONL: find a newline; fill until there is one or the file ends.
            if let Some(rel) = self.buf[self.i..self.filled].iter().position(|&b| b == b'\n') {
                let end = self.i + rel;
                let line = self.buf[self.i..end].to_vec();
                self.i = end + 1;
                if line.iter().all(|b| b.is_ascii_whitespace()) {
                    continue; // blank line: not a window, not an error
                }
                return Some(parse_json_window(&line, &self.path));
            }
            if !self.fill() {
                if self.filled - self.i == 0 {
                    return None;
                }
                let line = self.buf[self.i..self.filled].to_vec();
                self.i = self.filled;
                if line.iter().all(|b| b.is_ascii_whitespace()) {
                    return None;
                }
                die(&format!(
                    "consult: JSONL windows {}: the last line has no trailing newline (a torn \
                     window set is refused, not guessed): {:?}",
                    self.path,
                    String::from_utf8_lossy(&line[..line.len().min(120)])
                ));
            }
        }
    }
}

fn json_u64(obj: &serde_json::Map<String, serde_json::Value>, key: &str, path: &str) -> u64 {
    match obj.get(key) {
        Some(serde_json::Value::Number(n)) => n.as_u64().unwrap_or_else(|| {
            die(&format!(
                "consult: JSONL windows {path}: field '{key}' is not a non-negative integer"
            ))
        }),
        Some(_) => die(&format!(
            "consult: JSONL windows {path}: field '{key}' is not a number"
        )),
        None => die(&format!(
            "consult: JSONL windows {path}: window is missing required field '{key}' (schema: \
             doc, base, start, len, left; 'base' may be omitted only when a document \
             structure can resolve it from 'doc')"
        )),
    }
}

fn parse_json_window(line: &[u8], path: &str) -> ConsultWindow {
    let v: serde_json::Value = serde_json::from_slice(line).unwrap_or_else(|e| {
        die(&format!(
            "consult: JSONL windows {path}: unparsable window line: {e}\n  {:?}",
            String::from_utf8_lossy(&line[..line.len().min(160)])
        ))
    });
    let obj = v.as_object().unwrap_or_else(|| {
        die(&format!(
            "consult: JSONL windows {path}: a window must be a JSON object"
        ))
    });
    let doc = json_u64(obj, "doc", path);
    // "base" is the explicit document start (D1). ABSENT = resolve it from the
    // document structure by "doc"; never from content bytes.
    let base = match obj.get("base") {
        None => BASE_ABSENT,
        Some(serde_json::Value::Number(n)) => n.as_u64().unwrap_or_else(|| {
            die(&format!(
                "consult: JSONL windows {path}: field 'base' is not a non-negative integer"
            ))
        }),
        Some(_) => die(&format!(
            "consult: JSONL windows {path}: field 'base' must be the document's first-byte \
             corpus offset (an integer), not a string"
        )),
    };
    let start = match obj.get("start") {
        None => 0,
        Some(_) => json_u64(obj, "start", path),
    };
    let len_given = obj.get("len").is_some();
    let len = match obj.get("len") {
        None => 1u64,
        Some(_) => json_u64(obj, "len", path),
    };
    let left = match obj.get("left") {
        None => u64::from(LEFT_USE_DEFAULT),
        Some(serde_json::Value::Number(n)) => n.as_u64().unwrap_or_else(|| {
            die(&format!(
                "consult: JSONL windows {path}: field 'left' is not a non-negative integer"
            ))
        }),
        Some(_) => die(&format!(
            "consult: JSONL windows {path}: field 'left' must be the left-context byte COUNT (an \
             integer), not a string"
        )),
    };
    // a window that names neither base nor len is THE WHOLE DOCUMENT `doc`
    let whole_doc = base == BASE_ABSENT && !len_given;
    if !whole_doc && (len == 0 || len > (1 << 20)) {
        die(&format!(
            "consult: JSONL windows {path}: window len={len} outside 1..={}",
            1 << 20
        ));
    }
    ConsultWindow {
        doc,
        base,
        start,
        len: len as u32,
        left: u32::try_from(left).unwrap_or(LEFT_USE_DEFAULT),
        whole_doc,
    }
}

// ------------------------------------------------------- the corpus window

/// A windowed, read-only mmap over the corpus text. Nothing is materialised:
/// at most CONSULT_CORPUS_WINDOW bytes of the corpus are mapped at a time, and a
/// retired window is MADV_DONTNEED'd before it is dropped, so the corpus never
/// becomes this process's resident set.
struct ConsultCorpus {
    file: File,
    len: u64,
    cur: Option<(u64, u64, memmap2::Mmap)>, // (map offset, map len, map)
    remaps: u64,
}

impl ConsultCorpus {
    fn open(path: &str) -> ConsultCorpus {
        let file =
            File::open(path).unwrap_or_else(|e| die(&format!("consult: open corpus {path}: {e}")));
        let len = file
            .metadata()
            .unwrap_or_else(|e| die(&format!("consult: stat corpus {path}: {e}")))
            .len();
        ConsultCorpus {
            file,
            len,
            cur: None,
            remaps: 0,
        }
    }

    /// Copy exactly `dst.len()` corpus bytes at `off` into `dst`.
    fn read_into(&mut self, off: u64, dst: &mut [u8]) {
        let need = dst.len() as u64;
        if off.checked_add(need).map_or(true, |e| e > self.len) {
            die(&format!(
                "consult: corpus read [{off}, {}) is outside the corpus ({} bytes): a window names \
                 a position the corpus does not have",
                off + need,
                self.len
            ));
        }
        let inside = match &self.cur {
            Some((moff, mlen, _)) => off >= *moff && off + need <= *moff + *mlen,
            None => false,
        };
        if !inside {
            if let Some((_moff, mlen, map)) = self.cur.take() {
                // Bleed the retired window off before dropping it. The advice
                // range is INSIDE THIS MAPPING (offset 0 = the file offset the
                // mapping started at), never the file offset: advising a file
                // offset here is an out-of-range madvise and aborts the process.
                let _ = unsafe {
                    map.unchecked_advise_range(
                        memmap2::UncheckedAdvice::DontNeed,
                        0,
                        mlen as usize,
                    )
                };
                drop(map);
            }
            let moff = off & !4095u64;
            let mlen = CONSULT_CORPUS_WINDOW.min(self.len - moff);
            let map = unsafe {
                memmap2::MmapOptions::new()
                    .offset(moff)
                    .len(mlen as usize)
                    .map(&self.file)
            }
            .unwrap_or_else(|e| die(&format!("consult: corpus mmap at {moff}: {e}")));
            self.cur = Some((moff, mlen, map));
            self.remaps += 1;
        }
        let (moff, _, map) = self.cur.as_ref().unwrap();
        let lo = (off - *moff) as usize;
        dst.copy_from_slice(&map[lo..lo + dst.len()]);
    }
}

// ------------------------------------------------------- the answer kernel

/// ONE QUERY, one context length, no backoff: the next-byte branch histogram of
/// `pat`.
///
/// WHY NOT `branch_result`. That function answers with the same numbers, but it
/// first runs a full backward search of `pat` (2 rank steps per context byte) to
/// obtain the interval `[l,r)`, and then runs the per-candidate extension chains
/// (~2.1 rank steps per context byte) to get the counts. Half of that work is
/// redundant for THIS caller: the width is recoverable from the counts, because
/// every occurrence of the context is followed by exactly one present byte or by
/// the structural END, and there is at most one of the latter. So
///
///     width = sum(counts) + eof        (branch_result's own asserted identity)
///     absent  <=>  sum(counts) == 0 && eof == 0
///     eof == 1  <=>  the context IS a suffix of the corpus text
///
/// The extension chains are the SAME function `branch_result` calls
/// (`extension_counts`), so the counts are identical by construction, and
/// `--strict` recomputes every row through `branch_result` and refuses to write
/// a single byte of disagreement.
fn consult_answer(
    cx: &mut Ctx<'_>,
    cand: &Candidates,
    pat: &[u8],
    corpus_tail: &[u8],
    out: &mut Vec<(u8, u64)>,
) -> (u64, u64) {
    out.clear();
    let sigma = cx.idx.reader.sigma;
    let absent = pat
        .iter()
        .any(|&b| sigma[b as usize] == 0 || cx.idx.total[sigma[b as usize] as usize] == 0);
    if absent {
        return (0, 0);
    }
    let work: Vec<u8> = pat.iter().rev().map(|&b| sigma[b as usize]).collect();
    let mut counts = [0u64; 256];
    extension_counts(cx, cand, &work, &mut counts);
    let sum: u64 = counts.iter().sum();
    // EOF: the only occurrence of a context followed by the structural END is
    // the one at the very end of the text, so the flag is a suffix test.
    let eof = u64::from(pat.len() <= corpus_tail.len() && pat == &corpus_tail[corpus_tail.len() - pat.len()..]);
    for (b, &c) in counts.iter().enumerate() {
        if c > 0 {
            out.push((b as u8, c));
        }
    }
    // branch_result's ordering (count desc, tie byte asc), so the row bytes are
    // the same bytes in the same order.
    out.sort_unstable_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    (sum + eof, eof)
}

/// THE NO-CROSS DEFAULT: the exact boundary-aware histogram, by enumeration.
///
/// Every occurrence of the context is located and classified by THE LAW:
/// the continuation at `at = pos + ctxlen` is the END branch iff `at` is the
/// text end or a DOCUMENT START (select(rank(at)) == at); the occurrence is a
/// CROSSING match (counted nowhere) iff a document start lies inside
/// (pos, at-1], i.e. rank(pos) != rank(at-1); otherwise it contributes its
/// in-document content byte. `width` is therefore the number of NON-CROSSING
/// occurrences. Returns (width, eof, excluded, too_wide): `too_wide` = the
/// interval exceeded the cap and the caller must fall back (marked, never
/// silent) or refuse (--boundary-strict).
#[allow(clippy::too_many_arguments)]
fn boundary_answer(
    cx: &mut Ctx<'_>,
    corpus_file: &File,
    ds: &crate::docstarts::DocStarts,
    it: &ConsultItem,
    pat: &[u8],
    cap: u64,
    text_len: u64,
    out: &mut Vec<(u8, u64)>,
) -> (u64, u64, bool, bool) {
    use std::os::unix::fs::FileExt;
    out.clear();
    let sigma = cx.idx.reader.sigma;
    let absent = pat
        .iter()
        .any(|&b| sigma[b as usize] == 0 || cx.idx.total[sigma[b as usize] as usize] == 0);
    if absent {
        return (0, 0, false, false);
    }
    let work: Vec<u8> = pat.iter().rev().map(|&b| sigma[b as usize]).collect();
    let (l, r) = cx.search(&work);
    if r == l {
        return (0, 0, false, false);
    }
    if r - l > cap {
        return (0, 0, false, true);
    }
    let mut counts = [0u64; 256];
    let mut eof = 0u64;
    let mut excluded = false;
    let ctxlen = pat.len() as u64;
    let mut bytebuf = [0u8; 1];
    for row in l..r {
        let (pos, _) = cx.locate_steps(row);
        let at = pos + ctxlen;
        if at == text_len {
            // the one structural END: the context is a suffix of the text
            eof += 1;
            continue;
        }
        // CROSSING match: a document starts inside (pos, at-1]. The start of
        // the document containing at-1 is the largest start <= at-1; the match
        // crosses iff that start is past pos (rank(pos) != rank(at-1)).
        if ds.rank_start(at - 1).1 > pos {
            continue;
        }
        // --exclude-source: the own occurrence leaves before any count. It can
        // never be a crossing match (the spine is cut at the document base) and
        // its continuation `at == off` can never be a document start (off is
        // strictly inside its document), so it always leaves a content branch.
        if it.excl && at == it.off {
            excluded = true;
            continue;
        }
        // THE END BRANCH: at is a document start - "this context ends a document"
        if ds.rank_start(at).1 == at {
            eof += 1;
            continue;
        }
        corpus_file
            .read_exact_at(&mut bytebuf, at)
            .unwrap_or_else(|e| die(&format!("consult: corpus read at {at}: {e}")));
        counts[bytebuf[0] as usize] += 1;
    }
    let mut width = eof;
    for (b, &c) in counts.iter().enumerate() {
        if c > 0 {
            out.push((b as u8, c));
            width += c;
        }
    }
    out.sort_unstable_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    (width, eof, excluded, false)
}

/// The boundary-aware TRIE layer: contexts within the member's depth that the
/// trie materialises are answered from the member directly - O(depth), any
/// width, no locate - because the trie was built boundary-aware (the law held
/// by construction there). Returns None when the trie cannot answer (context
/// deeper than the trie, node below support, trie absent or unaware).
fn trie_boundary_answer(
    trie: &crate::ctx::Trie<'_>,
    it: &ConsultItem,
    pat: &[u8],
    out: &mut Vec<(u8, u64)>,
) -> Option<(u64, u64, bool)> {
    if pat.len() as u64 > trie.depth {
        return None;
    }
    let node = trie.find(pat)?;
    out.clear();
    let mut width = trie.count(node);
    let mut eof = 0u64;
    for ch in trie.children(node) {
        match ch.byte() {
            None => eof = eof.max(ch.count),
            Some(b) => out.push((b, ch.count)),
        }
    }
    // --exclude-source: the own occurrence is always an in-document CONTENT
    // continuation (see boundary_answer), so it leaves its content branch; it
    // can never be the trie's END edge (off is strictly inside its document).
    let mut excluded = false;
    if it.excl {
        if let Some(slot) = out.iter().position(|&(b, _)| b == it.own) {
            if out[slot].1 > 0 {
                out[slot].1 -= 1;
                if out[slot].1 == 0 {
                    out.swap_remove(slot);
                }
                width -= 1;
                excluded = true;
            }
        }
    }
    out.sort_unstable_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    Some((width, eof, excluded))
}

/// `--doc-restrict`: the EXACT document-restricted branch histogram.
///
/// The note's D2 semantics: a match or branch count must never span a document
/// boundary - the interval of occurrences for a context is restricted to the
/// document containing the query position, exactly rather than probabilistically
/// (the old 0x1E spine cut only made spanning UNLIKELY, and broke on documents
/// that contain an internal 0x1E). Exactness costs enumeration: every occurrence
/// of the context is located and kept only if the match lies fully inside the
/// document; an occurrence whose continuation lands ON the document end now
/// counts on the END branch (the no-cross law: the next document's first byte is
/// not content), a match that SPANS past the end is a crossing match and is
/// dropped. Intervals wider than `cap` are REFUSED, never silently returned
/// unrestricted.
fn restricted_answer(
    cx: &mut Ctx<'_>,
    corpus_file: &File,
    it: &ConsultItem,
    pat: &[u8],
    cap: u64,
    text_len: u64,
    out: &mut Vec<(u8, u64)>,
) -> (u64, u64, bool) {
    use std::os::unix::fs::FileExt;
    out.clear();
    let sigma = cx.idx.reader.sigma;
    let absent = pat
        .iter()
        .any(|&b| sigma[b as usize] == 0 || cx.idx.total[sigma[b as usize] as usize] == 0);
    if absent {
        return (0, 0, false);
    }
    let work: Vec<u8> = pat.iter().rev().map(|&b| sigma[b as usize]).collect();
    let (l, r) = cx.search(&work);
    if r == l {
        return (0, 0, false);
    }
    if r - l > cap {
        die(&format!(
            "consult: --doc-restrict at position {}: the context's occurrence interval is \
             {} rows and --doc-restrict-cap is {cap}; exact document-restricted counting \
             enumerates the interval (one LF walk per occurrence), so a wider cap costs \
             proportionally. REFUSING to return a silently UNrestricted histogram: raise \
             --doc-restrict-cap, drop --doc-restrict for this window set, or query a longer \
             context",
            it.off,
            r - l
        ));
    }
    let mut counts = [0u64; 256];
    let mut eof = 0u64;
    let mut excluded = false;
    let ctxlen = pat.len() as u64;
    let mut bytebuf = [0u8; 1];
    for row in l..r {
        let (q, _) = cx.locate_steps(row);
        // fully inside the query position's document, continuation byte and all
        if q < it.base {
            continue;
        }
        let at = q + ctxlen;
        if at == text_len {
            eof += 1;
            continue;
        }
        if at == it.doc_next {
            // THE LAW: the context ends this document - the continuation is the
            // NEXT document's first byte and belongs to the END branch, never
            // to content (the old rule dropped the occurrence entirely)
            eof += 1;
            continue;
        }
        if at > it.doc_next {
            // the match itself spans past the document end: a CROSSING match,
            // excluded entirely
            continue;
        }
        // --exclude-source: the query position's own occurrence leaves before
        // any count is returned
        if it.excl && at == it.off {
            excluded = true;
            continue;
        }
        corpus_file
            .read_exact_at(&mut bytebuf, at)
            .unwrap_or_else(|e| die(&format!("consult: corpus read at {at}: {e}")));
        counts[bytebuf[0] as usize] += 1;
    }
    let sum: u64 = counts.iter().sum();
    for (b, &c) in counts.iter().enumerate() {
        if c > 0 {
            out.push((b as u8, c));
        }
    }
    out.sort_unstable_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    (sum + eof, eof, excluded)
}

/// The `branch_result` row for the same context, for `--strict`: it recomputes
/// the answer through the full `branches --serve` path and compares
/// (context_bytes, width, eof, every (byte,count) pair, in order).
fn consult_strict_check(
    cx: &mut Ctx<'_>,
    cand: &Candidates,
    pat: &[u8],
    answer: (u64, u64),
    branches: &[(u8, u64)],
) {
    let r = branch_result(cx, cand, None, None, 0, pat, 256);
    let width = r.r - r.l;
    let mut identical = width == answer.0 && r.eof == answer.1 && r.branches.len() == branches.len();
    if identical {
        for (a, b) in r.branches.iter().zip(branches.iter()) {
            if a != b {
                identical = false;
                break;
            }
        }
    }
    if !identical {
        die(&format!(
            "consult: STRICT MISMATCH: the one-query answer disagrees with branch_result for a \
             context of {} bytes:\n  fast: width={} eof={} branches={:?}\n  serve: width={} \
             l={} r={} eof={} branches={:?}",
            pat.len(),
            answer.0,
            answer.1,
            branches,
            width,
            r.l,
            r.r,
            r.eof,
            r.branches
        ));
    }
}

// ------------------------------------------------------------ the work unit

/// One position handed to a worker: its identity plus a slice of the batch's
/// byte buffer.
#[derive(Debug, Clone, Copy)]
struct ConsultItem {
    doc: u64,
    off: u64,
    /// the window's EFFECTIVE document base (explicit or resolved) - the row's
    /// `base` field, and the spine's hard lower bound (D1: cut by it, never a scan)
    base: u64,
    /// container document id from the document structure, or DOCID_ABSENT
    /// (no structure, or the position is beyond the container's text)
    docid: u64,
    /// the next document's start (or text_len): the document-restricted
    /// counting interval's upper bound
    doc_next: u64,
    /// the corpus byte AT the position (the own occurrence's branch byte);
    /// valid iff `excl`
    own: u8,
    ctx_off: u32,
    ctx_len: u16,
    /// The 1024-byte cap (or the document start) cut this context short.
    truncated: bool,
    /// This position is the last one of its window.
    win_end: bool,
    /// --doc-restrict applies to this row (structure present, position inside
    /// the container's text)
    restrict: bool,
    /// --exclude-source applies (position inside the container's text)
    excl: bool,
}

struct ConsultBatch {
    /// Global position index of `items[0]`.
    seq0: u64,
    items: Vec<ConsultItem>,
    bytes: Vec<u8>,
    /// Windows completed by the whole batch (the commit cursor).
    windows_after: u64,
    /// Window-file byte cursor after the last window this batch completed.
    windows_bytes_after: u64,
}

struct ConsultDone {
    seq0: u64,
    windows_after: u64,
    windows_bytes_after: u64,
    bytes: Vec<u8>,
    rows: u64,
    ends_window: bool,
    /// per-batch row counters: rows whose width excluded the position's own
    /// occurrence, rows answered by document-restricted enumeration, and rows
    /// whose histogram is the boundary-UNAWARE conflated answer (marked
    /// boundary_exact=0 in the row; see CONSULT.md §5)
    excluded: u64,
    restricted: u64,
    inexact: u64,
    /// the χ layer ledger of this batch (rows, nanos, histogram)
    chi_rows: [u64; CHI_LAYERS],
    chi_nanos: [u64; CHI_LAYERS],
    chi_hist: [u64; CHI_LAYERS * CHI_HIST_BUCKETS],
}

/// Encode one position's row (layout in CONSULT.md). Little-endian throughout.
fn encode_consult_row(
    out: &mut Vec<u8>,
    it: &ConsultItem,
    ctxlen: u16,
    width: u64,
    eof: u64,
    branches: &[(u8, u64)],
    excluded: bool,
    restricted: bool,
    boundary_exact: bool,
) {
    encode_consult_row_v(out, it, ctxlen, width, eof, branches, excluded, restricted, boundary_exact, false, false)
}

/// The row encoder. `v4` prefixes every record with a 1-byte KIND
/// (CONSULT_VERSION_V4, `--rows-v4`); a `forced` row whose single pair is
/// exactly (cont, width - eof) takes the compact FORCED kind and the flag's
/// bit 6 — no 9-byte pair on disc for the 98.7% zero-locate mass. The DEFAULT
/// (v3) output is byte-identical to today: no kind byte, no bit 6.
#[allow(clippy::too_many_arguments)]
fn encode_consult_row_v(
    out: &mut Vec<u8>,
    it: &ConsultItem,
    ctxlen: u16,
    width: u64,
    eof: u64,
    branches: &[(u8, u64)],
    excluded: bool,
    restricted: bool,
    boundary_exact: bool,
    v4: bool,
    forced: bool,
) {
    if v4
        && forced
        && branches.len() == 1
        && width > eof
        && branches[0].1 == width - eof
    {
        // KIND 1 (FORCED): ctxlen u16, flags u16, doc u64, off u64, width u64,
        // eof u64, cont u8, then the v2 tail. 62 bytes; the v3 form of the same
        // row is 73. The pair decodes to (cont, width - eof) by construction.
        let before = out.len();
        let flags: u16 = (if eof != 0 { 1 } else { 0 })
            | (if it.truncated { 2 } else { 0 })
            | (if excluded { 4 } else { 0 })
            | (if restricted { 8 } else { 0 })
            | (if boundary_exact { 0 } else { 16 })
            // bit 6 exists ONLY in v4 forced records: "this row's answer was
            // proven forced (the width-1 or one-step-verification layer)"
            | 64;
        out.push(ROW_KIND_FORCED);
        out.extend_from_slice(&ctxlen.to_le_bytes());
        out.extend_from_slice(&flags.to_le_bytes());
        out.extend_from_slice(&it.doc.to_le_bytes());
        out.extend_from_slice(&it.off.to_le_bytes());
        out.extend_from_slice(&width.to_le_bytes());
        out.extend_from_slice(&eof.to_le_bytes());
        out.push(branches[0].0);
        out.extend_from_slice(&it.docid.to_le_bytes());
        out.extend_from_slice(&it.base.to_le_bytes());
        out.extend_from_slice(&(it.off - it.base).to_le_bytes());
        debug_assert_eq!(out.len() - before, 62);
        return;
    }
    let before = out.len();
    if v4 {
        out.push(ROW_KIND_FULL);
    }
    let flags: u16 = (if eof != 0 { 1 } else { 0 })
        | (if it.truncated { 2 } else { 0 })
        | (if excluded { 4 } else { 0 })
        | (if restricted { 8 } else { 0 })
        // bit 4: the row's histogram is the boundary-UNAWARE conflated answer
        // (no structure, --cross-doc, or an interval past --boundary-cap). The
        // mark is IN THE ROW so a consumer can filter per row, never silently.
        | (if boundary_exact { 0 } else { 16 });
    out.extend_from_slice(&(branches.len() as u32).to_le_bytes());
    out.extend_from_slice(&ctxlen.to_le_bytes());
    out.extend_from_slice(&flags.to_le_bytes());
    out.extend_from_slice(&it.doc.to_le_bytes());
    out.extend_from_slice(&it.off.to_le_bytes());
    out.extend_from_slice(&width.to_le_bytes());
    out.extend_from_slice(&eof.to_le_bytes());
    for &(b, c) in branches {
        out.push(b);
        out.extend_from_slice(&c.to_le_bytes());
    }
    // v2 tail: docid / base / depth_in_doc, so one row answers both "what
    // continues this" and "inside which document" (the note's D2)
    out.extend_from_slice(&it.docid.to_le_bytes());
    out.extend_from_slice(&it.base.to_le_bytes());
    out.extend_from_slice(&(it.off - it.base).to_le_bytes());
    // the row layout's own invariant: 40 fixed bytes plus 9 per branch plus the
    // 24-byte v2 tail, which is what the reader in CONSULT.md is written against
    debug_assert_eq!(
        out.len() - before,
        ROW_FIXED_BYTES + 9 * branches.len() + ROW_TAIL_BYTES + if v4 { 1 } else { 0 }
    );
}

// ---------------------------------------------------------------- the command

pub fn consult_command(raw: &[std::ffi::OsString]) {
    let args: Vec<String> = raw
        .iter()
        .map(|s| s.to_string_lossy().into_owned())
        .collect();
    let mut container: Option<String> = None;
    let mut windows_path: Option<String> = None;
    let mut output: Option<String> = None;
    let mut corpus_path: Option<String> = None;
    let mut rss_gb: f64 = 8.0;
    let mut jobs: usize = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(4)
        .min(16);
    let mut resume = false;
    let mut left_default: u32 = CTX_MAX;
    let mut chunk_rows: u64 = 1 << 16;
    let mut cache_gb: Option<f64> = None;
    let mut stateless = false;
    let mut binary = false;
    let mut verify_prefix = true;
    let mut strict = std::env::var("XSA_CONSULT_STRICT").ok().as_deref() == Some("1");
    // D1/D3 flags: `--docs DIR` is the document-structure sidecar (seps.bin);
    // the container's own member m15 is used when present. `--exclude-source`
    // removes the position's own occurrence before anything is reported.
    // `--doc-restrict` opts a window set into EXACT document-restricted branch
    // counting (enumerated; see restricted_answer).
    let mut docs_dir: Option<String> = None;
    let mut exclude_source = false;
    let mut doc_restrict = false;
    let mut doc_restrict_cap = DOC_RESTRICT_CAP_DEFAULT;
    // THE NO-CROSS LAW AT QUERY TIME: the default histogram is boundary-aware
    // (exactly - trie layer or enumeration - and marked boundary_exact=0 with a
    // per-row flag bit and a journal line wherever it could not be). `--cross-doc`
    // opts the WHOLE run back into the conflated answer; `--boundary-cap` is the
    // enumeration cost knob; `--boundary-strict` turns the marked fallback into
    // a refusal for exactness-or-nothing consumers.
    let mut cross_doc = false;
    let mut boundary_cap = BOUNDARY_CAP_DEFAULT;
    let mut boundary_strict = false;
    // opt-in row file v4: the compact FORCED row kind (see CONSULT.md). The
    // default output stays v3, byte-identical to today.
    let mut rows_v4 = false;
    let mut i = 0usize;
    while i < args.len() {
        let a = args[i].as_str();
        macro_rules! val {
            ($name:expr) => {{
                i += 1;
                match args.get(i) {
                    Some(v) => v.clone(),
                    None => die(&format!("consult: {} needs a value", $name)),
                }
            }};
        }
        match a {
            "--container" => container = Some(val!("--container")),
            "--windows" => windows_path = Some(val!("--windows")),
            "--output" => output = Some(val!("--output")),
            "--corpus" => corpus_path = Some(val!("--corpus")),
            "--rss-gb" => {
                let v = val!("--rss-gb");
                rss_gb = v
                    .parse()
                    .unwrap_or_else(|_| die(&format!("consult: --rss-gb '{v}' is not a number")));
            }
            "-j" | "--jobs" => {
                let v = val!("--jobs");
                jobs = v
                    .parse()
                    .unwrap_or_else(|_| die(&format!("consult: --jobs '{v}' is not a count")));
            }
            "--left" => {
                let v = val!("--left");
                left_default = v
                    .parse()
                    .unwrap_or_else(|_| die(&format!("consult: --left '{v}' is not a byte count")));
            }
            "--cache-gb" => {
                let v = val!("--cache-gb");
                cache_gb = Some(
                    v.parse()
                        .unwrap_or_else(|_| die(&format!("consult: --cache-gb '{v}' is not a number"))),
                );
            }
            "--chunk-rows" => {
                let v = val!("--chunk-rows");
                chunk_rows = v.parse().unwrap_or_else(|_| {
                    die(&format!("consult: --chunk-rows '{v}' is not a count"))
                });
            }
            "--stateless" => stateless = true,
            "--resume" => resume = true,
            "--bin" | "--binary" => binary = true,
            "--jsonl" => binary = false,
            "--no-verify-prefix" => verify_prefix = false,
            "--sep-cut" | "--no-sep-cut" => {
                // REMOVED (D1): a 0x1E scan is boundary INFERENCE from content
                // bytes, and it was wrong on the 78,715 documents that contain an
                // internal 0x1E. The spine is cut at the DOCUMENT BASE - explicit
                // in the window or resolved from the structure - never at a byte.
                die(&format!(
                    "consult: '{a}' is REMOVED: the 0x1E spine cut inferred boundaries from \
                     content bytes and broke on documents with an internal 0x1E. The spine is \
                     now cut at the document base (window 'base', or 'doc' resolved from \
                     member m15 / --docs DIR)"
                ));
            }
            "--docs" => docs_dir = Some(val!("--docs")),
            "--exclude-source" => exclude_source = true,
            "--doc-restrict" => doc_restrict = true,
            "--cross-doc" => cross_doc = true,
            "--boundary-cap" => {
                let v = val!("--boundary-cap");
                boundary_cap = v.parse().unwrap_or_else(|_| {
                    die(&format!("consult: --boundary-cap '{v}' is not a count"))
                });
            }
            "--boundary-strict" => boundary_strict = true,
            "--rows-v4" => rows_v4 = true,
            "--doc-restrict-cap" => {
                let v = val!("--doc-restrict-cap");
                doc_restrict_cap = v.parse().unwrap_or_else(|_| {
                    die(&format!("consult: --doc-restrict-cap '{v}' is not a count"))
                });
            }
            "--strict" => strict = true,
            other => {
                if other.starts_with('-') {
                    die(&format!("consult: unknown option '{other}'"));
                }
                if container.is_none() {
                    container = Some(other.to_string());
                } else {
                    die(&format!("consult: unexpected positional argument '{other}'"));
                }
            }
        }
        i += 1;
    }
    let container = container.unwrap_or_else(|| die("consult: need a container (--container FILE)"));
    let windows_path =
        windows_path.unwrap_or_else(|| die("consult: need a window stream (--windows FILE)"));
    let output = output.unwrap_or_else(|| die("consult: need an output file (--output FILE)"));
    if jobs == 0 {
        die("consult: --jobs must be at least 1");
    }
    if chunk_rows == 0 {
        die("consult: --chunk-rows must be at least 1");
    }
    if left_default == 0 || left_default > CTX_MAX {
        die(&format!("consult: --left must be in 1..={CTX_MAX}"));
    }
    if !(rss_gb.is_finite() && rss_gb >= 1.0) {
        die(
            "consult: --rss-gb must be a finite number >= 1 (below 1 GB the container's own \
             directories cannot be held resident)",
        );
    }
    // The corpus: explicit, or the container's own name with .txt. Both are
    // tried before refusing; a guess that does not exist is not a guess.
    let corpus_path = match corpus_path {
        Some(p) => p,
        None => {
            let guess = match container.strip_suffix(".sxi2") {
                Some(stem) => format!("{stem}.txt"),
                None => format!("{container}.txt"),
            };
            if std::path::Path::new(&guess).exists() {
                eprintln!("CONSULT_NOTE corpus_defaulted path={guess} (pass --corpus to be explicit)");
                guess
            } else {
                die(&format!(
                    "consult: need the corpus text (--corpus FILE); {guess} does not exist and a \
                     position's context bytes are corpus bytes, which the container does not store"
                ))
            }
        }
    };
    let journal_path = format!("{output}.journal");
    let row_version = if rows_v4 { CONSULT_VERSION_V4 } else { CONSULT_VERSION };

    // ---------------- resume: discard the torn tail, verify the committed prefix
    let mut start = ConsultCommit {
        seq: 0,
        windows_done: 0,
        windows_bytes_done: 0,
        positions_done: 0,
        output_bytes: 0,
        rows_sha256: String::new(),
    };
    let mut hasher = Sha256::new();
    let out_header_len = (CONSULT_MAGIC.len() + 8) as u64;
    let mut out_file: File;
    if resume {
        let found = read_last_commit(&journal_path);
        let (commit, lines, discarded) = match found {
            Some(t) => t,
            None => die(&format!(
                "consult: --resume was asked for, but {journal_path} holds no complete commit record"
            )),
        };
        let m = std::fs::metadata(&output).unwrap_or_else(|e| {
            die(&format!(
                "consult: --resume: output {output} is not there to resume: {e}"
            ))
        });
        if m.len() < commit.output_bytes {
            die(&format!(
                "consult: --resume: output {output} is {} bytes but the journal's last commit (seq \
                 {}) declares {} - the output LOST committed bytes; refusing to resume",
                m.len(),
                commit.seq,
                commit.output_bytes
            ));
        }
        out_file = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(&output)
            .unwrap_or_else(|e| die(&format!("consult: open {output}: {e}")));
        let torn = m.len() - commit.output_bytes;
        if torn > 0 {
            out_file
                .set_len(commit.output_bytes)
                .unwrap_or_else(|e| die(&format!("consult: truncate {output}: {e}")));
        }
        out_file.seek(std::io::SeekFrom::Start(0))
            .unwrap_or_else(|e| die(&format!("consult: seek {output}: {e}")));
        // THE ROW VERSION IS A RESUME INVARIANT: a v3 tail cannot continue a v4
        // file (the kind byte changes every record length) and vice versa.
        {
            let mut head = [0u8; 16];
            out_file.read_exact(&mut head).unwrap_or_else(|e| {
                die(&format!("consult: read header of {output}: {e}"))
            });
            if &head[..8] != CONSULT_MAGIC {
                die(&format!("consult: {output} does not carry the {CONSULT_MAGIC:?} magic"));
            }
            let v = u32::from_le_bytes(head[8..12].try_into().unwrap());
            if v != row_version {
                die(&format!(
                    "consult: --resume: {output} is a row file v{v} but this run writes v{row_version} \
                 (pass or drop --rows-v4 to match); the row kinds differ, so the files cannot be \
                 continued into each other",
                ));
            }
        }
        out_file
            .seek(std::io::SeekFrom::Start(0))
            .unwrap_or_else(|e| die(&format!("consult: seek {output}: {e}")));
        let mut remaining = commit.output_bytes;
        let mut chunk = vec![0u8; 1 << 20];
        while remaining > 0 {
            let want = (chunk.len() as u64).min(remaining) as usize;
            out_file
                .read_exact(&mut chunk[..want])
                .unwrap_or_else(|e| die(&format!("consult: read committed prefix of {output}: {e}")));
            hasher.update(&chunk[..want]);
            remaining -= want as u64;
        }
        let got = format!("{:x}", hasher.clone().finalize());
        if verify_prefix && got != commit.rows_sha256 {
            die(&format!(
                "consult: --resume: the committed prefix of {output} does NOT match the journal:\n  \
                 journal rows_sha256={}\n  prefix  rows_sha256={}\nrefusing to continue on top of a \
                 corrupted prefix (pass --no-verify-prefix to skip this check knowingly)",
                commit.rows_sha256, got
            ));
        }
        out_file
            .seek(std::io::SeekFrom::Start(commit.output_bytes))
            .unwrap_or_else(|e| die(&format!("consult: seek {output}: {e}")));
        let mut j = ConsultJournal::open_append(&journal_path);
        j.line(&format!(
            "CONSULT_RESUME seq={} journal_lines={} journal_torn_tail_lines={} truncated_bytes={} \
             prefix_verified={} windows_done={} windows_bytes_done={} positions_done={} \
             output_bytes={} rows_sha256={}",
            commit.seq,
            lines,
            discarded,
            torn,
            u8::from(verify_prefix),
            commit.windows_done,
            commit.windows_bytes_done,
            commit.positions_done,
            commit.output_bytes,
            commit.rows_sha256
        ));
        eprintln!(
            "CONSULT_RESUME from_seq={} windows_done={} positions_done={} output_bytes={} \
             truncated_bytes={} prefix_verified={}",
            commit.seq,
            commit.windows_done,
            commit.positions_done,
            commit.output_bytes,
            torn,
            u8::from(verify_prefix)
        );
        start = commit;
    } else {
        let existing = std::fs::metadata(&output).map(|m| m.len()).unwrap_or(0);
        if existing > 0 {
            die(&format!(
                "consult: output {output} already exists and is not empty; pass --resume to continue \
                 it, or remove it and {journal_path}"
            ));
        }
        out_file = std::fs::OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .open(&output)
            .unwrap_or_else(|e| die(&format!("consult: create {output}: {e}")));
        let _ = std::fs::remove_file(&journal_path);
        let mut header = Vec::with_capacity(out_header_len as usize);
        header.extend_from_slice(CONSULT_MAGIC);
        header.extend_from_slice(&row_version.to_le_bytes());
        header.extend_from_slice(&0u32.to_le_bytes()); // reserved
        out_file
            .write_all(&header)
            .unwrap_or_else(|e| die(&format!("consult: write header to {output}: {e}")));
        out_file
            .sync_data()
            .unwrap_or_else(|e| die(&format!("consult: fsync {output}: {e}")));
        hasher.update(&header);
        start.output_bytes = out_header_len;
    }

    // ---------------- open the query kernel and set the RSS budget
    let reader = Reader::open(&container).unwrap_or_else(|e| die(&e));
    let index_n = reader.n;
    let index_r = reader.r;
    let cap_bytes = (rss_gb * (1u64 << 30) as f64) as u64;
    // THE ARENA SPLIT. The budget is spent on two things: the decoded-block
    // arena (anonymous, fixed, never dropped) and the resident set of the
    // container mapping (page-cache backed, droppable by the governor). A
    // cache-build walks a dense set of BWT rows, so the arena earns its share;
    // it is an EIGHTH of the budget by default and can be pinned with
    // --cache-gb. Whatever it is, it is FIXED at open: `decoded_bytes` stays 0.
    let arena_bytes = if stateless { 0 } else { match cache_gb {
        Some(g) => (g * (1u64 << 30) as f64) as usize,
        None => (cap_bytes / 8) as usize,
    }
    .clamp(4 << 20, 64usize << 30) };
    let mut index =
        Index::open_with_cache(reader, jobs, arena_bytes).unwrap_or_else(|e| die(&e));
    index.set_rss_cap(cap_bytes);
    if stateless {
        eprintln!("CONSULT_STATELESS shared_cache=0 rss_law=immutable_file_mapping_plus_bounded_workers rss_gb_is_not_a_mapping_cap");
    }
    let cand = Candidates::of(&index);
    let mut corpus = ConsultCorpus::open(&corpus_path);
    // THE TEXT IS THE CONTAINER'S TEXT, NOT THE FILE'S LENGTH. A container built
    // over a PREFIX of a bigger file (the pile's head-100 GB container is the
    // first 100e9 bytes of a 1.3 TB pile.txt) declares its text length as n-1 -
    // one virtual END is appended - so positions are bounded by that, and the EOF
    // test is taken against ITS last byte, never against whatever else the corpus
    // file happens to contain after it.
    if index_n < 2 {
        die(&format!("consult: container declares n={index_n}, so its text is empty"));
    }
    let text_len = index_n - 1;
    if corpus.len < text_len {
        die(&format!(
            "consult: corpus {} is {} bytes but the container's text is {text_len} bytes; the \
             context bytes of a position are corpus bytes, so the corpus must contain them all",
            corpus_path, corpus.len
        ));
    }
    if corpus.len > text_len {
        eprintln!(
            "CONSULT_NOTE corpus_longer_than_text corpus_bytes={} container_text_bytes={text_len} \
             (only the container's text is addressable)",
            corpus.len
        );
    }
    // The structural END can only ever follow a context that IS a suffix of the
    // text, so the EOF flag is decided by a comparison against the text's last
    // CTX_MAX bytes. Read once: a fixed 1 KB, not a per-position read.
    let mut corpus_tail = vec![0u8; (text_len.min(u64::from(CTX_MAX))) as usize];
    if !corpus_tail.is_empty() {
        corpus.read_into(text_len - corpus_tail.len() as u64, &mut corpus_tail);
    }

    // ---------------- the document structure (D1/D2): metadata, never bytes
    // Sources, in precedence order: the container's own member m15 (SXI2 v10,
    // the deployment-grade arm) and the `--docs DIR` sidecar (seps.bin). Both
    // may be present; they are then CROSS-CHECKED (select at 64 random doc ids
    // and rank at 64 random positions) and a disagreement is a hard error -
    // two authorities that disagree about a boundary cannot both be obeyed.
    let docs_from_member = crate::docstarts::DocStarts::from_container(&index.reader);
    let docs_from_sidecar = docs_dir.as_ref().map(|dir| {
        let ds = crate::docstarts::DocStarts::from_xsep(std::path::Path::new(dir), text_len);
        if ds.text_len != text_len {
            die(&format!("consult: --docs {dir}: the map's text length {} is not this container's text length {text_len}", ds.text_len));
        }
        ds
    });
    if let (Some(a), Some(b)) = (&docs_from_member, &docs_from_sidecar) {
        let n = a.docs.min(b.docs);
        if n == 0 {
            die("consult: document structure with zero documents");
        }
        let mut state = 0x9E37_79B9_7F4A_7C15u64;
        let mut mix = |state: &mut u64| {
            *state = state.wrapping_add(0x9E37_79B9_7F4A_7C15);
            let mut z = *state;
            z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
            z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
            z ^ (z >> 31)
        };
        for _ in 0..64 {
            let d = mix(&mut state) % n;
            let (sa, sb) = (a.select(d), b.select(d));
            if sa != sb {
                die(&format!(
                    "consult: the container's member m15 and --docs {} DISAGREE: \
                     select({d}) = {sa} vs {sb}; boundaries are metadata and two \
                     authorities that disagree cannot both be obeyed",
                    docs_dir.clone().unwrap()
                ));
            }
            if sa >= text_len {
                break; // past the text: the rest of the map is not this container's
            }
        }
        for _ in 0..64 {
            let p = mix(&mut state) % text_len;
            if a.rank_start(p) != b.rank_start(p) {
                die(&format!(
                    "consult: the container's member m15 and --docs {} DISAGREE about the \
                     document containing position {p}",
                    docs_dir.clone().unwrap()
                ));
            }
        }
    }
    let member_present = docs_from_member.is_some();
    let sidecar_present = docs_from_sidecar.is_some();
    let docs = docs_from_sidecar.or(docs_from_member);
    let docs_source = match (sidecar_present, member_present) {
        (true, true) => "sidecar+member-cross-checked",
        (true, false) => "sidecar",
        (false, true) => "member-m15",
        (false, false) => "absent",
    };
    if let Some(ds) = docs.as_ref() {
        eprintln!(
            "CONSULT_NOTE docs_source={} docs={} text_bytes={} map_corpus_bytes={} \
             (boundaries are METADATA: bases resolve from the structure, spines cut at \
             the document base, and no content byte is ever scanned for a boundary)",
            docs_source,
            ds.docs,
            ds.text_len,
            ds.corpus_bytes,
        );
    }
    let docs_ref = docs.as_ref();
    if doc_restrict && docs_ref.is_none() {
        die("consult: --doc-restrict needs a document structure (member m15 or --docs DIR)");
    }
    if doc_restrict_cap == 0 {
        die("consult: --doc-restrict-cap must be at least 1");
    }
    if boundary_cap == 0 {
        die("consult: --boundary-cap must be at least 1");
    }
    if cross_doc && boundary_strict {
        die(
            "consult: --cross-doc and --boundary-strict contradict each other: the first asks for \
             the conflated crossing-inclusive answer, the second refuses it",
        );
    }
    // THE BOUNDARY-AWARE TRIE (m20): contexts within its depth answer from the
    // member itself - exact, any width, no locate - when the member was built
    // boundary-aware. An UNAWARE member is not consulted for the default (its
    // counts conflate crossing occurrences) and is named in the journal.
    let trie_member: Option<&[u8]> = index.reader.member_bytes(crate::ctx::TRIE_CODEC);
    let trie_owned = trie_member.map(crate::ctx::Trie::load);
    let trie_ref = trie_owned.as_ref();
    let trie_arm = match trie_ref {
        Some(t) if t.boundary_aware == 1 => format!("m20-depth{}-aware", t.depth),
        Some(_) => "m20-unaware".to_string(),
        None => "absent".to_string(),
    };
    if boundary_strict && docs_ref.is_none() {
        die(&format!(
            "consult: --boundary-strict was asked for, but there is no document structure to be \
             exact WITH: the container is SXI2 v{} and carries no member m15, and no --docs DIR was \
             given. Every row would be the conflated answer. Build with --docs-map DIR (or `xsa \
             docstarts embed`), pass --docs DIR, or drop --boundary-strict",
            index.reader.version,
        ));
    }
    if docs_ref.is_none() && !cross_doc {
        eprintln!(
            "CONSULT_NOTE boundary=absent: every row of this run is marked boundary_exact=0 \
             (the conflated histogram: a match or branch count MAY cross a document boundary). \
             Give a structure (member m15 via --docs-map at build time, or --docs DIR) for the \
             no-cross default, or pass --cross-doc to acknowledge it",
        );
    }

    let mut src = WindowSource::open(&windows_path, binary);
    if start.windows_bytes_done > 0 {
        src.seek_to(start.windows_bytes_done);
    }

    // ---------------- THE SEPARATOR-FRAMING LAW, verified per run
    // The zero-locate raw layers (width1/forced/rawfork below) are authorized
    // ONLY when every in-text document start is preceded by one 0x1E byte —
    // verified EXHAUSTIVELY here (a sample cannot authorize it: one unframed
    // boundary anywhere would let a crossing occurrence hide inside a forced
    // row). Containers without the property (adversarial fixtures whose
    // boundaries sit mid-content, HPRC-style maps, ...) keep the exact
    // m15-crossing-filtered enumeration for every row: exactness is never
    // traded for speed. Journaled either way.
    let mut sep_framing_verified = false;
    // (checked, violations, first_bad_start, seconds) for the journal receipt
    let mut sep_framing_receipt = (0u64, 0u64, 0u64, 0f64);
    if let Some(ds) = docs_ref {
        if !cross_doc {
            sep_framing_receipt = verify_sep_framing(&corpus_path, ds, text_len, jobs);
            sep_framing_verified = sep_framing_receipt.1 == 0 && sep_framing_receipt.0 > 0;
            eprintln!(
                "CONSULT_SEPFRAMING checked={} violations={} verified={} seconds={:.3} \
                 (the zero-locate raw layers are {})",
                sep_framing_receipt.0,
                sep_framing_receipt.1,
                u8::from(sep_framing_verified),
                sep_framing_receipt.3,
                if sep_framing_verified { "ARMED" } else { "OFF — every row enumerates exactly" }
            );
        }
    }

    let mut journal = ConsultJournal::open_append(&journal_path);
    if start.seq == 0 {
        journal.line(&format!(
            "CONSULT_OPEN container={} container_bytes={} index_n={} index_r={} corpus={} \
             corpus_bytes={} text_bytes={} output={} format=rows version={} jobs={} rss_cap_bytes={} cap_gb={} \
             chunk_rows={} left_default={} windows={} windows_bytes={} windows_format={} \
             batch_positions={} arena_bytes={} cache_bytes={} index_rss_cap_bytes={} \
             docs_source={} docs={} exclude_source={} doc_restrict={} doc_restrict_cap={} \
             boundary_mode={} boundary_cap={} boundary_strict={} trie={} sep_framing_verified={} \
             law=no_match_may_cross_a_document_boundary spine_cut=document_base",
            container,
            std::fs::metadata(&container).map(|m| m.len()).unwrap_or(0),
            index_n,
            index_r,
            corpus_path,
            corpus.len,
            text_len,
            output,
            row_version,
            jobs,
            cap_bytes,
            rss_gb,
            chunk_rows,
            left_default,
            windows_path,
            src.len,
            if binary { "xbatch" } else { "jsonl" },
            BATCH_POSITIONS,
            arena_bytes,
            index.cache_bytes(),
            index.rss_cap(),
            docs_source,
            docs.as_ref().map(|d| d.docs).unwrap_or(0),
            u8::from(exclude_source),
            u8::from(doc_restrict),
            doc_restrict_cap,
            if cross_doc { "cross-doc" } else { "no-cross-default" },
            boundary_cap,
            u8::from(boundary_strict),
            trie_arm,
            u8::from(sep_framing_verified),
        ));
    }
    // THE SEPFRAMING receipt rides right after the open line
    if docs_ref.is_some() && !cross_doc {
        let (checked, violations, first_bad, secs) = sep_framing_receipt;
        let mut j = ConsultJournal::open_append(&journal_path);
        j.line(&format!(
            "CONSULT_SEPFRAMING framing_byte=0x1e checked={} violations={} first_bad_start={} \
             verified={} seconds={secs:.3} \
             law=raw_equals_boundary_when_context_has_no_framing_byte \
             note=\"verified=0 disables the zero-locate raw layers for the whole run; every row then \
             takes the exact m15-crossing-filtered enumeration\"",
            checked, violations, first_bad, u8::from(sep_framing_verified)
        ));
    }
    eprintln!(
        "CONSULT_READY container={screen_container} candidates={} jobs={jobs} cap_gb={rss_gb} \
         resume_from_positions={} windows_done={}",
        cand.bytes.len(),
        start.positions_done,
        start.windows_done,
        screen_container = container,
    );

    let started = Instant::now();
    let backpressure = std::sync::atomic::AtomicU64::new(0);
    let backpressure_ref = &backpressure;

    // ---------------- the bounded pipeline
    let (permit_tx, permit_rx) = std::sync::mpsc::sync_channel::<()>(jobs * 8);
    for _ in 0..jobs * 8 {
        permit_tx.send(()).unwrap();
    }
    let mut req_tx = Vec::with_capacity(jobs);
    let mut req_rx = Vec::with_capacity(jobs);
    for _ in 0..jobs {
        let (t, r) = std::sync::mpsc::sync_channel::<ConsultBatch>(2);
        req_tx.push(t);
        req_rx.push(r);
    }
    let (res_tx, res_rx) = std::sync::mpsc::sync_channel::<ConsultDone>(jobs * 8);

    let idx_ref = &index;
    let cand_ref = &cand;
    let corpus_tail_ref = &corpus_tail[..];
    let corpus_path_for_threads = corpus_path.as_str();
    let doc_restrict_cap_for_threads = doc_restrict_cap;
    let text_len_for_threads = text_len;
    let boundary_cap_for_threads = boundary_cap;
    let cross_doc_for_threads = cross_doc;
    let boundary_strict_for_threads = boundary_strict;
    let sep_framing_for_threads = sep_framing_verified;
    let rows_v4_for_threads = rows_v4;
    let docs_for_threads: Option<&crate::docstarts::DocStarts> = docs_ref;
    let trie_for_threads: Option<&crate::ctx::Trie<'_>> = match trie_ref {
        Some(t) if t.boundary_aware == 1 => Some(t),
        _ => None,
    };
    let start_seq = start.seq;
    let start_positions = start.positions_done;
    let journal_for_writer = journal_path.clone();
    let journal_for_threads = journal_path.clone();
    let output_for_threads = output.clone();

    let mut totals = (
        0u64,
        0u64,
        0u64,
        0u64,
        String::new(),
        0u64,
        0u64,
        0u64,
        0u64,
        0u64,
        0u64,
        [0u64; CHI_LAYERS],
        [0u64; CHI_LAYERS],
        [0u64; CHI_LAYERS * CHI_HIST_BUCKETS],
    );
    std::thread::scope(|scope| {
        // ---- writer: owns the output file, the chunk commits and the sha ledger
        let writer = scope.spawn(move || {
            let mut j = ConsultJournal::open_append(&journal_for_writer);
            let mut out = BufWriter::with_capacity(1 << 20, out_file);
            let mut pending_rows: u64 = 0;
            let mut pending_bytes: u64 = 0;
            let mut output_bytes = start.output_bytes;
            let mut positions_done = start_positions;
            let mut windows_done_local = start.windows_done;
            let mut windows_bytes_done = start.windows_bytes_done;
            let mut windows_pending = start.windows_done;
            let mut windows_bytes_pending = start.windows_bytes_done;
            let mut seq = start_seq;
            let mut order = std::collections::BTreeMap::new();
            let mut next_seq0 = start_positions;
            let mut max_rss = 0u64;
            let mut last_rss_ts = Instant::now() - std::time::Duration::from_secs(10);
            let mut commits = 0u64;
            let mut excluded_rows = 0u64;
            let mut restricted_rows = 0u64;
            let mut boundary_inexact_rows = 0u64;
            let mut chi_rows = [0u64; CHI_LAYERS];
            let mut chi_nanos = [0u64; CHI_LAYERS];
            let mut chi_hist = [0u64; CHI_LAYERS * CHI_HIST_BUCKETS];
            while let Ok(d) = res_rx.recv() {
                excluded_rows += d.excluded;
                restricted_rows += d.restricted;
                boundary_inexact_rows += d.inexact;
                for i in 0..CHI_LAYERS {
                    chi_rows[i] += d.chi_rows[i];
                    chi_nanos[i] += d.chi_nanos[i];
                }
                for i in 0..CHI_LAYERS * CHI_HIST_BUCKETS {
                    chi_hist[i] += d.chi_hist[i];
                }
                order.insert(d.seq0, d);
                while let Some(d) = order.remove(&next_seq0) {
                    if d.rows > 0 {
                        out.write_all(&d.bytes)
                            .unwrap_or_else(|e| die(&format!("consult: write rows: {e}")));
                        hasher.update(&d.bytes);
                        pending_rows += d.rows;
                        pending_bytes += d.bytes.len() as u64;
                        positions_done = d.seq0 + d.rows;
                    }
                    next_seq0 = d.seq0 + d.rows;
                    let at_boundary = d.ends_window;
                    windows_pending = d.windows_after;
                    windows_bytes_pending = d.windows_bytes_after;
                    // A CHUNK IS COMMITTED HERE, INSIDE THE DRAIN, at the exact
                    // batch that ENDS a window: the byte cursor and the
                    // whole-window cursor therefore describe the same prefix.
                    // (Checking this after the drain instead made the commit
                    // depend on how many batches one wakeup happened to drain,
                    // which is a scheduling detail - a cache build must not have
                    // its commit cadence decided by the scheduler.)
                    if at_boundary && pending_rows >= chunk_rows {
                        out.flush()
                            .unwrap_or_else(|e| die(&format!("consult: flush rows: {e}")));
                        out.get_ref().sync_data().unwrap_or_else(|e| {
                            die(&format!("consult: fdatasync {output_for_threads}: {e}"))
                        });
                        output_bytes += pending_bytes;
                        let digest = format!("{:x}", hasher.clone().finalize());
                        seq += 1;
                        commits += 1;
                        windows_done_local = windows_pending;
                        windows_bytes_done = windows_bytes_pending;
                        j.line(&format!(
                            "CONSULT_COMMIT seq={} windows_done={} windows_bytes_done={} \
                             positions_done={} output_bytes={} rows_sha256={}",
                            seq,
                            windows_done_local,
                            windows_bytes_done,
                            positions_done,
                            output_bytes,
                            digest
                        ));
                        pending_rows = 0;
                        pending_bytes = 0;
                    }
                    let _ = permit_tx.send(());
                }
                let now = Instant::now();
                if now.duration_since(last_rss_ts).as_secs_f64() >= 1.0 {
                    last_rss_ts = now;
                    if let Some(rss) = consult_resident_bytes() {
                        max_rss = max_rss.max(rss);
                        j.line(&format!(
                            "CONSULT_RSS rss_gb={:.3} rss_bytes={} cap_gb={:.3} positions={} \
                             reorder_batches={} pending_rows={} backpressure_events={}",
                            gb(rss),
                            rss,
                            rss_gb,
                            positions_done,
                            order.len(),
                            pending_rows,
                            backpressure_ref.load(std::sync::atomic::Ordering::Relaxed)
                        ));
                    }
                }
            }
            if !order.is_empty() {
                die(&format!(
                    "consult: writer saw {} out-of-order batches with no sender left; the row order \
                     would have a hole at position {next_seq0}",
                    order.len()
                ));
            }
            // FINAL COMMIT: whatever is left is flushed, fsynced and journaled,
            // so the DONE sha256 is the sha256 of a COMMITTED prefix.
            out.flush()
                .unwrap_or_else(|e| die(&format!("consult: flush rows: {e}")));
            out.get_ref()
                .sync_data()
                .unwrap_or_else(|e| die(&format!("consult: fdatasync {output_for_threads}: {e}")));
            output_bytes += pending_bytes;
            if pending_rows > 0 || pending_bytes > 0 || commits == 0 {
                seq += 1;
                windows_done_local = windows_pending;
                windows_bytes_done = windows_bytes_pending;
                j.line(&format!(
                    "CONSULT_COMMIT seq={} windows_done={} windows_bytes_done={} positions_done={} \
                     output_bytes={} rows_sha256={}",
                    seq,
                    windows_done_local,
                    windows_bytes_done,
                    positions_done,
                    output_bytes,
                    format!("{:x}", hasher.clone().finalize())
                ));
            }
            if let Some(rss) = consult_resident_bytes() {
                max_rss = max_rss.max(rss);
            }
            (
                positions_done,
                windows_done_local,
                windows_bytes_done,
                output_bytes,
                format!("{:x}", hasher.finalize()),
                seq,
                max_rss,
                commits,
                excluded_rows,
                restricted_rows,
                boundary_inexact_rows,
                chi_rows,
                chi_nanos,
                chi_hist,
            )
        });

        // ---- workers: one Ctx each, one query per position, encode in place
        let mut worker_handles = Vec::with_capacity(jobs);
        for rx in req_rx {
            let tx = res_tx.clone();
            worker_handles.push(scope.spawn(move || {
                let arena = std::cell::RefCell::new(WorkerArena::new(idx_ref).unwrap());
                let mut cx = if stateless { idx_ref.worker_ctx(&arena) } else { idx_ref.ctx() };
                let mut pairs: Vec<(u8, u64)> = Vec::with_capacity(256);
                let mut out = Vec::with_capacity(BATCH_POSITIONS * 8 * 16);
                // --doc-restrict rows read the corpus directly (the byte after each
                // in-document occurrence): a private pread handle, never the
                // producer's windowed mapping
                let corpus_file = std::fs::File::open(&corpus_path_for_threads)
                    .unwrap_or_else(|e| die(&format!("consult: open corpus {corpus_path_for_threads}: {e}")));
                let mut excluded_rows = 0u64;
                let mut restricted_rows = 0u64;
                let mut boundary_inexact_rows = 0u64;
                let mut work: Vec<u8> = Vec::with_capacity(CTX_MAX as usize);
                while let Ok(batch) = rx.recv() {
                    out.clear();
                    let mut bl = ChiLedger::new();
                    let excl0 = excluded_rows;
                    let restr0 = restricted_rows;
                    let inex0 = boundary_inexact_rows;
                    for it in &batch.items {
                        let t0 = Instant::now();
                        let lo = it.ctx_off as usize;
                        let ctx = &batch.bytes[lo..lo + it.ctx_len as usize];
                        if ctx.is_empty() {
                            // EMPTY CONTEXT IS AN EMPTY HISTOGRAM: the cache
                            // contract says a zero-length context has no counts
                            // and no EOF, and the global corpus histogram is NOT
                            // its answer (c14_branch_serve.select: `if k else
                            // dict(width=0, eof=0, branches=[])`). A zero-length
                            // match cannot cross anything: the row is exact.
                            encode_consult_row_v(&mut out, it, 0, 0, 0, &[], false, false, true, rows_v4_for_threads, false);
                            bl.charge(CHI_EMPTY, t0.elapsed().as_nanos() as u64);
                            continue;
                        }
                        if it.restrict {
                            // D2, exact: branch counts restricted to the document
                            // containing the position (enumerated; the own
                            // occurrence leaves first when --exclude-source)
                            let (width, eof, excluded) = restricted_answer(
                                &mut cx,
                                &corpus_file,
                                it,
                                ctx,
                                doc_restrict_cap_for_threads,
                                text_len_for_threads,
                                &mut pairs,
                            );
                            restricted_rows += 1;
                            if excluded {
                                excluded_rows += 1;
                            }
                            encode_consult_row_v(&mut out, it, it.ctx_len, width, eof, &pairs, excluded, true, true, rows_v4_for_threads, false);
                            bl.charge(CHI_RESTRICT, t0.elapsed().as_nanos() as u64);
                            continue;
                        }
                        // THE NO-CROSS DEFAULT, in layers. `--cross-doc` is the
                        // explicit run-wide opt-in to the legacy conflated
                        // histogram; without it the default is exact wherever a
                        // structure lets it be, and MARKED (boundary_exact=0)
                        // wherever it cannot be - never silent, never fatal
                        // (except under --boundary-strict).
                        //
                        // THE χ-PROPORTIONAL CASCADE: layer 1 answers from the
                        // trie member; the raw layers (width1 / forced / rawfork)
                        // answer by pure rank arithmetic with ZERO LOCATE under
                        // the separator-framing law; everything else falls to
                        // the exact m15-crossing-filtered enumeration.
                        let mut marked_case = false; // this row's conflated fallback is the MARKED arm
                        if !cross_doc_for_threads {
                            if let Some(ds) = docs_for_threads {
                                // layer 1: the boundary-aware trie answers any
                                // width within its depth, no locate at all
                                if let Some(trie) = trie_for_threads {
                                    if let Some((width, eof, excluded)) =
                                        trie_boundary_answer(trie, it, ctx, &mut pairs)
                                    {
                                        if excluded {
                                            excluded_rows += 1;
                                        }
                                        encode_consult_row_v(&mut out, it, it.ctx_len, width, eof, &pairs, excluded, false, true, rows_v4_for_threads, false);
                                        bl.charge(CHI_TRIE, t0.elapsed().as_nanos() as u64);
                                        continue;
                                    }
                                }
                                // THE RAW LAYERS need the interval: one backward
                                // search of the full context (the existing rank
                                // machinery, zero locate).
                                work.clear();
                                work.extend(
                                    ctx.iter().rev().map(|&b| cx.idx.reader.sigma[b as usize]),
                                );
                                let (l, r) = cx.search(&work);
                                let w = r - l;
                                if w == 0 {
                                    // ABSENT: the context matches nowhere in the
                                    // text (an alphabet-absent byte empties the
                                    // interval too). Empty histogram, exact -
                                    // the same row the enumeration layer emits.
                                    encode_consult_row_v(&mut out, it, it.ctx_len, 0, 0, &[], false, false, true, rows_v4_for_threads, false);
                                    bl.charge(CHI_ABSENT, t0.elapsed().as_nanos() as u64);
                                    continue;
                                }
                                if w == 1 {
                                    // WIDTH-1 SHORT-CIRCUIT: the only raw
                                    // occurrence IS the query's own (the spine
                                    // is real text cut at the document base),
                                    // and the own is always an in-document
                                    // content continuation (off < doc_next and
                                    // off < text_len) - a crossing or END
                                    // occurrence would be a SECOND one. Raw
                                    // width 1 cannot hide either. Zero locate.
                                    let (width, excluded) = if it.excl {
                                        excluded_rows += 1;
                                        (0, true)
                                    } else {
                                        (1, false)
                                    };
                                    let pairs1: [(u8, u64); 1] = [(it.own, 1)];
                                    let branches: &[(u8, u64)] = if excluded { &[] } else { &pairs1 };
                                    if strict {
                                        // the PRE-exclusion raw answer: the row IS the
                                        // raw histogram here (width-1 proof), so the
                                        // serve-path recomputation must agree with it
                                        consult_strict_check(&mut cx, cand_ref, ctx, (1, 0), &pairs1);
                                    }
                                    encode_consult_row_v(&mut out, it, it.ctx_len, width, 0, branches, excluded, false, true, rows_v4_for_threads, true);
                                    bl.charge(CHI_WIDTH1, t0.elapsed().as_nanos() as u64);
                                    continue;
                                }
                                // THE SEPARATOR-FRAMING LAW arms the zero-locate
                                // raw layers: a 0x1E-free context on a container
                                // whose every in-text document start follows a
                                // 0x1E byte cannot touch a boundary, so the
                                // no-cross histogram IS the raw histogram
                                // (verified at open; see CONSULT_SEPFRAMING).
                                // The cap stays so a too-wide row keeps today's
                                // MARKED bytes.
                                if sep_framing_for_threads
                                    && !ctx.contains(&SEP_FRAMING)
                                    && w <= boundary_cap_for_threads
                                {
                                    let eof_raw = u64::from(
                                        ctx.len() <= corpus_tail_ref.len()
                                            && *ctx
                                                == corpus_tail_ref
                                                    [corpus_tail_ref.len() - ctx.len()..],
                                    );
                                    // LAYER 2 (single-child short-circuit): the
                                    // trie node of the context's LAST <=depth
                                    // bytes. A fork can never widen going
                                    // deeper: every occurrence of the full
                                    // context is a non-crossing occurrence of
                                    // that suffix at the SAME continuation
                                    // offset, so one child forces the byte with
                                    // ~8 node walks instead of a 2*ctxlen rank
                                    // chain. The own occurrence (in-document,
                                    // spine cut at the base) is always one of
                                    // the node's counted content children, so
                                    // the single child's byte must be `own` - a
                                    // disagreement is a corrupt member.
                                    let mut tforced = false;
                                    if let Some(trie) = trie_for_threads {
                                        let d = (trie.depth as usize).min(ctx.len());
                                        if let Some(node) = trie.find(&ctx[ctx.len() - d..]) {
                                            let kids = trie.children(node);
                                            if kids.len() == 1 {
                                                match kids[0].byte() {
                                                    // an END-only node cannot carry
                                                    // the own's content continuation;
                                                    // no verdict, use the chain
                                                    None => {}
                                                    Some(b) => {
                                                        if b != it.own {
                                                            die(&format!(
                                                                "consult: TRIE DISAGREEMENT at \
                                                                 position {}: the depth-{} node of the \
                                                                 context has one child, byte {}, but the \
                                                                 position's own continuation is {} - the \
                                                                 member and the corpus disagree",
                                                                it.off, d, b, it.own
                                                            ));
                                                        }
                                                        tforced = true;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    // LAYER 3 (one-step verification): candidate
                                    // b = the query's own continuation, from the
                                    // window; append and search once. Equality
                                    // means EVERY raw occurrence continues with b;
                                    // narrowing means fork (conservative: a
                                    // boundary-touching row inside falls through
                                    // to the exact enumeration below).
                                    let forced = if tforced {
                                        true
                                    } else {
                                        let sb = cx.idx.reader.sigma[it.own as usize];
                                        extension_width(&mut cx, sb, &work) == w - eof_raw
                                    };
                                    if forced {
                                        let count = w - eof_raw;
                                        if strict {
                                            // the PRE-exclusion raw answer (same position in
                                            // the cascade as the conflated arm's check)
                                            let pre: [(u8, u64); 1] = [(it.own, count)];
                                            consult_strict_check(
                                                &mut cx,
                                                cand_ref,
                                                ctx,
                                                (w, eof_raw),
                                                &pre,
                                            );
                                        }
                                        let (width, excluded, branches): (u64, bool, Vec<(u8, u64)>) =
                                            if it.excl {
                                                excluded_rows += 1;
                                                if count == 1 {
                                                    (w - 1, true, Vec::new())
                                                } else {
                                                    (w - 1, true, vec![(it.own, count - 1)])
                                                }
                                            } else {
                                                (w, false, vec![(it.own, count)])
                                            };
                                        encode_consult_row_v(
                                            &mut out,
                                            it,
                                            it.ctx_len,
                                            width,
                                            eof_raw,
                                            &branches,
                                            excluded,
                                            false,
                                            true,
                                            rows_v4_for_threads,
                                            true,
                                        );
                                        bl.charge(
                                            if tforced { CHI_TFORCED } else { CHI_FORCED },
                                            t0.elapsed().as_nanos() as u64,
                                        );
                                        continue;
                                    }
                                    // RAW FORK: the raw histogram IS the boundary
                                    // histogram (the framing law), so the full
                                    // extension-count histogram answers exactly,
                                    // zero locate, at any width within the cap.
                                    let (mut width, eof) =
                                        consult_answer(&mut cx, cand_ref, ctx, &corpus_tail_ref, &mut pairs);
                                    if strict {
                                        consult_strict_check(&mut cx, cand_ref, ctx, (width, eof), &pairs);
                                    }
                                    let mut excluded = false;
                                    if it.excl {
                                        if let Some(slot) = pairs.iter().position(|&(b, _)| b == it.own) {
                                            if pairs[slot].1 > 0 {
                                                pairs[slot].1 -= 1;
                                                if pairs[slot].1 == 0 {
                                                    pairs.swap_remove(slot);
                                                }
                                                width -= 1;
                                                excluded = true;
                                            }
                                        }
                                        if excluded {
                                            pairs.sort_unstable_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
                                            excluded_rows += 1;
                                        }
                                    }
                                    encode_consult_row_v(&mut out, it, it.ctx_len, width, eof, &pairs, excluded, false, true, rows_v4_for_threads, false);
                                    bl.charge(CHI_RAWFORK, t0.elapsed().as_nanos() as u64);
                                    continue;
                                }
                                // layer 5: exact enumeration within the cap
                                let (width, eof, excluded, too_wide) = boundary_answer(
                                    &mut cx,
                                    &corpus_file,
                                    ds,
                                    it,
                                    ctx,
                                    boundary_cap_for_threads,
                                    text_len_for_threads,
                                    &mut pairs,
                                );
                                if !too_wide {
                                    if excluded {
                                        excluded_rows += 1;
                                    }
                                    encode_consult_row_v(&mut out, it, it.ctx_len, width, eof, &pairs, excluded, false, true, rows_v4_for_threads, false);
                                    bl.charge(CHI_ENUM, t0.elapsed().as_nanos() as u64);
                                    continue;
                                }
                                // zone (c): wider than the cap AND deeper than the
                                // trie. There is no cheap exact algorithm here
                                // (the correction is a join between the match set
                                // and the doc-start metadata; see CONSULT.md
                                // §5), so the row falls back MARKED - or is
                                // refused under --boundary-strict.
                                if boundary_strict_for_threads {
                                    die(&format!(
                                        "consult: --boundary-strict at position {}: the context of {} \
                                         bytes has an occurrence interval past --boundary-cap {} \
                                         (raise --boundary-cap, query a longer context, or drop \
                                         --boundary-strict for the marked fallback)",
                                        it.off, ctx.len(), boundary_cap_for_threads
                                    ));
                                }
                                marked_case = true;
                            } else if boundary_strict_for_threads {
                                // refused at open; unreachable per-worker
                                die("consult: --boundary-strict without a document structure");
                            }
                        }
                        // THE CONFLATED FALLBACK (also the --cross-doc arm): the
                        // legacy boundary-unaware histogram, MARKED in the row.
                        let (mut width, eof) =
                            consult_answer(&mut cx, cand_ref, ctx, &corpus_tail_ref, &mut pairs);
                        if strict {
                            consult_strict_check(&mut cx, cand_ref, ctx, (width, eof), &pairs);
                        }
                        // D3 --exclude-source: the position's own occurrence IS in
                        // the histogram (the corpus IS the container's text over
                        // [0, text_len)), and it leaves BEFORE anything is
                        // reported. Its branch byte is the corpus byte AT the
                        // position; the row's flag bit 2 states whether the
                        // removal actually happened.
                        let mut excluded = false;
                        if it.excl {
                            if let Some(slot) = pairs.iter().position(|&(b, _)| b == it.own) {
                                if pairs[slot].1 > 0 {
                                    pairs[slot].1 -= 1;
                                    if pairs[slot].1 == 0 {
                                        pairs.swap_remove(slot);
                                    }
                                    width -= 1;
                                    excluded = true;
                                }
                            }
                            if excluded {
                                pairs.sort_unstable_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
                                excluded_rows += 1;
                            }
                        }
                        boundary_inexact_rows += 1;
                        encode_consult_row_v(&mut out, it, it.ctx_len, width, eof, &pairs, excluded, false, false, rows_v4_for_threads, false);
                        bl.charge(
                            if marked_case { CHI_MARKED } else { CHI_CONFLATED },
                            t0.elapsed().as_nanos() as u64,
                        );
                    }
                    let done = ConsultDone {
                        seq0: batch.seq0,
                        windows_after: batch.windows_after,
                        windows_bytes_after: batch.windows_bytes_after,
                        rows: batch.items.len() as u64,
                        ends_window: batch.items.last().map(|x| x.win_end).unwrap_or(false),
                        bytes: std::mem::take(&mut out),
                        excluded: excluded_rows - excl0,
                        restricted: restricted_rows - restr0,
                        inexact: boundary_inexact_rows - inex0,
                        chi_rows: bl.rows,
                        chi_nanos: bl.nanos,
                        chi_hist: bl.hist,
                    };
                    if tx.send(done).is_err() {
                        die("consult: writer closed before the last batch");
                    }
                }
            }));
        }
        drop(res_tx);

        // ---- producer: this thread. Reads windows + corpus, hands out batches.
        let mut slot = 0usize;
        let mut seq0 = start_positions;
        let mut windows_done = start.windows_done;
        let mut cur: Option<(ConsultWindow, u64, u64)> = None; // window, next k, cursor after
        let mut eof = false;
        let mut bp_lines = 0u64;
        let mut paused = false;
        let mut last_bp = Instant::now() - std::time::Duration::from_secs(10);
        let mut throttled_ms = 0u64;
        let mut last_rss = Instant::now() - std::time::Duration::from_secs(10);
        let mut bytes_read = 0u64;
        let mut resolved_bases = 0u64;
        let mut whole_docs = 0u64;
        loop {
            let mut batch = ConsultBatch {
                seq0,
                items: Vec::with_capacity(BATCH_POSITIONS),
                bytes: Vec::with_capacity(BATCH_POSITIONS * 256),
                windows_after: windows_done,
                windows_bytes_after: src.cursor(),
            };
            while batch.items.len() < BATCH_POSITIONS {
                if cur.is_none() {
                    if eof {
                        break;
                    }
                    match src.next() {
                        Some(w) => {
                            let after = src.cursor();
                            let mut w = w;
                            // D1: a window without a base resolves it from the
                            // document structure by `doc`. REFUSED when no
                            // structure is available - a base is never inferred
                            // from content bytes.
                            if w.base == BASE_ABSENT {
                                let ds = docs_ref.unwrap_or_else(|| {
                                    die(&format!(
                                        "consult: window doc={} has no 'base' and there is no \
                                         document structure to resolve it from: the container is \
                                         SXI2 v{} and carries no member m15, and no --docs DIR was \
                                         given. Boundaries are METADATA: give 'base' in every \
                                         window, build with --docs-map DIR (or `xsa docstarts \
                                         embed`), or pass --docs DIR",
                                        w.doc, index.reader.version
                                    ))
                                });
                                if w.doc >= ds.docs {
                                    die(&format!(
                                        "consult: window doc={} cannot be resolved: the document \
                                         structure holds {} documents",
                                        w.doc, ds.docs
                                    ));
                                }
                                w.base = ds.select(w.doc);
                                if w.base >= text_len {
                                    die(&format!(
                                        "consult: window doc={} resolves to start {}, at or \
                                         past the end of the container's text ({text_len} bytes)",
                                        w.doc, w.base
                                    ));
                                }
                                resolved_bases += 1;
                                if w.whole_doc {
                                    let next = if w.doc + 1 < ds.docs {
                                        ds.select(w.doc + 1)
                                    } else {
                                        text_len
                                    };
                                    if next > text_len {
                                        die(&format!(
                                            "consult: window doc={} asks for the whole document \
                                             but that document straddles the container's text \
                                             end (start {}, next start {} > text {text_len}); \
                                             name its in-text positions explicitly",
                                            w.doc, w.base, next
                                        ));
                                    }
                                    let len64 = next - w.base;
                                    if len64 > u32::MAX as u64 {
                                        die(&format!(
                                            "consult: window doc={} is {} bytes long, past the \
                                             u32 position count",
                                            w.doc, len64
                                        ));
                                    }
                                    w.len = len64 as u32;
                                    whole_docs += 1;
                                }
                            }
                            if w.base > text_len {
                                die(&format!(
                                    "consult: window doc={} base={} is past the end of the \
                                     container's text ({text_len} bytes)",
                                    w.doc, w.base
                                ));
                            }
                            if w.len == 0 {
                                die(&format!("consult: window doc={} has len=0", w.doc));
                            }
                            cur = Some((w, 0, after));
                        }
                        None => {
                            eof = true;
                            break;
                        }
                    }
                }
                let (w, k, after) = cur.unwrap();
                let docpos = w.start.saturating_add(k);
                let off = w.base.saturating_add(docpos);
                if off >= text_len {
                    die(&format!(
                        "consult: window doc={} base={} start={} len={} names position {off} at or \
                         past the end of the container's text ({text_len} bytes)",
                        w.doc, w.base, w.start, w.len
                    ));
                }
                // D1/D2: the container document containing this position, from
                // the structure - and, when the window also stated a base, the
                // parity check that the two authorities agree. A window whose
                // base disagrees with the structure is a wrong window set, not
                // a difference to average away.
                let (docid, _dstart, dnext) = match docs_ref {
                    Some(ds) => {
                        let (docid, dstart) = ds.rank_start(off);
                        if dstart != w.base {
                            die(&format!(
                                "consult: window doc={} base={} disagrees with the document \
                                 structure: position {off} is inside container document {docid} \
                                 (start {dstart}). Boundaries are METADATA; fix the window set \
                                 or rebuild the structure it was made from",
                                w.doc, w.base
                            ));
                        }
                        let dnext = if docid + 1 < ds.docs {
                            ds.select(docid + 1)
                        } else {
                            text_len
                        };
                        (docid, dstart, dnext.min(text_len))
                    }
                    None => (DOCID_ABSENT, w.base, text_len),
                };
                // LEFT CONTEXT: the last min(left, 1024) bytes BEFORE the
                // position, cut at the DOCUMENT BASE (D1: max(0, off - base)
                // bytes, never a byte scan). ONE query, one context length, no
                // backoff. The 0x1E scan is GONE: a separator inside a document
                // is content (78,715 documents over the full pile contain one),
                // and boundaries are metadata.
                let left = if w.left == LEFT_USE_DEFAULT {
                    left_default
                } else {
                    w.left.min(CTX_MAX)
                };
                let avail = off - w.base;
                let want = avail.min(u64::from(left));
                let mut ctx_len = want as usize;
                let ctx_off = batch.bytes.len() as u32;
                if ctx_len > 0 {
                    let at = batch.bytes.len();
                    batch.bytes.resize(at + ctx_len, 0);
                    corpus.read_into(off - want, &mut batch.bytes[at..]);
                    bytes_read += ctx_len as u64;
                }
                // D3: the position's own byte - the branch byte of the
                // occurrence --exclude-source removes. Read here, once, with the
                // spine bytes (the position is inside the text, checked above).
                let mut own = [0u8; 1];
                corpus.read_into(off, &mut own);
                batch.items.push(ConsultItem {
                    doc: w.doc,
                    off,
                    base: w.base,
                    docid,
                    doc_next: dnext,
                    own: own[0],
                    ctx_off,
                    ctx_len: ctx_len as u16,
                    truncated: avail > want,
                    win_end: k + 1 == u64::from(w.len),
                    restrict: doc_restrict && docid != DOCID_ABSENT,
                    excl: exclude_source,
                });
                if k + 1 == u64::from(w.len) {
                    windows_done += 1;
                    batch.windows_after = windows_done;
                    batch.windows_bytes_after = after;
                    cur = None;
                } else {
                    cur = Some((w, k + 1, after));
                }
            }
            if batch.items.is_empty() {
                break;
            }
            let rows = batch.items.len() as u64;
            seq0 += rows;

            // ---- in-flight permit: the batch is one permit's worth of memory,
            // and the permit comes back only when the writer has written it.
            permit_rx
                .recv()
                .unwrap_or_else(|_| die("consult: writer died holding the in-flight permits"));

            // ---- backpressure: a full queue is the producer's signal that the
            // consumers are saturated. Journaled (rate-limited), and it is the
            // ONLY thing that makes the producer wait for the consumers.
            match req_tx[slot].try_send(batch) {
                Ok(()) => {
                    if paused && last_bp.elapsed().as_secs_f64() >= 1.0 {
                        paused = false;
                        last_bp = Instant::now();
                        bp_lines += 1;
                        ConsultJournal::open_append(&journal_for_threads).line(&format!(
                            "CONSULT_BACKPRESSURE state=resume positions={} events={}",
                            seq0, bp_lines
                        ));
                    }
                }
                Err(std::sync::mpsc::TrySendError::Full(batch)) => {
                    if !paused && last_bp.elapsed().as_secs_f64() >= 1.0 {
                        paused = true;
                        last_bp = Instant::now();
                        bp_lines += 1;
                        backpressure_ref.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                        ConsultJournal::open_append(&journal_for_threads).line(&format!(
                            "CONSULT_BACKPRESSURE state=pause positions={} events={} \
                             cause=worker_queue_full",
                            seq0, bp_lines
                        ));
                    }
                    req_tx[slot]
                        .send(batch)
                        .unwrap_or_else(|_| die("consult: worker channel closed"));
                }
                Err(std::sync::mpsc::TrySendError::Disconnected(_)) => {
                    die("consult: worker channel closed")
                }
            }
            slot += 1;
            if slot == jobs {
                slot = 0;
            }

            // ---- RSS throttle: while the process is AT its budget the producer
            // waits, which gives the governor time to bleed the mapping. This is
            // "pause input", not "buffer more input".
            if !stateless && last_rss.elapsed().as_secs_f64() >= 0.25 {
                last_rss = Instant::now();
                if let Some(rss) = consult_resident_bytes() {
                    if rss > cap_bytes {
                        let mut jt = ConsultJournal::open_append(&journal_for_threads);
                        jt.line(&format!(
                            "CONSULT_BACKPRESSURE state=pause positions={} rss_gb={:.3} \
                             cap_gb={:.3} cause=rss_budget",
                            seq0,
                            gb(rss),
                            rss_gb
                        ));
                        let t = Instant::now();
                        let mut waited = 0u64;
                        while waited < 500 && consult_resident_bytes().unwrap_or(0) > cap_bytes {
                            std::thread::sleep(std::time::Duration::from_millis(2));
                            waited += 2;
                        }
                        throttled_ms += t.elapsed().as_millis() as u64;
                        jt.line(&format!(
                            "CONSULT_BACKPRESSURE state=resume positions={} rss_gb={:.3} \
                             cap_gb={:.3} waited_ms={}",
                            seq0,
                            gb(consult_resident_bytes().unwrap_or(0)),
                            rss_gb,
                            t.elapsed().as_millis()
                        ));
                    }
                }
            }
        }
        drop(req_tx);
        for h in worker_handles {
            h.join()
                .unwrap_or_else(|_| die("consult: a query worker panicked"));
        }
        let w = writer
            .join()
            .unwrap_or_else(|_| die("consult: the writer panicked"));
        totals = (w.0, w.1, w.2, w.3, w.4, w.5, w.6, w.7, w.8, w.9, w.10, w.11, w.12, w.13);
        // the producer's receipts ride out through these
        eprintln!(
            "CONSULT_PRODUCER corpus_bytes_read={} backpressure_events={} throttled_ms={} \
             windows_after_producer={} resolved_bases={} whole_doc_windows={} spine_cut=document_base",
            bytes_read, bp_lines, throttled_ms, windows_done, resolved_bases, whole_docs
        );
    });

    let (positions_done, windows_done, windows_bytes_done, output_bytes, digest, seq, writer_max_rss, commits, excluded_rows, restricted_rows, boundary_inexact_rows, chi_rows, chi_nanos, chi_hist) = totals;
    // THE χ LAYER LEDGER: rows and per-row cost per answer layer (the journal's
    // CONSULT_LAYERS receipt; the zero-locate raw layers are the χ-proportional
    // mass - width1 + tforced + forced + rawfork). The p50 comes from the
    // log2-nanosecond histogram's cdf.
    let mut ledger = ChiLedger {
        rows: chi_rows,
        nanos: chi_nanos,
        hist: chi_hist,
    };
    let mut layers_line = String::from("CONSULT_LAYERS");
    for (i, name) in CHI_LAYER_NAMES.iter().enumerate() {
        layers_line.push_str(&format!(
            " {}_rows={} {}_ns={} {}_p50_us={:.1}",
            name,
            ledger.rows[i],
            name,
            ledger.nanos[i],
            name,
            ledger.p50_ns(i) / 1000.0
        ));
    }
    let zero_locate = ledger.rows[CHI_WIDTH1]
        + ledger.rows[CHI_TFORCED]
        + ledger.rows[CHI_FORCED]
        + ledger.rows[CHI_RAWFORK];
    {
        let mut j = ConsultJournal::open_append(&journal_path);
        j.line(&layers_line);
        j.line(&format!(
            "CHI_ZEROLOCATE rows={} of {} ({:.2}%) layers=width1+tforced+forced+rawfork",
            zero_locate,
            positions_done,
            100.0 * zero_locate as f64 / positions_done.max(1) as f64
        ));
    }
    let seconds = started.elapsed().as_secs_f64();
    let peak = consult_peak_rss_bytes().unwrap_or(writer_max_rss).max(writer_max_rss);
    let (hits, misses, decodes, drops) = index.cache_stats();
    let (decode_ns, drop_ns, prefetch) = index.wall_stats();
    let backpressure_events = backpressure.load(std::sync::atomic::Ordering::Relaxed);
    let mut j = ConsultJournal::open_append(&journal_path);
    j.line(&format!(
        "CONSULT_DONE positions={positions_done} windows={windows_done} seconds={seconds:.3} \
         positions_per_s={:.3} output_bytes={output_bytes} rows_sha256={digest} \
         last_commit_seq={seq} commits={commits} peak_rss_gb={:.3} peak_rss_bytes={peak} \
         cap_gb={rss_gb} jobs={jobs} cache_hits={hits} cache_misses={misses} \
         block_decodes={decodes} drop_epochs={drops} decode_ns={decode_ns} drop_ns={drop_ns} \
         prefetch_calls={prefetch} corpus_remaps={} backpressure_events={backpressure_events} \
         windows_bytes_done={windows_bytes_done} excluded_rows={excluded_rows} \
         restricted_rows={restricted_rows} boundary_exact_rows={} boundary_inexact_rows={boundary_inexact_rows}",
        positions_done as f64 / seconds.max(1e-9),
        gb(peak),
        corpus.remaps,
        positions_done - boundary_inexact_rows,
    ));
    // THE LOUD FALLBACK LEDGER (the supervisor's contract): every run states how
    // many rows carry the conflated answer, even when the number is zero - the
    // mark lives in each row as well, so a consumer can never mistake a conflated
    // histogram for an exact one.
    j.line(&format!(
        "BOUNDARY_INEXACT rows={boundary_inexact_rows} total={positions_done} ratio={:.9} \
         law=no_match_may_cross_a_document_boundary \
         note=\"rows=0 is the goal; a nonzero count means that many histograms could not be \
         made boundary-exact (no structure, --cross-doc, or an interval past --boundary-cap) and \
         carry the conflated counts, MARKED boundary_exact=0 in the row's flag bit 4\"",
        boundary_inexact_rows as f64 / positions_done.max(1) as f64,
    ));
    eprintln!(
        "CONSULT_STATS positions={positions_done} windows={windows_done} \
         positions_per_s={:.3} seconds={seconds:.3} peak_rss_gb={:.3} cap_gb={rss_gb} \
         rows_sha256={digest} output_bytes={output_bytes} commits={commits} cache_hits={hits} \
         cache_misses={misses} block_decodes={decodes} drop_epochs={drops} \
         backpressure_events={backpressure_events} excluded_rows={excluded_rows} \
         restricted_rows={restricted_rows} boundary_inexact_rows={boundary_inexact_rows}",
        positions_done as f64 / seconds.max(1e-9),
        gb(peak),
    );
}

#[cfg(test)]
mod consult_tests {
    use super::*;

    /// A ready-made item for the layout tests.
    fn item(doc: u64, off: u64, base: u64, docid: u64) -> ConsultItem {
        ConsultItem {
            doc,
            off,
            base,
            docid,
            doc_next: u64::MAX,
            own: 0,
            ctx_off: 0,
            ctx_len: 0,
            truncated: false,
            win_end: false,
            restrict: false,
            excl: false,
        }
    }

    /// The row layout is a published contract (CONSULT.md §3): 40 fixed bytes
    /// plus 9 per branch plus the 24-byte v2 tail (docid, base, depth),
    /// little-endian, pairs count-desc/byte-asc.
    #[test]
    fn row_layout_is_the_documented_one() {
        let mut out = Vec::new();
        let it = item(7, 1_000_000_000_000, 999_999_999_000, 12);
        encode_consult_row(&mut out, &it, 1024, 42, 1, &[(9, 41), (3, 1)], true, true, false);
        assert_eq!(out.len(), ROW_FIXED_BYTES + 2 * 9 + ROW_TAIL_BYTES);
        assert_eq!(u32::from_le_bytes(out[0..4].try_into().unwrap()), 2); // nsym
        assert_eq!(u16::from_le_bytes(out[4..6].try_into().unwrap()), 1024); // ctxlen
        assert_eq!(u16::from_le_bytes(out[6..8].try_into().unwrap()), 1 | 4 | 8 | 16); // eof|excluded|restricted|boundary_exact=0
        assert_eq!(u64::from_le_bytes(out[8..16].try_into().unwrap()), 7); // doc
        assert_eq!(
            u64::from_le_bytes(out[16..24].try_into().unwrap()),
            1_000_000_000_000
        ); // off
        assert_eq!(u64::from_le_bytes(out[24..32].try_into().unwrap()), 42); // width
        assert_eq!(u64::from_le_bytes(out[32..40].try_into().unwrap()), 1); // eof
        let tail = out.len() - ROW_TAIL_BYTES;
        assert_eq!(u64::from_le_bytes(out[tail..tail + 8].try_into().unwrap()), 12); // docid
        assert_eq!(
            u64::from_le_bytes(out[tail + 8..tail + 16].try_into().unwrap()),
            999_999_999_000
        ); // base
        assert_eq!(
            u64::from_le_bytes(out[tail + 16..tail + 24].try_into().unwrap()),
            1_000
        ); // depth
        assert_eq!(out[40], 9);
        assert_eq!(u64::from_le_bytes(out[41..49].try_into().unwrap()), 41);
        assert_eq!(out[49], 3);
        // an empty context is an empty histogram: no pairs, no EOF
        let mut e = Vec::new();
        encode_consult_row(&mut e, &item(0, 5, 2, DOCID_ABSENT), 0, 0, 0, &[], false, false, true);
        assert_eq!(e.len(), ROW_FIXED_BYTES + ROW_TAIL_BYTES);
        assert_eq!(u32::from_le_bytes(e[0..4].try_into().unwrap()), 0);
        assert_eq!(u16::from_le_bytes(e[4..6].try_into().unwrap()), 0);
        assert_eq!(u16::from_le_bytes(e[6..8].try_into().unwrap()), 0);
        assert_eq!(u64::from_le_bytes(e[16..24].try_into().unwrap()), 5); // off
        assert_eq!(u64::from_le_bytes(e[24..32].try_into().unwrap()), 0); // width
        assert_eq!(u64::from_le_bytes(e[32..40].try_into().unwrap()), 0); // eof
        let t = e.len() - ROW_TAIL_BYTES;
        assert_eq!(u64::from_le_bytes(e[t..t + 8].try_into().unwrap()), DOCID_ABSENT);
        assert_eq!(u64::from_le_bytes(e[t + 8..t + 16].try_into().unwrap()), 2); // base
        assert_eq!(u64::from_le_bytes(e[t + 16..t + 24].try_into().unwrap()), 3); // depth
    }

    #[test]
    fn json_windows_carry_defaults_and_refuse_a_missing_document() {
        let w = parse_json_window(br#"{"doc":3,"base":10}"#, "t");
        assert_eq!((w.doc, w.base, w.start, w.len, w.left, w.whole_doc),
                   (3, 10, 0, 1, LEFT_USE_DEFAULT, false));
        let w = parse_json_window(br#"{"doc":3,"base":10,"start":2048,"len":2048,"left":64}"#, "t");
        assert_eq!((w.start, w.len, w.left, w.whole_doc), (2048, 2048, 64, false));
        // D1: a window with no base resolves it from the structure; with no len
        // either, it is the whole document
        let w = parse_json_window(br#"{"doc":9}"#, "t");
        assert_eq!((w.doc, w.base, w.len, w.whole_doc), (9, BASE_ABSENT, 1, true));
        let w = parse_json_window(br#"{"doc":9,"len":128}"#, "t");
        assert_eq!((w.base, w.len, w.whole_doc), (BASE_ABSENT, 128, false));
    }

    #[test]
    fn absent_context_is_an_empty_histogram_not_the_corpus_totals() {
        // the shape consult_answer returns for an unqueryable spine: width 0,
        // eof 0, so the row carries no counts (their `if k == 0` rule)
        let mut out = Vec::new();
        encode_consult_row(&mut out, &item(0, 0, 0, 0), 0, 0, 0, &[], false, false, true);
        let nsym = u32::from_le_bytes(out[0..4].try_into().unwrap());
        let width = u64::from_le_bytes(out[24..32].try_into().unwrap());
        assert_eq!((nsym, width), (0, 0));
    }

    /// A torn journal tail is DISCARDED, a complete record is used, and the
    /// cursor never moves backwards.
    #[test]
    fn journal_tail_is_never_trusted() {
        let dir = std::env::temp_dir().join(format!("consult-journal-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("j").to_string_lossy().into_owned();
        let good = |seq: u64, pos: u64, bytes: u64| {
            format!(
                "CONSULT_COMMIT seq={seq} windows_done={pos} windows_bytes_done={pos} \
                 positions_done={pos} output_bytes={bytes} rows_sha256=deadbeef\n"
            )
        };
        let mut body = String::from("CONSULT_OPEN container=x\n");
        body.push_str(&good(1, 2048, 100));
        body.push_str("CONSULT_RSS rss_gb=1.0\n");
        let torn = good(2, 4096, 200);
        body.push_str(&torn[..torn.len() - 20]); // no trailing newline: the torn tail
        std::fs::write(&p, &body).unwrap();
        let (c, _lines, discarded) = read_last_commit(&p).expect("a complete commit");
        assert_eq!(c.seq, 1);
        assert_eq!(c.windows_done, 2048);
        assert_eq!(c.output_bytes, 100);
        assert_eq!(discarded, 1, "the un-terminated line must be reported");

        // (a COMPLETE but unparsable commit DIES rather than being discarded -
        // `die` exits the process, so that path is exercised by the crash gate,
        // not from inside the test harness.)
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// Write a minimal XSEP v1 sidecar (seps.bin) with the given doc starts and
    /// stride, and return the DocStarts built from it.
    fn xsep_dir(starts: &[u64], stride: u64, text_len: u64) -> (std::path::PathBuf, crate::docstarts::DocStarts) {
        let dir = std::env::temp_dir().join(format!("consult-xsep-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let docs = starts.len() as u64;
        let anchors = docs.div_ceil(stride);
        let mut b = Vec::new();
        b.extend_from_slice(b"XSEP");
        b.extend_from_slice(&1u32.to_le_bytes());
        b.extend_from_slice(&0u32.to_le_bytes()); // reserved
        b.extend_from_slice(&(stride as u32).to_le_bytes());
        b.extend_from_slice(&docs.to_le_bytes());
        b.extend_from_slice(&text_len.to_le_bytes()); // corpus_bytes (>= text_len)
        b.extend_from_slice(&anchors.to_le_bytes());
        b.extend_from_slice(&0u64.to_le_bytes()); // base: the first doc starts at 0
        for (i, &s) in starts.iter().enumerate() {
            let d = if i == 0 { s } else { s - starts[i - 1] };
            b.extend_from_slice(&(d as u32).to_le_bytes());
        }
        for (k, &s) in starts.iter().enumerate() {
            if k as u64 % stride == 0 {
                b.extend_from_slice(&s.to_le_bytes());
            }
        }
        std::fs::write(dir.join("seps.bin"), &b).unwrap();
        let ds = crate::docstarts::DocStarts::from_xsep(&dir, text_len);
        (dir, ds)
    }

    /// THE SEPARATOR-FRAMING LAW'S VERIFIER: a corpus whose every doc start
    /// follows one 0x1E verifies clean; one unframed boundary is reported and
    /// fails the whole run's authorization (verified=0 disables the raw layers).
    #[test]
    fn sep_framing_verification_is_exhaustive_and_loud() {
        // docs at 0, 5, 11, 20 over a 24-byte text; framing 0x1E at 4, 10, 19
        let starts = [0u64, 5, 11, 20];
        let (dir, ds) = xsep_dir(&starts, 64, 24);
        let cpath = dir.join("corpus");
        let framed = {
            let mut t = b"hell".to_vec();
            t.push(0x1E);
            t.extend_from_slice(b"world");
            t.push(0x1E);
            t.extend_from_slice(b"01234567");
            t.push(0x1E);
            t.extend_from_slice(b"tail"); // text is 24 bytes
            t
        };
        assert_eq!(framed.len(), 24);
        std::fs::write(&cpath, &framed).unwrap();
        let (checked, bad, first, _) =
            verify_sep_framing(&cpath.to_string_lossy(), &ds, 24, 4);
        assert_eq!((checked, bad), (3, 0), "all in-text doc starts checked, no violations");

        // one unframed boundary: byte 10 is 'X' instead of 0x1E
        let mut unframed = framed.clone();
        unframed[10] = b'X';
        let upath = dir.join("corpus_bad");
        std::fs::write(&upath, &unframed).unwrap();
        let (checked, bad, first, _) =
            verify_sep_framing(&upath.to_string_lossy(), &ds, 24, 4);
        assert_eq!((checked, bad, first), (3, 1, 11), "the violation is found and named");
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// The v4 FORCED row is the compact form of exactly the v3 single-pair row:
    /// same fields, the pair implied as (cont, width - eof), and flag bit 6 set
    /// ONLY in v4 (the v3 bytes stay today's bytes).
    #[test]
    fn v4_forced_row_is_the_compact_v3_row() {
        let it = item(7, 1_000_000_000_001, 1_000_000_000_000, 12);
        // v3: 40 fixed + 9 + 24 = 73 bytes, flags 0 (nothing set: not truncated,
        // no eof, exact, not excluded/restricted)
        let mut v3 = Vec::new();
        encode_consult_row_v(&mut v3, &it, 1024, 42, 0, &[(b'x', 42)], false, false, true, false, true);
        assert_eq!(v3.len(), ROW_FIXED_BYTES + 9 + ROW_TAIL_BYTES);
        assert_eq!(u16::from_le_bytes(v3[6..8].try_into().unwrap()), 0);
        // v4 FORCED: 1 kind + 61 = 62 bytes, no pair on disc, bit 6 set
        let mut v4 = Vec::new();
        encode_consult_row_v(&mut v4, &it, 1024, 42, 0, &[(b'x', 42)], false, false, true, true, true);
        assert_eq!(v4.len(), 62);
        assert_eq!(v4[0], ROW_KIND_FORCED);
        assert_eq!(u16::from_le_bytes(v4[3..5].try_into().unwrap()), 64);
        assert_eq!(v4[5 + 8 + 8 + 8 + 8], b'x'); // the cont byte
        // decode the v4 forced record and compare every field against the v3 row
        let ctxlen = u16::from_le_bytes(v4[1..3].try_into().unwrap());
        let flags = u16::from_le_bytes(v4[3..5].try_into().unwrap());
        let doc = u64::from_le_bytes(v4[5..13].try_into().unwrap());
        let off = u64::from_le_bytes(v4[13..21].try_into().unwrap());
        let width = u64::from_le_bytes(v4[21..29].try_into().unwrap());
        let eof = u64::from_le_bytes(v4[29..37].try_into().unwrap());
        let cont = v4[37];
        assert_eq!((ctxlen, flags & !64, doc, off, width, eof, cont),
                   (1024, 0, 7, 1_000_000_000_001, 42, 0, b'x'));
        // the v3 row's own fields must agree
        assert_eq!(u16::from_le_bytes(v3[4..6].try_into().unwrap()), ctxlen);
        assert_eq!(u16::from_le_bytes(v3[6..8].try_into().unwrap()), flags & !64);
        assert_eq!(u64::from_le_bytes(v3[8..16].try_into().unwrap()), doc);
        assert_eq!(u64::from_le_bytes(v3[16..24].try_into().unwrap()), off);
        assert_eq!(u64::from_le_bytes(v3[24..32].try_into().unwrap()), width);
        assert_eq!(u64::from_le_bytes(v3[32..40].try_into().unwrap()), eof);
        assert_eq!(v3[40], cont);
        assert_eq!(u64::from_le_bytes(v3[41..49].try_into().unwrap()), width - eof);
        // a v4 row that is NOT forced-shape (multi-pair) keeps the FULL kind and
        // the v3 body; a v3 row never carries bit 6
        let mut v4f = Vec::new();
        encode_consult_row_v(&mut v4f, &it, 8, 5, 0, &[(b'a', 3), (b'b', 2)], false, false, true, true, true);
        assert_eq!(v4f[0], ROW_KIND_FULL);
        assert_eq!(v4f.len(), 1 + ROW_FIXED_BYTES + 18 + ROW_TAIL_BYTES);
        assert_eq!(u16::from_le_bytes(v4f[7..9].try_into().unwrap()) & 64, 0);
    }

    /// The layer ledger's log2 histogram yields sane p50s.
    #[test]
    fn chi_ledger_p50_follows_the_samples() {
        let mut l = ChiLedger::new();
        for _ in 0..3 {
            l.charge(CHI_WIDTH1, 1_000); // 1us
        }
        for _ in 0..1 {
            l.charge(CHI_WIDTH1, 8_000); // 8us
        }
        let p = l.p50_ns(CHI_WIDTH1);
        assert!(p >= 500.0 && p <= 2_000.0, "p50 in the 1us bucket, got {p}");
        assert_eq!(l.p50_ns(CHI_FORCED), 0.0, "an empty layer has no p50");
        assert_eq!(l.rows[CHI_WIDTH1], 4);
    }
}
