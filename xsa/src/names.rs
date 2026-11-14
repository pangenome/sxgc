//! `xsa names` — the per-document title column (XNAM v1).
//!
//! `tools/names_extract.py` writes it: payload of extracted titles, then a u64
//! offset and a u32 length per document. A zero length means the document's set
//! has no extractable title (the Pile's web and code sets were published without
//! one), and the reader then answers with the set name from `titles.bin` (XTIT
//! v1) — never a guess, always the truth that the artifact holds.
//!
//! Random access is two preads: (payload + 8·doc) and (payload + 8·docs + 4·doc),
//! so a 7.5 GB column answers a lookup without being mapped.

use std::fs::File;
use std::io::BufRead;
use std::os::unix::fs::FileExt;
use std::path::{Path, PathBuf};

use crate::die;

const NAMES_MAGIC: &[u8] = b"XNAM";
const NAMES_VERSION: u32 = 1;
const TITLE_MAGIC: &[u8] = b"XTIT";

struct Names {
    f: File,
    docs: u64,
    payload: u64,
}

impl Names {
    fn open(path: &Path) -> Result<Self, String> {
        let f = File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
        let mut h = [0u8; 20];
        f.read_exact_at(&mut h, 0)
            .map_err(|e| format!("{}: header: {e}", path.display()))?;
        if &h[..4] != NAMES_MAGIC {
            return Err(format!(
                "{}: not an XNAM column (magic {:?}); write it with tools/names_extract.py",
                path.display(),
                String::from_utf8_lossy(&h[..4])
            ));
        }
        let version = u32::from_le_bytes(h[4..8].try_into().unwrap());
        if version != NAMES_VERSION {
            return Err(format!(
                "{}: XNAM v{version}, this build reads v{NAMES_VERSION}",
                path.display()
            ));
        }
        let docs = u32::from_le_bytes(h[8..12].try_into().unwrap()) as u64;
        let payload = u64::from_le_bytes(h[12..20].try_into().unwrap());
        Ok(Self { f, docs, payload })
    }

    /// (title, is_present)
    fn title(&self, doc: u64) -> Result<Option<String>, String> {
        if doc >= self.docs {
            return Err(format!(
                "doc {doc} is past the column's {} documents",
                self.docs
            ));
        }
        let off_at = 20 + self.payload + 8 * doc;
        let len_at = 20 + self.payload + 8 * self.docs + 4 * doc;
        let mut b8 = [0u8; 8];
        self.f
            .read_exact_at(&mut b8, off_at)
            .map_err(|e| format!("names offset read: {e}"))?;
        let off = u64::from_le_bytes(b8);
        let mut b4 = [0u8; 4];
        self.f
            .read_exact_at(&mut b4, len_at)
            .map_err(|e| format!("names length read: {e}"))?;
        let len = u32::from_le_bytes(b4) as u64;
        if len == 0 {
            return Ok(None);
        }
        if off + len > self.payload {
            return Err(format!(
                "names column corrupt: doc {doc} has offset {off} + length {len} past the payload {}",
                self.payload
            ));
        }
        let mut v = vec![0u8; len as usize];
        self.f
            .read_exact_at(&mut v, 20 + off)
            .map_err(|e| format!("names payload read: {e}"))?;
        Ok(Some(String::from_utf8_lossy(&v).into_owned()))
    }
}

/// The set-name column (XTIT v1), used when a document has no extracted title.
struct Titles {
    payload: Vec<u8>,
    off: Vec<u32>,
    len: Vec<u16>,
}

impl Titles {
    fn open(path: &Path) -> Result<Self, String> {
        let b = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
        if b.len() < 20 || &b[..4] != TITLE_MAGIC {
            return Err(format!("{}: not an XTIT column", path.display()));
        }
        let count = u32::from_le_bytes(b[8..12].try_into().unwrap()) as usize;
        let payload_bytes = u64::from_le_bytes(b[12..20].try_into().unwrap()) as usize;
        let off_at = 20 + payload_bytes;
        let len_at = off_at + 4 * count;
        if b.len() < len_at + 2 * count {
            return Err(format!("{}: truncated XTIT column", path.display()));
        }
        let mut off = vec![0u32; count];
        let mut len = vec![0u16; count];
        for i in 0..count {
            off[i] = u32::from_le_bytes(b[off_at + 4 * i..off_at + 4 * i + 4].try_into().unwrap());
            len[i] = u16::from_le_bytes(b[len_at + 2 * i..len_at + 2 * i + 2].try_into().unwrap());
        }
        Ok(Self {
            payload: b[20..20 + payload_bytes].to_vec(),
            off,
            len,
        })
    }

    fn get(&self, doc: u64) -> Option<String> {
        let i = doc as usize;
        if i >= self.off.len() {
            return None;
        }
        let o = self.off[i] as usize;
        let l = self.len[i] as usize;
        self.payload
            .get(o..o + l)
            .map(|s| String::from_utf8_lossy(s).into_owned())
    }
}

pub fn command(args: &[String]) {
    let mut map: Option<PathBuf> = None;
    let mut docs: Vec<u64> = Vec::new();
    let mut docids_file: Option<PathBuf> = None;
    let mut jsonl = false;
    let mut i = 0;
    while i < args.len() {
        let a = args[i].as_str();
        i += 1;
        let mut next = || {
            args.get(i)
                .cloned()
                .unwrap_or_else(|| die(&format!("names: {a} needs a value")))
        };
        match a {
            "--map" | "--docs" => {
                map = Some(PathBuf::from(next()));
                i += 1;
            }
            "--doc" => {
                docs.push(next().parse().unwrap_or_else(|_| die("names: --doc takes a number")));
                i += 1;
            }
            "--docids" => {
                docids_file = Some(PathBuf::from(next()));
                i += 1;
            }
            "--jsonl" => jsonl = true,
            other => die(&format!("names: unknown option '{other}'")),
        }
    }
    let map = map.unwrap_or_else(|| die("names: --map DIR (the document map directory)"));
    let names = Names::open(&map.join("names.bin")).unwrap_or_else(|e| die(&e));
    let titles = Titles::open(&map.join("titles.bin")).ok();
    if let Some(f) = docids_file {
        let fh = File::open(&f).unwrap_or_else(|e| die(&format!("names: {}: {e}", f.display())));
        for line in std::io::BufReader::new(fh).lines() {
            let line = line.unwrap_or_else(|e| die(&format!("names: read: {e}")));
            let t = line.trim();
            if t.is_empty() {
                continue;
            }
            docs.push(t.parse().unwrap_or_else(|_| die(&format!("names: '{t}' is not a docid"))));
        }
    }
    if docs.is_empty() {
        die("names: give --doc N (repeatable) or --docids FILE");
    }
    for d in docs {
        let title = names.title(d).unwrap_or_else(|e| die(&e));
        let set = titles.as_ref().and_then(|t| t.get(d));
        let (kind, name) = match (&title, &set) {
            (Some(t), _) => ("title", t.clone()),
            (None, Some(s)) => ("set", s.clone()),
            (None, None) => ("none", String::new()),
        };
        if jsonl {
            println!(
                "{}",
                serde_json::json!({"doc": d, "kind": kind, "name": name, "set": set})
            );
        } else {
            println!("{d}\t{kind}\t{name}");
        }
    }
}
