//! xsa corpus --pile-map — the document map for the concatenated Pile stream.
//!
//!     xsa corpus --pile-map --out DIR [--pile FILE] [--shards DIR]
//!                [--limit-bytes N] [--jobs J] [--pile-sha256]
//!
//!     xsa corpus --pile-verify --map DIR [--pile FILE] [--shards DIR]
//!                [--samples N] [--jobs J]
//!
//! `pile.txt` was produced by
//!
//!     zstdcat pile/EleutherAI_ThePile_v1/pile/train/*.zst \
//!         | json2bytes --null-delim - > pile.txt
//!
//! i.e. it is, for every shard in filename order and every record in file order,
//! the record's `text` value (JSON-unescaped, no other transformation) followed
//! by one 0x1E byte.
//!
//! THE 0x1E IS NOT A RELIABLE BOUNDARY. Some document texts contain literal
//! 0x1E bytes (measured: a few thousand in the first 100 GB). A scan of pile.txt
//! for 0x1E therefore INVENTS boundaries and splits documents mid-text. This
//! tool derives every offset from the SHARD RECORD BOUNDARIES (the running sum
//! of `text length + 1` over shards 00..29) and reports the internal 0x1E count
//! as its own measurement:
//!
//!     separators_in_range == records_in_range + internal_0x1e_in_range
//!
//! The layout above is not assumed either: every document the map claims is
//! checked byte for byte against pile.txt (a SHA-256 per document over
//! `[offset, offset+length)`, plus the framing byte at `offset+length`), and a
//! single mismatch aborts the pass with the first differing document named.
//!
//! TWO PASSES
//!
//!   A. the SHARDS decide the document order: each shard is decompressed with
//!      `zstd -dc` and parsed as JSONL; per record we keep the byte length of
//!      `text`, its internal 0x1E count and its `meta.pile_set_name`.
//!   B. pile.txt VERIFIES the account: it is read once (windowed mmap, in
//!      parallel) and every document is checked; the 0x1E bytes in the scanned
//!      range are counted independently and reconciled with the shard-side
//!      internal counts.
//!
//! SIDECARS (all little-endian; seps.bin and titles.bin are the existing
//! `xsa corpus --wikipedia` formats, so `xsa query --docs DIR` reads them
//! unchanged):
//!
//!   seps.bin    XSEP v1, exactly as corpus.rs writes it.
//!   titles.bin  XTIT v1: the per-document title column, which for the Pile is
//!               the document's `pile_set_name` (e.g. "ArXiv", "Pile-CC").
//!   meta.bin    XPM1 v1 (new; pile-specific), see `write_meta` / MANIFEST.
//!
//! Nothing here materialises corpus text: records are parsed inside a streaming
//! window over the decompressor's stdout, the pile is read through a windowed
//! mmap, and no per-suffix or per-position column is written.

use std::collections::HashMap;
use std::fs::File;
use std::io::{BufWriter, ErrorKind, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc::{self, RecvTimeoutError};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use memmap2::MmapOptions;
use sha2::{Digest, Sha256};

use crate::die;

pub const SEP: u8 = 0x1E;
const ANCHOR_STRIDE: u64 = 64;
const SEP_HEADER: u64 = 48;
const TITLE_HEADER: u64 = 20;
const META_HEADER: u64 = 68;
/// u64 offset, u64 length, u16 shard, u32 pile_set_name, u64 record, u32 internal 0x1E
const META_ROW: u64 = 34;
const READ_BUF: usize = 8 << 20;
/// Verification windows are cut at document boundaries and are at least this
/// big, so the pile pass is a sequential read with one mmap per worker task.
const VERIFY_WINDOW: u64 = 1 << 30;
const NO_BUDGET: u64 = u64::MAX;

pub struct BuildArgs {
    pub pile: String,
    pub shards: String,
    pub out: String,
    pub limit: Option<u64>,
    pub jobs: usize,
    pub pile_sha256: bool,
}

/// Live state of the shard pass: how much each worker has decoded, and the pid
/// of its decompressor. The driver polls this once a second, so a stalled
/// decompressor is reported in the log instead of inferred from /proc.
struct Progress {
    bytes: AtomicU64,
    per_shard: Mutex<Vec<ShardProgress>>,
}

#[derive(Clone, Copy, Default)]
struct ShardProgress {
    records: u64,
    bytes: u64,
    pid: u32,
    done: bool,
}

impl Progress {
    fn new(n: usize) -> Progress {
        Progress { bytes: AtomicU64::new(0), per_shard: Mutex::new(vec![ShardProgress::default(); n]) }
    }
}

fn ck<T>(r: std::io::Result<T>, what: &str) -> T {
    r.unwrap_or_else(|e| die(&format!("pile-map: {}: {}", what, e)))
}

fn count_byte(hay: &[u8], b: u8) -> u64 {
    let mut n = 0u64;
    let mut i = 0;
    let pat = u64::from_le_bytes([b; 8]);
    while i + 8 <= hay.len() {
        let w = u64::from_le_bytes(hay[i..i + 8].try_into().unwrap());
        let x = w ^ pat;
        let m = x.wrapping_sub(0x0101010101010101) & !x & 0x8080808080808080;
        n += m.count_ones() as u64;
        i += 8;
    }
    while i < hay.len() {
        n += u64::from(hay[i] == b);
        i += 1;
    }
    n
}

// ---------------------------------------------------------------------------
// Pass A: the shards decide the document order
// ---------------------------------------------------------------------------

#[derive(Default)]
struct ShardRows {
    /// text byte length of record i
    lens: Vec<u64>,
    /// interned pile_set_name of record i (index into `tab`)
    names: Vec<u32>,
    tab: Vec<Vec<u8>>,
    /// literal 0x1E bytes inside record i's own text
    internal: Vec<u32>,
    /// SHA-256 of the record's text bytes, as produced from the shard JSON
    hashes: Vec<[u8; 32]>,
    /// sum of (len + 1): the bytes this shard contributes to pile.txt
    bytes: u64,
    blank_lines: u64,
}

fn scan_shard(path: &Path, budget: u64, idx: usize, prog: &Progress) -> ShardRows {
    let mut cmd = Command::new("zstd");
    cmd.arg("-dc").arg(path).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::inherit());
    let mut child = cmd.spawn().unwrap_or_else(|e| {
        die(&format!("pile-map: cannot run `zstd -dc {}` (a zstd on PATH is required): {}", path.display(), e))
    });
    {
        let mut st = prog.per_shard.lock().unwrap();
        st[idx].pid = child.id();
    }
    let mut out = child.stdout.take().expect("piped stdout");
    let mut rows = ShardRows::default();
    let mut intern: HashMap<Vec<u8>, u32> = HashMap::new();
    let mut buf = vec![0u8; READ_BUF];
    let mut line: Vec<u8> = Vec::with_capacity(1 << 16);
    let mut scanned = 0usize; // bytes of `line` already searched for '\n'
    let mut record = 0u64;
    let mut stopped_early = false;
    'read: loop {
        let n = match out.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) if e.kind() == ErrorKind::Interrupted => continue,
            Err(e) => die(&format!("pile-map: read {}: {}", path.display(), e)),
        };
        prog.bytes.fetch_add(n as u64, Ordering::Relaxed);
        line.extend_from_slice(&buf[..n]);
        let mut start = 0usize;
        while let Some(rel) = line[scanned..].iter().position(|&b| b == b'\n') {
            let end = scanned + rel;
            if record_line(&line[start..end], path, record, &mut rows, &mut intern) {
                record += 1;
            }
            start = end + 1;
            scanned = start;
        }
        if start > 0 {
            line.drain(..start);
        }
        scanned = line.len();
        {
            let mut st = prog.per_shard.lock().unwrap();
            st[idx].records = record;
            st[idx].bytes = rows.bytes;
        }
        if budget != NO_BUDGET && rows.bytes >= budget {
            // Everything past the --limit-bytes boundary would be discarded by
            // the merge; stop decoding and keep what is already exact. The
            // leftover buffer is a PARTIAL record and must not be parsed.
            stopped_early = true;
            break 'read;
        }
    }
    if !stopped_early && !line.is_empty() {
        let tail = std::mem::take(&mut line);
        record_line(&tail, path, record, &mut rows, &mut intern);
    }
    // The reader must CLOSE ITS PIPE before waiting: an open pipe keeps the
    // decompressor writing into a full pipe buffer and wait() never returns.
    drop(out);
    if stopped_early {
        // We closed the pipe on purpose. zstd dies of SIGPIPE (or is killed
        // here); either way this shard contributed exactly the records already
        // collected, and the merge only reads up to the byte budget.
        let _ = child.kill();
    }
    // A failed decompression must be named as such: a short stream would
    // silently shrink the map and the pile verification would blame the wrong
    // file.
    let st = ck(child.wait(), "wait for zstd");
    if !st.success() && !(stopped_early && st.code().is_none()) {
        die(&format!(
            "pile-map: `zstd -dc {}` failed ({}): the shard stream did not decode; refusing to emit a map from a truncated shard",
            path.display(),
            st
        ));
    }
    {
        let mut st = prog.per_shard.lock().unwrap();
        st[idx].records = record;
        st[idx].bytes = rows.bytes;
        st[idx].done = true;
        st[idx].pid = 0;
    }
    rows
}

/// Returns true when the line carried a record.
fn record_line(
    l: &[u8],
    path: &Path,
    record: u64,
    rows: &mut ShardRows,
    intern: &mut HashMap<Vec<u8>, u32>,
) -> bool {
    if l.iter().all(|b| b.is_ascii_whitespace()) {
        rows.blank_lines += 1;
        return false;
    }
    let v: serde_json::Value = serde_json::from_slice(l)
        .unwrap_or_else(|e| die(&format!("pile-map: {}: record {} is not JSON ({})", path.display(), record, e)));
    let text = v
        .get("text")
        .and_then(|t| t.as_str())
        .unwrap_or_else(|| die(&format!("pile-map: {}: record {} has no string \"text\" field", path.display(), record)));
    let name = v
        .get("meta")
        .and_then(|m| m.get("pile_set_name"))
        .and_then(|s| s.as_str())
        .unwrap_or_else(|| {
            die(&format!("pile-map: {}: record {} has no string meta.pile_set_name", path.display(), record))
        });
    let id = match intern.get(name.as_bytes()) {
        Some(&id) => id,
        None => {
            let id = rows.tab.len() as u32;
            let bytes = name.as_bytes().to_vec();
            intern.insert(bytes.clone(), id);
            rows.tab.push(bytes);
            id
        }
    };
    let body = text.as_bytes();
    let mut h = Sha256::new();
    h.update(body);
    rows.hashes.push(h.finalize().into());
    rows.lens.push(body.len() as u64);
    rows.internal.push(count_byte(body, SEP) as u32);
    rows.names.push(id);
    rows.bytes += body.len() as u64 + 1;
    true
}

// ---------------------------------------------------------------------------
// The map
// ---------------------------------------------------------------------------

struct DocMap {
    off: Vec<u64>,
    len: Vec<u64>,
    shard: Vec<u16>,
    rec: Vec<u64>,
    name: Vec<u32>,
    internal: Vec<u32>,
    hash: Vec<[u8; 32]>,
    names_tab: Vec<Vec<u8>>,
    shard_names: Vec<String>,
    corpus_bytes: u64,
    partial_tail: bool,
    /// text of the record behind a cut tail document (prefix-checked against
    /// the pile); empty when there is no cut tail
    cut_text: Vec<u8>,
}

impl DocMap {
    fn doc_count(&self) -> u64 {
        self.off.len() as u64
    }
    fn name_of(&self, i: usize) -> String {
        String::from_utf8_lossy(&self.names_tab[self.name[i] as usize]).into_owned()
    }
}

fn build_map(results: &[ShardRows], shard_names: Vec<String>, limit: Option<u64>) -> DocMap {
    let mut names_tab: Vec<Vec<u8>> = Vec::new();
    let mut name_ids: HashMap<Vec<u8>, u32> = HashMap::new();
    let total: usize = results.iter().map(|r| r.lens.len()).sum();
    let mut m = DocMap {
        off: Vec::with_capacity(total),
        len: Vec::with_capacity(total),
        shard: Vec::with_capacity(total),
        rec: Vec::with_capacity(total),
        name: Vec::with_capacity(total),
        internal: Vec::with_capacity(total),
        hash: Vec::with_capacity(total),
        names_tab: Vec::new(),
        shard_names,
        corpus_bytes: 0,
        partial_tail: false,
        cut_text: Vec::new(),
    };
    let mut cursor = 0u64;
    'shards: for (si, rows) in results.iter().enumerate() {
        for j in 0..rows.lens.len() {
            let l = rows.lens[j];
            if let Some(lim) = limit {
                if cursor >= lim {
                    // The range ends exactly on a document boundary: nothing is
                    // cut, so there is no partial tail.
                    break 'shards;
                }
                if cursor + l + 1 > lim {
                    // The byte budget cuts this document (its text, its
                    // separator, or both). It keeps its identity; the length is
                    // what fits. Its internal-0x1E count covers only the bytes
                    // in range and is filled in by the pile pass.
                    let fits = (lim - cursor).min(l);
                    let local = &rows.tab[rows.names[j] as usize];
                    let id = match name_ids.get(local) {
                        Some(&id) => id,
                        None => {
                            let id = names_tab.len() as u32;
                            names_tab.push(local.clone());
                            name_ids.insert(local.clone(), id);
                            id
                        }
                    };
                    m.off.push(cursor);
                    m.len.push(fits);
                    m.shard.push(si as u16);
                    m.rec.push(j as u64);
                    m.name.push(id);
                    m.internal.push(u32::MAX); // "measure it from the pile"
                    m.hash.push(rows.hashes[j]);
                    cursor += fits;
                    m.partial_tail = true;
                    break 'shards;
                }
            }
            let local = &rows.tab[rows.names[j] as usize];
            let id = match name_ids.get(local) {
                Some(&id) => id,
                None => {
                    let id = names_tab.len() as u32;
                    names_tab.push(local.clone());
                    name_ids.insert(local.clone(), id);
                    id
                }
            };
            m.off.push(cursor);
            m.len.push(l);
            m.shard.push(si as u16);
            m.rec.push(j as u64);
            m.name.push(id);
            m.internal.push(rows.internal[j]);
            m.hash.push(rows.hashes[j]);
            cursor += l + 1;
        }
    }
    m.names_tab = names_tab;
    m.corpus_bytes = cursor;
    m
}

/// On a content mismatch: read the pile bytes the map claims and the shard
/// record's own text, and name the first byte that differs, with context. The
/// build has both sides only here, so the gate's "first differing byte" is
/// produced on the failure path instead of being guessed at.
fn explain_mismatch(pile: &Path, m: &DocMap, shards: &[PathBuf], d: usize) -> String {
    use std::io::{Seek, SeekFrom};
    let off = m.off[d];
    let len = m.len[d];
    if len > (64 << 20) {
        return format!("\n  document {} is {} bytes; refusing to load both sides to locate the first difference", d, len);
    }
    let mut f = match File::open(pile) {
        Ok(f) => f,
        Err(e) => return format!("\n  (cannot reopen {} for detail: {})", pile.display(), e),
    };
    if f.seek(SeekFrom::Start(off)).is_err() {
        return format!("\n  (cannot seek {} to {})", pile.display(), off);
    }
    let mut mine = vec![0u8; len as usize];
    if f.read_exact(&mut mine).is_err() {
        return format!("\n  (cannot read {} bytes at {} of {})", len, off, pile.display());
    }
    let text = extract_record(&shards[m.shard[d] as usize], m.rec[d]);
    let ctx = |s: &[u8], at: usize| -> String {
        let lo = at.saturating_sub(20);
        let hi = (at + 20).min(s.len());
        s[lo..hi]
            .iter()
            .map(|&b| if (0x20..0x7f).contains(&b) { b as char } else { '.' })
            .collect::<String>()
    };
    match mine.iter().zip(text.iter()).position(|(a, b)| a != b) {
        Some(at) => format!(
            "\n  doc={} shard={} record={} offset={} length={} first_differing_byte={} (relative {}) pile=0x{:02x} shard=0x{:02x}\n  pile  [..]=[{}]\n  shard [..]=[{}]",
            d,
            m.shard_names[m.shard[d] as usize],
            m.rec[d],
            off,
            len,
            off + at as u64,
            at,
            mine[at],
            text[at],
            ctx(&mine, at),
            ctx(&text, at)
        ),
        None => format!(
            "\n  doc={} shard={} record={} offset={} length={} map_length={} shard_text_length={} (a length difference, no differing byte)",
            d,
            m.shard_names[m.shard[d] as usize],
            m.rec[d],
            off,
            len,
            mine.len(),
            text.len()
        ),
    }
}

/// Extract the text of one record from one shard, stopping as soon as it is
/// found. Used only for the single document a --limit-bytes run cuts: a cut
/// document can only be verified as a prefix of the record it names.
fn extract_record(path: &Path, want: u64) -> Vec<u8> {
    let mut cmd = Command::new("zstd");
    cmd.arg("-dc").arg(path).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::inherit());
    let mut child = cmd.spawn().unwrap_or_else(|e| die(&format!("pile-map: cannot run `zstd -dc {}`: {}", path.display(), e)));
    let mut out = child.stdout.take().expect("piped stdout");
    let mut buf = vec![0u8; READ_BUF];
    let mut line: Vec<u8> = Vec::new();
    let mut scanned = 0usize;
    let mut record = 0u64;
    let mut found: Option<Vec<u8>> = None;
    'read: loop {
        let n = match out.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) if e.kind() == ErrorKind::Interrupted => continue,
            Err(e) => die(&format!("pile-map: read {}: {}", path.display(), e)),
        };
        line.extend_from_slice(&buf[..n]);
        let mut start = 0usize;
        while let Some(rel) = line[scanned..].iter().position(|&b| b == b'\n') {
            let end = scanned + rel;
            let blank = line[start..end].iter().all(|b| b.is_ascii_whitespace());
            if !blank {
                if record == want {
                    found = Some(record_text(&line[start..end], path, record));
                    break 'read;
                }
                record += 1;
            }
            start = end + 1;
            scanned = start;
        }
        if start > 0 {
            line.drain(..start);
        }
        scanned = line.len();
    }
    drop(out);
    let st = ck(child.wait(), "wait for zstd");
    if !st.success() && found.is_none() {
        die(&format!("pile-map: `zstd -dc {}` failed ({})", path.display(), st));
    }
    found.unwrap_or_else(|| die(&format!("pile-map: {} ended before record {} it was expected to hold", path.display(), want)))
}

// ---------------------------------------------------------------------------
// Pass B: pile.txt verifies the account
// ---------------------------------------------------------------------------

struct VerifyOut {
    compared: u64,
    mismatches: u64,
    /// every 0x1E byte in the covered range (articles + separators)
    seps: u64,
    /// documents whose framing 0x1E was found inside the range
    framed: u64,
    /// 0x1E bytes inside the cut tail document's in-range bytes
    tail_internal: u32,
    first_bad: Option<(usize, String)>,
}

/// Read the covered range of `pile` once and check every document against the
/// shard-side account. Windows are cut at document boundaries, so each worker
/// reads sequentially and the per-window 0x1E counts sum to the whole range.
fn verify_pile(pile: &Path, m: &DocMap, jobs: usize) -> VerifyOut {
    let n = m.off.len();
    if n == 0 {
        return VerifyOut { compared: 0, mismatches: 0, seps: 0, framed: 0, tail_internal: 0, first_bad: None };
    }
    let mut groups: Vec<(usize, usize)> = Vec::new();
    let mut g0 = 0usize;
    while g0 < n {
        let mut g1 = g0 + 1;
        let mut span = m.len[g0] + 1;
        while g1 < n && span < VERIFY_WINDOW {
            span += m.len[g1] + 1;
            g1 += 1;
        }
        groups.push((g0, g1));
        g0 = g1;
    }
    let pool = rayon::ThreadPoolBuilder::new()
        .num_threads(jobs.max(1))
        .build()
        .unwrap_or_else(|e| die(&format!("pile-map: thread pool: {}", e)));
    let outs: Vec<VerifyOut> = pool.install(|| {
        use rayon::prelude::*;
        groups
            .par_iter()
            .map(|&(g0, g1)| {
                let last = g1 == n;
                let start = m.off[g0];
                let end = if last { m.corpus_bytes } else { m.off[g1 - 1] + m.len[g1 - 1] + 1 };
                let mut o = VerifyOut { compared: 0, mismatches: 0, seps: 0, framed: 0, tail_internal: 0, first_bad: None };
                if end <= start {
                    return o;
                }
                let f = File::open(pile).unwrap_or_else(|e| die(&format!("open {}: {}", pile.display(), e)));
                let page = 4096u64;
                let aligned = start - start % page;
                let map = unsafe { MmapOptions::new().offset(aligned).len((end - aligned) as usize).map(&f) }
                    .unwrap_or_else(|e| die(&format!("mmap {} [{}, {}): {}", pile.display(), aligned, end, e)));
                let win = &map[(start - aligned) as usize..(end - aligned) as usize];
                assert_eq!(win.len() as u64, end - start);
                o.seps = count_byte(win, SEP);
                for i in g0..g1 {
                    let rel = (m.off[i] - start) as usize;
                    let l = m.len[i] as usize;
                    let cut_tail = last && m.partial_tail && i + 1 == n;
                    o.compared += 1;
                    if cut_tail {
                        o.tail_internal = count_byte(&win[rel..rel + l], SEP) as u32;
                    }
                    let mut bad: Option<String> = None;
                    if cut_tail && !m.cut_text.is_empty() {
                        // A cut document has no full-text hash to match: prove
                        // it is the exact prefix of the record it names.
                        let mine = &win[rel..rel + l];
                        let want = &m.cut_text[..l.min(m.cut_text.len())];
                        if l > m.cut_text.len() || mine != want {
                            let at = mine.iter().zip(want.iter()).position(|(a, b)| a != b).unwrap_or(mine.len().min(want.len()));
                            bad = Some(format!(
                                "cut tail document {} at [{}, {}) is not a prefix of shard {} record {} (first difference at relative {})",
                                i,
                                m.off[i],
                                m.off[i] + m.len[i],
                                m.shard[i],
                                m.rec[i],
                                at
                            ));
                        }
                    }
                    if !cut_tail {
                        let has_sep = rel + l < win.len();
                        if !has_sep || win[rel + l] != SEP {
                            bad = Some(format!(
                                "document {} at [{}, {}): expected its 0x1E at offset {} but found {}",
                                i,
                                m.off[i],
                                m.off[i] + m.len[i] + 1,
                                m.off[i] + m.len[i],
                                match win.get(rel + l) {
                                    Some(&b) => format!("0x{:02x} ({})", b, m.name_of(i)),
                                    None => "end of range".to_string(),
                                }
                            ));
                        } else {
                            o.framed += 1;
                        }
                    }
                    if bad.is_none() && !cut_tail {
                        let mut h = Sha256::new();
                        h.update(&win[rel..rel + l]);
                        let got: [u8; 32] = h.finalize().into();
                        if got != m.hash[i] {
                            let pile_sha = format!("{:x}", Sha256::digest(&win[rel..rel + l]));
                            let mut first = 0usize;
                            let _ = &mut first;
                            bad = Some(format!(
                                "document {} (shard {} {} record {}) text [{}, {}) differs from the shard record: pile sha256={} shard sha256={}",
                                i,
                                m.shard[i],
                                m.shard_names[m.shard[i] as usize],
                                m.rec[i],
                                m.off[i],
                                m.off[i] + m.len[i],
                                pile_sha,
                                hex32(&m.hash[i])
                            ));
                        }
                    }
                    if let Some(msg) = bad {
                        o.mismatches += 1;
                        if o.first_bad.is_none() {
                            o.first_bad = Some((i, msg));
                        }
                    }
                }
                o
            })
            .collect()
    });
    let mut t = VerifyOut { compared: 0, mismatches: 0, seps: 0, framed: 0, tail_internal: 0, first_bad: None };
    for o in outs {
        t.compared += o.compared;
        t.mismatches += o.mismatches;
        t.seps += o.seps;
        t.framed += o.framed;
        t.tail_internal += o.tail_internal;
        if t.first_bad.is_none() {
            t.first_bad = o.first_bad;
        }
    }
    t
}

fn hex32(h: &[u8; 32]) -> String {
    let mut s = String::with_capacity(64);
    for b in h {
        s.push_str(&format!("{:02x}", b));
    }
    s
}

// ---------------------------------------------------------------------------
// Sidecars
// ---------------------------------------------------------------------------

struct PatchFile {
    w: BufWriter<File>,
    path: PathBuf,
    n: u64,
}

impl PatchFile {
    fn new(path: &Path, placeholder: u64) -> PatchFile {
        let f = ck(File::create(path), &format!("create {}", path.display()));
        let mut p = PatchFile { w: BufWriter::with_capacity(1 << 20, f), path: path.to_path_buf(), n: 0 };
        p.put(&vec![0u8; placeholder as usize]);
        p
    }
    fn put(&mut self, b: &[u8]) {
        ck(self.w.write_all(b), &format!("write {}", self.path.display()));
        self.n += b.len() as u64;
    }
    fn put_u16(&mut self, v: u16) {
        self.put(&v.to_le_bytes());
    }
    fn put_u32(&mut self, v: u32) {
        self.put(&v.to_le_bytes());
    }
    fn put_u64(&mut self, v: u64) {
        self.put(&v.to_le_bytes());
    }
    fn patch(&mut self, off: u64, b: &[u8]) {
        use std::io::{Seek, SeekFrom};
        let end = self.n;
        ck(self.w.seek(SeekFrom::Start(off)), &format!("seek {}", self.path.display()));
        ck(self.w.write_all(b), &format!("patch {}", self.path.display()));
        ck(self.w.seek(SeekFrom::Start(end)), &format!("seek back {}", self.path.display()));
    }
    fn finish(mut self) -> (u64, String) {
        ck(self.w.flush(), &format!("flush {}", self.path.display()));
        let f = self
            .w
            .into_inner()
            .unwrap_or_else(|e| die(&format!("close {}: {}", self.path.display(), e)));
        ck(f.sync_all(), &format!("fsync {}", self.path.display()));
        let len = ck(f.metadata(), &format!("stat {}", self.path.display())).len();
        if len != self.n {
            die(&format!("pile-map: {} is {} bytes but {} were written", self.path.display(), len, self.n));
        }
        let sha = hash_file(&self.path);
        (len, sha)
    }
}

fn hash_file(path: &Path) -> String {
    let mut f = ck(File::open(path), &format!("open {}", path.display()));
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        match f.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => h.update(&buf[..n]),
            Err(e) if e.kind() == ErrorKind::Interrupted => continue,
            Err(e) => die(&format!("read {}: {}", path.display(), e)),
        }
    }
    format!("{:x}", h.finalize())
}

/// Sequential SHA-256 of the first `upto` bytes of `path` (the identity of the
/// exact byte range this map describes).
fn hash_prefix(path: &Path, upto: u64) -> String {
    let mut f = ck(File::open(path), &format!("open {}", path.display()));
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 8 << 20];
    let mut left = upto;
    while left > 0 {
        let want = (buf.len() as u64).min(left) as usize;
        match f.read(&mut buf[..want]) {
            Ok(0) => die(&format!("pile-map: {} ended {} bytes into the range", path.display(), upto - left)),
            Ok(n) => {
                h.update(&buf[..n]);
                left -= n as u64;
            }
            Err(e) if e.kind() == ErrorKind::Interrupted => continue,
            Err(e) => die(&format!("read {}: {}", path.display(), e)),
        }
    }
    format!("{:x}", h.finalize())
}

fn write_seps(m: &DocMap, path: &Path) -> (u64, String) {
    let mut f = PatchFile::new(path, SEP_HEADER);
    let mut prev = 0u64;
    for &s in &m.off {
        let d = s - prev;
        if d > u32::MAX as u64 {
            die(&format!(
                "pile-map: the document at {} is {} bytes after its predecessor; the u32 delta sidecar cannot address it",
                s, d
            ));
        }
        f.put_u32(d as u32);
        prev = s;
    }
    let anchors: Vec<u64> = m.off.iter().step_by(ANCHOR_STRIDE as usize).copied().collect();
    for a in &anchors {
        f.put_u64(*a);
    }
    let mut h = Vec::with_capacity(SEP_HEADER as usize);
    h.extend_from_slice(b"XSEP");
    h.extend_from_slice(&1u32.to_le_bytes());
    h.extend_from_slice(&1u32.to_le_bytes()); // bit0: 0x1E is emitted between docs
    h.extend_from_slice(&(ANCHOR_STRIDE as u32).to_le_bytes());
    h.extend_from_slice(&m.doc_count().to_le_bytes());
    h.extend_from_slice(&m.corpus_bytes.to_le_bytes());
    h.extend_from_slice(&(anchors.len() as u64).to_le_bytes());
    h.extend_from_slice(&0u64.to_le_bytes()); // base
    assert_eq!(h.len() as u64, SEP_HEADER);
    f.patch(0, &h);
    let (bytes, sha) = f.finish();
    let want = SEP_HEADER + 4 * m.doc_count() + 8 * anchors.len() as u64;
    if bytes != want {
        die(&format!("pile-map: seps.bin is {} bytes, layout says {}", bytes, want));
    }
    (bytes, sha)
}

fn write_titles(m: &DocMap, path: &Path) -> (u64, String) {
    if m.doc_count() >= u32::MAX as u64 {
        die("pile-map: more than 2^32-1 documents: the u32 sidecar tables cannot address them");
    }
    let mut payload = 0u64;
    let mut f = PatchFile::new(path, TITLE_HEADER);
    for id in &m.name {
        let t = &m.names_tab[*id as usize];
        if t.len() > u16::MAX as usize {
            die(&format!("pile-map: a pile_set_name of {} bytes does not fit the u16 titles.bin length column", t.len()));
        }
        if t.iter().any(|&b| b == b'\n' || b == SEP) {
            die("pile-map: a pile_set_name contains a newline or 0x1E; in-band framing is impossible");
        }
        f.put(t);
        payload += t.len() as u64;
    }
    if payload > u32::MAX as u64 {
        die("pile-map: the titles payload exceeds 2^32-1 bytes");
    }
    let mut off = 0u64;
    for id in &m.name {
        f.put_u32(off as u32);
        off += m.names_tab[*id as usize].len() as u64;
    }
    for id in &m.name {
        f.put_u16(m.names_tab[*id as usize].len() as u16);
    }
    let mut h = Vec::with_capacity(TITLE_HEADER as usize);
    h.extend_from_slice(b"XTIT");
    h.extend_from_slice(&1u32.to_le_bytes());
    h.extend_from_slice(&(m.doc_count() as u32).to_le_bytes());
    h.extend_from_slice(&payload.to_le_bytes());
    assert_eq!(h.len() as u64, TITLE_HEADER);
    f.patch(0, &h);
    let (bytes, sha) = f.finish();
    let want = TITLE_HEADER + payload + 6 * m.doc_count();
    if bytes != want {
        die(&format!("pile-map: titles.bin is {} bytes, layout says {}", bytes, want));
    }
    (bytes, sha)
}

fn write_meta(m: &DocMap, path: &Path) -> (u64, String) {
    let mut f = PatchFile::new(path, META_HEADER);
    let name_payload: u64 = m.names_tab.iter().map(|n| n.len() as u64).sum();
    let shard_payload: u64 = m.shard_names.iter().map(|n| n.len() as u64).sum();
    for n in &m.names_tab {
        f.put(n);
    }
    let mut o = 0u64;
    for n in &m.names_tab {
        f.put_u32(o as u32);
        o += n.len() as u64;
    }
    for n in &m.names_tab {
        f.put_u16(n.len() as u16);
    }
    for n in &m.shard_names {
        f.put(n.as_bytes());
    }
    let mut o = 0u64;
    for n in &m.shard_names {
        f.put_u32(o as u32);
        o += n.len() as u64;
    }
    for n in &m.shard_names {
        f.put_u16(n.len() as u16);
    }
    for i in 0..m.doc_count() as usize {
        f.put_u64(m.off[i]);
        f.put_u64(m.len[i]);
        f.put_u16(m.shard[i]);
        f.put_u32(m.name[i]);
        f.put_u64(m.rec[i]);
        f.put_u32(m.internal[i]);
    }
    let mut h = Vec::with_capacity(META_HEADER as usize);
    h.extend_from_slice(b"XPM1");
    h.extend_from_slice(&1u32.to_le_bytes());
    h.extend_from_slice(&m.doc_count().to_le_bytes());
    h.extend_from_slice(&m.corpus_bytes.to_le_bytes());
    h.extend_from_slice(&(m.shard_names.len() as u64).to_le_bytes());
    h.extend_from_slice(&(m.names_tab.len() as u64).to_le_bytes());
    h.extend_from_slice(&name_payload.to_le_bytes());
    h.extend_from_slice(&shard_payload.to_le_bytes());
    h.extend_from_slice(&(META_ROW as u32).to_le_bytes());
    h.extend_from_slice(&u64::from(m.partial_tail).to_le_bytes());
    assert_eq!(h.len() as u64, META_HEADER);
    f.patch(0, &h);
    let (bytes, sha) = f.finish();
    let want = META_HEADER
        + name_payload
        + 6 * m.names_tab.len() as u64
        + shard_payload
        + 6 * m.shard_names.len() as u64
        + META_ROW * m.doc_count();
    if bytes != want {
        die(&format!("pile-map: meta.bin is {} bytes, layout says {}", bytes, want));
    }
    (bytes, sha)
}

fn read_meta(path: &Path) -> DocMap {
    let bytes = ck(std::fs::read(path), &format!("read {}", path.display()));
    let u32at = |o: usize| u32::from_le_bytes(bytes[o..o + 4].try_into().unwrap()) as u64;
    let u64at = |o: usize| u64::from_le_bytes(bytes[o..o + 8].try_into().unwrap());
    if bytes.len() < META_HEADER as usize || &bytes[..4] != b"XPM1" || u32at(4) != 1 {
        die(&format!("pile-map: {} is not an XPM1 v1 map", path.display()));
    }
    let n = u64at(8) as usize;
    let corpus_bytes = u64at(16);
    let shard_count = u64at(24) as usize;
    let name_count = u64at(32) as usize;
    let name_payload = u64at(40) as usize;
    let shard_payload = u64at(48) as usize;
    if u32at(56) != META_ROW || u64at(60) > 1 {
        die(&format!("pile-map: {}: unexpected row size or partial_tail flag", path.display()));
    }
    let partial_tail = u64at(60) == 1;
    let mut p = META_HEADER as usize;
    let names_blob = &bytes[p..p + name_payload];
    p += name_payload;
    let name_offs = p;
    p += 4 * name_count;
    let name_lens = p;
    p += 2 * name_count;
    let shard_blob = &bytes[p..p + shard_payload];
    p += shard_payload;
    let shard_offs = p;
    p += 4 * shard_count;
    let shard_lens = p;
    p += 2 * shard_count;
    let rows = p;
    if bytes.len() != rows + META_ROW as usize * n {
        die(&format!("pile-map: {}: file length does not match the XPM1 header", path.display()));
    }
    let mut names_tab = Vec::with_capacity(name_count);
    for i in 0..name_count {
        let o = u32::from_le_bytes(bytes[name_offs + 4 * i..name_offs + 4 * i + 4].try_into().unwrap()) as usize;
        let l = u16::from_le_bytes(bytes[name_lens + 2 * i..name_lens + 2 * i + 2].try_into().unwrap()) as usize;
        names_tab.push(names_blob[o..o + l].to_vec());
    }
    let mut shard_names = Vec::with_capacity(shard_count);
    for i in 0..shard_count {
        let o = u32::from_le_bytes(bytes[shard_offs + 4 * i..shard_offs + 4 * i + 4].try_into().unwrap()) as usize;
        let l = u16::from_le_bytes(bytes[shard_lens + 2 * i..shard_lens + 2 * i + 2].try_into().unwrap()) as usize;
        shard_names.push(String::from_utf8_lossy(&shard_blob[o..o + l]).into_owned());
    }
    let mut m = DocMap {
        off: Vec::with_capacity(n),
        len: Vec::with_capacity(n),
        shard: Vec::with_capacity(n),
        rec: Vec::with_capacity(n),
        name: Vec::with_capacity(n),
        internal: Vec::with_capacity(n),
        hash: Vec::new(),
        names_tab,
        shard_names,
        corpus_bytes,
        partial_tail,
        cut_text: Vec::new(),
    };
    for i in 0..n {
        let r = rows + META_ROW as usize * i;
        m.off.push(u64::from_le_bytes(bytes[r..r + 8].try_into().unwrap()));
        m.len.push(u64::from_le_bytes(bytes[r + 8..r + 16].try_into().unwrap()));
        m.shard.push(u16::from_le_bytes(bytes[r + 16..r + 18].try_into().unwrap()));
        m.name.push(u32::from_le_bytes(bytes[r + 18..r + 22].try_into().unwrap()));
        m.rec.push(u64::from_le_bytes(bytes[r + 22..r + 30].try_into().unwrap()));
        m.internal.push(u32::from_le_bytes(bytes[r + 30..r + 34].try_into().unwrap()));
    }
    // Structural invariants: rows tile the covered range with no gap and no
    // invented boundary.
    let mut expect = 0u64;
    for i in 0..n {
        if m.off[i] != expect {
            die(&format!(
                "pile-map: {}: row {} starts at {} but the previous row ends at {}",
                path.display(),
                i,
                m.off[i],
                expect
            ));
        }
        if m.shard[i] as usize >= m.shard_names.len() || m.name[i] as usize >= m.names_tab.len() {
            die(&format!("pile-map: {}: row {} names a missing shard or set entry", path.display(), i));
        }
        expect = m.off[i] + m.len[i] + 1;
    }
    // A cut tail document does not own its 0x1E, so the covered range ends at
    // its last text byte; every other final row ends one byte later.
    if n > 0 {
        let end = if partial_tail { expect - 1 } else { expect };
        if end != corpus_bytes {
            die(&format!("pile-map: {}: rows end at {} but corpus_bytes is {}", path.display(), end, corpus_bytes));
        }
    }
    m
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

fn shard_list(dir: &Path) -> Vec<PathBuf> {
    let rd = ck(std::fs::read_dir(dir), &format!("read shard dir {}", dir.display()));
    let mut v: Vec<PathBuf> = Vec::new();
    for e in rd {
        let e = ck(e, &format!("read shard dir {}", dir.display()));
        let p = e.path();
        if p.extension().is_some_and(|x| x == "zst") {
            v.push(p);
        }
    }
    v.sort();
    if v.is_empty() {
        die(&format!("pile-map: no *.zst shards under {}", dir.display()));
    }
    for (i, p) in v.iter().enumerate() {
        let name = p.file_name().unwrap().to_string_lossy();
        if !name.starts_with(&format!("{:02}", i)) {
            die(&format!(
                "pile-map: shard {} is {}, which does not continue the filename order 00,01,...; the Pile stream order IS filename order",
                i,
                p.display()
            ));
        }
    }
    v
}

fn read_sha256sums(dir: &Path) -> HashMap<String, String> {
    let mut out = HashMap::new();
    let mut cands = vec![dir.join("SHA256SUMS.txt")];
    if let Some(p) = dir.parent() {
        cands.push(p.join("SHA256SUMS.txt"));
    }
    for cand in cands {
        if let Ok(s) = std::fs::read_to_string(&cand) {
            for line in s.lines() {
                let mut it = line.split_whitespace();
                if let (Some(sha), Some(name)) = (it.next(), it.next()) {
                    let base = name.rsplit('/').next().unwrap_or(name).to_string();
                    out.insert(base, sha.to_string());
                }
            }
            if !out.is_empty() {
                return out;
            }
        }
    }
    out
}

fn now_unix() -> i64 {
    match std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH) {
        Ok(d) => d.as_secs() as i64,
        Err(e) => -(e.duration().as_secs() as i64),
    }
}

fn peak_rss_kb() -> u64 {
    std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| {
            s.lines()
                .find(|l| l.starts_with("VmHWM:"))
                .and_then(|l| l.split_whitespace().nth(1).and_then(|v| v.parse().ok()))
        })
        .unwrap_or(0)
}

pub fn build(a: &BuildArgs) {
    let t0 = Instant::now();
    let dst = Path::new(&a.out);
    ck(std::fs::create_dir_all(dst), &format!("mkdir {}", dst.display()));
    for name in ["seps.bin", "titles.bin", "meta.bin", "MANIFEST.json"] {
        for p in [dst.join(name), dst.join(format!("{}.partial", name))] {
            if p.exists() {
                die(&format!(
                    "pile-map: {} already exists; refusing to mix a new pass into an old directory",
                    p.display()
                ));
            }
        }
    }
    let pile = PathBuf::from(&a.pile);
    let pile_bytes = ck(std::fs::metadata(&pile), &format!("stat {}", pile.display())).len();
    if let Some(lim) = a.limit {
        if lim == 0 || lim > pile_bytes {
            die(&format!("pile-map: --limit-bytes {} is not inside {} ({} bytes)", lim, pile.display(), pile_bytes));
        }
    }
    let covered_cap = a.limit.unwrap_or(pile_bytes);
    let shard_dir = PathBuf::from(&a.shards);
    let shards = shard_list(&shard_dir);
    if shards.len() > u16::MAX as usize {
        die("pile-map: more than 65535 shards does not fit the u16 shard column");
    }
    let shard_names: Vec<String> = shards.iter().map(|p| p.file_name().unwrap().to_string_lossy().into_owned()).collect();
    let sums = read_sha256sums(&shard_dir);
    if sums.is_empty() {
        die(&format!(
            "pile-map: no SHA256SUMS.txt beside {}: the shard identities must be recordable in the MANIFEST",
            shard_dir.display()
        ));
    }
    for n in &shard_names {
        if !sums.contains_key(n) {
            die(&format!("pile-map: shard {} is not listed in SHA256SUMS.txt: unverifiable provenance", n));
        }
    }

    // ---- pass A: decode shards in order, launching a bounded window so a
    // ---- --limit-bytes run never decodes shards it cannot use. One reader
    // ---- thread per decompressor, never N writers into one reader; and every
    // ---- child is killed or fully drained before any wait().
    let results: Arc<Mutex<Vec<Option<ShardRows>>>> = Arc::new(Mutex::new((0..shards.len()).map(|_| None).collect()));
    let prog = Arc::new(Progress::new(shards.len()));
    let jobs = a.jobs.max(1);
    let (tx, rx) = mpsc::channel::<usize>();
    let mut handles: Vec<std::thread::JoinHandle<()>> = Vec::new();
    let mut started = 0usize;
    let mut finished = 0usize;
    let mut last_bytes = 0u64;
    let mut last_moved = Instant::now();
    let mut last_print = Instant::now();
    let n_shards = shards.len();
    loop {
        let (contig, contig_bytes, all_done) = {
            let res = results.lock().unwrap();
            let mut b = 0u64;
            let mut contig = 0usize;
            for r in res.iter() {
                match r {
                    Some(r) => {
                        b += r.bytes;
                        contig += 1;
                    }
                    None => break,
                }
            }
            let all = res.iter().all(|r| r.is_some());
            while started < n_shards && started < contig + jobs && b < covered_cap {
                let path = shards[started].clone();
                let idx = started;
                // Only the bytes up to the budget are needed; stopping early
                // keeps a 10 GB slice from decoding a 44 GB shard.
                let budget = covered_cap.saturating_sub(b).saturating_add(READ_BUF as u64);
                let res = Arc::clone(&results);
                let prog = Arc::clone(&prog);
                let tx = tx.clone();
                handles.push(std::thread::spawn(move || {
                    let rows = scan_shard(&path, budget, idx, &prog);
                    res.lock().unwrap()[idx] = Some(rows);
                    let _ = tx.send(idx);
                }));
                started += 1;
            }
            (contig, b, all)
        };
        if all_done || (started == n_shards && finished == started) || (started == finished && contig_bytes >= covered_cap) {
            break;
        }
        let _ = contig;
        match rx.recv_timeout(Duration::from_secs(1)) {
            Ok(_) => finished += 1,
            Err(RecvTimeoutError::Timeout) => {
                let now = prog.bytes.load(Ordering::Relaxed);
                if now != last_bytes {
                    last_bytes = now;
                    last_moved = Instant::now();
                } else if last_moved.elapsed() > Duration::from_secs(120) {
                    // A hang must be a failure with a message, never silence.
                    let st = prog.per_shard.lock().unwrap().clone();
                    for (i, s) in st.iter().enumerate() {
                        if s.pid != 0 {
                            let _ = Command::new("kill").arg("-9").arg(s.pid.to_string()).status();
                            eprintln!(
                                "PILE_MAP_STALL shard={}/{} records={} bytes={} pid={}",
                                i, n_shards, s.records, s.bytes, s.pid
                            );
                        }
                    }
                    die(&format!(
                        "pile-map: no decompressor made progress for 120 s ({} of {} shards launched, {} finished); killed the children and stopped. Nothing was published",
                        started, n_shards, finished
                    ));
                }
                if last_print.elapsed() > Duration::from_secs(30) {
                    last_print = Instant::now();
                    let st = prog.per_shard.lock().unwrap().clone();
                    let (mut recs, mut bytes) = (0u64, 0u64);
                    let mut live = 0usize;
                    for s in st.iter() {
                        recs += s.records;
                        bytes += s.bytes;
                        if s.pid != 0 {
                            live += 1;
                        }
                    }
                    let el = t0.elapsed().as_secs_f64().max(1e-9);
                    println!(
                        "PILE_MAP_PROGRESS shard={}/{} records={} bytes={} mb_per_s={:.1} live={}",
                        finished, n_shards, recs, bytes, bytes as f64 / el / 1e6, live
                    );
                }
            }
            Err(RecvTimeoutError::Disconnected) => break,
        }
    }
    for h in handles.drain(..) {
        h.join().unwrap_or_else(|_| die("pile-map: a shard worker panicked"));
    }
    let results = Arc::try_unwrap(results)
        .unwrap_or_else(|_| die("pile-map: shard workers outlived the pass"))
        .into_inner()
        .unwrap();
    let mut rows: Vec<ShardRows> = Vec::new();
    let mut _launched = 0usize;
    for (i, r) in results.into_iter().enumerate() {
        if i >= started {
            // Not launched: --limit-bytes was already covered by earlier
            // shards, so this one contributes nothing to the map.
            break;
        }
        match r {
            Some(r) => {
                if r.bytes > 0 {
                    _launched += 1;
                }
                rows.push(r)
            }
            None => die(&format!("pile-map: shard {} ({}) never ran", i, shard_names[i])),
        }
    }
    if rows.is_empty() {
        die("pile-map: no shard was decoded");
    }
    let blank: u64 = rows.iter().map(|r| r.blank_lines).sum();
    if blank > 0 {
        die(&format!("pile-map: {} blank line(s) in the shard stream: a record would be silently missing from the map", blank));
    }
    crate::trace_since("pile_map_shards", t0);
    let t_scan = Instant::now();

    // ---- the map
    let mut m = build_map(&rows, shard_names.clone(), a.limit);
    let doc_count = m.doc_count();
    if doc_count == 0 {
        die("pile-map: the shards yielded 0 documents");
    }
    // Shards that actually appear in the map (the launch window may have
    // started one or two more than the byte budget needed).
    let shards_used = m.shard[m.shard.len() - 1] as usize + 1;
    let complete = doc_count - u64::from(m.partial_tail);
    if m.partial_tail {
        let last = doc_count as usize - 1;
        let si = m.shard[last] as usize;
        m.cut_text = extract_record(&shards[si], m.rec[last]);
        if (m.cut_text.len() as u64) < m.len[last] {
            die(&format!(
                "pile-map: the cut tail document claims {} bytes but {} record {} holds only {}",
                m.len[last],
                shard_names[si],
                m.rec[last],
                m.cut_text.len()
            ));
        }
    }

    // ---- pass B: pile.txt verifies every document
    let t_ver = Instant::now();
    let v = verify_pile(&pile, &m, jobs);
    crate::trace_since("pile_map_verify", t_ver);
    if let Some((d, bad)) = &v.first_bad {
        let detail = explain_mismatch(&pile, &m, &shards, *d);
        die(&format!(
            "pile-map: THE SHARD STREAM DOES NOT REPRODUCE {}: {}{}\n\
             xsa: note: the map must describe the file we index, not an approximation; nothing was published",
            pile.display(),
            bad,
            detail
        ));
    }
    if m.corpus_bytes > covered_cap {
        die(&format!(
            "pile-map: the shards account for {} bytes, more than the {} byte range asked for",
            m.corpus_bytes, covered_cap
        ));
    }
    // With --limit-bytes the shard stream may end first (a head cut of the
    // whole corpus): the map then covers what the shards hold and SAYS how much
    // of the requested range is uncovered. Without a limit the map must cover
    // the file exactly, or the shards are truncated and we refuse.
    let limit_trailing = covered_cap - m.corpus_bytes;
    if a.limit.is_none() && limit_trailing != 0 {
        die(&format!(
            "pile-map: the shards account for {} bytes but {} holds {}: the shard stream is short; refusing to publish a map that silently ends early",
            m.corpus_bytes,
            pile.display(),
            pile_bytes
        ));
    }
    // The cut tail's internal count is only knowable from the pile bytes.
    if m.partial_tail && !m.internal.is_empty() {
        let last = m.internal.len() - 1;
        assert_eq!(m.internal[last], u32::MAX);
        m.internal[last] = v.tail_internal;
    }
    // Identity the 0x1E scan must satisfy: every 0x1E in the range is either
    // the framing byte of a document that owns it or a byte inside a text.
    let internal_in_range: u64 = m.internal.iter().map(|&x| x as u64).sum();
    if v.seps != complete + internal_in_range {
        die(&format!(
            "pile-map: 0x1E reconciliation failed: pile carries {} in the range, map says {} framing + {} internal",
            v.seps, complete, internal_in_range
        ));
    }
    if v.framed != complete {
        die(&format!("pile-map: {} documents should own their framing 0x1E but {} do", complete, v.framed));
    }
    let pile_sha = if a.pile_sha256 { Some(hash_prefix(&pile, covered_cap)) } else { None };
    if a.pile_sha256 {
        crate::trace_since("pile_map_sha256", t_ver);
    }

    // ---- sidecars
    let t_write = Instant::now();
    let (seps_bytes, seps_sha) = write_seps(&m, &dst.join("seps.bin.partial"));
    let (titles_bytes, titles_sha) = write_titles(&m, &dst.join("titles.bin.partial"));
    let (meta_bytes, meta_sha) = write_meta(&m, &dst.join("meta.bin.partial"));
    for name in ["seps.bin", "titles.bin", "meta.bin"] {
        let part = dst.join(format!("{}.partial", name));
        let done = dst.join(name);
        ck(std::fs::rename(&part, &done), &format!("rename {} -> {}", part.display(), done.display()));
    }
    if let Ok(d) = File::open(dst) {
        ck(d.sync_all(), &format!("fsync {}", dst.display()));
    }
    crate::trace_since("pile_map_write", t_write);

    // A digest that pins the corpus content given this map: the document
    // SHA-256s in doc_id order.
    let mut cd = Sha256::new();
    for h in &m.hash {
        cd.update(h);
    }
    let content_digest = format!("{:x}", cd.finalize());

    // Per-component internal-0x1E census: which part of the Pile carries them.
    let mut per_name: Vec<(u64, u64, u64)> = vec![(0, 0, 0); m.names_tab.len()]; // docs, bytes, internal
    for i in 0..doc_count as usize {
        let e = &mut per_name[m.name[i] as usize];
        e.0 += 1;
        e.1 += m.len[i];
        e.2 += m.internal[i] as u64;
    }
    let mut census: Vec<(String, u64, u64, u64)> = m
        .names_tab
        .iter()
        .enumerate()
        .map(|(i, n)| (String::from_utf8_lossy(n).into_owned(), per_name[i].0, per_name[i].1, per_name[i].2))
        .collect();
    census.sort_by_key(|c| std::cmp::Reverse(c.3));
    for c in &census {
        println!(
            "SEP_INTERNAL_COMPONENT name=\"{}\" docs={} bytes={} internal_0x1e={} per_gb={:.3}",
            c.0,
            c.1,
            c.2,
            c.3,
            if c.2 > 0 { c.3 as f64 * 1e9 / c.2 as f64 } else { 0.0 }
        );
    }
    let total_internal = internal_in_range;
    println!(
        "SEP_INTERNAL records={} internal_0x1e={} ratio={:.9} bytes={}",
        doc_count,
        total_internal,
        total_internal as f64 / doc_count as f64,
        m.corpus_bytes
    );

    let shard_entries: Vec<serde_json::Value> = shards
        .iter()
        .enumerate()
        .map(|(i, p)| {
            let name = shard_names[i].clone();
            serde_json::json!({
                "index": i,
                "name": name,
                "path": p.display().to_string(),
                "bytes": std::fs::metadata(p).map(|md| md.len()).unwrap_or(0),
                "sha256_expected": sums.get(&name),
                "sha256_note": "from the release's SHA256SUMS.txt; the compressed file is not re-hashed here, but every decoded record of it is checked byte for byte against the pile",
            })
        })
        .collect();
    let manifest = serde_json::json!({
        "tool": "xsa corpus --pile-map",
        "manifest_version": 1,
        "kind": "pile-document-map",
        "pile_path": pile.display().to_string(),
        "pile_bytes_on_disk": pile_bytes,
        "pile_sha256_covered_prefix": pile_sha,
        "pile_sha256_note": "sha256 of pile[0, corpus_bytes); pass --pile-sha256 to record it. corpus_bytes == pile_bytes_on_disk for a map over the whole file",
        "corpus_bytes": m.corpus_bytes,
        "limit_bytes": a.limit,
        "limit_trailing_bytes": if a.limit.is_some() { Some(limit_trailing) } else { None },
        "trailing_bytes_uncovered": pile_bytes - m.corpus_bytes,
        "shards_used": shards_used,
        "doc_count": doc_count,
        "documents_owning_separator": complete,
        "partial_tail": m.partial_tail,
        "partial_tail_meaning": "the last document is cut by --limit-bytes (a head slice of the file); it has no trailing 0x1E inside the range",
        "separator": "0x1e",
        "separator_byte": SEP,
        "separator_count": v.seps,
        "internal_separator_bytes": total_internal,
        "separator_identity": "separator_count == documents_owning_separator + internal_separator_bytes (asserted during the pass)",
        "internal_separator_by_component": census.iter().map(|c| serde_json::json!({"pile_set_name": c.0, "docs": c.1, "bytes": c.2, "internal_0x1e": c.3})).collect::<Vec<_>>(),
        "shards_dir": shard_dir.display().to_string(),
        "shard_count": shards.len(),
        "shards": shard_entries,
        "construction": "zstdcat pile/EleutherAI_ThePile_v1/pile/train/*.zst | json2bytes --null-delim - > pile.txt",
        "document_layout": "for shard 00..29 in filename order, for each record in file order: the JSON-unescaped <text> value followed by one 0x1E byte. A text may itself contain 0x1E, so doc i's boundaries come from the shard accounting, never from scanning the pile for 0x1E. doc i's text is [offset, offset+length); its framing 0x1E, when present, is at offset+length",
        "verification": {
            "mode": "every document: pile[offset, offset+length) SHA-256 == shard record text SHA-256, and pile[offset+length] == 0x1E",
            "documents_compared": v.compared,
            "mismatches": v.mismatches,
            "content_digest_sha256": content_digest,
            "content_digest_meaning": "SHA-256 over the concatenation of the per-document SHA-256s in doc_id order: with seps.bin+meta.bin it pins every byte of the covered pile range",
        },
        "sidecars": {
            "seps.bin": { "bytes": seps_bytes, "sha256": seps_sha, "format": "XSEP v1", "anchor_stride": ANCHOR_STRIDE },
            "titles.bin": { "bytes": titles_bytes, "sha256": titles_sha, "format": "XTIT v1", "column": "pile_set_name" },
            "meta.bin": { "bytes": meta_bytes, "sha256": meta_sha, "format": "XPM1 v1", "row_bytes": META_ROW },
        },
        "set_names": m.names_tab.iter().map(|n| String::from_utf8_lossy(n).into_owned()).collect::<Vec<_>>(),
        "layout": {
            "seps.bin": "48B header: b\"XSEP\", u32 version=1, u32 flags(bit0=0x1E emitted), u32 anchor_stride=64, u64 doc_count, u64 corpus_bytes, u64 anchor_count, u64 base; then doc_count*u32 LE start deltas; then anchor_count*u64 LE absolute starts of every 64th document",
            "titles.bin": "20B header: b\"XTIT\", u32 version=1, u32 doc_count, u64 payload_bytes; then concatenated pile_set_names; then doc_count*u32 LE offsets; then doc_count*u16 LE lengths",
            "meta.bin": "68B header: b\"XPM1\", u32 version=1, u64 doc_count, u64 corpus_bytes, u64 shard_count, u64 name_count, u64 name_payload, u64 shard_payload, u32 row_bytes=34, u64 partial_tail; then set-name payload, name_count*u32 offsets, name_count*u16 lengths; then shard-name payload, shard_count*u32 offsets, shard_count*u16 lengths; then doc_count rows of {u64 offset, u64 length, u16 shard, u32 set_name, u64 record, u32 internal_0x1e}",
        },
        "query": "xsa query <container> --docs <this dir> <pattern>  ->  TSV pattern_id, doc_id, offset_in_doc, pile_set_name, global_offset",
        "build_command": format!(
            "xsa corpus --pile-map --pile {} --shards {} --out {}{}",
            pile.display(),
            shard_dir.display(),
            dst.display(),
            match a.limit { Some(l) => format!(" --limit-bytes {}", l), None => String::new() }
        ),
        "jobs": jobs,
        "peak_rss_kb": peak_rss_kb(),
        "timings_ms": {
            "shards_decode": t_scan.elapsed().as_secs_f64() * 1e3,
            "pile_verify": t_ver.elapsed().as_secs_f64() * 1e3,
            "sidecars": t_write.elapsed().as_secs_f64() * 1e3,
            "total": t0.elapsed().as_secs_f64() * 1e3,
        },
        "finished_at_unix": now_unix(),
    });
    let mpath = dst.join("MANIFEST.json");
    let mut body = serde_json::to_string_pretty(&manifest).unwrap_or_else(|e| die(&format!("serialise manifest: {}", e)));
    body.push('\n');
    let mut f = ck(File::create(&mpath), &format!("create {}", mpath.display()));
    ck(f.write_all(body.as_bytes()), &format!("write {}", mpath.display()));
    ck(f.sync_all(), &format!("fsync {}", mpath.display()));
    crate::trace_since("pile_map_wall", t0);

    println!(
        "PILEMAP docs={} corpus_bytes={} covered={} partial_tail={} shards={} seps.bin={} titles.bin={} meta.bin={}",
        doc_count,
        m.corpus_bytes,
        covered_cap,
        u64::from(m.partial_tail),
        shards.len(),
        seps_bytes,
        titles_bytes,
        meta_bytes
    );
    if a.limit.is_some() {
        let lasti = doc_count as usize - 1;
        println!(
            "PILE_MAP_LIMIT limit_bytes={} covered_bytes={} trailing_bytes={} shards_used={} last_shard={} last_record={} partial_tail={}",
            covered_cap,
            m.corpus_bytes,
            limit_trailing,
            shards_used,
            m.shard_names[m.shard[lasti] as usize],
            m.rec[lasti],
            u64::from(m.partial_tail)
        );
    }
    println!(
        "DOCCOUNT range_bytes={} records={} internal_0x1e={} separators={} identity_ok={} entries={} partial_tail={}",
        m.corpus_bytes,
        complete,
        total_internal,
        v.seps,
        u64::from(v.seps == complete + total_internal && v.framed == complete),
        doc_count,
        u64::from(m.partial_tail)
    );
    println!(
        "DOCHASH docs_compared={} mismatches={} separators={} content_digest={}",
        v.compared, v.mismatches, v.seps, content_digest
    );
    let last = doc_count as usize - 1;
    let ends = m.off[last] + m.len[last] + u64::from(!m.partial_tail || m.off[last] + m.len[last] < m.corpus_bytes);
    println!(
        "DOCEND last_doc={} last_offset={} last_length={} covered_end={} corpus_bytes={} file_bytes={} uncovered={}",
        last,
        m.off[last],
        m.len[last],
        ends,
        m.corpus_bytes,
        pile_bytes,
        pile_bytes - m.corpus_bytes
    );
    println!("PILEMAP_MANIFEST {}", mpath.display());
}

// ---------------------------------------------------------------------------
// Sample verification: re-extract records from the shards and compare bytes
// ---------------------------------------------------------------------------

pub struct VerifyArgs {
    pub map: String,
    pub pile: String,
    pub shards: Option<String>,
    pub samples: usize,
    pub jobs: usize,
}

pub fn verify(a: &VerifyArgs) {
    let dir = Path::new(&a.map);
    let m = read_meta(&dir.join("meta.bin"));
    let n = m.doc_count();
    if n == 0 {
        die("pile-verify: the map holds no documents");
    }
    let shard_dir = match &a.shards {
        Some(s) => PathBuf::from(s),
        None => {
            let txt = ck(std::fs::read_to_string(dir.join("MANIFEST.json")), "read MANIFEST.json");
            let v: serde_json::Value = serde_json::from_str(&txt).unwrap_or_else(|e| die(&format!("MANIFEST.json: {}", e)));
            PathBuf::from(v["shards_dir"].as_str().unwrap_or_else(|| die("MANIFEST.json has no shards_dir; pass --shards")))
        }
    };
    let want = a.samples.max(1) as u64;
    let mut ids: Vec<usize> = (0..want)
        .map(|i| ((i as u128 * (n as u128 - 1)) / (want as u128 - 1).max(1)) as usize)
        .collect();
    // The gate that matters: documents that carry a literal 0x1E in their text.
    // A boundary found by scanning the pile would split exactly these.
    let internal: Vec<usize> = (0..n as usize).filter(|&i| m.internal[i] > 0).collect();
    let take = internal.len().min(ids.len().max(20));
    for k in 0..take {
        ids.push(internal[(k as u128 * internal.len() as u128 / take as u128) as usize]);
    }
    ids.sort_unstable();
    ids.dedup();
    let sampled_internal = ids.iter().filter(|&&i| m.internal[i] > 0).count();
    // Group by shard; doc ids ascend so record indices ascend inside a shard.
    let mut by_shard: HashMap<u16, Vec<usize>> = HashMap::new();
    for &id in &ids {
        by_shard.entry(m.shard[id]).or_default().push(id);
    }
    let pile = PathBuf::from(&a.pile);
    let pile_len = ck(std::fs::metadata(&pile), &format!("stat {}", pile.display())).len();
    if m.corpus_bytes > pile_len {
        die(&format!("pile-verify: the map covers {} bytes but {} is only {}", m.corpus_bytes, pile.display(), pile_len));
    }
    let f = ck(File::open(&pile), &format!("open {}", pile.display()));
    let pmap = unsafe { memmap2::Mmap::map(&f) }.unwrap_or_else(|e| die(&format!("mmap {}: {}", pile.display(), e)));
    let pool = rayon::ThreadPoolBuilder::new()
        .num_threads(a.jobs.max(1))
        .build()
        .unwrap_or_else(|e| die(&format!("pile-verify: thread pool: {}", e)));
    let mut shards: Vec<(u16, Vec<usize>)> = by_shard.into_iter().collect();
    shards.sort();
    println!("DOCVERIFY_START samples={} internal_samples={} shards={}", ids.len(), sampled_internal, shards.len());
    let out: Vec<(u64, u64, Vec<String>)> = pool.install(|| {
        use rayon::prelude::*;
        shards
            .par_iter()
            .map(|(si, doc_ids)| {
                let path = shard_dir.join(&m.shard_names[*si as usize]);
                let mut targets: Vec<(u64, usize)> = doc_ids.iter().map(|&d| (m.rec[d], d)).collect();
                targets.sort();
                let mut ti = 0usize;
                let mut exact = 0u64;
                let mut internal_ok = 0u64;
                let mut bad: Vec<String> = Vec::new();
                let mut cmd = Command::new("zstd");
                cmd.arg("-dc").arg(&path).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::inherit());
                let mut child = cmd.spawn().unwrap_or_else(|e| die(&format!("pile-verify: zstd -dc {}: {}", path.display(), e)));
                let mut so = child.stdout.take().expect("piped stdout");
                let mut buf = vec![0u8; READ_BUF];
                let mut line: Vec<u8> = Vec::new();
                let mut scanned = 0usize;
                let mut record = 0u64;
                'read: loop {
                    let k = match so.read(&mut buf) {
                        Ok(0) => break,
                        Ok(k) => k,
                        Err(e) if e.kind() == ErrorKind::Interrupted => continue,
                        Err(e) => die(&format!("pile-verify: read {}: {}", path.display(), e)),
                    };
                    line.extend_from_slice(&buf[..k]);
                    let mut start = 0usize;
                    while let Some(rel) = line[scanned..].iter().position(|&b| b == b'\n') {
                        let end = scanned + rel;
                        let blank = line[start..end].iter().all(|b| b.is_ascii_whitespace());
                        if !blank && ti < targets.len() && targets[ti].0 == record {
                            let d = targets[ti].1;
                            let text = record_text(&line[start..end], &path, record);
                            let internal_hits = count_byte(&text, SEP);
                            match compare(&pmap, &m, d, &text) {
                                Ok(()) => {
                                    exact += 1;
                                    if internal_hits > 0 {
                                        internal_ok += 1;
                                        if internal_hits as u32 != m.internal[d] {
                                            bad.push(format!(
                                                "doc={} internal_0x1e shard={} map={}",
                                                d, internal_hits, m.internal[d]
                                            ));
                                        }
                                    }
                                }
                                Err(msg) => bad.push(msg),
                            }
                            ti += 1;
                        }
                        if !blank {
                            record += 1;
                        }
                        start = end + 1;
                        scanned = start;
                        if ti == targets.len() {
                            break 'read;
                        }
                    }
                    if start > 0 {
                        line.drain(..start);
                    }
                    scanned = line.len();
                }
                drop(so);
                let st = ck(child.wait(), "wait for zstd");
                if !st.success() {
                    die(&format!("pile-verify: zstd -dc {} failed ({})", path.display(), st));
                }
                if ti != targets.len() {
                    die(&format!(
                        "pile-verify: shard {} ended after {} records; the map claims record {} exists",
                        path.display(),
                        record,
                        targets.last().unwrap().0
                    ));
                }
                (exact, internal_ok, bad)
            })
            .collect()
    });
    let mut exact = 0u64;
    let mut internal_ok = 0u64;
    let mut bad: Vec<String> = Vec::new();
    for (e, i, b) in out {
        exact += e;
        internal_ok += i;
        bad.extend(b);
    }
    println!(
        "DOCVERIFY n={} exact={} mismatches={} internal_sampled={} internal_exact={}",
        ids.len(),
        exact,
        bad.len(),
        sampled_internal,
        internal_ok
    );
    if !bad.is_empty() {
        for b in bad.iter().take(20) {
            eprintln!("DOCVERIFY_MISMATCH {}", b);
        }
        die("pile-verify: the map does not describe pile.txt");
    }
}

fn record_text(line: &[u8], path: &Path, record: u64) -> Vec<u8> {
    let v: serde_json::Value = serde_json::from_slice(line)
        .unwrap_or_else(|e| die(&format!("pile-verify: {}: record {} is not JSON ({})", path.display(), record, e)));
    v.get("text")
        .and_then(|t| t.as_str())
        .unwrap_or_else(|| die(&format!("pile-verify: {}: record {} has no string \"text\"", path.display(), record)))
        .as_bytes()
        .to_vec()
}

fn compare(pmap: &[u8], m: &DocMap, d: usize, text: &[u8]) -> Result<(), String> {
    let (off, len) = (m.off[d], m.len[d]);
    if text.len() as u64 != len {
        return Err(format!(
            "doc={} shard={} record={} offset={} map_length={} shard_text_length={} first_differing_byte=n/a(length)",
            d,
            m.shard[d],
            m.rec[d],
            off,
            len,
            text.len()
        ));
    }
    let mine = &pmap[off as usize..(off + len) as usize];
    if mine != text {
        let at = mine.iter().zip(text.iter()).position(|(a, b)| a != b).unwrap_or(0);
        let ctx = |s: &[u8]| -> String {
            let lo = at.saturating_sub(20);
            let hi = (at + 20).min(s.len());
            s[lo..hi].iter().map(|&b| if (0x20..0x7f).contains(&b) { b as char } else { '.' }).collect::<String>()
        };
        return Err(format!(
            "doc={} shard={} record={} offset={} length={} first_differing_byte={} (relative {}) pile=[{}] shard=[{}]",
            d,
            m.shard[d],
            m.rec[d],
            off,
            len,
            off + at as u64,
            at,
            ctx(mine),
            ctx(text)
        ));
    }
    // The framing claim for this document, on the same read.
    if !(d + 1 == m.off.len() && m.partial_tail) {
        let sep = pmap[(off + len) as usize];
        if sep != SEP {
            return Err(format!(
                "doc={} offset={} length={} expected 0x1E at {} but found 0x{:02x}",
                d,
                off,
                len,
                off + len,
                sep
            ));
        }
    }
    Ok(())
}
