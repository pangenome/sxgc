//! xsa corpus — one streaming pass over a Wikipedia XML dump into an
//! in-band-framed corpus plus document sidecars.
//!
//!     xsa corpus --wikipedia <dump.xml.bz2|-> --out <dir> \
//!                [--max-bytes 90G] [--separator 0x1e|none]
//!
//! The dump is never materialised: the compressed file is hashed in one pass,
//! then `bzip2 -dc` (or `pbzip2 -dc`) is spawned and its stdout is parsed as a
//! byte stream with a fixed 1 MiB window. Every document is written to
//! `corpus.bin` as
//!
//!     "<title>\n<body bytes>\x1E"      (default: --separator 0x1e)
//!     "<title>\n<body bytes>"          (--separator none)
//!
//! so a title is searchable in-band and `doc_id = rank_1E(pos)`. Body bytes are
//! copied verbatim out of `<text>` — no wikitext conversion, no XML entity
//! decoding: what is indexed is what the dump carries between the tags.
//!
//! Rule 1 (enforced, fatal): 0x1E cannot occur inside a body, because then the
//! separator would stop framing documents. At the end of the pass the emitted
//! 0x1E count must equal the document count; otherwise we exit non-zero and say
//! how many bytes of content collide with the separator.
//!
//! Sidecar layouts (also echoed verbatim into MANIFEST.json, all little-endian):
//!
//!   seps.bin   64B header, then doc_count u32 deltas, then anchors
//!              [0..4)   b"XSEP"
//!              [4..8)   u32 version = 1
//!              [8..12)  u32 flags: bit0 set = 0x1E is emitted between docs
//!              [12..16) u32 anchor_stride = 64
//!              [16..24) u64 doc_count
//!              [24..32) u64 corpus_bytes
//!              [32..40) u64 anchor_count = ceil(doc_count / 64)
//!              [40..48) u64 base = start offset of document 0 (0 today)
//!              then doc_count * u32: d[i] = start[i] - start[i-1], d[0] = start[0] - base
//!              then anchor_count * u64: absolute start offset of doc k*64
//!              rank_1E(pos): doc_id of the document whose start <= pos < start+1
//!              (in --separator 0x1e mode the 0x1E of doc i is at start[i+1]-1).
//!   titles.bin 20B header, then payload, then offsets, then lengths
//!              [0..4)    b"XTIT"
//!              [4..8)    u32 version = 1
//!              [8..12)   u32 doc_count
//!              [12..20)  u64 payload_bytes
//!              then payload_bytes of concatenated titles
//!              then doc_count * u32 offsets (relative to payload start)
//!              then doc_count * u16 lengths
//!
//! Determinism: corpus.bin / seps.bin / titles.bin carry no timestamps and the
//! pass is single-threaded and order-preserving, so the same dump produces
//! byte-identical sidecars. Only MANIFEST.json carries `finished_at`.
//!
//! Failure hygiene: the sidecars are written as `*.partial` and renamed into
//! place only after rule 1 has passed and every file is fsynced. MANIFEST.json
//! is written last, so its presence means the pass completed.

use std::fs::File;
use std::io::{self, BufWriter, ErrorKind, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::Instant;

use sha2::{Digest, Sha256};

use crate::die;

/// The framing byte. 0x1E can never be part of a UTF-8 character (all
/// multi-byte sequences use bytes >= 0x80), so any occurrence inside content is
/// either a collision (fatal, see rule 1) or an explicitly requested no-op
/// (`--separator none`).
pub const SEP: u8 = 0x1E;
const ANCHOR_STRIDE: u64 = 64;
const SEP_MAGIC: &[u8] = b"XSEP";
const TITLE_MAGIC: &[u8] = b"XTIT";
const SEP_HEADER: u64 = 48;
const TITLE_HEADER: u64 = 20;
/// A single document larger than this aborts the pass instead of silently
/// reserving a terabyte of RAM for one page.
const MAX_DOC_BYTES: usize = 512 << 20;
const READ_BUF: usize = 1 << 20;
const WRITE_BUF: usize = 1 << 20;

macro_rules! ck {
    ($e:expr, $what:expr) => {
        $e.unwrap_or_else(|e| die(&format!("{}: {}", $what, e)))
    };
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

pub fn command(args: &[String]) -> () {
    let mut dump: Option<String> = None;
    let mut out: Option<String> = None;
    let mut max_bytes: Option<u64> = None;
    let mut sep_mode = true;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--wikipedia" if i + 1 < args.len() => {
                dump = Some(args[i + 1].clone());
                i += 2;
            }
            "--out" if i + 1 < args.len() => {
                out = Some(args[i + 1].clone());
                i += 2;
            }
            "--max-bytes" if i + 1 < args.len() => {
                max_bytes = Some(parse_size(&args[i + 1]));
                i += 2;
            }
            "--separator" if i + 1 < args.len() => {
                let v = args[i + 1].to_ascii_lowercase();
                sep_mode = match v.as_str() {
                    "0x1e" | "1e" | "30" => true,
                    "none" => false,
                    other => die(&format!(
                        "corpus: --separator {} not understood (want 0x1e or none)",
                        other
                    )),
                };
                i += 2;
            }
            "-h" | "--help" => usage(),
            other => die(&format!("corpus: unknown arg {}", other)),
        }
    }
    let dump = dump.unwrap_or_else(|| die("corpus: --wikipedia <dump.xml.bz2|-> is required"));
    let out = out.unwrap_or_else(|| die("corpus: --out <dir> is required"));

    let t0 = Instant::now();
    run(&dump, &out, max_bytes, sep_mode, t0);
}

fn usage() -> ! {
    eprintln!(
        "xsa corpus --wikipedia <dump.xml.bz2|-> --out <dir> [--max-bytes 90G] [--separator 0x1e|none]

  One streaming pass over a Wikipedia XML dump (pages-articles style) into
  <dir>/corpus.bin (\"<title>\\n<body>\\x1E\" per document), seps.bin (authoritative
  document starts: u32 deltas + every-64th u64 anchor) and titles.bin
  (concatenated titles + u32 offsets + u16 lengths), plus MANIFEST.json.

  The compressed stream is decompressed by an external bzip2/pbzip2 -dc;
  '-' reads the compressed stream from stdin. Body bytes are copied verbatim
  between <text> tags (no wikitext conversion, no entity decoding), namespace
  filtering is not attempted: every <page> is indexed.

  --max-bytes N   stop at the first document boundary at or past N bytes
                  (suffixes K/M/G/T are binary: 1G = 1024^3)
  --separator     default 0x1e frames documents in-band; none omits the byte
                  and leaves seps.bin as the only boundary record"
    );
    std::process::exit(2);
}

/// Parse "90G" / "1024" / "16M" into bytes. Suffixes are binary.
fn parse_size(s: &str) -> u64 {
    let t = s.trim();
    if t.is_empty() {
        die("corpus: --max-bytes: empty value");
    }
    let (num, mult) = match t.as_bytes()[t.len() - 1].to_ascii_lowercase() {
        b'k' => (&t[..t.len() - 1], 1u64 << 10),
        b'm' => (&t[..t.len() - 1], 1u64 << 20),
        b'g' => (&t[..t.len() - 1], 1u64 << 30),
        b't' => (&t[..t.len() - 1], 1u64 << 40),
        _ => (t, 1),
    };
    let n: u64 = num
        .parse()
        .unwrap_or_else(|_| die(&format!("corpus: --max-bytes {}: integer expected", s)));
    n.checked_mul(mult)
        .unwrap_or_else(|| die(&format!("corpus: --max-bytes {} overflows u64", s)))
}

// ---------------------------------------------------------------------------
// Sinks
// ---------------------------------------------------------------------------

/// Sequential writer that hashes what it writes (corpus.bin is far too large to
/// read back for a checksum).
struct Sink {
    w: Option<BufWriter<File>>,
    h: Sha256,
    n: u64,
    what: String,
}

impl Sink {
    fn new(path: &Path) -> Sink {
        let f = File::create(path).unwrap_or_else(|e| die(&format!("create {}: {}", path.display(), e)));
        Sink {
            w: Some(BufWriter::with_capacity(WRITE_BUF, f)),
            h: Sha256::new(),
            n: 0,
            what: path.display().to_string(),
        }
    }
    fn put(&mut self, b: &[u8]) {
        let w = self.w.as_mut().unwrap();
        ck!(w.write_all(b), format!("write {}", self.what));
        self.h.update(b);
        self.n += b.len() as u64;
    }
    fn finish(mut self) -> (u64, String) {
        let mut w = self.w.take().unwrap();
        ck!(w.flush(), format!("flush {}", self.what));
        let f = ck!(w.into_inner(), format!("close {}", self.what));
        ck!(f.sync_all(), format!("fsync {}", self.what));
        let digest = format!("{:x}", self.h.finalize_reset());
        (self.n, digest)
    }
}

/// Sidecar writer whose header (doc counts, payload sizes) is only known at the
/// end: write zeroed placeholders, then patch the header and hash the finished
/// file. These files are small next to corpus.bin.
struct PatchFile {
    w: Option<BufWriter<File>>,
    path: PathBuf,
    n: u64,
}

impl PatchFile {
    fn new(path: &Path, placeholder: u64) -> PatchFile {
        let f = File::create(path).unwrap_or_else(|e| die(&format!("create {}: {}", path.display(), e)));
        let mut p = PatchFile {
            w: Some(BufWriter::with_capacity(WRITE_BUF, f)),
            path: path.to_path_buf(),
            n: 0,
        };
        p.put(&vec![0u8; placeholder as usize]);
        p
    }
    fn put(&mut self, b: &[u8]) {
        let w = self.w.as_mut().unwrap();
        ck!(w.write_all(b), format!("write {}", self.path.display()));
        self.n += b.len() as u64;
    }
    fn put_u32(&mut self, v: u32) {
        self.put(&v.to_le_bytes());
    }
    fn put_u64(&mut self, v: u64) {
        self.put(&v.to_le_bytes());
    }
    fn patch(&mut self, off: u64, b: &[u8]) {
        let w = self.w.as_mut().unwrap();
        let end = self.n;
        ck!(
            w.seek(SeekFrom::Start(off)),
            format!("seek {} to {}", self.path.display(), off)
        );
        ck!(w.write_all(b), format!("patch {}", self.path.display()));
        ck!(
            w.seek(SeekFrom::Start(end)),
            format!("seek {} back to {}", self.path.display(), end)
        );
    }
    fn finish(mut self) -> (u64, String, String) {
        let mut w = self.w.take().unwrap();
        ck!(w.flush(), format!("flush {}", self.path.display()));
        let f = ck!(w.into_inner(), format!("close {}", self.path.display()));
        ck!(f.sync_all(), format!("fsync {}", self.path.display()));
        let nl = ck!(f.metadata(), format!("stat {}", self.path.display())).len();
        if nl != self.n {
            die(&format!(
                "{}: wrote {} bytes but the file is {}",
                self.path.display(),
                self.n,
                nl
            ));
        }
        let digest = hash_file(&self.path);
        (self.n, digest, self.path.display().to_string())
    }
}

fn hash_file(path: &Path) -> String {
    let mut f = File::open(path).unwrap_or_else(|e| die(&format!("open {}: {}", path.display(), e)));
    let mut h = Sha256::new();
    let mut buf = vec![0u8; READ_BUF];
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

// ---------------------------------------------------------------------------
// Document emitter
// ---------------------------------------------------------------------------

struct Emitter {
    out: PathBuf,
    corpus: Option<Sink>,
    seps: Option<PatchFile>,
    titles: Option<PatchFile>,
    sep_mode: bool,
    doc_count: u64,
    corpus_bytes: u64,
    prev_start: u64,
    anchors: Vec<u64>,
    title_offsets: Vec<u32>,
    title_lens: Vec<u16>,
    title_payload: u64,
    /// 0x1E bytes found inside document content (collision or, with
    /// --separator none, simply part of the corpus).
    content_seps: u64,
    /// Documents that contain at least one 0x1E in their content.
    docs_with_content_sep: u64,
    max_bytes: Option<u64>,
    cut: bool,
}

impl Emitter {
    fn new(out: &Path, sep_mode: bool, max_bytes: Option<u64>) -> Emitter {
        ck!(std::fs::create_dir_all(out), format!("mkdir {}", out.display()));
        let corpus_path = out.join("corpus.bin.partial");
        let seps_path = out.join("seps.bin.partial");
        let titles_path = out.join("titles.bin.partial");
        // A refused pass must never leave something that looks like a corpus:
        // the sidecars are written under `.partial` names and only renamed into
        // place once rule 1 has passed and every file is fsynced.
        for p in [
            &corpus_path,
            &seps_path,
            &titles_path,
            &out.join("corpus.bin"),
            &out.join("seps.bin"),
            &out.join("titles.bin"),
            &out.join("MANIFEST.json"),
        ] {
            if p.exists() {
                die(&format!(
                    "corpus: {} already exists; refusing to mix a new pass into an old directory",
                    p.display()
                ));
            }
        }
        Emitter {
            out: out.to_path_buf(),
            corpus: Some(Sink::new(&corpus_path)),
            seps: Some(PatchFile::new(&seps_path, SEP_HEADER)),
            titles: Some(PatchFile::new(&titles_path, TITLE_HEADER)),
            sep_mode,
            doc_count: 0,
            corpus_bytes: 0,
            prev_start: 0,
            anchors: Vec::new(),
            title_offsets: Vec::new(),
            title_lens: Vec::new(),
            title_payload: 0,
            content_seps: 0,
            docs_with_content_sep: 0,
            max_bytes,
            cut: false,
        }
    }

    /// Append one document. Returns true when the pass should stop because
    /// --max-bytes has been reached at this boundary.
    fn doc(&mut self, title: &[u8], body: &[u8]) -> bool {
        if self.doc_count >= u32::MAX as u64 {
            die("corpus: more than 2^32-1 documents: the u32 delta/offset sidecars cannot address them");
        }
        if title.len() > u16::MAX as usize {
            die(&format!(
                "corpus: title of {} bytes does not fit the u16 titles.bin length column",
                title.len()
            ));
        }
        if title.iter().any(|&b| b == b'\n' || b == SEP) {
            die(&format!(
                "corpus: title contains a newline or 0x1E (first 64 bytes: {:?}); in-band framing is impossible",
                String::from_utf8_lossy(&title[..title.len().min(64)])
            ));
        }

        // seps.bin: delta of document starts, every 64th absolute anchor.
        let start = self.corpus_bytes;
        let delta = start - self.prev_start;
        if delta > u32::MAX as u64 {
            die(&format!(
                "corpus: document {} is {} bytes, more than a u32 delta can express",
                self.doc_count, delta
            ));
        }
        if self.doc_count % ANCHOR_STRIDE == 0 {
            self.anchors.push(start);
        }
        {
            let seps = self.seps.as_mut().unwrap();
            seps.put_u32(delta as u32);
        }
        self.prev_start = start;

        // titles.bin
        if self.title_payload + title.len() as u64 > u32::MAX as u64 {
            die("corpus: titles payload exceeds 2^32-1 bytes: titles.bin offsets are u32");
        }
        {
            let titles = self.titles.as_mut().unwrap();
            titles.put(title);
        }
        self.title_offsets.push(self.title_payload as u32);
        self.title_lens.push(title.len() as u16);
        self.title_payload += title.len() as u64;

        // corpus.bin
        let hits = count_byte(body, SEP);
        if hits > 0 {
            self.content_seps += hits;
            self.docs_with_content_sep += 1;
        }
        {
            let corpus = self.corpus.as_mut().unwrap();
            corpus.put(title);
            corpus.put(b"\n");
            corpus.put(body);
            if self.sep_mode {
                corpus.put(&[SEP]);
            }
        }
        self.corpus_bytes = start + title.len() as u64 + 1 + body.len() as u64 + u64::from(self.sep_mode);
        self.doc_count += 1;

        match self.max_bytes {
            Some(limit) if self.corpus_bytes >= limit => {
                self.cut = true;
                return true;
            }
            _ => false,
        }
    }

    /// Total number of 0x1E bytes the finished corpus.bin will carry.
    fn separator_count(&self) -> u64 {
        self.content_seps + if self.sep_mode { self.doc_count } else { 0 }
    }

    fn finish(mut self) -> Finished {
        // seps.bin trailer + header
        let anchor_count = self.anchors.len() as u64;
        {
            let seps = self.seps.as_mut().unwrap();
            for a in &self.anchors {
                seps.put_u64(*a);
            }
            let mut h = Vec::with_capacity(SEP_HEADER as usize);
            h.extend_from_slice(SEP_MAGIC);
            h.extend_from_slice(&1u32.to_le_bytes());
            h.extend_from_slice(&(u32::from(self.sep_mode)).to_le_bytes());
            h.extend_from_slice(&(ANCHOR_STRIDE as u32).to_le_bytes());
            h.extend_from_slice(&self.doc_count.to_le_bytes());
            h.extend_from_slice(&self.corpus_bytes.to_le_bytes());
            h.extend_from_slice(&anchor_count.to_le_bytes());
            h.extend_from_slice(&0u64.to_le_bytes()); // base
            assert_eq!(h.len() as u64, SEP_HEADER);
            seps.patch(0, &h);
        }
        // titles.bin offsets/lengths + header
        {
            let titles = self.titles.as_mut().unwrap();
            for o in &self.title_offsets {
                titles.put_u32(*o);
            }
            for l in &self.title_lens {
                titles.put(&l.to_le_bytes());
            }
            let mut h = Vec::with_capacity(TITLE_HEADER as usize);
            h.extend_from_slice(TITLE_MAGIC);
            h.extend_from_slice(&1u32.to_le_bytes());
            h.extend_from_slice(&(self.doc_count as u32).to_le_bytes());
            h.extend_from_slice(&self.title_payload.to_le_bytes());
            assert_eq!(h.len() as u64, TITLE_HEADER);
            titles.patch(0, &h);
        }
        let (corpus_bytes, corpus_sha) = self.corpus.take().unwrap().finish();
        if corpus_bytes != self.corpus_bytes {
            die(&format!(
                "corpus: corpus.bin is {} bytes but the document accounting says {}",
                corpus_bytes, self.corpus_bytes
            ));
        }
        let (seps_bytes, seps_sha, _) = self.seps.take().unwrap().finish();
        let (titles_bytes, titles_sha, _) = self.titles.take().unwrap().finish();
        let expected_seps = SEP_HEADER + 4 * self.doc_count + 8 * self.anchors.len() as u64;
        if seps_bytes != expected_seps {
            die(&format!(
                "corpus: seps.bin is {} bytes, layout says {}",
                seps_bytes, expected_seps
            ));
        }
        let expected_titles = TITLE_HEADER + self.title_payload + 6 * self.doc_count;
        if titles_bytes != expected_titles {
            die(&format!(
                "corpus: titles.bin is {} bytes, layout says {}",
                titles_bytes, expected_titles
            ));
        }
        // Every file is fsynced and consistent: publish the sidecars under
        // their final names. Until this point the pass is visibly incomplete.
        for name in ["corpus.bin", "seps.bin", "titles.bin"] {
            let part = self.out.join(format!("{}.partial", name));
            let done = self.out.join(name);
            ck!(
                std::fs::rename(&part, &done),
                format!("rename {} -> {}", part.display(), done.display())
            );
        }
        if let Ok(d) = File::open(&self.out) {
            ck!(d.sync_all(), format!("fsync {}", self.out.display()));
        }
        Finished {
            doc_count: self.doc_count,
            corpus_bytes,
            corpus_sha,
            seps_bytes,
            seps_sha,
            titles_bytes,
            titles_sha,
            anchor_count,
            content_seps: self.content_seps,
            docs_with_content_sep: self.docs_with_content_sep,
            cut: self.cut,
            separator_count: self.separator_count(),
        }
    }
}

fn count_byte(hay: &[u8], b: u8) -> u64 {
    let mut n = 0u64;
    let mut i = 0;
    // 8 bytes at a time over the common case, byte-wise tail.
    while i + 8 <= hay.len() {
        let w = u64::from_le_bytes(hay[i..i + 8].try_into().unwrap());
        let pat = u64::from_le_bytes([b; 8]);
        let x = w ^ pat;
        // has_zero_byte
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

struct Finished {
    doc_count: u64,
    corpus_bytes: u64,
    corpus_sha: String,
    seps_bytes: u64,
    seps_sha: String,
    titles_bytes: u64,
    titles_sha: String,
    anchor_count: u64,
    content_seps: u64,
    docs_with_content_sep: u64,
    cut: bool,
    separator_count: u64,
}

// ---------------------------------------------------------------------------
// Streaming byte window over the decompressed XML
// ---------------------------------------------------------------------------

struct Stream<R: Read> {
    r: R,
    buf: Vec<u8>,
    pos: usize,
    end: usize,
    base_abs: u64,
    eof: bool,
}

impl<R: Read> Stream<R> {
    fn new(r: R) -> Stream<R> {
        Stream { r, buf: vec![0u8; READ_BUF], pos: 0, end: 0, base_abs: 0, eof: false }
    }

    fn abs_pos(&self) -> u64 {
        self.base_abs + self.pos as u64
    }

    fn avail(&self) -> usize {
        self.end - self.pos
    }

    /// Make at least `n` contiguous bytes visible (fewer only at EOF).
    fn ensure(&mut self, n: usize) -> io::Result<usize> {
        assert!(n <= self.buf.len(), "ensure({}) exceeds the {} byte window", n, self.buf.len());
        while self.avail() < n && !self.eof {
            if self.pos > 0 {
                self.buf.copy_within(self.pos..self.end, 0);
                self.base_abs += self.pos as u64;
                self.end -= self.pos;
                self.pos = 0;
            }
            match self.r.read(&mut self.buf[self.end..]) {
                Ok(0) => self.eof = true,
                Ok(k) => self.end += k,
                Err(e) if e.kind() == ErrorKind::Interrupted => {}
                Err(e) => return Err(e),
            }
        }
        Ok(self.avail())
    }

    fn fill(&mut self) -> io::Result<bool> {
        Ok(self.ensure(1)? > 0)
    }

    fn peek(&mut self) -> io::Result<Option<u8>> {
        if !self.fill()? {
            return Ok(None);
        }
        Ok(Some(self.buf[self.pos]))
    }

    fn bump(&mut self) {
        debug_assert!(self.pos < self.end);
        self.pos += 1;
    }

    /// Advance to the next occurrence of `byte` without consuming it.
    fn skip_to(&mut self, byte: u8) -> io::Result<bool> {
        loop {
            if !self.fill()? {
                return Ok(false);
            }
            let at = self.buf[self.pos..self.end].iter().position(|&b| b == byte);
            match at {
                Some(i) => {
                    self.pos += i;
                    return Ok(true);
                }
                None => self.pos = self.end,
            }
        }
    }

    /// Copy bytes into `sink` up to and including the first occurrence of
    /// `pat`. false = the stream ended first (sink holds everything remaining).
    fn scan_to(&mut self, pat: &[u8], sink: &mut Vec<u8>) -> io::Result<bool> {
        let first = pat[0];
        loop {
            if !self.fill()? {
                return Ok(false);
            }
            let at = {
                let hay = &self.buf[self.pos..self.end];
                match hay.iter().position(|&b| b == first) {
                    None => {
                        sink.extend_from_slice(hay);
                        None
                    }
                    Some(i) => {
                        sink.extend_from_slice(&hay[..i]);
                        Some(i)
                    }
                }
            };
            match at {
                None => self.pos = self.end,
                Some(i) => {
                    self.pos += i;
                    if self.ensure(pat.len())? < pat.len() {
                        sink.extend_from_slice(&self.buf[self.pos..self.end]);
                        self.pos = self.end;
                        return Ok(false);
                    }
                    if &self.buf[self.pos..self.pos + pat.len()] == pat {
                        self.pos += pat.len();
                        return Ok(true);
                    }
                    sink.push(self.buf[self.pos]);
                    self.pos += 1;
                }
            }
        }
    }

    fn starts_with(&mut self, s: &[u8]) -> io::Result<bool> {
        if self.ensure(s.len())? < s.len() {
            return Ok(false);
        }
        Ok(&self.buf[self.pos..self.pos + s.len()] == s)
    }
}

// ---------------------------------------------------------------------------
// XML structure pass
// ---------------------------------------------------------------------------

enum Step {
    Continue,
    /// --max-bytes boundary reached: stop reading.
    Stop,
}

struct Parser<R: Read> {
    s: Stream<R>,
    stack: Vec<Vec<u8>>,
    scratch: Vec<u8>,
    title: Vec<u8>,
    has_title: bool,
    body: Vec<u8>,
    has_body: bool,
    revisions: u32,
    multi_revision_pages: u64,
    pages: u64,
    saw_root: bool,
    e: Emitter,
}

impl<R: Read> Parser<R> {
    fn new(s: Stream<R>, e: Emitter) -> Parser<R> {
        Parser {
            s,
            stack: Vec::new(),
            scratch: Vec::new(),
            title: Vec::new(),
            has_title: false,
            body: Vec::new(),
            has_body: false,
            revisions: 0,
            multi_revision_pages: 0,
            pages: 0,
            saw_root: false,
            e,
        }
    }

    fn at(&self) -> u64 {
        self.s.abs_pos()
    }

    fn run(&mut self) -> Step {
        loop {
            match ck!(self.s.peek(), "read dump stream") {
                None => break,
                Some(b'<') => match self.element() {
                    Step::Continue => {}
                    Step::Stop => return Step::Stop,
                },
                Some(_) => {
                    // Character data outside any element of interest (whitespace
                    // between pages, <sitename> text, <comment> text...). Only
                    // <text> content is indexed, so this is skipped, but the
                    // stream is still fully consumed and validated.
                    ck!(self.s.skip_to(b'<'), "read dump stream");
                }
            }
        }
        if !self.stack.is_empty() {
            let open = self.stack.last().unwrap().clone();
            die(&format!(
                "corpus: truncated or malformed XML: stream ended at offset {} with <{}> still open",
                self.at(),
                String::from_utf8_lossy(&open)
            ));
        }
        if !self.saw_root {
            die("corpus: truncated or malformed XML: no <mediawiki> root element in the stream");
        }
        Step::Stop
    }

    /// Consume one markup item; called with the stream positioned on '<'.
    fn element(&mut self) -> Step {
        let start = self.at();
        self.s.bump(); // '<'
        match ck!(self.s.peek(), "read dump stream") {
            None => die(&format!("corpus: truncated XML: stream ended inside a tag opened at offset {}", start)),
            Some(b'!') => {
                self.s.bump();
                if ck!(self.s.starts_with(b"--"), "read dump stream") {
                    self.scratch.clear();
                    if !ck!(self.s.scan_to(b"-->", &mut self.scratch), "read dump stream") {
                        die(&format!("corpus: truncated XML: unterminated comment opened at offset {}", start));
                    }
                } else if ck!(self.s.starts_with(b"[CDATA["), "read dump stream") {
                    die(&format!(
                        "corpus: CDATA section at offset {} is not supported; refusing to guess at its content",
                        start
                    ));
                } else {
                    // <!DOCTYPE ...> and friends: no internal subset support is
                    // needed for MediaWiki dumps, and anything weird shows up
                    // later as a mismatched tag.
                    self.scratch.clear();
                    if !ck!(self.s.scan_to(b">", &mut self.scratch), "read dump stream") {
                        die(&format!("corpus: truncated XML: unterminated declaration at offset {}", start));
                    }
                }
                Step::Continue
            }
            Some(b'?') => {
                self.s.bump();
                self.scratch.clear();
                if !ck!(self.s.scan_to(b"?>", &mut self.scratch), "read dump stream") {
                    die(&format!("corpus: truncated XML: unterminated processing instruction at offset {}", start));
                }
                Step::Continue
            }
            Some(b'/') => {
                self.s.bump();
                let name = self.tag_name();
                self.close_tag(&name, start);
                Step::Continue
            }
            Some(_) => {
                let name = self.tag_name();
                self.open_tag(&name, start)
            }
        }
    }

    fn tag_name(&mut self) -> Vec<u8> {
        let mut name = Vec::new();
        loop {
            match ck!(self.s.peek(), "read dump stream") {
                None => die(&format!(
                    "corpus: truncated XML: stream ended inside a tag name at offset {}",
                    self.at()
                )),
                Some(c) if c == b'>' || c == b'/' || c.is_ascii_whitespace() => break,
                Some(c) => {
                    name.push(c);
                    self.s.bump();
                }
            }
        }
        if name.is_empty() {
            die(&format!("corpus: malformed XML: empty tag name at offset {}", self.at()));
        }
        name
    }

    /// Consume the rest of an open tag. Returns self_closing.
    fn tag_tail(&mut self, start: u64) -> bool {
        let mut quote: Option<u8> = None;
        let mut last = 0u8;
        loop {
            let c = match ck!(self.s.peek(), "read dump stream") {
                None => die(&format!("corpus: truncated XML: unterminated tag opened at offset {}", start)),
                Some(c) => c,
            };
            if let Some(q) = quote {
                if c == q {
                    quote = None;
                }
                last = c;
                self.s.bump();
                continue;
            }
            match c {
                b'"' | b'\'' => {
                    quote = Some(c);
                    last = c;
                }
                b'>' => {
                    self.s.bump();
                    return last == b'/';
                }
                _ => {
                    if !c.is_ascii_whitespace() {
                        last = c;
                    }
                }
            }
            self.s.bump();
        }
    }

    fn open_tag(&mut self, name: &[u8], start: u64) -> Step {
        let self_closing = self.tag_tail(start);
        let top = self.stack.last().map(|v| v.as_slice());

        if self_closing {
            // An empty element. Only <text/> carries meaning for us.
            if name == b"text" {
                self.require_in_revision(name, start);
                self.has_body = true;
                self.body.clear();
            }
            return Step::Continue;
        }

        match name {
            b"mediawiki" => {
                if !self.stack.is_empty() {
                    die(&format!("corpus: malformed XML: <mediawiki> at offset {} is not the root", start));
                }
                self.saw_root = true;
                self.stack.push(name.to_vec());
            }
            b"page" => {
                if top != Some(b"mediawiki".as_slice()) {
                    die(&format!(
                        "corpus: malformed XML: <page> at offset {} is not a child of <mediawiki>",
                        start
                    ));
                }
                self.title.clear();
                self.has_title = false;
                self.body.clear();
                self.has_body = false;
                self.revisions = 0;
                self.stack.push(name.to_vec());
            }
            b"revision" => {
                if !self.stack.iter().any(|n| n == b"page") {
                    die(&format!("corpus: malformed XML: <revision> at offset {} outside <page>", start));
                }
                self.revisions += 1;
                // Only the last revision of a page is indexed (dump dumps are
                // single-revision; history dumps are newest-last).
                self.body.clear();
                self.has_body = false;
                self.stack.push(name.to_vec());
            }
            b"title" => {
                if top == Some(b"page".as_slice()) {
                    if self.has_title {
                        die(&format!(
                            "corpus: malformed XML: page has two <title> elements (second at offset {})",
                            start
                        ));
                    }
                    self.title.clear();
                    if !ck!(self.s.scan_to(b"</title>", &mut self.title), "read dump stream") {
                        die(&format!("corpus: truncated XML: <title> opened at offset {} is unterminated", start));
                    }
                    if self.title.iter().any(|&b| b == b'\n' || b == SEP) {
                        die(&format!(
                            "corpus: title at offset {} contains a newline or 0x1E; in-band framing is impossible",
                            start
                        ));
                    }
                    self.has_title = true;
                } else {
                    // <title> outside a page (not present in MediaWiki dumps):
                    // consume it as an ordinary element.
                    self.stack.push(name.to_vec());
                }
            }
            b"text" => {
                self.require_in_revision(name, start);
                self.has_body = true;
                self.body.clear();
                if !ck!(self.s.scan_to(b"</text>", &mut self.body), "read dump stream") {
                    die(&format!("corpus: truncated XML: <text> opened at offset {} is unterminated", start));
                }
                if self.body.len() > MAX_DOC_BYTES {
                    die(&format!(
                        "corpus: one document body exceeds {} bytes; refusing to buffer it",
                        MAX_DOC_BYTES
                    ));
                }
            }
            _ => self.stack.push(name.to_vec()),
        }
        Step::Continue
    }

    fn require_in_revision(&self, name: &[u8], start: u64) {
        if self.stack.last().map(|v| v.as_slice()) != Some(b"revision".as_slice()) {
            die(&format!(
                "corpus: malformed XML: <{}> at offset {} is not a direct child of <revision>",
                String::from_utf8_lossy(name),
                start
            ));
        }
    }

    fn close_tag(&mut self, name: &[u8], start: u64) {
        // Optional whitespace before '>'.
        loop {
            match ck!(self.s.peek(), "read dump stream") {
                None => die(&format!("corpus: truncated XML: unterminated close tag at offset {}", start)),
                Some(b'>') => {
                    self.s.bump();
                    break;
                }
                Some(c) if c.is_ascii_whitespace() => self.s.bump(),
                Some(_) => die(&format!(
                    "corpus: malformed XML: junk in close tag </{}> at offset {}",
                    String::from_utf8_lossy(name),
                    start
                )),
            }
        }
        let open = match self.stack.pop() {
            None => die(&format!(
                "corpus: malformed XML: </{}> at offset {} closes nothing",
                String::from_utf8_lossy(name),
                start
            )),
            Some(v) => v,
        };
        if open != name {
            die(&format!(
                "corpus: malformed XML: </{}> at offset {} closes <{}>",
                String::from_utf8_lossy(name),
                start,
                String::from_utf8_lossy(&open)
            ));
        }

        if name == b"page" {
            if !self.has_title {
                die(&format!(
                    "corpus: malformed XML: page closed at offset {} has no <title>",
                    start
                ));
            }
            if self.revisions == 0 {
                die(&format!(
                    "corpus: malformed XML: page '{}' has no <revision>",
                    String::from_utf8_lossy(&self.title)
                ));
            }
            if !self.has_body {
                die(&format!(
                    "corpus: malformed XML: page '{}' has no <text> element",
                    String::from_utf8_lossy(&self.title)
                ));
            }
            if self.revisions > 1 {
                self.multi_revision_pages += 1;
            }
            self.pages += 1;
            // Copies are unavoidable here (the emitter outlives the page state)
            // and amount to one document of scratch memory.
            let title = std::mem::take(&mut self.title);
            let body = std::mem::take(&mut self.body);
            let stop = self.e.doc(&title, &body);
            self.title = title;
            self.body = body;
            self.title.clear();
            self.body.clear();
            self.has_title = false;
            self.has_body = false;
            self.revisions = 0;
            if stop {
                return;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

fn spawn_decompressor(dump: &str) -> (Child, String) {
    let mut last_err = String::new();
    for prog in ["pbzip2", "bzip2"] {
        let mut cmd = Command::new(prog);
        cmd.arg("-dc");
        if dump != "-" {
            cmd.arg(dump);
            cmd.stdin(Stdio::null());
        } else {
            cmd.stdin(Stdio::inherit());
        }
        cmd.stdout(Stdio::piped()).stderr(Stdio::inherit());
        match cmd.spawn() {
            Ok(c) => return (c, format!("{} -dc {}", prog, if dump == "-" { "<stdin>" } else { dump })),
            Err(e) if e.kind() == ErrorKind::NotFound => {
                last_err = format!("{}: {}", prog, e);
                continue;
            }
            Err(e) => die(&format!("corpus: cannot run {}: {}", prog, e)),
        }
    }
    die(&format!(
        "corpus: no bzip2 decompressor on PATH (tried pbzip2, bzip2; last error: {})",
        last_err
    ))
}

fn run(dump: &str, out: &str, max_bytes: Option<u64>, sep_mode: bool, t0: Instant) {
    // 1. Hash and size the compressed dump before touching the output directory:
    //    MANIFEST identifies the input, and an unreadable path fails early.
    let (dump_sha, dump_bytes) = if dump == "-" {
        (None, None)
    } else {
        let p = Path::new(dump);
        let md = std::fs::metadata(p).unwrap_or_else(|e| die(&format!("stat {}: {}", dump, e)));
        if !md.is_file() {
            die(&format!("corpus: {} is not a regular file", dump));
        }
        let mut magic = [0u8; 3];
        let mut f = File::open(p).unwrap_or_else(|e| die(&format!("open {}: {}", dump, e)));
        match f.read_exact(&mut magic) {
            Ok(()) => {
                if &magic != b"BZh" {
                    die(&format!(
                        "corpus: {} does not start with 'BZh': not a bzip2 stream (pass the compressed dump)",
                        dump
                    ));
                }
            }
            Err(e) => die(&format!("read {}: {}", dump, e)),
        }
        drop(f);
        let sha = hash_file(p);
        (Some(sha), Some(md.len()))
    };
    crate::trace_since("corpus_hash_input", t0);

    // 2. Decompress + parse + emit, all in one streaming pass.
    let t1 = Instant::now();
    let (mut child, decompressor) = spawn_decompressor(dump);
    let stdout = match child.stdout.take() {
        Some(s) => s,
        None => die("corpus: decompressor produced no stdout pipe"),
    };
    let emitter = Emitter::new(Path::new(out), sep_mode, max_bytes);
    let mut parser = Parser::new(Stream::new(stdout), emitter);
    let step = parser.run();
    crate::trace_since("corpus_parse", t1);

    let cut = matches!(step, Step::Stop) && parser.e.cut;
    let pages = parser.pages;
    let multi_rev = parser.multi_revision_pages;

    // 3. Reap the decompressor. A non-zero exit is a truncated or corrupt bz2
    //    stream: the XML we just consumed is not the dump.
    if cut {
        let _ = child.kill();
        let _ = child.wait();
    } else {
        match child.wait() {
            Ok(st) if st.success() => {}
            Ok(st) => die(&format!(
                "corpus: decompressor failed ({}): truncated or invalid bzip2 stream; refusing to emit a short corpus",
                st
            )),
            Err(e) => die(&format!("corpus: waiting for the decompressor: {}", e)),
        }
    }

    // 4. Rule 1: the framing byte must equal the document count in separator mode.
    let doc_count = parser.e.doc_count;
    let sep_count = parser.e.separator_count();
    let content_seps = parser.e.content_seps;
    let docs_hit = parser.e.docs_with_content_sep;
    if sep_mode && sep_count != doc_count {
        die(&format!(
            "0x1E occurs inside document content (count={} docs={}); rerun with --separator none\n\
             xsa: note: {} of those {} 0x1E bytes came from document content ({} document(s) affected)\n\
             xsa: note: nothing was published; the incomplete sidecars keep their .partial names",
            sep_count, doc_count, content_seps, sep_count, docs_hit
        ));
    }

    // 5. Close the sidecars and write the manifest last (its presence means the
    //    pass completed and every sidecar is consistent).
    let t2 = Instant::now();
    let fin = parser.e.finish();
    crate::trace_since("corpus_finish", t2);

    let finished_at = iso8601_utc(now_unix());
    let manifest = serde_json::json!({
        "tool": "xsa corpus --wikipedia",
        "manifest_version": 1,
        "dump_path": dump,
        "dump_sha256": dump_sha,
        "dump_bytes_compressed": dump_bytes,
        "decompressor": decompressor,
        "separator": if sep_mode { "0x1e" } else { "none" },
        "separator_byte": if sep_mode { serde_json::json!(SEP) } else { serde_json::Value::Null },
        "doc_count": fin.doc_count,
        "corpus_bytes": fin.corpus_bytes,
        "separator_count": fin.separator_count,
        "content_separator_bytes": fin.content_seps,
        "documents_with_content_separator": fin.docs_with_content_sep,
        "max_bytes": max_bytes,
        "cut_at_document_boundary": fin.cut,
        "cut_corpus_bytes": if fin.cut { serde_json::json!(fin.corpus_bytes) } else { serde_json::Value::Null },
        "pages_parsed": pages,
        "pages_with_multiple_revisions": multi_rev,
        "namespace_filter": "none: every <page> is indexed regardless of namespace prefix",
        "xml_entities": "not decoded: title and <text> bytes are copied verbatim from the XML",
        "sidecars": {
            "corpus.bin": { "bytes": fin.corpus_bytes, "sha256": fin.corpus_sha },
            "seps.bin": { "bytes": fin.seps_bytes, "sha256": fin.seps_sha, "anchor_count": fin.anchor_count, "anchor_stride": ANCHOR_STRIDE },
            "titles.bin": { "bytes": fin.titles_bytes, "sha256": fin.titles_sha }
        },
        "layout": {
            "corpus.bin": "<title>\\n<body bytes>\\x1E per document; the 0x1E is omitted with --separator none",
            "seps.bin": "48B header: b\"XSEP\", u32 version=1, u32 flags(bit0=0x1E emitted), u32 anchor_stride=64, u64 doc_count, u64 corpus_bytes, u64 anchor_count, u64 base; then doc_count*u32 LE deltas (d[0]=start[0]-base, d[i]=start[i]-start[i-1]); then anchor_count*u64 LE absolute start offsets of every 64th document. rank_1E(pos): document i with start[i] <= pos; with 0x1e framing the separator of doc i is at start[i+1]-1",
            "titles.bin": "20B header: b\"XTIT\", u32 version=1, u32 doc_count, u64 payload_bytes; then the concatenated titles; then doc_count*u32 LE offsets relative to payload start; then doc_count*u16 LE lengths"
        },
        "finished_at": finished_at
    });
    let mpath = Path::new(out).join("MANIFEST.json");
    let mut body = serde_json::to_string_pretty(&manifest)
        .unwrap_or_else(|e| die(&format!("serialise manifest: {}", e)));
    body.push('\n');
    let mut f = File::create(&mpath).unwrap_or_else(|e| die(&format!("create {}: {}", mpath.display(), e)));
    ck!(f.write_all(body.as_bytes()), format!("write {}", mpath.display()));
    ck!(f.sync_all(), format!("fsync {}", mpath.display()));

    if fin.cut {
        eprintln!(
            "xsa corpus: --max-bytes {} reached: cut at document boundary after doc {}: corpus_bytes={} docs={}",
            max_bytes.unwrap(),
            fin.doc_count,
            fin.corpus_bytes,
            fin.doc_count
        );
    }
    println!(
        "xsa corpus: docs={} corpus_bytes={} separators={} separator={} seps.bin={} titles.bin={} sha256(corpus.bin)={}",
        fin.doc_count,
        fin.corpus_bytes,
        fin.separator_count,
        if sep_mode { "0x1e" } else { "none" },
        fin.seps_bytes,
        fin.titles_bytes,
        fin.corpus_sha
    );
    println!("xsa corpus: manifest {}", mpath.display());
    crate::trace_since("corpus_wall", t0);
}

fn now_unix() -> i64 {
    match std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH) {
        Ok(d) => d.as_secs() as i64,
        Err(e) => -(e.duration().as_secs() as i64),
    }
}

/// ISO-8601 UTC (days-to-civil, Howard Hinnant's algorithm) without a chrono
/// dependency. Only MANIFEST.json carries a timestamp; the sidecars never do.
fn iso8601_utc(secs: i64) -> String {
    let days = secs.div_euclid(86_400);
    let rem = secs.rem_euclid(86_400);
    let (hh, mm, ss) = (rem / 3600, (rem % 3600) / 60, rem % 60);
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    format!("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", y, m, d, hh, mm, ss)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn size_suffixes_are_binary() {
        assert_eq!(parse_size("1K"), 1024);
        assert_eq!(parse_size("90G"), 90u64 << 30);
        assert_eq!(parse_size("0"), 0);
    }

    #[test]
    fn positions_between_utc_times() {
        assert_eq!(iso8601_utc(0), "1970-01-01T00:00:00Z");
        assert_eq!(iso8601_utc(1_700_000_000), "2023-11-14T22:13:20Z");
    }

    #[test]
    fn byte_counting() {
        assert_eq!(count_byte(b"", SEP), 0);
        assert_eq!(count_byte(b"a\x1eb\x1e\x1e", SEP), 3);
        let long = vec![b'x'; 1000];
        assert_eq!(count_byte(&long, SEP), 0);
        let mut v = long.clone();
        v[999] = SEP;
        v[7] = SEP;
        assert_eq!(count_byte(&v, SEP), 2);
    }
}
