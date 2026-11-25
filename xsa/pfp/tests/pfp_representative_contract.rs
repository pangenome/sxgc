//! Can the historical nonoverlapping parser's trigger events be used unchanged
//! as classical PFP anchors? A 71-byte counterexample, with the existing
//! overlapping constructor as a positive control.

#[path = "../src/col.rs"]
mod col;
#[path = "../src/laws.rs"]
mod laws;
#[path = "../src/overlap.rs"]
mod overlap;
#[path = "../src/parse.rs"]
mod parse;

use std::fs;
use std::os::unix::fs::DirBuilderExt;
use std::path::PathBuf;
use std::time::{SystemTime, UNIX_EPOCH};

/// The parser's trigger k-mer length, `XSA_PFP_K` in `src/parse.rs`.
const K: usize = 8;
/// The parser's comparison window `XSA_PFP_W` in `src/parse.rs`, which is also
/// the overlap width a representative carries.
const W: usize = 8;

fn fixture_text() -> Vec<u8> {
    let mut text: Vec<u8> = (0..70)
        .map(|i| if i % 16 == 0 { b'a' } else { b'b' })
        .collect();
    text.push(b'Z');
    text
}

/// A unique directory under the process temp dir, owner-only and removed on drop.
struct Scratch {
    dir: PathBuf,
}

impl Scratch {
    fn new(tag: &str) -> Scratch {
        let stamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("clock before epoch")
            .as_nanos();
        let dir = std::env::temp_dir().join(format!(
            "pfp_representative_contract_{}_{}_{}",
            tag,
            std::process::id(),
            stamp
        ));
        fs::DirBuilder::new().mode(0o700).create(&dir).expect("create private scratch dir");
        Scratch { dir }
    }

    /// A directory owned by this scratch, created on demand and returned as a path.
    fn dir(&self, name: &str) -> String {
        let path = self.dir.join(name);
        fs::create_dir(&path).expect("create scratch subdirectory");
        path.to_str().expect("utf-8 scratch path").to_string()
    }

    fn file(&self, name: &str) -> String {
        // A path inside this scratch; nothing is created.
        self.dir
            .join(name)
            .to_str()
            .expect("utf-8 scratch path")
            .to_string()
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.dir);
    }
}

/// The parser's phrases as `(start, len)` in text order, read from the 12-byte
/// little-endian records of its occurrence sink.
fn read_occurrences(path: &str) -> Vec<(u64, u32)> {
    let raw = fs::read(path).expect("read the occurrence sink");
    assert_eq!(raw.len() % 12, 0, "records are (u64 start, u32 len)");
    raw.chunks_exact(12)
        .map(|r| {
            (
                u64::from_le_bytes(r[..8].try_into().expect("8 bytes")),
                u32::from_le_bytes(r[8..].try_into().expect("4 bytes")),
            )
        })
        .collect()
}

/// Nonterminal representatives end with the first trigger w-gram starting
/// strictly AFTER the suffix start. The two witnesses are both nonterminal.
fn overlap_representative<'a>(text: &'a [u8], anchors: &[usize], r: usize) -> Option<&'a [u8]> {
    let anchor = anchors.iter().copied().find(|&a| a > r)?;
    text.get(r..anchor + W)
}

/// Independent brute enumerator over the same text: rows in dictionary order,
/// with the adjacent byte LCP and the raw predecessor byte. Position 0 reports
/// predecessor 0 and the END row reports the last corpus byte, which is this
/// constructor's callback convention, not the v10 writer's code 256.
fn brute_rows(text: &[u8]) -> Vec<(u64, u64, u8)> {
    let n = text.len();
    let mut suffixes: Vec<usize> = (0..n).collect();
    suffixes.sort_by(|&a, &b| text[a..].cmp(&text[b..]));
    let mut rows = vec![(n as u64, 0u64, text[n - 1])];
    let mut previous: Option<usize> = None;
    for &pos in &suffixes {
        let lcp = match previous {
            None => 0,
            Some(prev) => text[prev..]
                .iter()
                .zip(&text[pos..])
                .take_while(|(a, b)| a == b)
                .count() as u64,
        };
        rows.push((pos as u64, lcp, if pos == 0 { 0 } else { text[pos - 1] }));
        previous = Some(pos);
    }
    rows
}

#[test]
fn parser_triggers_are_not_classical_overlap_anchors() {
    let text = fixture_text();
    assert_eq!(text.len(), 71);
    let file_n = text.len() as u64;

    let scratch = Scratch::new("triggers");
    let corpus = scratch.file("corpus.bin");
    fs::write(&corpus, &text).expect("write corpus");
    let corpus = fs::File::open(&corpus).expect("open corpus");
    let modeb = scratch.dir("modeb");
    let laws = laws::Laws::new(
        scratch.file("stop"),
        1 << 30,
        1 << 30,
        file_n,
        modeb.clone(),
        scratch.dir("merged"),
        scratch.dir("bands"),
    );
    let out = parse::parse(&corpus, file_n, &modeb, &laws);

    let occurrences = read_occurrences(&out.occ_path);
    assert_eq!(occurrences.len() as u64, out.m0);
    assert_eq!(out.forced_splits, 0, "a 71-byte text cannot reach the 65535 cap");

    let mut at = 0u64;
    for &(start, len) in &occurrences {
        assert_eq!(start, at, "parse rows are contiguous and in text order");
        at += u64::from(len);
    }
    assert_eq!(at, file_n, "parse rows cover the corpus exactly");

    // A phrase ends with its trigger k-mer, so the nonterminal phrase ends minus
    // K are the natural trigger starts. The final phrase runs to the end of the
    // text and is not a trigger.
    let triggers: Vec<usize> = occurrences[..occurrences.len() - 1]
        .iter()
        .map(|&(start, len)| (start + u64::from(len)) as usize - K)
        .collect();
    assert_eq!(triggers, vec![28, 60], "natural trigger starts of this text");

    // Interpret only those two events as if they were valid classical overlap
    // anchors, and read two rows across the first one: row 13 ends at the anchor
    // starting at 28, row 29 begins one byte inside that same 8-byte anchor w-gram
    // (28..36) and runs to the next anchor at 60.
    let before = overlap_representative(&text, &triggers, 13).expect("anchor at 28");
    let after = overlap_representative(&text, &triggers, 29).expect("anchor at 60");
    assert_eq!(before, &text[13..36]);
    assert_eq!(after, &text[29..68]);
    assert!(
        after.len() > before.len() && after.starts_with(before),
        "rep(13)={:?} is not a strict proper prefix of rep(29)={:?}",
        before,
        after
    );
    assert!(before < after);

    // The representative order and the real text order disagree, and not through a
    // tie: the two suffixes share 41 bytes and then Z at byte 70 sorts below the b
    // at byte 54, so the row at 29 sorts first. Reading the one-sided triggers as
    // classical anchors would place 13 first while the byte suffixes place 29
    // first. That disqualifies this splice; it is not evidence against one-sided
    // policies in general or against the historical product.
    let shared = text[13..].iter().zip(&text[29..]).take_while(|(a, b)| a == b).count();
    assert_eq!(shared, 41);
    assert_eq!(text[13 + shared], b'b');
    assert_eq!(text[29 + shared], b'Z');
    assert!(text[13..] > text[29..]);
}

#[test]
fn constructor_rows_match_the_brute_enumerator() {
    let text = fixture_text();
    // The constructor picks its own anchors from the bytes; nothing measured in
    // the control above is handed to it.
    let mut emitted = Vec::new();
    overlap::construct(
        &text,
        |_stage: &str| {},
        |pos, lcp, predecessor| emitted.push((pos, lcp, predecessor)),
    );

    let expected = brute_rows(&text);
    assert_eq!(emitted.len(), text.len() + 1, "one END row and one row per byte");
    for (row, (got, want)) in emitted.iter().zip(&expected).enumerate() {
        assert_eq!(got, want, "row {} of the representative stream", row);
    }

    // The same rows across the two positions that control 1 splices: the real
    // constructor emits the row at 29 before the row at 13.
    let rank = |pos: u64| emitted.iter().position(|&(p, _, _)| p == pos).unwrap();
    assert!(rank(29) < rank(13), "rows 29 and 13 are in text-suffix order");
}
