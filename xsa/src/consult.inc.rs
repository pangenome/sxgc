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
const CONSULT_VERSION: u32 = 2;
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
/// Sliding corpus mapping width. The corpus is read through a WINDOWED mmap of
/// this size and nothing else: a 100 GB corpus never becomes this process's
/// resident set, and a retired window is MADV_DONTNEED'd before it is dropped.
const CONSULT_CORPUS_WINDOW: u64 = 64 << 20;
/// Positions per work batch. One batch is one message and one buffer, so this
/// is the granularity at which in-flight memory is accounted.
const BATCH_POSITIONS: usize = 64;
/// A binary window record whose `left` field is this means "use --left".
const LEFT_USE_DEFAULT: u32 = 0xFFFF_FFFF;

fn gb(n: u64) -> f64 {
    n as f64 / (1u64 << 30) as f64
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

/// `--doc-restrict`: the EXACT document-restricted branch histogram.
///
/// The note's D2 semantics: a match or branch count must never span a document
/// boundary - the interval of occurrences for a context is restricted to the
/// document containing the query position, exactly rather than probabilistically
/// (the old 0x1E spine cut only made spanning UNLIKELY, and broke on documents
/// that contain an internal 0x1E). Exactness costs enumeration: every occurrence
/// of the context is located and kept only if the match lies fully inside the
/// document AND its continuation byte does too (an occurrence ending at the
/// document's last byte has no continuation inside the document and is not
/// counted - counting the next document's first byte would be the very leakage
/// the restriction exists to remove). Intervals wider than `cap` are REFUSED,
/// never silently returned unrestricted.
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
        if at >= it.doc_next {
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
    let r = branch_result(cx, cand, None, 0, pat, 256);
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
    /// occurrence, and rows answered by document-restricted enumeration
    excluded: u64,
    restricted: u64,
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
) {
    let before = out.len();
    let flags: u16 = (if eof != 0 { 1 } else { 0 })
        | (if it.truncated { 2 } else { 0 })
        | (if excluded { 4 } else { 0 })
        | (if restricted { 8 } else { 0 });
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
        ROW_FIXED_BYTES + 9 * branches.len() + ROW_TAIL_BYTES
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
        header.extend_from_slice(&CONSULT_VERSION.to_le_bytes());
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

    let mut src = WindowSource::open(&windows_path, binary);
    if start.windows_bytes_done > 0 {
        src.seek_to(start.windows_bytes_done);
    }

    let mut journal = ConsultJournal::open_append(&journal_path);
    if start.seq == 0 {
        journal.line(&format!(
            "CONSULT_OPEN container={} container_bytes={} index_n={} index_r={} corpus={} \
             corpus_bytes={} text_bytes={} output={} format=rows version={} jobs={} rss_cap_bytes={} cap_gb={} \
             chunk_rows={} left_default={} windows={} windows_bytes={} windows_format={} \
             batch_positions={} arena_bytes={} cache_bytes={} index_rss_cap_bytes={} \
             docs_source={} docs={} exclude_source={} doc_restrict={} doc_restrict_cap={} \
             spine_cut=document_base",
            container,
            std::fs::metadata(&container).map(|m| m.len()).unwrap_or(0),
            index_n,
            index_r,
            corpus_path,
            corpus.len,
            text_len,
            output,
            CONSULT_VERSION,
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
    let start_seq = start.seq;
    let start_positions = start.positions_done;
    let journal_for_writer = journal_path.clone();
    let journal_for_threads = journal_path.clone();
    let output_for_threads = output.clone();

    let mut totals = (0u64, 0u64, 0u64, 0u64, String::new(), 0u64, 0u64, 0u64, 0u64, 0u64);
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
            while let Ok(d) = res_rx.recv() {
                excluded_rows += d.excluded;
                restricted_rows += d.restricted;
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
                while let Ok(batch) = rx.recv() {
                    out.clear();
                    for it in &batch.items {
                        let lo = it.ctx_off as usize;
                        let ctx = &batch.bytes[lo..lo + it.ctx_len as usize];
                        if ctx.is_empty() {
                            // EMPTY CONTEXT IS AN EMPTY HISTOGRAM: the cache
                            // contract says a zero-length context has no counts
                            // and no EOF, and the global corpus histogram is NOT
                            // its answer (c14_branch_serve.select: `if k else
                            // dict(width=0, eof=0, branches=[])`).
                            encode_consult_row(&mut out, it, 0, 0, 0, &[], false, false);
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
                            encode_consult_row(&mut out, it, it.ctx_len, width, eof, &pairs, excluded, true);
                            continue;
                        }
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
                        encode_consult_row(&mut out, it, it.ctx_len, width, eof, &pairs, excluded, false);
                    }
                    let done = ConsultDone {
                        seq0: batch.seq0,
                        windows_after: batch.windows_after,
                        windows_bytes_after: batch.windows_bytes_after,
                        rows: batch.items.len() as u64,
                        ends_window: batch.items.last().map(|x| x.win_end).unwrap_or(false),
                        bytes: std::mem::take(&mut out),
                        excluded: excluded_rows,
                        restricted: restricted_rows,
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
        totals = (w.0, w.1, w.2, w.3, w.4, w.5, w.6, w.7, w.8, w.9);
        // the producer's receipts ride out through these
        eprintln!(
            "CONSULT_PRODUCER corpus_bytes_read={} backpressure_events={} throttled_ms={} \
             windows_after_producer={} resolved_bases={} whole_doc_windows={} spine_cut=document_base",
            bytes_read, bp_lines, throttled_ms, windows_done, resolved_bases, whole_docs
        );
    });

    let (positions_done, windows_done, windows_bytes_done, output_bytes, digest, seq, writer_max_rss, commits, excluded_rows, restricted_rows) = totals;
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
         restricted_rows={restricted_rows}",
        positions_done as f64 / seconds.max(1e-9),
        gb(peak),
        corpus.remaps,
    ));
    eprintln!(
        "CONSULT_STATS positions={positions_done} windows={windows_done} \
         positions_per_s={:.3} seconds={seconds:.3} peak_rss_gb={:.3} cap_gb={rss_gb} \
         rows_sha256={digest} output_bytes={output_bytes} commits={commits} cache_hits={hits} \
         cache_misses={misses} block_decodes={decodes} drop_epochs={drops} \
         backpressure_events={backpressure_events} excluded_rows={excluded_rows} \
         restricted_rows={restricted_rows}",
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
        encode_consult_row(&mut out, &it, 1024, 42, 1, &[(9, 41), (3, 1)], true, true);
        assert_eq!(out.len(), ROW_FIXED_BYTES + 2 * 9 + ROW_TAIL_BYTES);
        assert_eq!(u32::from_le_bytes(out[0..4].try_into().unwrap()), 2); // nsym
        assert_eq!(u16::from_le_bytes(out[4..6].try_into().unwrap()), 1024); // ctxlen
        assert_eq!(u16::from_le_bytes(out[6..8].try_into().unwrap()), 1 | 4 | 8); // eof|excluded|restricted
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
        encode_consult_row(&mut e, &item(0, 5, 2, DOCID_ABSENT), 0, 0, 0, &[], false, false);
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
        encode_consult_row(&mut out, &item(0, 0, 0, 0), 0, 0, 0, &[], false, false);
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
}
