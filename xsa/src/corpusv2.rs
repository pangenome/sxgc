//! xsa corpus --v2-* — CORPUS V2: the separator-free corpus format.
//!
//! CORPUS_V2.md is the specification of record; this module implements it.
//! The corpus is the METADATA (sources, starts, docs, order); the byte file
//! `<name>.bin` is an optional, regenerable cache. There is no framing byte
//! anywhere: the corpus stream is the raw concatenation of document texts and
//! boundaries live only in `<name>.starts`.
//!
//!     xsa corpus --v2-meta        --shards DIR --out PREFIX
//!                                 [--limit-bytes N] [--order FILE | --order-seed S]
//!     xsa corpus --v2-materialize --prefix P [--shards DIR] [--jobs J]
//!     xsa corpus --v2-reorder     --from P1 --out P2 (--order FILE | --order-seed S)
//!     xsa corpus --v2-verify      --prefix P [--shards DIR] [--samples N]
//!                                 [--reference P1] [--jobs J]
//!     xsa corpus --v2-locate      --prefix P --offset O[,O...]
//!
//! Decompressor discipline (pilemap's, debugged twice, kept exactly): ONE
//! `zstd -dc` alive at a time on the meta scan (shards are decoded
//! sequentially by a single worker thread); one reader per decompressor; the
//! reader CLOSES ITS END OF THE PIPE before wait() so a full pipe can never
//! wedge the reap; a decompressor stopped early on purpose is killed and its
//! SIGPIPE/kill exit is accepted; a decompressor that fails when it was NOT
//! stopped on purpose is a hard error - never emit tables from a truncated
//! decode. The meta driver polls progress once a second and kills the child
//! after 120 s with no decode progress, naming the shard and record.
//!
//! Nothing materialises text except --v2-materialize (the explicit cache write):
//! the meta pass keeps only lengths and provenance, verify and locate read
//! windows, and there are no per-suffix or per-position columns here.

use std::collections::HashMap;
use std::fs::File;
use std::io::{BufWriter, ErrorKind, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use memmap2::Mmap;
use sha2::{Digest, Sha256};

use crate::die;

const ANCHOR_STRIDE: u64 = 64;
const STARTS_HEADER: u64 = 48; // XCS2 v1: same shape as XSEP v1
const DOCS_HEADER: u64 = 68; // XPM1 v1 shape
const DOCS_ROW: u64 = 34;
const ORDER_HEADER: u64 = 28; // XORD v1
const READ_BUF: usize = 8 << 20;
const NO_BUDGET: u64 = u64::MAX;

fn ck<T>(r: std::io::Result<T>, what: &str) -> T {
    r.unwrap_or_else(|e| die(&format!("corpusv2: {}: {}", what, e)))
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
            Err(e) => die(&format!("corpusv2: read {}: {}", path.display(), e)),
        }
    }
    format!("{:x}", h.finalize())
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

fn now_unix() -> i64 {
    match std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH) {
        Ok(d) => d.as_secs() as i64,
        Err(e) => -(e.duration().as_secs() as i64),
    }
}

// ---------------------------------------------------------------------------
// Small writing helper (patch-header-then-finish, pilemap's PatchFile shape)
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
            die(&format!("corpusv2: {} is {} bytes but {} were written", self.path.display(), len, self.n));
        }
        (len, hash_file(&self.path))
    }
}

// ---------------------------------------------------------------------------
// The seed generator (CORPUS_V2.md §4.3; the manifest names the same algorithm)
// ---------------------------------------------------------------------------

struct SplitMix64(u64);

impl SplitMix64 {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }
}

pub const ORDER_ALGORITHM: &str = "splitmix64+fisher-yates-descending+lemire-widening-multiply";

/// perm[k] = the source document id placed at corpus position k.
fn permutation_from_seed(n: u64, seed: u64) -> Vec<u32> {
    assert!(n >= 1, "corpusv2: an empty corpus has no permutation");
    assert!(n <= u32::MAX as u64, "corpusv2: more than 2^32-1 documents cannot carry u32 source ids");
    let mut perm: Vec<u32> = (0..n as u32).collect();
    let mut rng = SplitMix64(seed);
    let mut i = n;
    while i > 1 {
        i -= 1;
        let u = rng.next();
        // Lemire: floor(u * (i+1) / 2^64) in u128 - unbiased in [0, i].
        let j = ((u as u128) * ((i as u128) + 1) >> 64) as u64;
        perm.swap(i as usize, j as usize);
    }
    perm
}

/// A bijection of 0..n or refuse. Never assumed (CORPUS_V2.md §4.4).
fn assert_bijection(perm: &[u32], what: &str) {
    let n = perm.len();
    let mut seen = vec![false; n];
    for &v in perm {
        if v as usize >= n || seen[v as usize] {
            die(&format!(
                "corpusv2: {} is not a bijection of 0..{} (value {} repeats or is out of range); refusing",
                what, n, v
            ));
        }
        seen[v as usize] = true;
    }
}

// ---------------------------------------------------------------------------
// The corpus tables
// ---------------------------------------------------------------------------

/// Scan output, SOURCE order (shard order, record order, empty texts excluded).
#[derive(Default)]
struct ScanOut {
    len: Vec<u64>,
    shard: Vec<u16>,
    rec: Vec<u64>,
    comp: Vec<u32>,
    comp_tab: Vec<Vec<u8>>,
    excluded_empty: u64,
    shards_scanned: usize,
}

/// The corpus in CORPUS order: what every writer writes and every reader reads.
struct Corpus {
    doc_count: u64,
    corpus_bytes: u64,
    /// corpus start of document k
    start: Vec<u64>,
    /// corpus rows, in CORPUS order (row k = the text at [start[k], start[k]+len[k]))
    len: Vec<u64>,
    shard: Vec<u16>,
    rec: Vec<u64>,
    comp: Vec<u32>,
    /// construction-time source doc id of corpus position k
    src_id: Vec<u32>,
    ordered: bool,
    order_seed: Option<u64>,
    comp_tab: Vec<Vec<u8>>,
    shard_names: Vec<String>,
}

impl Corpus {
    /// Build the corpus-order tables from the source-order scan. When a
    /// permutation is given, the corpus ROWS (len/shard/rec/comp) are permuted
    /// into corpus order; `.docs` row k is then corpus document k, whose
    /// provenance names source record perm[k].
    fn from_scan(mut s: ScanOut, shard_names: Vec<String>, perm: Option<Vec<u32>>, order_seed: Option<u64>) -> Corpus {
        if s.len.is_empty() {
            die("corpusv2: the shards yielded 0 documents; refusing to emit an empty corpus");
        }
        for &l in &s.len {
            if l == 0 || l > u32::MAX as u64 {
                die(&format!(
                    "corpusv2: a document of {} bytes cannot be addressed by the u32 delta column",
                    l
                ));
            }
        }
        let n = s.len.len() as u64;
        let corpus_bytes: u64 = s.len.iter().sum();
        let mut c = Corpus {
            doc_count: n,
            corpus_bytes,
            start: Vec::with_capacity(n as usize),
            len: std::mem::take(&mut s.len),
            shard: std::mem::take(&mut s.shard),
            rec: std::mem::take(&mut s.rec),
            comp: std::mem::take(&mut s.comp),
            src_id: (0..n as u32).collect(),
            ordered: perm.is_some(),
            order_seed,
            comp_tab: std::mem::take(&mut s.comp_tab),
            shard_names,
        };
        if let Some(p) = perm {
            assert_bijection(&p, "--order permutation");
            c.src_id = p.clone();
            let len = std::mem::take(&mut c.len);
            let shard = std::mem::take(&mut c.shard);
            let rec = std::mem::take(&mut c.rec);
            let comp = std::mem::take(&mut c.comp);
            c.len = p.iter().map(|&k| len[k as usize]).collect();
            c.shard = p.iter().map(|&k| shard[k as usize]).collect();
            c.rec = p.iter().map(|&k| rec[k as usize]).collect();
            c.comp = p.iter().map(|&k| comp[k as usize]).collect();
        }
        let mut cursor = 0u64;
        for &l in &c.len {
            c.start.push(cursor);
            cursor += l;
        }
        assert_eq!(cursor, c.corpus_bytes);
        c
    }
}

// ---------------------------------------------------------------------------
// The meta scan over the shards (lengths + provenance only; NO text kept)
// ---------------------------------------------------------------------------

/// Live state for the stall watchdog: bytes decoded and the current child pid.
struct ScanProgress {
    bytes: AtomicU64,
    records: AtomicU64,
    pid: AtomicU32,
}

/// Runs on the worker thread; decodes shards SEQUENTIALLY so exactly one
/// decompressor is alive at any moment. `limit` is the --limit-bytes budget
/// (None = unlimited). Stops as soon as a shard has yielded the whole remaining
/// budget: every later document ends beyond it and cannot be admitted.
fn scan_all_shards(shards: &[PathBuf], limit: Option<u64>, prog: &ScanProgress) -> ScanOut {
    let mut out = ScanOut::default();
    let mut intern: HashMap<Vec<u8>, u32> = HashMap::new();
    let mut remaining = limit.unwrap_or(NO_BUDGET);
    for (si, path) in shards.iter().enumerate() {
        if remaining == 0 {
            break;
        }
        out.shards_scanned = si + 1;
        let mut cmd = Command::new("zstd");
        cmd.arg("-dc").arg(path).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::inherit());
        let mut child = cmd.spawn().unwrap_or_else(|e| {
            die(&format!(
                "corpusv2: cannot run `zstd -dc {}` (a zstd on PATH is required): {}",
                path.display(),
                e
            ))
        });
        prog.pid.store(child.id(), Ordering::SeqCst);
        let mut so = child.stdout.take().expect("piped stdout");
        let mut buf = vec![0u8; READ_BUF];
        let mut line: Vec<u8> = Vec::with_capacity(1 << 16);
        let mut scanned = 0usize;
        let mut record = 0u64; // non-blank records, the pilemap convention
        let mut shard_bytes = 0u64; // text bytes this shard yielded so far
        let mut stopped_early = false;
        'read: loop {
            let n = match so.read(&mut buf) {
                Ok(0) => break,
                Ok(n) => n,
                Err(e) if e.kind() == ErrorKind::Interrupted => continue,
                Err(e) => die(&format!("corpusv2: read {}: {}", path.display(), e)),
            };
            prog.bytes.fetch_add(n as u64, Ordering::Relaxed);
            line.extend_from_slice(&buf[..n]);
            let mut start = 0usize;
            while let Some(rel) = line[scanned..].iter().position(|&b| b == b'\n') {
                let end = scanned + rel;
                let raw = &line[start..end];
                if raw.iter().all(|&b| b.is_ascii_whitespace()) {
                    die(&format!(
                        "corpusv2: {} line {}: blank line in the shard stream: a record would be silently missing",
                        path.display(),
                        record
                    ));
                }
                let v: serde_json::Value = serde_json::from_slice(raw).unwrap_or_else(|e| {
                    die(&format!("corpusv2: {}: record {} is not JSON ({})", path.display(), record, e))
                });
                let text = v
                    .get("text")
                    .and_then(|t| t.as_str())
                    .unwrap_or_else(|| {
                        die(&format!("corpusv2: {}: record {} has no string \"text\" field", path.display(), record))
                    });
                let name = v
                    .get("meta")
                    .and_then(|m| m.get("pile_set_name"))
                    .and_then(|s| s.as_str())
                    .unwrap_or_else(|| {
                        die(&format!(
                            "corpusv2: {}: record {} has no string meta.pile_set_name",
                            path.display(),
                            record
                        ))
                    });
                let body = text.as_bytes();
                if body.is_empty() {
                    // CORPUS_V2.md §3.2: an empty document is not representable
                    // (its delta would be 0); exclude, count, report.
                    out.excluded_empty += 1;
                } else {
                    let id = match intern.get(name.as_bytes()) {
                        Some(&id) => id,
                        None => {
                            let id = out.comp_tab.len() as u32;
                            let bytes = name.as_bytes().to_vec();
                            intern.insert(bytes.clone(), id);
                            out.comp_tab.push(bytes);
                            id
                        }
                    };
                    out.len.push(body.len() as u64);
                    out.shard.push(si as u16);
                    out.rec.push(record);
                    out.comp.push(id);
                    shard_bytes += body.len() as u64;
                }
                prog.records.fetch_add(1, Ordering::Relaxed);
                record += 1;
                start = end + 1;
                scanned = start;
                if remaining != NO_BUDGET && shard_bytes >= remaining {
                    stopped_early = true;
                    break 'read;
                }
            }
            if start > 0 {
                line.drain(..start);
            }
            scanned = line.len();
        }
        if !stopped_early && !line.is_empty() {
            die(&format!(
                "corpusv2: {}: a partial record (no trailing newline) ended the shard stream; refusing to parse it",
                path.display()
            ));
        }
        // The pipe must close before wait: an open pipe keeps zstd writing into
        // a full buffer and wait never returns.
        drop(so);
        if stopped_early {
            // Closed on purpose: zstd dies of SIGPIPE (or is killed here);
            // this shard contributed exactly the records already collected.
            let _ = child.kill();
        }
        let st = ck(child.wait(), "wait for zstd");
        if !st.success() && !(stopped_early && st.code().is_none()) {
            die(&format!(
                "corpusv2: `zstd -dc {}` failed ({}): the shard stream did not decode; refusing tables from a truncated shard",
                path.display(),
                st
            ));
        }
        prog.pid.store(0, Ordering::SeqCst);
        if stopped_early {
            break;
        }
        remaining = remaining.saturating_sub(shard_bytes);
    }
    out
}

/// The admission rule on top of the scan: documents are admitted while they
/// fit the budget whole; the first that does not fit ends the corpus
/// (CORPUS_V2.md §3.3 - V2 never cuts a document). Returns the corpus bytes.
fn apply_budget(s: &mut ScanOut, limit: u64) -> u64 {
    let mut total = 0u64;
    let mut k = 0usize;
    while k < s.len.len() {
        let l = s.len[k];
        if total + l > limit {
            break;
        }
        total += l;
        k += 1;
    }
    s.len.truncate(k);
    s.shard.truncate(k);
    s.rec.truncate(k);
    s.comp.truncate(k);
    if k == 0 {
        die(&format!(
            "corpusv2: --limit-bytes {} admits no document (the first document is larger than the budget)",
            limit
        ));
    }
    total
}

// ---------------------------------------------------------------------------
// Record extraction (materialize, verify): one decompressor per shard, one
// reader per decompressor, targets in record order, early stop + kill.
// ---------------------------------------------------------------------------

fn extract_records<F: FnMut(u64, &[u8])>(path: &Path, targets: &[u64], mut hit: F) {
    if targets.is_empty() {
        return;
    }
    let mut cmd = Command::new("zstd");
    cmd.arg("-dc").arg(path).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::inherit());
    let mut child = cmd.spawn().unwrap_or_else(|e| die(&format!("corpusv2: zstd -dc {}: {}", path.display(), e)));
    let mut so = child.stdout.take().expect("piped stdout");
    let mut buf = vec![0u8; READ_BUF];
    let mut line: Vec<u8> = Vec::new();
    let mut scanned = 0usize;
    let mut record = 0u64;
    let mut ti = 0usize;
    let mut stopped_early = false;
    'read: loop {
        let n = match so.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) if e.kind() == ErrorKind::Interrupted => continue,
            Err(e) => die(&format!("corpusv2: read {}: {}", path.display(), e)),
        };
        line.extend_from_slice(&buf[..n]);
        let mut start = 0usize;
        while let Some(rel) = line[scanned..].iter().position(|&b| b == b'\n') {
            let end = scanned + rel;
            let raw = &line[start..end];
            let blank = raw.iter().all(|&b| b.is_ascii_whitespace());
            if !blank {
                if ti < targets.len() && targets[ti] == record {
                    let v: serde_json::Value = serde_json::from_slice(raw).unwrap_or_else(|e| {
                        die(&format!("corpusv2: {}: record {} is not JSON ({})", path.display(), record, e))
                    });
                    let text = v
                        .get("text")
                        .and_then(|t| t.as_str())
                        .unwrap_or_else(|| {
                            die(&format!("corpusv2: {}: record {} has no string \"text\"", path.display(), record))
                        });
                    hit(record, text.as_bytes());
                    ti += 1;
                    if ti == targets.len() {
                        stopped_early = true;
                        break 'read;
                    }
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
    drop(so);
    if stopped_early {
        let _ = child.kill();
    }
    let st = ck(child.wait(), "wait for zstd");
    if !st.success() && !(stopped_early && st.code().is_none()) {
        die(&format!("corpusv2: zstd -dc {} failed ({})", path.display(), st));
    }
    if ti != targets.len() {
        die(&format!(
            "corpusv2: {} ended after {} records; the tables claim record {} exists",
            path.display(),
            record,
            targets[ti]
        ));
    }
}

// ---------------------------------------------------------------------------
// Writers (CORPUS_V2.md §2)
// ---------------------------------------------------------------------------

fn write_starts(c: &Corpus, path: &Path) -> (u64, String) {
    let n = c.doc_count;
    let anchors = n.div_ceil(ANCHOR_STRIDE);
    if c.start.len() as u64 != n {
        die("corpusv2: internal: start table does not match doc_count");
    }
    let mut f = PatchFile::new(path, STARTS_HEADER);
    for k in 0..n as usize {
        let d = if k == 0 { c.start[0] } else { c.start[k] - c.start[k - 1] };
        if d > u32::MAX as u64 {
            die(&format!(
                "corpusv2: document {} is {} bytes after its predecessor; the u32 delta column cannot address it",
                k, d
            ));
        }
        f.put_u32(d as u32);
    }
    for k in 0..anchors as usize {
        f.put_u64(c.start[k * ANCHOR_STRIDE as usize]);
    }
    let mut h = Vec::with_capacity(STARTS_HEADER as usize);
    h.extend_from_slice(b"XCS2");
    h.extend_from_slice(&1u32.to_le_bytes()); // version
    h.extend_from_slice(&0u32.to_le_bytes()); // flags: bit0 (separator emitted) is ALWAYS 0 in V2
    h.extend_from_slice(&(ANCHOR_STRIDE as u32).to_le_bytes());
    h.extend_from_slice(&n.to_le_bytes());
    h.extend_from_slice(&c.corpus_bytes.to_le_bytes());
    h.extend_from_slice(&anchors.to_le_bytes());
    h.extend_from_slice(&0u64.to_le_bytes()); // base
    assert_eq!(h.len() as u64, STARTS_HEADER);
    f.patch(0, &h);
    let (bytes, sha) = f.finish();
    let want = STARTS_HEADER + 4 * n + 8 * anchors;
    if bytes != want {
        die(&format!("corpusv2: {} is {} bytes, layout says {}", path.display(), bytes, want));
    }
    (bytes, sha)
}

fn write_docs(c: &Corpus, path: &Path) -> (u64, String) {
    let n = c.doc_count;
    let name_payload: u64 = c.comp_tab.iter().map(|t| t.len() as u64).sum();
    let shard_payload: u64 = c.shard_names.iter().map(|t| t.len() as u64).sum();
    let mut f = PatchFile::new(path, DOCS_HEADER);
    for t in &c.comp_tab {
        f.put(t);
    }
    let mut o = 0u64;
    for t in &c.comp_tab {
        f.put_u32(o as u32);
        o += t.len() as u64;
    }
    for t in &c.comp_tab {
        f.put_u16(t.len() as u16);
    }
    for t in &c.shard_names {
        f.put(t.as_bytes());
    }
    let mut o = 0u64;
    for t in &c.shard_names {
        f.put_u32(o as u32);
        o += t.len() as u64;
    }
    for t in &c.shard_names {
        f.put_u16(t.len() as u16);
    }
    for k in 0..n as usize {
        f.put_u64(c.start[k]);
        f.put_u64(c.len[k]);
        f.put_u16(c.shard[k]);
        f.put_u32(c.comp[k]);
        f.put_u64(c.rec[k]);
        f.put_u32(0); // internal_0x1e: RESERVED, ALWAYS 0 in V2
    }
    let mut h = Vec::with_capacity(DOCS_HEADER as usize);
    h.extend_from_slice(b"XPM1");
    h.extend_from_slice(&1u32.to_le_bytes());
    h.extend_from_slice(&n.to_le_bytes());
    h.extend_from_slice(&c.corpus_bytes.to_le_bytes());
    h.extend_from_slice(&(c.shard_names.len() as u64).to_le_bytes());
    h.extend_from_slice(&(c.comp_tab.len() as u64).to_le_bytes());
    h.extend_from_slice(&name_payload.to_le_bytes());
    h.extend_from_slice(&shard_payload.to_le_bytes());
    h.extend_from_slice(&(DOCS_ROW as u32).to_le_bytes());
    h.extend_from_slice(&0u64.to_le_bytes()); // partial_tail: ALWAYS 0 in V2 (a document is never cut)
    assert_eq!(h.len() as u64, DOCS_HEADER);
    f.patch(0, &h);
    let (bytes, sha) = f.finish();
    let want = DOCS_HEADER
        + name_payload
        + 6 * c.comp_tab.len() as u64
        + shard_payload
        + 6 * c.shard_names.len() as u64
        + DOCS_ROW * n;
    if bytes != want {
        die(&format!("corpusv2: {} is {} bytes, layout says {}", path.display(), bytes, want));
    }
    (bytes, sha)
}

/// The .order file is written exactly when the order differs from source order;
/// ABSENT means source order (CORPUS_V2.md §2.3).
fn write_order(c: &Corpus, path: &Path, from_seed: bool) -> (u64, String) {
    if !c.ordered {
        die("corpusv2: internal: writing an .order file for a source-order corpus");
    }
    let mut f = PatchFile::new(path, 0);
    let mut h = Vec::with_capacity(ORDER_HEADER as usize);
    h.extend_from_slice(b"XORD");
    h.extend_from_slice(&1u32.to_le_bytes());
    h.extend_from_slice(&u32::from(from_seed).to_le_bytes());
    h.extend_from_slice(&c.doc_count.to_le_bytes());
    h.extend_from_slice(&c.order_seed.unwrap_or(0).to_le_bytes());
    assert_eq!(h.len() as u64, ORDER_HEADER);
    f.put(&h);
    for &s in &c.src_id {
        f.put_u32(s);
    }
    let (bytes, sha) = f.finish();
    let want = ORDER_HEADER + 4 * c.doc_count;
    if bytes != want {
        die(&format!("corpusv2: {} is {} bytes, layout says {}", path.display(), bytes, want));
    }
    (bytes, sha)
}

fn write_manifest(path: &Path, manifest: &serde_json::Value) {
    let mut body = serde_json::to_string_pretty(manifest).unwrap_or_else(|e| die(&format!("serialise manifest: {}", e)));
    body.push('\n');
    let mut f = ck(File::create(path), &format!("create {}", path.display()));
    ck(f.write_all(body.as_bytes()), &format!("write {}", path.display()));
    ck(f.sync_all(), &format!("fsync {}", path.display()));
}

fn corpus_paths(prefix: &Path) -> (PathBuf, PathBuf, PathBuf, PathBuf) {
    let dir = prefix.parent().map(|p| p.to_path_buf()).unwrap_or_else(|| PathBuf::from("."));
    let dir = if dir.as_os_str().is_empty() { PathBuf::from(".") } else { dir };
    let name = prefix
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| die("corpusv2: the corpus prefix must name a file stem (got a directory)"));
    let sfx = |ext: &str| dir.join(format!("{}.{}", name, ext));
    (sfx("starts"), sfx("docs"), sfx("order"), dir.join("MANIFEST.json"))
}

fn bin_paths(prefix: &Path) -> (PathBuf, PathBuf) {
    let name = prefix.file_name().unwrap().to_string_lossy().into_owned();
    (prefix.with_file_name(format!("{}.bin", name)), prefix.with_file_name(format!("{}.bin.sha256", name)))
}

/// Claim the output of a table-writing pass: nothing may pre-exist (a refused
/// pass must never leave something that looks like a corpus), and the
/// directory's MANIFEST.json must not exist (one manifest per directory).
fn claim_output(prefix: &Path, want_order: bool) -> (PathBuf, PathBuf, PathBuf, PathBuf) {
    let (starts, docs, order, manifest) = corpus_paths(prefix);
    let mut paths = vec![starts.clone(), docs.clone(), manifest.clone(), order.clone()];
    for p in &paths {
        if p.exists() {
            die(&format!(
                "corpusv2: {} already exists; refusing to mix a new pass into an old corpus",
                p.display()
            ));
        }
        let partial = p.with_extension(format!("{}.partial", p.extension().unwrap_or_default().to_string_lossy()));
        if partial.exists() {
            die(&format!("corpusv2: {} already exists (an incomplete pass); refusing to mix passes", partial.display()));
        }
    }
    if !want_order && order.exists() {
        die(&format!(
            "corpusv2: {} exists but this pass is source order; an .order file must not pre-exist",
            order.display()
        ));
    }
    (starts, docs, order, manifest)
}

fn mkdir_of(prefix: &Path) -> PathBuf {
    let dir = prefix.parent().map(|p| p.to_path_buf()).unwrap_or_else(|| PathBuf::from("."));
    let dir = if dir.as_os_str().is_empty() { PathBuf::from(".") } else { dir };
    ck(std::fs::create_dir_all(&dir), &format!("mkdir {}", dir.display()));
    dir
}

// ---------------------------------------------------------------------------
// Readers (every invariant in CORPUS_V2.md §2 is asserted here)
// ---------------------------------------------------------------------------

fn read_starts(path: &Path) -> (u64, u64, Vec<u64>) {
    // (doc_count, corpus_bytes, starts)
    let bytes = ck(std::fs::read(path), &format!("read {}", path.display()));
    let u32at = |o: usize| u32::from_le_bytes(bytes[o..o + 4].try_into().unwrap()) as u64;
    let u64at = |o: usize| u64::from_le_bytes(bytes[o..o + 8].try_into().unwrap());
    if bytes.len() < STARTS_HEADER as usize || &bytes[..4] != b"XCS2" || u32at(4) != 1 {
        die(&format!("corpusv2: {} is not an XCS2 v1 starts table", path.display()));
    }
    if u32at(8) != 0 {
        die(&format!(
            "corpusv2: {}: flags say a separator is emitted; V2 has no separator (flags must be 0)",
            path.display()
        ));
    }
    if u32at(12) != ANCHOR_STRIDE {
        die(&format!("corpusv2: {}: anchor stride {} is not {}", path.display(), u32at(12), ANCHOR_STRIDE));
    }
    let n = u64at(16);
    let corpus_bytes = u64at(24);
    let anchors = u64at(32);
    let base = u64at(40);
    if n == 0 {
        die(&format!("corpusv2: {}: zero documents", path.display()));
    }
    if anchors != n.div_ceil(ANCHOR_STRIDE) {
        die(&format!("corpusv2: {}: anchor_count {} does not match doc_count {}", path.display(), anchors, n));
    }
    if bytes.len() as u64 != STARTS_HEADER + 4 * n + 8 * anchors {
        die(&format!("corpusv2: {}: file length does not match the XCS2 header", path.display()));
    }
    let mut start = Vec::with_capacity(n as usize);
    let mut cursor = base;
    let mut sum = 0u64;
    for k in 0..n as usize {
        let d = u32at(STARTS_HEADER as usize + 4 * k);
        if k == 0 && d != 0 {
            die(&format!("corpusv2: {}: the first delta is {} but document 0 starts at base 0", path.display(), d));
        }
        if k > 0 && d == 0 {
            die(&format!("corpusv2: {}: delta {} is 0: a zero-length document is not representable in V2", path.display(), k));
        }
        cursor = cursor
            .checked_add(d)
            .unwrap_or_else(|| die(&format!("corpusv2: {}: start overflow at document {}", path.display(), k)));
        sum += d;
        start.push(cursor);
    }
    if sum != corpus_bytes {
        die(&format!(
            "corpusv2: {}: the deltas sum to {} but the header says corpus_bytes {}",
            path.display(), sum, corpus_bytes
        ));
    }
    for a in 0..anchors as usize {
        let want = start[a * ANCHOR_STRIDE as usize];
        let got = u64at(STARTS_HEADER as usize + 4 * n as usize + 8 * a);
        if got != want {
            die(&format!("corpusv2: {}: anchor {} is {} but the deltas say {}", path.display(), a, got, want));
        }
    }
    (n, corpus_bytes, start)
}

#[allow(clippy::too_many_arguments /* the XPM1 row, spelled out */)]
fn read_docs(
    path: &Path,
) -> (u64, u64, Vec<u64>, Vec<u64>, Vec<u16>, Vec<u64>, Vec<u32>, Vec<Vec<u8>>, Vec<String>) {
    // (doc_count, corpus_bytes, start, len, shard, rec, comp, comp_tab, shard_names)
    let bytes = ck(std::fs::read(path), &format!("read {}", path.display()));
    let u32at = |o: usize| u32::from_le_bytes(bytes[o..o + 4].try_into().unwrap()) as u64;
    let u64at = |o: usize| u64::from_le_bytes(bytes[o..o + 8].try_into().unwrap());
    let u16at = |o: usize| u16::from_le_bytes(bytes[o..o + 2].try_into().unwrap()) as u64;
    if bytes.len() < DOCS_HEADER as usize || &bytes[..4] != b"XPM1" || u32at(4) != 1 {
        die(&format!("corpusv2: {} is not an XPM1 v1 docs table", path.display()));
    }
    let n = u64at(8);
    let corpus_bytes = u64at(16);
    let shard_count = u64at(24) as usize;
    let name_count = u64at(32) as usize;
    let name_payload = u64at(40) as usize;
    let shard_payload = u64at(48) as usize;
    if u32at(56) != DOCS_ROW {
        die(&format!("corpusv2: {}: row size is not {}", path.display(), DOCS_ROW));
    }
    if u64at(60) != 0 {
        die(&format!("corpusv2: {}: partial_tail is set; a V2 corpus never cuts a document", path.display()));
    }
    let mut p = DOCS_HEADER as usize;
    let comp_blob = &bytes[p..p + name_payload];
    p += name_payload;
    let comp_offs = p;
    p += 4 * name_count;
    let comp_lens = p;
    p += 2 * name_count;
    let shard_blob = &bytes[p..p + shard_payload];
    p += shard_payload;
    let shard_offs = p;
    p += 4 * shard_count;
    let shard_lens = p;
    p += 2 * shard_count;
    let rows = p;
    if bytes.len() != rows + DOCS_ROW as usize * n as usize {
        die(&format!("corpusv2: {}: file length does not match the XPM1 header", path.display()));
    }
    let mut comp_tab = Vec::with_capacity(name_count);
    for i in 0..name_count {
        let o = u32at(comp_offs + 4 * i) as usize;
        let l = u16at(comp_lens + 2 * i) as usize;
        if o + l > comp_blob.len() {
            die(&format!("corpusv2: {}: component name {} is out of range", path.display(), i));
        }
        comp_tab.push(comp_blob[o..o + l].to_vec());
    }
    let mut shard_names = Vec::with_capacity(shard_count);
    for i in 0..shard_count {
        let o = u32at(shard_offs + 4 * i) as usize;
        let l = u16at(shard_lens + 2 * i) as usize;
        if o + l > shard_blob.len() {
            die(&format!("corpusv2: {}: shard name {} is out of range", path.display(), i));
        }
        shard_names.push(String::from_utf8_lossy(&shard_blob[o..o + l]).into_owned());
    }
    let mut start = Vec::with_capacity(n as usize);
    let mut len = Vec::with_capacity(n as usize);
    let mut shard = Vec::with_capacity(n as usize);
    let mut rec = Vec::with_capacity(n as usize);
    let mut comp = Vec::with_capacity(n as usize);
    let mut expect = 0u64;
    for k in 0..n as usize {
        let r = rows + DOCS_ROW as usize * k;
        let off = u64at(r);
        let l = u64at(r + 8);
        let sh = u16at(r + 16) as u16;
        let cp = u32at(r + 18) as u32;
        let rc = u64at(r + 22);
        if u32at(r + 30) != 0 {
            die(&format!(
                "corpusv2: {}: row {} carries a nonzero internal_0x1e; the column is reserved 0 in V2",
                path.display(), k
            ));
        }
        if l == 0 {
            die(&format!(
                "corpusv2: {}: row {} has length 0; a zero-length document is not representable",
                path.display(), k
            ));
        }
        if off != expect {
            die(&format!(
                "corpusv2: {}: row {} starts at {} but the previous row ends at {} (rows must tile)",
                path.display(), k, off, expect
            ));
        }
        if sh as usize >= shard_count || cp as usize >= name_count {
            die(&format!("corpusv2: {}: row {} names a missing shard or component entry", path.display(), k));
        }
        start.push(off);
        len.push(l);
        shard.push(sh);
        rec.push(rc);
        comp.push(cp);
        expect = off + l;
    }
    if n > 0 && expect != corpus_bytes {
        die(&format!(
            "corpusv2: {}: rows end at {} but corpus_bytes is {} (sum of lengths must be the corpus)",
            path.display(), expect, corpus_bytes
        ));
    }
    (n, corpus_bytes, start, len, shard, rec, comp, comp_tab, shard_names)
}

fn read_order(path: &Path, doc_count: u64) -> Option<Vec<u32>> {
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) if e.kind() == ErrorKind::NotFound => return None,
        Err(e) => die(&format!("corpusv2: read {}: {}", path.display(), e)),
    };
    let u32at = |o: usize| u32::from_le_bytes(bytes[o..o + 4].try_into().unwrap());
    let u64at = |o: usize| u64::from_le_bytes(bytes[o..o + 8].try_into().unwrap());
    if bytes.len() < ORDER_HEADER as usize || &bytes[..4] != b"XORD" || u32at(4) != 1 {
        die(&format!("corpusv2: {} is not an XORD v1 order table", path.display()));
    }
    if u64at(12) != doc_count {
        die(&format!(
            "corpusv2: {}: holds {} documents but the corpus has {}",
            path.display(), u64at(12), doc_count
        ));
    }
    if bytes.len() as u64 != ORDER_HEADER + 4 * doc_count {
        die(&format!("corpusv2: {}: file length does not match the XORD header", path.display()));
    }
    let mut perm = Vec::with_capacity(doc_count as usize);
    for k in 0..doc_count as usize {
        perm.push(u32at(ORDER_HEADER as usize + 4 * k));
    }
    assert_bijection(&perm, &format!("{}", path.display()));
    Some(perm)
}

/// Load a whole V2 corpus (tables only, no bytes). All §2 invariants asserted,
/// including the starts/docs agreement (the same data by construction).
fn load_corpus(prefix: &Path) -> Loaded {
    let (starts_path, docs_path, order_path, manifest_path) = corpus_paths(prefix);
    let (n1, corpus_bytes, starts) = read_starts(&starts_path);
    let (n2, corpus_bytes2, start2, len, shard, rec, comp, comp_tab, shard_names) = read_docs(&docs_path);
    if n1 != n2 {
        die(&format!(
            "corpusv2: {} says {} documents but {} says {}",
            starts_path.display(), n1, docs_path.display(), n2
        ));
    }
    if corpus_bytes != corpus_bytes2 {
        die(&format!(
            "corpusv2: {} says corpus_bytes {} but {} says {}",
            starts_path.display(), corpus_bytes, docs_path.display(), corpus_bytes2
        ));
    }
    if starts != start2 {
        die(&format!(
            "corpusv2: {} and {} disagree on document starts; the tables must agree by construction",
            starts_path.display(), docs_path.display()
        ));
    }
    let perm = read_order(&order_path, n1);
    let src_id = perm.clone().unwrap_or_else(|| (0..n1 as u32).collect());
    let manifest = match std::fs::read_to_string(&manifest_path) {
        Ok(s) => Some(serde_json::from_str::<serde_json::Value>(&s).unwrap_or_else(|e| {
            die(&format!("corpusv2: {}: {}", manifest_path.display(), e))
        })),
        Err(e) if e.kind() == ErrorKind::NotFound => None,
        Err(e) => die(&format!("corpusv2: read {}: {}", manifest_path.display(), e)),
    };
    if let Some(m) = &manifest {
        if m["kind"].as_str() != Some("corpus-v2") {
            die(&format!("corpusv2: {}: kind is not corpus-v2", manifest_path.display()));
        }
    }
    Loaded {
        prefix: prefix.to_path_buf(),
        doc_count: n1,
        corpus_bytes,
        start: starts,
        len,
        shard,
        rec,
        comp,
        comp_tab,
        shard_names,
        src_id,
        ordered: perm.is_some(),
        manifest,
    }
}

struct Loaded {
    prefix: PathBuf,
    doc_count: u64,
    corpus_bytes: u64,
    start: Vec<u64>,
    len: Vec<u64>,
    shard: Vec<u16>,
    rec: Vec<u64>,
    comp: Vec<u32>,
    comp_tab: Vec<Vec<u8>>,
    shard_names: Vec<String>,
    src_id: Vec<u32>,
    ordered: bool,
    manifest: Option<serde_json::Value>,
}

impl Loaded {
    fn comp_of(&self, k: usize) -> String {
        String::from_utf8_lossy(&self.comp_tab[self.comp[k] as usize]).into_owned()
    }
    fn shards_dir(&self, explicit: Option<&str>) -> PathBuf {
        if let Some(s) = explicit {
            return PathBuf::from(s);
        }
        if let Some(m) = &self.manifest {
            if let Some(s) = m["source_access"]["shards_dir"].as_str() {
                return PathBuf::from(s);
            }
        }
        die(&format!(
            "corpusv2: the manifest of {} has no source_access.shards_dir; pass --shards DIR",
            self.prefix.display()
        ));
    }
    /// Offset -> (doc_id, offset_in_doc) through the V2 starts: anchor-aware
    /// binary search over the start column (at most 63 deltas per block).
    fn locate(&self, pos: u64) -> (u64, u64) {
        if pos >= self.corpus_bytes {
            die(&format!("corpusv2: offset {} is outside the corpus (0..{})", pos, self.corpus_bytes));
        }
        let mut lo = 0usize;
        let mut hi = self.doc_count as usize - 1;
        while lo < hi {
            let mid = lo + (hi - lo + 1) / 2;
            if self.start[mid] <= pos {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        (lo as u64, pos - self.start[lo])
    }
}

// ---------------------------------------------------------------------------
// Shard directory handling (pilemap's rules: filename order IS the order)
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
        die(&format!("corpusv2: no *.zst shards under {}", dir.display()));
    }
    for (i, p) in v.iter().enumerate() {
        let name = p.file_name().unwrap().to_string_lossy();
        if !name.starts_with(&format!("{:02}", i)) {
            die(&format!(
                "corpusv2: shard {} is {}, which does not continue the filename order 00,01,...; the source sequence IS filename order",
                i, p.display()
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

fn manifest_shards(
    shards_dir: &Path,
    shards: &[PathBuf],
    used: usize,
    sums: &HashMap<String, String>,
    note: &str,
) -> serde_json::Value {
    serde_json::json!({
        "shards_dir": shards_dir.display().to_string(),
        "shard_count": shards.len(),
        "shards_used": used,
        "shards": shards.iter().enumerate().map(|(i, p)| serde_json::json!({
            "index": i,
            "name": p.file_name().unwrap().to_string_lossy(),
            "path": p.display().to_string(),
            "bytes": std::fs::metadata(p).map(|m| m.len()).unwrap_or(0),
            "sha256_expected": sums.get(p.file_name().unwrap().to_string_lossy().as_ref()),
            "sha256_note": "from the release's SHA256SUMS.txt; not re-hashed here, but every sampled record is re-extracted and byte-compared by --v2-verify",
            "used": i < used,
        })).collect::<Vec<_>>(),
        "note": note,
    })
}

// ---------------------------------------------------------------------------
// Command: --v2-meta (the primary, always-legal operation)
// ---------------------------------------------------------------------------

pub struct MetaArgs {
    pub shards: String,
    pub out: String,
    pub limit: Option<u64>,
    pub order: Option<String>,
    pub order_seed: Option<u64>,
}

pub fn meta(a: &MetaArgs) {
    let t0 = Instant::now();
    if a.order.is_some() && a.order_seed.is_some() {
        die("corpusv2: give either --order FILE or --order-seed S, not both");
    }
    let prefix = PathBuf::from(&a.out);
    mkdir_of(&prefix);
    let want_order = a.order.is_some() || a.order_seed.is_some();
    let (starts_path, docs_path, order_path, manifest_path) = claim_output(&prefix, want_order);
    let shards_dir = PathBuf::from(&a.shards);
    let shards = shard_list(&shards_dir);
    if shards.len() > u16::MAX as usize {
        die("corpusv2: more than 65535 shards does not fit the u16 shard column");
    }
    let shard_names: Vec<String> = shards
        .iter()
        .map(|p| p.file_name().unwrap().to_string_lossy().into_owned())
        .collect();
    let sums = read_sha256sums(&shards_dir);
    if sums.is_empty() {
        die(&format!(
            "corpusv2: no SHA256SUMS.txt beside {}: source identity must be recordable in the MANIFEST",
            shards_dir.display()
        ));
    }

    // One decompressor at a time, on one worker thread; the driver polls and
    // kills after 120 s without decode progress (the pilemap discipline).
    let prog = Arc::new(ScanProgress { bytes: AtomicU64::new(0), records: AtomicU64::new(0), pid: AtomicU32::new(0) });
    let (tx, rx) = std::sync::mpsc::channel::<ScanOut>();
    let shards2 = shards.clone();
    let limit2 = a.limit;
    let prog2 = Arc::clone(&prog);
    let worker = std::thread::spawn(move || {
        let out = scan_all_shards(&shards2, limit2, &prog2);
        let _ = tx.send(out);
    });
    let mut last_bytes = 0u64;
    let mut last_moved = Instant::now();
    let mut last_print = Instant::now();
    let mut scan = loop {
        match rx.recv_timeout(Duration::from_secs(1)) {
            Ok(out) => break out,
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {
                let now = prog.bytes.load(Ordering::Relaxed);
                if now != last_bytes {
                    last_bytes = now;
                    last_moved = Instant::now();
                } else if last_moved.elapsed() > Duration::from_secs(120) {
                    let pid = prog.pid.load(Ordering::SeqCst);
                    if pid != 0 {
                        let _ = Command::new("kill").arg("-9").arg(pid.to_string()).status();
                    }
                    die(&format!(
                        "corpusv2: no decompressor made progress for 120 s ({} records, {} bytes decoded; killed zstd pid {}); nothing was published",
                        prog.records.load(Ordering::Relaxed),
                        now,
                        pid
                    ));
                }
                if last_print.elapsed() > Duration::from_secs(30) {
                    last_print = Instant::now();
                    println!(
                        "V2_META_PROGRESS records={} bytes={} pid={}",
                        prog.records.load(Ordering::Relaxed),
                        prog.bytes.load(Ordering::Relaxed),
                        prog.pid.load(Ordering::SeqCst)
                    );
                }
            }
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
                worker.join().unwrap_or_else(|_| die("corpusv2: the scan worker panicked"));
                die("corpusv2: the scan worker exited without a result");
            }
        }
    };
    worker.join().unwrap_or_else(|_| die("corpusv2: the scan worker panicked"));
    crate::trace_since("v2_meta_scan", t0);
    let t_adm = Instant::now();

    let excluded = scan.excluded_empty;
    let corpus_bytes = match a.limit {
        Some(lim) => apply_budget(&mut scan, lim),
        None => scan.len.iter().sum(),
    };
    let budget_end = if a.limit.is_some() {
        let last = scan.len.len() - 1;
        Some((scan.shard[last] as usize, scan.rec[last]))
    } else {
        None
    };
    let shards_used = scan.shard.last().map(|&s| s as usize + 1).unwrap_or(1).min(shards.len());
    for name in shard_names.iter().take(shards_used) {
        if !sums.contains_key(name) {
            die(&format!("corpusv2: shard {} is not listed in SHA256SUMS.txt: unverifiable provenance", name));
        }
    }
    let perm = if let Some(seed) = a.order_seed {
        Some(permutation_from_seed(scan.len.len() as u64, seed))
    } else if let Some(file) = &a.order {
        match read_order(&PathBuf::from(file), scan.len.len() as u64) {
            Some(perm) => Some(perm),
            None => die(&format!("corpusv2: --order file {} does not exist or is not XORD v1", file)),
        }
    } else {
        None
    };
    let c = Corpus::from_scan(scan, shard_names.clone(), perm, a.order_seed);
    assert_eq!(c.corpus_bytes, corpus_bytes);
    crate::trace_since("v2_meta_admission", t_adm);

    // ---- write the tables
    let t_write = Instant::now();
    let (starts_bytes, starts_sha) = write_starts(&c, &starts_path);
    let (docs_bytes, docs_sha) = write_docs(&c, &docs_path);
    let (order_bytes, order_sha) = if c.ordered {
        let (b, s) = write_order(&c, &order_path, a.order_seed.is_some());
        (Some(b), Some(s))
    } else {
        (None, None)
    };
    if let Some(dir) = starts_path.parent() {
        if let Ok(d) = File::open(dir) {
            ck(d.sync_all(), &format!("fsync {}", dir.display()));
        }
    }
    crate::trace_since("v2_meta_write", t_write);

    let name = prefix.file_name().unwrap().to_string_lossy().into_owned();
    let order_json = match (&a.order_seed, &a.order) {
        (Some(seed), _) => serde_json::json!({
            "kind": "seed",
            "seed": seed,
            "algorithm": ORDER_ALGORITHM,
            "materialized": true,
            "materialized_because": "the permutation is data: no consumer reimplements the generator",
        }),
        (None, Some(f)) => serde_json::json!({
            "kind": "file",
            "file": f,
            "materialized": true,
        }),
        (None, None) => serde_json::json!({ "kind": "source", "meaning": "corpus document k IS source document k (identity; no .order file)" }),
    };
    let source_access = manifest_shards(
        &shards_dir,
        &shards,
        shards_used,
        &sums,
        "index-time source kinds: a raw file (materialize with --v2-materialize) or a block-compressed archive with a block index (RESERVED extension); plain .jsonl.zst is NOT random-access capable and must be converted first (CORPUS_V2.md 1.1)",
    );
    let construction = format!(
        "xsa corpus --v2-meta --shards {} --out {}{}{}",
        shards_dir.display(),
        prefix.display(),
        a.limit.map(|l| format!(" --limit-bytes {}", l)).unwrap_or_default(),
        match (&a.order, a.order_seed) {
            (Some(f), _) => format!(" --order {}", f),
            (None, Some(s)) => format!(" --order-seed {}", s),
            _ => String::new(),
        }
    );
    let manifest = serde_json::json!({
        "tool": "xsa corpus --v2-meta",
        "manifest_version": 1,
        "kind": "corpus-v2",
        "name": name,
        "separator": "none",
        "framing_byte": serde_json::Value::Null,
        "principle": "the framing byte does not exist: the corpus is the raw concatenation of document texts and boundaries live only in this metadata (CORPUS_V2.md 0)",
        "doc_count": c.doc_count,
        "corpus_bytes": c.corpus_bytes,
        "order": order_json,
        "limit_bytes": a.limit,
        "limit_rule": if a.limit.is_some() {
            "documents admitted while they fit the budget whole; the first that does not fit ends the corpus; V2 never cuts a document (boundary_aligned by construction)"
        } else { serde_json::Value::Null },
        "budget_ended_at": budget_end.map(|(s, r)| serde_json::json!({"shard": s, "record": r, "meaning": "provenance of the LAST ADMITTED document; the corpus ends at a document boundary by construction"})),
        "excluded": { "empty_text_records": excluded, "rule": "an empty document is not representable (its delta would be 0); excluded records keep their record numbering for provenance" },
        "source_access": source_access,
        "construction": construction,
        "sidecars": {
            "starts": { "file": format!("{}.starts", name), "bytes": starts_bytes, "sha256": starts_sha, "format": "XCS2 v1" },
            "docs": { "file": format!("{}.docs", name), "bytes": docs_bytes, "sha256": docs_sha, "format": "XPM1 v1 shape, corpus order, internal_0x1e reserved 0" },
            "order": if let (Some(b), Some(s)) = (order_bytes, order_sha) {
                serde_json::json!({ "file": format!("{}.order", name), "bytes": b, "sha256": s, "format": "XORD v1" })
            } else { serde_json::Value::Null },
        },
        "bin": "optional cache, NOT written by this pass; regenerate any time with: xsa corpus --v2-materialize --prefix <this prefix>",
        "timings_ms": {
            "scan": t_adm.elapsed().as_secs_f64() * 1e3,
            "write": t_write.elapsed().as_secs_f64() * 1e3,
            "total": t0.elapsed().as_secs_f64() * 1e3,
        },
        "peak_rss_kb": peak_rss_kb(),
        "finished_at_unix": now_unix(),
    });
    write_manifest(&manifest_path, &manifest);
    println!(
        "V2_META prefix={} docs={} corpus_bytes={} excluded_empty={} order={} starts_bytes={} docs_bytes={} order_bytes={}",
        prefix.display(),
        c.doc_count,
        c.corpus_bytes,
        excluded,
        if c.ordered { "file" } else { "source" },
        starts_bytes,
        docs_bytes,
        order_bytes.unwrap_or(0)
    );
    if let Some(lim) = a.limit {
        println!(
            "V2_META_LIMIT limit_bytes={} corpus_bytes={} boundary_aligned=true shards_used={} last_admitted_shard={} last_admitted_record={}",
            lim,
            c.corpus_bytes,
            shards_used,
            budget_end.map(|b| b.0).unwrap_or(0),
            budget_end.map(|b| b.1).unwrap_or(0)
        );
    }
    println!("V2_META_MANIFEST {}", manifest_path.display());
    crate::trace_since("v2_meta_wall", t0);
}

// ---------------------------------------------------------------------------
// Command: --v2-materialize (the optional cache write)
// ---------------------------------------------------------------------------

pub struct MaterializeArgs {
    pub prefix: String,
    pub shards: Option<String>,
    pub jobs: usize,
}

pub fn materialize(a: &MaterializeArgs) {
    let t0 = Instant::now();
    let prefix = PathBuf::from(&a.prefix);
    let l = load_corpus(&prefix);
    let (bin_path, sha_path) = bin_paths(&prefix);
    for p in [&bin_path, &sha_path] {
        if p.exists() {
            die(&format!(
                "corpusv2: {} already exists; the cache is regenerable - delete it first, never mix passes",
                p.display()
            ));
        }
    }
    let shards_dir = l.shards_dir(a.shards.as_deref());
    let shards = shard_list(&shards_dir);
    let n = l.doc_count;
    // Where does each source record go in the corpus? Positions are known from
    // the tables before the first byte moves, so each shard's texts are written
    // straight into place in ONE pass, in record order (the writes are sparse
    // when the corpus is permuted; sequential when it is not).
    let mut by_shard: HashMap<u16, Vec<(u64, u64, u64)>> = HashMap::new(); // shard -> (record, corpus_pos, len)
    for k in 0..n as usize {
        by_shard.entry(l.shard[k]).or_default().push((l.rec[k], k as u64, l.len[k]));
    }
    let mut shard_plan: Vec<(u16, Vec<(u64, u64, u64)>)> = by_shard.into_iter().collect();
    shard_plan.sort();
    for (_, v) in shard_plan.iter_mut() {
        v.sort();
    }
    let total_targets: u64 = shard_plan.iter().map(|(_, v)| v.len() as u64).sum();
    if total_targets != n {
        die(&format!(
            "corpusv2: the docs table routes {} documents but the corpus has {} (shard columns are inconsistent)",
            total_targets, n
        ));
    }
    // Preallocate the cache exactly.
    {
        let f = ck(File::create(&bin_path), &format!("create {}", bin_path.display()));
        ck(f.set_len(l.corpus_bytes), &format!("preallocate {}", bin_path.display()));
        drop(f);
    }
    let written_docs = Arc::new(AtomicU64::new(0));
    let written_bytes = Arc::new(AtomicU64::new(0));
    let jobs = a.jobs.max(1);
    let pool = rayon::ThreadPoolBuilder::new()
        .num_threads(jobs)
        .build()
        .unwrap_or_else(|e| die(&format!("corpusv2: thread pool: {}", e)));
    pool.install(|| {
        use rayon::prelude::*;
        shard_plan.par_iter().for_each(|(si, targets)| {
            let path = match shards.get(*si as usize) {
                Some(p) => p.clone(),
                None => die(&format!(
                    "corpusv2: the docs table names shard {} but {} holds only {} shards",
                    si, shards_dir.display(), shards.len()
                )),
            };
            let mut out = ck(
                File::options().write(true).open(&bin_path),
                &format!("open {}", bin_path.display()),
            );
            let recs: Vec<u64> = targets.iter().map(|t| t.0).collect();
            extract_records(&path, &recs, |record, text| {
                let pos = targets.iter().position(|t| t.0 == record).unwrap();
                let (corpus_pos, len) = (targets[pos].1, targets[pos].2);
                if text.len() as u64 != len {
                    die(&format!(
                        "corpusv2: {} record {} is {} bytes but the docs table says {}",
                        path.display(),
                        record,
                        text.len(),
                        len
                    ));
                }
                let start = l.start[corpus_pos as usize];
                ck(out.seek(SeekFrom::Start(start)), &format!("seek {}", bin_path.display()));
                ck(out.write_all(text), &format!("write {}", bin_path.display()));
                written_docs.fetch_add(1, Ordering::Relaxed);
                written_bytes.fetch_add(text.len() as u64, Ordering::Relaxed);
            });
        });
    });
    let got_docs = written_docs.load(Ordering::SeqCst);
    let got_bytes = written_bytes.load(Ordering::SeqCst);
    if got_docs != n {
        die(&format!(
            "corpusv2: {} of {} documents were materialized; the cache is incomplete and was NOT published",
            got_docs, n
        ));
    }
    if got_bytes != l.corpus_bytes {
        die(&format!(
            "corpusv2: {} bytes were materialized but corpus_bytes is {}; the cache was NOT published",
            got_bytes, l.corpus_bytes
        ));
    }
    let sha = hash_file(&bin_path);
    let size = ck(std::fs::metadata(&bin_path), &format!("stat {}", bin_path.display())).len();
    if size != l.corpus_bytes {
        die(&format!("corpusv2: {} is {} bytes, corpus_bytes is {}", bin_path.display(), size, l.corpus_bytes));
    }
    let name = prefix.file_name().unwrap().to_string_lossy().into_owned();
    let body = format!("{}  {}.bin\n", sha, name); // the sha256sum text-mode format
    let mut f = ck(File::create(&sha_path), &format!("create {}", sha_path.display()));
    ck(f.write_all(body.as_bytes()), &format!("write {}", sha_path.display()));
    ck(f.sync_all(), &format!("fsync {}", sha_path.display()));
    if let Some(dir) = sha_path.parent() {
        if let Ok(d) = File::open(dir) {
            let _ = d.sync_all();
        }
    }
    crate::trace_since("v2_materialize", t0);
    println!(
        "V2_MATERIALIZED prefix={} bin={} docs={} bytes={} sha256={} (the cache; delete and regenerate any time)",
        prefix.display(),
        bin_path.display(),
        n,
        size,
        sha
    );
}

// ---------------------------------------------------------------------------
// Command: --v2-reorder (the flagship: tables only, zero bytes move)
// ---------------------------------------------------------------------------

pub struct ReorderArgs {
    pub from: String,
    pub out: String,
    pub order: Option<String>,
    pub order_seed: Option<u64>,
}

pub fn reorder(a: &ReorderArgs) {
    let t0 = Instant::now();
    if a.order.is_some() == a.order_seed.is_some() {
        die("corpusv2: --v2-reorder needs exactly one of --order FILE or --order-seed S");
    }
    let src_prefix = PathBuf::from(&a.from);
    let out_prefix = PathBuf::from(&a.out);
    let s = load_corpus(&src_prefix);
    mkdir_of(&out_prefix);
    let (starts_path, docs_path, order_path, manifest_path) = claim_output(&out_prefix, true);

    // The new permutation is over the SOURCE corpus's positions; composition
    // keeps .order referencing construction-time source ids (CORPUS_V2.md 4.2).
    let n = s.doc_count;
    let perm2 = match a.order_seed {
        Some(seed) => permutation_from_seed(n, seed),
        None => {
            let file = a.order.clone().unwrap();
            match read_order(&PathBuf::from(&file), n) {
                Some(p) => p,
                None => die(&format!("corpusv2: --order file {} does not exist or is not XORD v1", file)),
            }
        }
    };
    let composed: Vec<u32> = (0..n as usize).map(|k| s.src_id[perm2[k] as usize]).collect();
    assert_bijection(&composed, "composed order");

    let mut c = Corpus {
        doc_count: n,
        corpus_bytes: s.corpus_bytes,
        start: Vec::with_capacity(n as usize),
        len: Vec::with_capacity(n as usize),
        shard: Vec::with_capacity(n as usize),
        rec: Vec::with_capacity(n as usize),
        comp: Vec::with_capacity(n as usize),
        src_id: composed,
        ordered: true,
        order_seed: a.order_seed,
        comp_tab: s.comp_tab.clone(),
        shard_names: s.shard_names.clone(),
    };
    let mut cursor = 0u64;
    for k in 0..n as usize {
        let src = perm2[k] as usize;
        c.start.push(cursor);
        c.len.push(s.len[src]);
        c.shard.push(s.shard[src]);
        c.rec.push(s.rec[src]);
        c.comp.push(s.comp[src]);
        cursor += s.len[src];
    }
    assert_eq!(cursor, s.corpus_bytes);
    assert_eq!(c.len.iter().sum::<u64>(), s.corpus_bytes);

    let t_write = Instant::now();
    let (starts_bytes, starts_sha) = write_starts(&c, &starts_path);
    let (docs_bytes, docs_sha) = write_docs(&c, &docs_path);
    let (order_bytes, order_sha) = write_order(&c, &order_path, a.order_seed.is_some());
    if let Some(dir) = starts_path.parent() {
        if let Ok(d) = File::open(dir) {
            ck(d.sync_all(), &format!("fsync {}", dir.display()));
        }
    }
    crate::trace_since("v2_reorder_write", t_write);

    let name = out_prefix.file_name().unwrap().to_string_lossy().into_owned();
    let mut source_access = serde_json::json!({
        "kind": "tables-only reorder of another V2 corpus",
        "source_corpus": src_prefix.display().to_string(),
        "bytes_read": 0,
        "bytes_written": 0,
        "note": "order is policy, never a shuf of bytes: this corpus differs from its source by tables only; no text was read or written",
    });
    if let Some(m) = &s.manifest {
        if let Some(sa) = m.get("source_access") {
            if sa.get("shards_dir").is_some() {
                source_access["shards_dir"] = sa["shards_dir"].clone();
                source_access["shards"] = sa["shards"].clone();
                source_access["shard_count"] = sa["shard_count"].clone();
                source_access["shards_used"] = sa["shards_used"].clone();
            }
        }
    }
    let manifest = serde_json::json!({
        "tool": "xsa corpus --v2-reorder",
        "manifest_version": 1,
        "kind": "corpus-v2",
        "name": name,
        "separator": "none",
        "framing_byte": serde_json::Value::Null,
        "doc_count": n,
        "corpus_bytes": c.corpus_bytes,
        "order": {
            "kind": if a.order_seed.is_some() { "seed" } else { "file" },
            "seed": a.order_seed,
            "algorithm": ORDER_ALGORITHM,
            "composed": true,
            "composition": "src_id[k] = source_corpus.src_id[perm[k]] (CORPUS_V2.md 4.2); .order always references construction-time source ids",
            "materialized": true,
        },
        "source_corpus": src_prefix.display().to_string(),
        "source_access": source_access,
        "construction": format!(
            "xsa corpus --v2-reorder --from {} --out {} {}",
            src_prefix.display(),
            out_prefix.display(),
            match (&a.order, a.order_seed) {
                (Some(f), _) => format!("--order {}", f),
                (None, Some(s)) => format!("--order-seed {}", s),
                _ => String::new(),
            }
        ),
        "sidecars": {
            "starts": { "file": format!("{}.starts", name), "bytes": starts_bytes, "sha256": starts_sha, "format": "XCS2 v1" },
            "docs": { "file": format!("{}.docs", name), "bytes": docs_bytes, "sha256": docs_sha, "format": "XPM1 v1 shape, corpus order" },
            "order": { "file": format!("{}.order", name), "bytes": order_bytes, "sha256": order_sha, "format": "XORD v1" },
        },
        "bin": "optional cache, NOT written by this pass; regenerate any time with: xsa corpus --v2-materialize --prefix <this prefix>",
        "timings_ms": { "total": t0.elapsed().as_secs_f64() * 1e3 },
        "peak_rss_kb": peak_rss_kb(),
        "finished_at_unix": now_unix(),
    });
    write_manifest(&manifest_path, &manifest);
    println!(
        "V2_REORDER from={} out={} docs={} corpus_bytes={} seed={} tables_bytes={} wall_ms={:.0} bytes_moved=0",
        src_prefix.display(),
        out_prefix.display(),
        n,
        c.corpus_bytes,
        a.order_seed.unwrap_or(0),
        starts_bytes + docs_bytes + order_bytes,
        t0.elapsed().as_secs_f64() * 1e3
    );
}

// ---------------------------------------------------------------------------
// Command: --v2-verify
// ---------------------------------------------------------------------------

pub struct VerifyArgs {
    pub prefix: String,
    pub shards: Option<String>,
    pub samples: usize,
    pub jobs: usize,
    pub reference: Option<String>,
}

pub fn verify(a: &VerifyArgs) {
    let t0 = Instant::now();
    let prefix = PathBuf::from(&a.prefix);
    let l = load_corpus(&prefix);
    let (bin_path, _) = bin_paths(&prefix);
    let bin: Option<Mmap> = match File::open(&bin_path) {
        Ok(f) => {
            let m = unsafe { Mmap::map(&f) }.unwrap_or_else(|e| die(&format!("mmap {}: {}", bin_path.display(), e)));
            Some(m)
        }
        Err(e) if e.kind() == ErrorKind::NotFound => None,
        Err(e) => die(&format!("open {}: {}", bin_path.display(), e)),
    };
    let ref_loaded = a.reference.as_ref().map(|r| {
        let rp = PathBuf::from(r);
        let rl = load_corpus(&rp);
        let (rb_path, _) = bin_paths(&rp);
        let f = File::open(&rb_path).unwrap_or_else(|e| {
            die(&format!(
                "corpusv2: --reference {}: cannot open {} ({}); the reference must be materialized",
                rp.display(),
                rb_path.display(),
                e
            ))
        });
        let m = unsafe { Mmap::map(&f) }.unwrap_or_else(|e| die(&format!("mmap {}: {}", rb_path.display(), e)));
        (rl, m)
    });

    let n = l.doc_count;
    println!(
        "V2_VERIFY prefix={} docs={} corpus_bytes={} order={}",
        prefix.display(),
        n,
        l.corpus_bytes,
        if l.ordered { "file" } else { "source" }
    );
    if let Some(b) = &bin {
        if b.len() as u64 != l.corpus_bytes {
            die(&format!(
                "corpusv2: {} is {} bytes but the tables say corpus_bytes {}; a materialized cache must match exactly",
                bin_path.display(),
                b.len(),
                l.corpus_bytes
            ));
        }
        println!("V2_CACHE bin=present bytes={}==corpus_bytes", b.len());
    } else {
        println!("V2_CACHE bin=absent (optional cache; the tables are the corpus)");
    }

    // 1. Table invariants were asserted by load_corpus (deltas positive,
    //    sum(lengths) == corpus_bytes, rows tile, anchors agree, bijection).
    println!(
        "V2_TABLES deltas_positive=all sum_lengths={}==corpus_bytes starts_docs_agreement=ok bijection=ok",
        l.corpus_bytes
    );

    // 2. Reference multiset check: the same documents, the same total bytes.
    if let Some((rl, _)) = &ref_loaded {
        if rl.doc_count != n {
            die(&format!(
                "corpusv2: --reference holds {} documents but the corpus holds {}; not the same document set",
                rl.doc_count, n
            ));
        }
        if l.corpus_bytes != rl.corpus_bytes {
            die(&format!(
                "corpusv2: total bytes differ: {} vs the reference's {}",
                l.corpus_bytes, rl.corpus_bytes
            ));
        }
        let mut a_rows: Vec<(u16, u64, u64)> = (0..n as usize).map(|k| (l.shard[k], l.rec[k], l.len[k])).collect();
        let mut b_rows: Vec<(u16, u64, u64)> = (0..n as usize).map(|k| (rl.shard[k], rl.rec[k], rl.len[k])).collect();
        a_rows.sort_unstable();
        b_rows.sort_unstable();
        if a_rows != b_rows {
            die("corpusv2: the corpus and its reference do not carry the same multiset of documents (shard, record, length)");
        }
        println!(
            "V2_REFMULTISET docs={} multiset=identical corpus_bytes src={} ref={}",
            n, l.corpus_bytes, rl.corpus_bytes
        );
    }

    // 3. Byte identity on samples: re-extract from the shards and compare.
    let want = a.samples.max(1) as u64;
    let mut ids: Vec<usize> = (0..want)
        .map(|i| ((i as u128 * (n as u128 - 1)) / (want as u128 - 1).max(1)) as usize)
        .collect();
    ids.sort_unstable();
    ids.dedup();
    let sampled = ids.len();
    let shards_dir = l.shards_dir(a.shards.as_deref());
    let shards = shard_list(&shards_dir);
    let mut by_shard: HashMap<u16, Vec<usize>> = HashMap::new();
    for &k in &ids {
        if l.shard[k] as usize >= shards.len() {
            die(&format!(
                "corpusv2: doc {} names shard {} but {} holds {} shards",
                k,
                l.shard[k],
                shards_dir.display(),
                shards.len()
            ));
        }
        by_shard.entry(l.shard[k]).or_default().push(k);
    }
    let mut plans: Vec<(u16, Vec<usize>)> = by_shard.into_iter().collect();
    plans.sort();
    for (_, v) in plans.iter_mut() {
        v.sort_by_key(|&k| l.rec[k]);
    }
    println!(
        "V2_SAMPLE docs_sampled={} shards_touched={} compare_to={} reference={}",
        sampled,
        plans.len(),
        if bin.is_some() { "bin+shards" } else { "shards" },
        if ref_loaded.is_some() { "given" } else { "none" }
    );
    let pool = rayon::ThreadPoolBuilder::new()
        .num_threads(a.jobs.max(1))
        .build()
        .unwrap_or_else(|e| die(&format!("corpusv2: thread pool: {}", e)));
    let mismatch = Mutex::new(Vec::<String>::new());
    let exact = AtomicU64::new(0);
    pool.install(|| {
        use rayon::prelude::*;
        plans.par_iter().for_each(|(si, docs)| {
            let path = shards[*si as usize].clone();
            let mut recs: Vec<u64> = docs.iter().map(|&k| l.rec[k]).collect();
            recs.sort_unstable();
            recs.dedup();
            extract_records(&path, &recs, |record, text| {
                let k = *docs.iter().find(|&&k| l.rec[k] == record).unwrap();
                if text.len() as u64 != l.len[k] {
                    mismatch.lock().unwrap().push(format!(
                        "doc={} shard={} record={} shard_text_length={} docs_length={}",
                        k, l.shard[k], record, text.len(), l.len[k]
                    ));
                    return;
                }
                if let Some(b) = &bin {
                    let mine = &b[l.start[k] as usize..(l.start[k] + l.len[k]) as usize];
                    if mine != text {
                        mismatch.lock().unwrap().push(format!(
                            "doc={} at [{},{}) differs from its shard record (bin side)",
                            k,
                            l.start[k],
                            l.start[k] + l.len[k]
                        ));
                        return;
                    }
                }
                if let Some((rl, rb)) = &ref_loaded {
                    let src = l.src_id[k] as usize;
                    if rl.len[src] != l.len[k] || rl.rec[src] != l.rec[k] || rl.shard[src] != l.shard[k] {
                        mismatch.lock().unwrap().push(format!(
                            "doc={} provenance differs from reference doc {} (shard/record/length)",
                            k, src
                        ));
                        return;
                    }
                    let mine = &rb[rl.start[src] as usize..(rl.start[src] + rl.len[src]) as usize];
                    if mine != text {
                        mismatch.lock().unwrap().push(format!(
                            "doc={} is NOT byte-identical to reference document {} (source doc id {})",
                            k, src, l.src_id[k]
                        ));
                        return;
                    }
                }
                exact.fetch_add(1, Ordering::Relaxed);
            });
        });
    });
    let bad = mismatch.into_inner().unwrap();
    for b in bad.iter().take(20) {
        eprintln!("V2_MISMATCH {}", b);
    }
    println!(
        "V2_BYTES sampled={} byte_identical={} mismatches={}",
        sampled,
        exact.load(Ordering::Relaxed),
        bad.len()
    );
    if !bad.is_empty() {
        die("corpusv2: the tables do not describe the corpus the shards produce");
    }
    crate::trace_since("v2_verify", t0);
    println!(
        "V2_VERIFY_OK prefix={} docs={} sampled={} corpus_bytes={}",
        prefix.display(),
        n,
        sampled,
        l.corpus_bytes
    );
}

// ---------------------------------------------------------------------------
// Command: --v2-locate (offset -> doc_id / offset_in_doc through the starts)
// ---------------------------------------------------------------------------

pub struct LocateArgs {
    pub prefix: String,
    pub offsets: Vec<u64>,
}

pub fn locate(a: &LocateArgs) {
    let prefix = PathBuf::from(&a.prefix);
    let l = load_corpus(&prefix);
    for &pos in &a.offsets {
        let (doc, off) = l.locate(pos);
        let k = doc as usize;
        println!(
            "V2_LOCATE offset={} doc_id={} offset_in_doc={} length={} shard={} record={} component={} source_doc_id={}",
            pos,
            doc,
            off,
            l.len[k],
            l.shard_names[l.shard[k] as usize],
            l.rec[k],
            l.comp_of(k),
            l.src_id[k]
        );
    }
}

// ---------------------------------------------------------------------------
// The build's --docs-map hook (chain.rs; CORPUS_V2.md 6)
// ---------------------------------------------------------------------------

/// Validate a V2 prefix for `xsa build --docs-map`: the light checks return
/// Err (operator-fixable), the deep table invariants run via load_corpus and
/// die loudly on corruption. Returns (doc_count, ordered).
pub fn docs_map_check(prefix: &Path, input_bytes: u64) -> Result<(u64, bool), String> {
    let (starts_path, docs_path, order_path, _) = corpus_paths(prefix);
    if !starts_path.exists() {
        return Err(format!(
            "--docs-map {}: <prefix>.starts is missing; the docstarts sidecar is REQUIRED for a V2 corpus (there is no fallback: the bytes carry no boundary information)",
            prefix.display()
        ));
    }
    if !docs_path.exists() {
        return Err(format!("--docs-map {}: <prefix>.docs is missing", prefix.display()));
    }
    // Light header checks (the deep invariants follow via load_corpus).
    let head = std::fs::read(&starts_path)
        .map_err(|e| format!("--docs-map {}: read {}: {e}", prefix.display(), starts_path.display()))?;
    let h32 = |o: usize| u32::from_le_bytes(head[o..o + 4].try_into().unwrap());
    let h64 = |o: usize| u64::from_le_bytes(head[o..o + 8].try_into().unwrap());
    if head.len() < STARTS_HEADER as usize || &head[..4] != b"XCS2" || h32(4) != 1 {
        return Err(format!("--docs-map {}: {} is not an XCS2 v1 starts table", prefix.display(), starts_path.display()));
    }
    if h64(24) != input_bytes {
        return Err(format!(
            "--docs-map {}: the tables say corpus_bytes {} but the build input is {} bytes; the sidecars do not describe this corpus",
            prefix.display(),
            h64(24),
            input_bytes
        ));
    }
    if order_path.exists() {
        let obytes = std::fs::read(&order_path)
            .map_err(|e| format!("--docs-map {}: read {}: {e}", prefix.display(), order_path.display()))?;
        if obytes.len() < ORDER_HEADER as usize || &obytes[..4] != b"XORD" {
            return Err(format!("--docs-map {}: {} is not an XORD v1 order table", prefix.display(), order_path.display()));
        }
    }
    // Deep invariants (deltas positive, sum == corpus_bytes, tiling, bijection):
    // corruption here is fatal and dies loudly with a precise message.
    let l = load_corpus(prefix);
    Ok((l.doc_count, l.ordered))
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

pub fn command(args: &[String]) {
    let sub = args.iter().find_map(|a| match a.as_str() {
        "--v2-meta" => Some("meta"),
        "--v2-materialize" => Some("materialize"),
        "--v2-reorder" => Some("reorder"),
        "--v2-verify" => Some("verify"),
        "--v2-locate" => Some("locate"),
        _ => None,
    });
    let sub = sub.unwrap_or_else(|| {
        eprintln!("corpusv2: no --v2-* mode flag given");
        usage();
    });
    match sub {
        "meta" => {
            let mut shards = None;
            let mut out = None;
            let mut limit = None;
            let mut order = None;
            let mut order_seed = None;
            let mut i = 0;
            while i < args.len() {
                match args[i].as_str() {
                    "--v2-meta" => i += 1,
                    "--shards" if i + 1 < args.len() => {
                        shards = Some(args[i + 1].clone());
                        i += 2;
                    }
                    "--out" if i + 1 < args.len() => {
                        out = Some(args[i + 1].clone());
                        i += 2;
                    }
                    "--limit-bytes" if i + 1 < args.len() => {
                        limit = Some(
                            args[i + 1]
                                .parse::<u64>()
                                .unwrap_or_else(|_| die("corpusv2: --limit-bytes expects a byte count")),
                        );
                        i += 2;
                    }
                    "--order" if i + 1 < args.len() => {
                        order = Some(args[i + 1].clone());
                        i += 2;
                    }
                    "--order-seed" if i + 1 < args.len() => {
                        order_seed = Some(
                            args[i + 1]
                                .parse::<u64>()
                                .unwrap_or_else(|_| die("corpusv2: --order-seed expects a u64")),
                        );
                        i += 2;
                    }
                    "-h" | "--help" => usage(),
                    other => die(&format!("corpus --v2-meta: unknown arg {}", other)),
                }
            }
            let a = MetaArgs {
                shards: shards.unwrap_or_else(|| die("corpus --v2-meta: --shards DIR is required")),
                out: out.unwrap_or_else(|| die("corpus --v2-meta: --out PREFIX is required")),
                limit,
                order,
                order_seed,
            };
            meta(&a);
        }
        "materialize" => {
            let mut prefix = None;
            let mut shards = None;
            let mut jobs = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1).min(32);
            let mut i = 0;
            while i < args.len() {
                match args[i].as_str() {
                    "--v2-materialize" => i += 1,
                    "--prefix" if i + 1 < args.len() => {
                        prefix = Some(args[i + 1].clone());
                        i += 2;
                    }
                    "--shards" if i + 1 < args.len() => {
                        shards = Some(args[i + 1].clone());
                        i += 2;
                    }
                    "--jobs" | "-j" if i + 1 < args.len() => {
                        jobs = args[i + 1]
                            .parse::<usize>()
                            .unwrap_or_else(|_| die("corpusv2: --jobs expects an integer"))
                            .max(1);
                        i += 2;
                    }
                    "-h" | "--help" => usage(),
                    other => die(&format!("corpus --v2-materialize: unknown arg {}", other)),
                }
            }
            let a = MaterializeArgs {
                prefix: prefix.unwrap_or_else(|| die("corpus --v2-materialize: --prefix P is required")),
                shards,
                jobs,
            };
            materialize(&a);
        }
        "reorder" => {
            let mut from = None;
            let mut out = None;
            let mut order = None;
            let mut order_seed = None;
            let mut i = 0;
            while i < args.len() {
                match args[i].as_str() {
                    "--v2-reorder" => i += 1,
                    "--from" if i + 1 < args.len() => {
                        from = Some(args[i + 1].clone());
                        i += 2;
                    }
                    "--out" if i + 1 < args.len() => {
                        out = Some(args[i + 1].clone());
                        i += 2;
                    }
                    "--order" if i + 1 < args.len() => {
                        order = Some(args[i + 1].clone());
                        i += 2;
                    }
                    "--order-seed" if i + 1 < args.len() => {
                        order_seed = Some(
                            args[i + 1]
                                .parse::<u64>()
                                .unwrap_or_else(|_| die("corpusv2: --order-seed expects a u64")),
                        );
                        i += 2;
                    }
                    "-h" | "--help" => usage(),
                    other => die(&format!("corpus --v2-reorder: unknown arg {}", other)),
                }
            }
            let a = ReorderArgs {
                from: from.unwrap_or_else(|| die("corpus --v2-reorder: --from P1 is required")),
                out: out.unwrap_or_else(|| die("corpus --v2-reorder: --out P2 is required")),
                order,
                order_seed,
            };
            reorder(&a);
        }
        "verify" => {
            let mut prefix = None;
            let mut shards = None;
            let mut samples = 200usize;
            let mut jobs = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1).min(32);
            let mut reference = None;
            let mut i = 0;
            while i < args.len() {
                match args[i].as_str() {
                    "--v2-verify" => i += 1,
                    "--prefix" if i + 1 < args.len() => {
                        prefix = Some(args[i + 1].clone());
                        i += 2;
                    }
                    "--shards" if i + 1 < args.len() => {
                        shards = Some(args[i + 1].clone());
                        i += 2;
                    }
                    "--samples" if i + 1 < args.len() => {
                        samples = args[i + 1]
                            .parse::<usize>()
                            .unwrap_or_else(|_| die("corpusv2: --samples expects an integer"))
                            .max(1);
                        i += 2;
                    }
                    "--jobs" | "-j" if i + 1 < args.len() => {
                        jobs = args[i + 1]
                            .parse::<usize>()
                            .unwrap_or_else(|_| die("corpusv2: --jobs expects an integer"))
                            .max(1);
                        i += 2;
                    }
                    "--reference" if i + 1 < args.len() => {
                        reference = Some(args[i + 1].clone());
                        i += 2;
                    }
                    "-h" | "--help" => usage(),
                    other => die(&format!("corpus --v2-verify: unknown arg {}", other)),
                }
            }
            let a = VerifyArgs {
                prefix: prefix.unwrap_or_else(|| die("corpus --v2-verify: --prefix P is required")),
                shards,
                samples,
                jobs,
                reference,
            };
            verify(&a);
        }
        "locate" => {
            let mut prefix = None;
            let mut offsets = String::new();
            let mut i = 0;
            while i < args.len() {
                match args[i].as_str() {
                    "--v2-locate" => i += 1,
                    "--prefix" if i + 1 < args.len() => {
                        prefix = Some(args[i + 1].clone());
                        i += 2;
                    }
                    "--offset" if i + 1 < args.len() => {
                        offsets = args[i + 1].clone();
                        i += 2;
                    }
                    "-h" | "--help" => usage(),
                    other => die(&format!("corpus --v2-locate: unknown arg {}", other)),
                }
            }
            let offsets: Vec<u64> = offsets
                .split(',')
                .filter(|s| !s.is_empty())
                .map(|s| s.parse::<u64>().unwrap_or_else(|_| die(&format!("corpusv2: --offset expects u64 values, got {}", s))))
                .collect();
            if offsets.is_empty() {
                die("corpus --v2-locate: --offset O[,O...] is required");
            }
            let a = LocateArgs { prefix: prefix.unwrap_or_else(|| die("corpus --v2-locate: --prefix P is required")), offsets };
            locate(&a);
        }
        _ => unreachable!(),
    }
}

fn usage() -> ! {
    eprintln!(
        "xsa corpus --v2-meta --shards DIR --out PREFIX [--limit-bytes N] [--order FILE | --order-seed S]
  Define a CORPUS V2 (the metadata is the corpus; CORPUS_V2.md): stream the Pile
  shards once, keep ONLY lengths + provenance, write <prefix>.starts (XCS2 v1),
  <prefix>.docs (XPM1 v1 shape) and, when reordered, <prefix>.order (XORD v1)
  plus MANIFEST.json. NO text bytes are written.
  --limit-bytes   admit documents while they fit N bytes whole; the first that
                  does not fit ends the corpus (V2 never cuts a document)
  --order FILE    an XORD v1 permutation; must be a bijection, or refuse
  --order-seed S splitmix64 + Fisher-Yates (documented in CORPUS_V2.md 4.3);
                  the permutation is always materialized as .order

xsa corpus --v2-materialize --prefix P [--shards DIR] [--jobs J]
  Write the OPTIONAL <prefix>.bin cache: re-extract every document from the
  shards in corpus order, in place (starts are known before the first byte
  moves). Writes <prefix>.bin.sha256. Deleting the cache never loses anything.

xsa corpus --v2-reorder --from P1 --out P2 (--order FILE | --order-seed S)
  Reorder an existing corpus by writing TABLES ONLY: zero text bytes are read
  or written. Composes onto P1's order (CORPUS_V2.md 4.2).

xsa corpus --v2-verify --prefix P [--shards DIR] [--samples N] [--reference P1]
  Assert every table invariant, then re-extract >= N sampled documents from the
  shards and compare BYTES at [start, start+len): against <prefix>.bin when it
  exists, and against --reference's .bin at the source document's start (the
  reordered-corpus gate: doc k is byte-identical to doc perm[k] of the source).

xsa corpus --v2-locate --prefix P --offset O[,O...]
  Resolve global offsets through the V2 starts: doc_id, offset_in_doc, length,
  provenance (shard, record, component), source_doc_id when ordered."
    );
    std::process::exit(2);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn splitmix64_matches_the_reference_constants() {
        // Published splitmix64 values (computed with rustc independently of
        // this module): seed 0 -> e220a8397b1dcdaf, 6e789e6aa1b965f4;
        // seed 42 -> bdd732262feb6e95.
        let mut r = SplitMix64(0);
        assert_eq!(r.next(), 0xe220_a839_7b1d_cdaf);
        assert_eq!(r.next(), 0x6e78_9e6a_a1b9_65f4);
        let mut r = SplitMix64(42);
        assert_eq!(r.next(), 0xbdd7_3226_2feb_6e95);
    }

    #[test]
    fn seed_permutation_is_a_deterministic_bijection() {
        let p1 = permutation_from_seed(1000, 42);
        let p2 = permutation_from_seed(1000, 42);
        assert_eq!(p1, p2, "same seed must give the same permutation");
        let p3 = permutation_from_seed(1000, 43);
        assert_ne!(p1, p3, "different seeds must differ");
        let mut sorted = p1.clone();
        sorted.sort_unstable();
        assert_eq!(sorted, (0..1000u32).collect::<Vec<u32>>(), "must be a bijection");
        assert_bijection(&p1, "test");
        // Fisher-Yates must move documents (n > 1): a seed that produced the
        // identity for large n would be a generator bug.
        let moved = p1.iter().enumerate().filter(|(i, &v)| v != *i as u32).count();
        assert!(moved > 900, "seed 42 must scramble; only {} of 1000 moved", moved);
    }

    #[test]
    fn one_document_corpus_has_identity_start() {
        assert_eq!(permutation_from_seed(1, 7), vec![0]);
    }

    #[test]
    fn tables_round_trip_and_invariants_hold() {
        let dir = tempfile::tempdir().unwrap();
        let prefix = dir.path().join("t");
        let c = Corpus {
            doc_count: 3,
            corpus_bytes: 30,
            start: vec![0, 10, 25],
            len: vec![10, 15, 5],
            shard: vec![0, 0, 1],
            rec: vec![0, 1, 0],
            comp: vec![0, 1, 1],
            src_id: vec![2, 0, 1],
            ordered: true,
            order_seed: Some(42),
            comp_tab: vec![b"Pile-CC".to_vec(), b"ArXiv".to_vec()],
            shard_names: vec!["00.jsonl.zst".to_string(), "01.jsonl.zst".to_string()],
        };
        let (s_path, d_path, o_path) = (
            dir.path().join("t.starts"),
            dir.path().join("t.docs"),
            dir.path().join("t.order"),
        );
        write_starts(&c, &s_path);
        write_docs(&c, &d_path);
        write_order(&c, &o_path, true);
        let (n, bytes, starts) = read_starts(&s_path);
        assert_eq!((n, bytes), (3, 30));
        assert_eq!(starts, vec![0, 10, 25]);
        let (n2, bytes2, start2, len, shard, rec, comp, comp_tab, shard_names) = read_docs(&d_path);
        assert_eq!((n2, bytes2), (3, 30));
        assert_eq!(start2, starts);
        assert_eq!(len, vec![10, 15, 5]);
        assert_eq!(shard, vec![0, 0, 1]);
        assert_eq!(rec, vec![0, 1, 0]);
        assert_eq!(comp, vec![0, 1, 1]);
        assert_eq!(comp_tab.len(), 2);
        assert_eq!(shard_names.len(), 2);
        let perm = read_order(&o_path, 3).unwrap();
        assert_eq!(perm, vec![2, 0, 1]);
    }

    #[test]
    fn locate_resolves_offsets_through_the_starts() {
        let dir = tempfile::tempdir().unwrap();
        let prefix = dir.path().join("t");
        let c = Corpus {
            doc_count: 4,
            corpus_bytes: 40,
            start: vec![0, 10, 25, 31],
            len: vec![10, 15, 6, 9],
            shard: vec![0; 4],
            rec: vec![0, 1, 2, 3],
            comp: vec![0; 4],
            src_id: vec![0, 1, 2, 3],
            ordered: false,
            order_seed: None,
            comp_tab: vec![b"x".to_vec()],
            shard_names: vec!["00.jsonl.zst".to_string()],
        };
        write_starts(&c, &dir.path().join("t.starts"));
        write_docs(&c, &dir.path().join("t.docs"));
        let l = load_corpus(&prefix);
        assert_eq!(l.locate(0), (0, 0));
        assert_eq!(l.locate(9), (0, 9));
        assert_eq!(l.locate(10), (1, 0));
        assert_eq!(l.locate(30), (2, 5));
        assert_eq!(l.locate(39), (3, 8));
    }

    #[test]
    fn end_to_end_from_a_fake_shard() {
        // A tiny oracle pass at tiny scale (the verification pyramid's base):
        // build a corpus from a synthetic .jsonl.zst including adversarial
        // documents (0x1E inside text, a separator-at-the-tail document),
        // then meta -> materialize -> verify -> reorder -> verify-vs-reference.
        let st = std::process::Command::new("zstd").arg("--version").output().expect("zstd is required on PATH");
        assert!(st.status.success());
        let root = tempfile::tempdir().unwrap();
        let shards = root.path().join("shards");
        std::fs::create_dir_all(&shards).unwrap();
        let docs: Vec<(&str, &str)> = vec![
            ("Pile-CC", "alpha text with a separator\x1einside and another\x1e here"),
            ("ArXiv", "beta"),
            ("Pile-CC", "gamma"), // the near-boundary pair: short adjacent docs
            ("ArXiv", "delta\x1e"), // separator at the very tail: boundaries must come from the tables only
            ("Github", "epsilon"),
        ];
        let mut jsonl = String::new();
        for (name, text) in &docs {
            let rec = serde_json::json!({ "text": text, "meta": { "pile_set_name": name } });
            jsonl.push_str(&rec.to_string());
            jsonl.push('\n');
        }
        let raw = shards.join("00.jsonl.zst.raw");
        std::fs::write(&raw, jsonl).unwrap();
        let st = std::process::Command::new("zstd")
            .arg("-q")
            .arg("-f")
            .arg("-o")
            .arg(shards.join("00.jsonl.zst"))
            .arg(&raw)
            .status()
            .unwrap();
        assert!(st.success());
        std::fs::write(shards.join("SHA256SUMS.txt"), "deadbeef  00.jsonl.zst\n").unwrap();

        // One directory per corpus (one MANIFEST.json per directory).
        let c1_dir = root.path().join("c1");
        let c2_dir = root.path().join("c2");
        std::fs::create_dir_all(&c1_dir).unwrap();
        std::fs::create_dir_all(&c2_dir).unwrap();
        let c1 = c1_dir.join("p");
        let c2 = c2_dir.join("p");
        meta(&MetaArgs {
            shards: shards.display().to_string(),
            out: c1.display().to_string(),
            limit: None,
            order: None,
            order_seed: None,
        });
        let l = load_corpus(&c1);
        assert_eq!(l.doc_count, docs.len() as u64);
        let expect_bytes: u64 = docs.iter().map(|(_, t)| t.len() as u64).sum();
        assert_eq!(l.corpus_bytes, expect_bytes);

        materialize(&MaterializeArgs { prefix: c1.display().to_string(), shards: None, jobs: 2 });
        verify(&VerifyArgs { prefix: c1.display().to_string(), shards: None, samples: 5, jobs: 2, reference: None });

        // THE FLAGSHIP: reorder by tables only, then prove byte identity
        // against the source's own cache through the reference gate.
        reorder(&ReorderArgs { from: c1.display().to_string(), out: c2.display().to_string(), order: None, order_seed: Some(42) });
        let l2 = load_corpus(&c2);
        assert_eq!(l2.doc_count, l.doc_count);
        assert_eq!(l2.corpus_bytes, l.corpus_bytes);
        assert!(l2.ordered);
        let mut srcs = l2.src_id.clone();
        srcs.sort_unstable();
        assert_eq!(srcs, (0..l.doc_count as u32).collect::<Vec<u32>>());
        verify(&VerifyArgs { prefix: c2.display().to_string(), shards: None, samples: 5, jobs: 2, reference: Some(c1.display().to_string()) });

        // locate: every document start resolves to its own doc
        for k in 0..l2.doc_count as usize {
            let (doc, off) = l2.locate(l2.start[k]);
            assert_eq!(doc as usize, k);
            assert_eq!(off, 0);
        }
    }
}
