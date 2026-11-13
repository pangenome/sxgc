// ============================================================================
// `xsa search` — boundary-aware pattern search over the modern lite container.
//
// THE NO-CROSS LAW (the owner's semantic requirement): a match [s,e) CROSSES a
// document boundary iff rank(s) != rank(e-1), and crossing matches are
// EXCLUDED from the default output; `--cross-doc` opts back in and the
// exclusion is JOURNALED. Structure comes from the container's own member m15
// (SXI2 v10): without it a match cannot be classified at all, and the command
// REFUSES rather than returning a crossing-inclusive list silently — this is a
// new surface, so its contract is the law from birth (the legacy `xsa
// query`/`mems` path keeps its own loud-note behaviour for old containers).
//
// Occurrences are LOCATED (the container's sampled-SA LF walk, the same
// `locate_steps` consult's enumeration uses), so every emitted position is a
// real text position, classified by the exact predicate:
//
//     crossing  <=>  select(rank(pos + len - 1)) > pos
//                    (the document holding the match's last byte starts after
//                     the match's first byte; equivalently rank(pos) !=
//                     rank(pos+len-1))
//
// With `--corpus FILE` every emitted match is VERIFIED byte for byte against
// the corpus (a pread + memcmp per match): the position list is then
// self-certified, not just classified.
//
// Bounded by `--max-occ` (default 65536): a pattern with more occurrences
// than that is refused BY NAME with its count, never silently truncated -
// truncation would corrupt the excluded-set semantics the gate depends on.
// ============================================================================

use std::io::{BufRead as _, Write as _};

fn search_die(msg: &str) -> ! {
    die(&format!("search: {msg}"))
}

pub fn search_command(raw: &[std::ffi::OsString]) {
    let args: Vec<String> = raw
        .iter()
        .map(|s| s.to_string_lossy().into_owned())
        .collect();
    let mut container: Option<String> = None;
    let mut patterns: Option<String> = None;
    let mut pattern: Option<String> = None;
    let mut cross_doc = false;
    let mut corpus: Option<String> = None;
    let mut json = false;
    let mut max_occ: u64 = 65536;
    let mut hex = false;
    let mut i = 0usize;
    macro_rules! val {
        ($name:expr) => {{
            i += 1;
            match args.get(i) {
                Some(v) => v.clone(),
                None => search_die(&format!("{} needs a value", $name)),
            }
        }};
    }
    while i < args.len() {
        match args[i].as_str() {
            "--patterns" => patterns = Some(val!("--patterns")),
            "--pattern" => pattern = Some(val!("--pattern")),
            "--cross-doc" => cross_doc = true,
            "--corpus" => corpus = Some(val!("--corpus")),
            "--jsonl" => json = true,
            "--hex" => hex = true,
            "--max-occ" => {
                let v = val!("--max-occ");
                max_occ = v.parse().unwrap_or_else(|_| search_die("--max-occ: integer"));
            }
            other if !other.starts_with('-') && container.is_none() => container = Some(other.to_string()),
            other => search_die(&format!("unknown option {other}")),
        }
        i += 1;
    }
    if container.is_none() || (patterns.is_none() && pattern.is_none()) {
        search_die("usage: xsa search <container> (--patterns FILE | --pattern S) \
                    [--cross-doc] [--corpus FILE] [--jsonl] [--hex] [--max-occ N]");
    }
    if max_occ == 0 {
        search_die("--max-occ must be at least 1");
    }
    let container = container.unwrap();
    let reader = Reader::open(&container).unwrap_or_else(|e| search_die(&e.to_string()));
    let index = Index::open(reader, 1).unwrap_or_else(|e| search_die(&e.to_string()));
    // THE STRUCTURE: member m15, always present (it is part of the one container
    // shape). The default classification is the no-cross law; `--cross-doc` is a
    // SEMANTIC opt-in to the crossing-inclusive list, not a format fallback.
    let ds = crate::docstarts::DocStarts::from_container(&index.reader);
    let ds_ref = &ds;
    let text_len = index.reader.n - 1;

    // the corpus, for optional per-match byte verification
    let corpus_file = corpus.as_ref().map(|p| {
        let f = std::fs::File::open(p)
            .unwrap_or_else(|e| search_die(&format!("open {p}: {e}")));
        let len = f.metadata().map(|m| m.len()).unwrap_or_else(|e| search_die(&e.to_string()));
        if len < text_len {
            search_die(&format!(
                "corpus {p} is {len} bytes, shorter than the container's text {text_len}"
            ));
        }
        f
    });

    // the patterns: newline-delimited bytes from the file, plus the one
    // --pattern if both were given (id order: --pattern first)
    let mut pats: Vec<Vec<u8>> = Vec::new();
    if let Some(p) = &pattern {
        pats.push(p.as_bytes().to_vec());
    }
    if let Some(path) = &patterns {
        let f = std::fs::File::open(path)
            .unwrap_or_else(|e| search_die(&format!("open {path}: {e}")));
        for line in std::io::BufReader::new(f).lines() {
            let line = line.unwrap_or_else(|e| search_die(&format!("read {path}: {e}")));
            if hex {
                let h = line.trim();
                if h.len() % 2 != 0 {
                    search_die(&format!("{path}: a --hex pattern line has an odd number of digits"));
                }
                let mut v = Vec::with_capacity(h.len() / 2);
                for k in (0..h.len()).step_by(2) {
                    let b = u8::from_str_radix(&h[k..k + 2], 16)
                        .unwrap_or_else(|_| search_die(&format!("{path}: bad hex digit")));
                    v.push(b);
                }
                pats.push(v);
            } else {
                pats.push(line.into_bytes());
            }
        }
        // a trailing newline must not invent an empty pattern
        if pats.last().is_some_and(|p| p.is_empty()) && pattern.is_none() && pats.len() > 1 {
            pats.pop();
        }
    }
    if pats.is_empty() {
        search_die("no patterns given");
    }
    // NOTE: 0x1E is an ordinary corpus byte on the pile (the framing separator
    // belongs to the document that owns it); only the PATTERN FILE is
    // newline-delimited, so patterns containing newlines need --hex.

    let stdout = std::io::stdout();
    let mut out = std::io::BufWriter::new(stdout.lock());
    let mut excluded_total = 0u64;
    let mut kept_total = 0u64;
    let mut refused = 0u64;
    let mut absent = 0u64;
    let mut verified = 0u64;
    let sigma = &index.reader.sigma;
    let t0 = Instant::now();
    let mut cx = index.ctx();

    for (id, p) in pats.iter().enumerate() {
        if p.is_empty() || p.iter().any(|&b| sigma[b as usize] == 0) {
            absent += 1;
            continue;
        }
        let work: Vec<u8> = p.iter().rev().map(|&b| sigma[b as usize]).collect();
        let (l, r) = cx.search(&work);
        if r == l {
            absent += 1;
            continue;
        }
        if r - l > max_occ {
            refused += 1;
            eprintln!(
                "SEARCH_PATTERN_REFUSED id={} len={} occurrences={} max_occ={max_occ} \
                 note=\"refused, not truncated: a truncated list would corrupt the excluded-set semantics\"",
                id,
                p.len(),
                r - l
            );
            continue;
        }
        let plen = p.len() as u64;
        for row in l..r {
            let (pos, _) = cx.locate_steps(row);
            let last = pos + plen - 1;
            // THE EXACT PREDICATE: crossing <=> rank(pos) != rank(last)
            let crossing = !(last < ds_ref.text_len && ds_ref.rank_start(last).1 <= pos);
            if let Some(f) = &corpus_file {
                use std::os::unix::fs::FileExt as _;
                let mut buf = vec![0u8; plen as usize];
                f.read_exact_at(&mut buf, pos)
                    .unwrap_or_else(|e| search_die(&format!("corpus read at {pos}: {e}")));
                if buf != p[..] {
                    search_die(&format!(
                        "VERIFICATION FAILED at pattern {id} position {pos}: the corpus bytes \
                         there are not the pattern - the locate or the container is wrong"
                    ));
                }
                verified += 1;
            }
            if crossing && !cross_doc {
                excluded_total += 1;
                continue;
            }
            kept_total += 1;
            if json {
                writeln!(
                    out,
                    "{{\"id\":{},\"pos\":{},\"len\":{},\"cross\":{}}}",
                    id,
                    pos,
                    plen,
                    u8::from(crossing)
                )
                .unwrap_or_else(|e| search_die(&e.to_string()));
            } else {
                writeln!(out, "{}\t{}\t{}\t{}", id, pos, plen, u8::from(crossing))
                    .unwrap_or_else(|e| search_die(&e.to_string()));
            }
        }
    }
    out.flush().unwrap_or_else(|e| search_die(&e.to_string()));
    eprintln!(
        "SEARCH_BOUNDARY mode={} structure={} patterns={} absent={} refused={} \
         excluded_cross_doc={excluded_total} matches_kept={kept_total} verified={verified} \
         wall_s={:.3} law=no_match_may_cross_a_document_boundary",
        if cross_doc { "cross-doc" } else { "no-cross-default" },
        "member-m15",
        pats.len(),
        absent,
        refused,
        t0.elapsed().as_secs_f64(),
    );
}
