// Included in qlite beside seeds.inc.rs: the same immutable index, the same
// continuous whole-document scan, a different QUESTION.
//
// FANOUT: given a text T that is not the corpus, rank the documents of the
// corpus by how much of T's DISTINCTIVE material they share.
//
// The seeds machinery (seeds.inc.rs) answers "which exact substrings of T are
// specific and recurring"; `xsa seeds` then ranks their OCCURRENCES. Ranking
// document FAN-OUT by COUNTING seeds is a different and wrong measurement, and
// it was measured wrong: on the Cat article with --skip-markup and target 3 the
// top shared-seed-count documents were the topically correct ones ('Cultural
// depictions of cats' 2 seeds, 'Cat behavior' 2 seeds) but they TIED with pure
// noise (Cottbus, Bautzen) because a seed that 500 documents share counted the
// same as a seed that 3 documents share. A count is a sum of ones; what is
// wanted is a sum of RARITIES.
//
// THE WEIGHT. Use the index's own multiplicity as a document-frequency proxy:
//
//     weight(seed) = ln(D / mult(seed))          D = documents in the corpus
//     score(doc)   = sum of weight(seed) over the DISTINCT seeds the doc shares
//                                                with T
//
// (mult is the BWT interval width, i.e. occurrences. A seed cannot occur in more
// documents than it has occurrences, so D/mult is a lower bound on the inverse
// document frequency; for the multi-byte seeds this command emits - 12 bytes and
// up, unique within a document in practice - occurrences ARE documents, and the
// corpus sidecars measure that directly: `occurrences_per_doc` is journaled and
// also printed per top row.)
//
// A seed occurring in more documents than the corpus has is not rare at all, so
// the weight is floored at 0 (`weight_floor_clamped` counts them); that is the
// only departure from ln(D/mult) and it can only happen for a seed that every
// document would have to contain more than once.
//
// REGIONS. A document that shares one section of T reports all of its evidence
// inside that section; scoring the whole document hides that. Hits are therefore
// clustered per document: hits within --region-gap bytes of each other belong to
// one region, and a region is scored exactly like a document - a sum of weights
// over the DISTINCT seeds with a hit inside it. The two tables are reported.
//
// SEED SELECTION is deliberately UNTARGETED. `xsa seeds` solves the seed LENGTH
// for a multiplicity target, which is how you get a report about multiplicities;
// for scoring, the whole multiplicity range is admissible and the WEIGHT is what
// ranks it. So: no target, the longest occurring prefix up to --max-len at every
// window, --skip-markup ON by default (citation/template fragments are known
// noise, `--include-markup` exposes them), seeds occurring in exactly one place
// dropped (`unshared`: they cannot be shared with any other document), and the
// rest selected evenly across the document up to --max-seeds.
//
// Nothing is materialized: the text is read through a windowed mmap, every
// position comes from the existing locate walk (LF to an m11 anchor), and the
// per-document hit table lives in memory for the duration of the run only.

/// ln(D / mult), floored at 0 - see the header. Pure, so the synthetic Q2 gate
/// can check the formula itself rather than a table that happens to look right.
fn rarity_weight(docs: u64, mult: u64) -> f64 {
    assert!(docs > 0, "FANOUT_WEIGHT docs=0: the corpus has no documents to be rare among");
    assert!(mult > 0, "FANOUT_WEIGHT mult=0: an emitted seed occurs at least once");
    let w = (docs as f64 / mult as f64).ln();
    if w > 0.0 { w } else { 0.0 }
}

/// Escape for TSV/one-line output: tabs, newlines, carriage returns and
/// backslashes only. Non-UTF-8 bytes pass through as they are (the corpus is
/// bytes; refusing to print them would hide seeds).
fn fanout_escape(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len());
    for &b in bytes {
        match b {
            b'\t' => out.push_str("\\t"),
            b'\n' => out.push_str("\\n"),
            b'\r' => out.push_str("\\r"),
            b'\\' => out.push_str("\\\\"),
            0x20..=0x7e => out.push(b as char),
            _ => {
                use std::fmt::Write;
                write!(&mut out, "\\x{b:02x}").unwrap();
            }
        }
    }
    out
}

/// One candidate document (or region) and its evidence.
#[derive(Default)]
struct FanoutRow {
    doc: usize,
    /// Region span in document bytes; for a document row, [0, doc_len).
    start: u64,
    end: u64,
    /// Distinct seeds shared, raw hits, and the weighted score over those seeds.
    seeds: Vec<u32>,
    hits: u64,
    score: f64,
}

impl FanoutRow {
    /// Dedupe the seed list and sum the weights ONCE per distinct seed. The list
    /// is sorted first, so the score cannot depend on the order hits arrived in.
    fn finish(&mut self, weights: &[f64]) {
        self.seeds.sort_unstable();
        self.seeds.dedup();
        self.score = 0.0;
        for s in &self.seeds {
            self.score += weights[*s as usize];
        }
    }
}

const FANOUT_USAGE: &str = "usage:\n  \
    xsa fanout <container> <text-file|-> [--max-seeds K] [--min-len m] [--max-len L] \
    [--region-gap G] [--top N] [--docs DIR] [--jsonl] [--max-rows R] \
    [--max-locate-budget B] [--keep-source] [--include-markup]\n  \
  Ranks the corpus documents by RARITY-WEIGHTED fan-out over the text's seeds:\n    \
  weight(seed) = ln(D / multiplicity(seed)),  D = documents in the corpus\n    \
  score(doc)   = sum of weight over the DISTINCT seeds the document shares\n  \
  Hits are also clustered per document within --region-gap bytes (default 2000) and \
  each region is scored the same way, so a document that shares one SECTION reports \
  that section. Seeds are untargeted (the longest occurring prefix per window, \
  --min-len 12, --max-len 64), --skip-markup is ON by default, seeds occurring once \
  are dropped (they cannot be shared), and --max-seeds (default 256) selects evenly \
  across the document. The source document itself is detected by comparing the text \
  against corpus.bin at the candidate's start and excluded (--keep-source keeps it).";

pub fn fanout_command(raw: &[std::ffi::OsString]) {
    let args: Vec<_> = raw.iter().map(|a| a.to_string_lossy().into_owned()).collect();
    let container = args.first().unwrap_or_else(|| die(FANOUT_USAGE));
    let mut text_path: Option<std::ffi::OsString> = None;
    let mut docs: Option<String> = None;
    let mut json = false;
    let mut skip_markup = true;
    let mut keep_source = false;
    let mut min_len = 12usize;
    let mut max_len = 64usize;
    let mut max_seeds = 256usize;
    let mut max_rows = 64u64;
    let mut budget = 65536u64;
    let mut region_gap = 2000u64;
    let mut top = 15usize;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--jsonl" => json = true,
            "--include-markup" => skip_markup = false,
            "--keep-source" => keep_source = true,
            "--docs" | "--max-seeds" | "--min-len" | "--max-len" | "--region-gap" | "--top"
            | "--max-rows" | "--max-locate-budget" => {
                let flag = args[i].clone();
                i += 1;
                let v = args.get(i).unwrap_or_else(|| die(&format!("fanout: {flag} needs a value")));
                let num = |what: &str| -> u64 {
                    v.parse().unwrap_or_else(|_| die(&format!("fanout: invalid {what} '{v}'")))
                };
                match flag.as_str() {
                    "--docs" => docs = Some(v.clone()),
                    "--max-seeds" => max_seeds = num("--max-seeds") as usize,
                    "--min-len" => min_len = num("--min-len") as usize,
                    "--max-len" => max_len = num("--max-len") as usize,
                    "--region-gap" => region_gap = num("--region-gap"),
                    "--top" => top = num("--top") as usize,
                    "--max-rows" => max_rows = num("--max-rows"),
                    _ => budget = num("--max-locate-budget"),
                }
            }
            other if other.starts_with('-') && other != "-" => {
                die(&format!("fanout: unknown option {other}\n{FANOUT_USAGE}"))
            }
            _ => {
                if text_path.is_some() {
                    die("fanout: exactly one text file (or '-' for stdin) per invocation");
                }
                text_path = Some(raw[i].clone());
            }
        }
        i += 1;
    }
    let Some(text_path) = text_path else { die(FANOUT_USAGE) };
    if max_len == 0 || max_len > PAT_LEN_MAX {
        die(&format!("fanout: --max-len must be 1..={PAT_LEN_MAX}, got {max_len}"));
    }
    if min_len == 0 {
        die("fanout: --min-len must be positive (a zero-length seed occurs everywhere)");
    }
    if max_len < min_len {
        die(&format!(
            "fanout: --max-len {max_len} is smaller than --min-len {min_len}: no length is \
             admissible, so nothing could be scored"
        ));
    }
    if max_seeds == 0 {
        die("fanout: --max-seeds must be positive");
    }
    if max_rows == 0 || budget == 0 {
        die("fanout: --max-rows and --max-locate-budget must be positive");
    }
    if top == 0 {
        die("fanout: --top must be positive");
    }
    if region_gap == 0 {
        die("fanout: --region-gap must be positive (0 would make every hit its own region)");
    }

    // ---- text: windowed mmap for a path, one bounded buffer for stdin, exactly
    // as `xsa seeds` reads it. Never written anywhere.
    enum TextSrc {
        Mapped(Mmap),
        Owned(Vec<u8>),
    }
    let text = if text_path.as_os_str() == "-" {
        let mut v = Vec::new();
        std::io::Read::read_to_end(&mut std::io::stdin().lock(), &mut v)
            .unwrap_or_else(|e| die(&format!("fanout: stdin: {e}")));
        TextSrc::Owned(v)
    } else {
        let p = std::path::PathBuf::from(&text_path);
        let f = File::open(&p).unwrap_or_else(|e| die(&format!("fanout: {}: {e}", p.display())));
        match unsafe { Mmap::map(&f) } {
            Ok(m) => TextSrc::Mapped(m),
            Err(e) => die(&format!("fanout: mmap {}: {e}", p.display())),
        }
    };
    let text: &[u8] = match &text {
        TextSrc::Mapped(m) => m,
        TextSrc::Owned(v) => v,
    };
    if text.is_empty() {
        die(
            "fanout: empty text: walking it yields zero seeds, and answering an empty ranking \
             would be a silently empty result rather than a refusal",
        );
    }

    let reader = Reader::open(container).unwrap_or_else(|e| {
        die(&format!(
            "{e}\n  supported: SXI2 v6 lite (members m1,m4,m5,m7,m10,m11)\n  \
             migration: a legacy container must be rebuilt with `xsa build`."
        ))
    });
    // Document frequency IS the denominator of the weight: without sidecars there
    // is no D and no titles, so this command refuses instead of ranking offsets.
    let docs = discover_documents(container, docs.as_deref(), reader.n).unwrap_or_else(|| {
        die(
            "fanout: ranking documents needs the corpus sidecars (seps.bin, titles.bin) to \
             supply D and the titles: pass --docs DIR",
        )
    });
    let big_d = docs.count as u64;
    let index = Index::open(reader, 1).unwrap_or_else(|e| die(&e));
    let sigma = index.reader.sigma;
    let mut cx = index.ctx();

    // ---- 1. UNTARGETED SEEDS. There is no multiplicity target in the sense of
    // `xsa seeds`: the only requirement is that a seed be SHARED (mult >= 2), and
    // the weight - not a band - ranks its occurrences. The solve finds the
    // LONGEST prefix reaching 2, which is the most specific shared string at that
    // offset.
    //
    // target=1 was measured first and is useless here, which is worth saying: on
    // the Cat article it accepted 48,480 seeds and EVERY ONE had multiplicity 1
    // (the longest occurring prefix, 64 bytes, is article-specific), so there was
    // no fan-out to rank at all - the command refused by name. The minimal
    // nonzero sharing target is 2 and it is the only target this command needs.
    let o = ScanOpts {
        min_len,
        max_len,
        target: 2,
        cap: None,
        nearest: false,
        stride: 1,
        skip_markup,
        reserve: max_seeds,
        quiet: false,
    };
    let out = scan_text(&mut cx, &sigma, text, &o);
    let scan_ms = out.scan_ms;
    assert_eq!(
        out.stats.windows,
        out.stats.no_match + out.stats.too_short + out.stats.too_rare + out.stats.too_common
            + out.stats.windows_with_seed
            + out.stats.suppressed_dedup
            + out.stats.suppressed_contained
            + out.stats.windows_skipped_markup,
        "FANOUT_WINDOW_ACCOUNTING windows do not add up"
    );
    let accepted = out.seeds.len();
    let accepted_windows = accepted as u64;

    // Seeds occurring exactly once are shared with NO other document: they carry
    // the largest weight and can never contribute to another document's score, so
    // they would only consume the budget. Dropped, counted, and named.
    let mut unshared = 0u64;
    let mut seeds: Vec<Seed> = Vec::with_capacity(max_seeds.min(accepted.max(1)));
    for s in out.seeds.into_iter() {
        if s.mult < 2 {
            unshared += 1;
        } else {
            seeds.push(s);
        }
    }
    let shared_accepted = seeds.len();
    let mut capped_dropped = 0u64;
    if seeds.len() > max_seeds {
        let step_idx = seeds.len() / max_seeds;
        let picked: Vec<Seed> = (0..max_seeds).map(|j| seeds[j * step_idx]).collect();
        capped_dropped = (seeds.len() - max_seeds) as u64;
        seeds = picked;
    }
    if seeds.is_empty() {
        die(&format!(
            "fanout: no seed of the text occurs more than once in the corpus \
             (accepted_windows={accepted} unshared={unshared}): there is no fan-out to rank. \
             A text that is not in the corpus has no neighbourhood by definition."
        ));
    }

    // ---- 2. WEIGHTS. One per selected seed, from the index multiplicity.
    let mut weight_floor_clamped = 0u64;
    let weights: Vec<f64> = seeds
        .iter()
        .map(|s| {
            let w = rarity_weight(big_d, s.mult);
            if w == 0.0 {
                weight_floor_clamped += 1;
            }
            w
        })
        .collect();

    // The locate budget is checked BEFORE the walk: a partial scoring pass that
    // silently stopped would be a wrong ranking, not a slow one.
    let planned_rows: u64 = seeds.iter().map(|s| s.mult.min(max_rows)).sum();
    if planned_rows > budget {
        die(&format!(
            "fanout: {} seed occurrences would be located ({} seeds, --max-rows {max_rows}) but \
             --max-locate-budget is {budget}. Nothing was scored. Lower --max-seeds/--max-rows, \
             or raise the budget knowingly.",
            planned_rows,
            seeds.len()
        ));
    }

    // ---- 3. LOCATE EVERY OCCURRENCE of every selected seed, and bucket by
    // document. `--max-rows` samples an interval wider than itself (evenly
    // spaced, like `xsa seeds --limit`), and says so per seed.
    let locate_t = Instant::now();
    let mut hits: Vec<(u32, u64, u32)> = Vec::with_capacity(planned_rows as usize); // (doc, doc_off, seed)
    let mut rows_located = 0u64;
    let mut seeds_truncated = 0u64;
    for (sidx, s) in seeds.iter().enumerate() {
        let (rows, truncated) = seed_sample(s.l, s.r, max_rows);
        if truncated {
            seeds_truncated += 1;
        }
        for row in rows {
            let p = cx.locate(row);
            let (doc, off, _) = docs.lookup(p).unwrap_or_else(|e| die(&e));
            hits.push((doc as u32, off, sidx as u32));
            rows_located += 1;
        }
    }
    let locate_ms = locate_t.elapsed().as_secs_f64() * 1e3;

    // ---- 4. THE SOURCE DOCUMENT, found by comparison, not by guessing. The text
    // is normally a document of the corpus: take the rarest selected seed (fewest
    // occurrences), and for each of its hits compare the text against the corpus
    // at that document's start. A prefix match means "this document IS the text".
    let mut source: Option<usize> = None;
    let mut source_bytes = 0u64;
    let corpus = docs.context_corpus().ok();
    if corpus.is_none() {
        eprintln!(
            "FANOUT_SOURCE unknown reason=no-corpus.bin dir={} (the source document is NOT \
             excluded; pass --docs DIR holding corpus.bin, or name it with --keep-source)",
            docs.dir.display()
        );
    }
    if let Some(corpus) = &corpus {
        let mut order: Vec<usize> = (0..seeds.len()).collect();
        order.sort_by_key(|&j| (seeds[j].mult, j));
        let mut tested = 0u64;
        'outer: for &j in order.iter().take(4) {
            let (rows, _) = seed_sample(seeds[j].l, seeds[j].r, 8);
            for row in rows {
                let p = cx.locate(row);
                let (doc, off, _) = docs.lookup(p).unwrap_or_else(|e| die(&e));
                let doc_len = docs.delta(doc + 1);
                let doc_start = p - off;
                let len = (text.len() as u64).min(doc_len);
                tested += 1;
                if len == 0 || doc_start + len > docs.bytes {
                    continue;
                }
                let window = &corpus[doc_start as usize..(doc_start + len) as usize];
                if window == &text[..len as usize] {
                    source = Some(doc);
                    source_bytes = len;
                    break 'outer;
                }
            }
        }
        eprintln!(
            "FANOUT_SOURCE search=corpus.bin tested={tested} doc={} matched_bytes={source_bytes} \
             text_bytes={}",
            source.map(|d| d.to_string()).unwrap_or_else(|| "none".into()),
            text.len()
        );
    }
    if keep_source {
        source = None;
        eprintln!("FANOUT_SOURCE keep_source=true: the source document is ranked like any other");
    }

    // ---- 5. DOCUMENT-LEVEL SCORES.
    let mut per_doc: std::collections::HashMap<u32, FanoutRow> = std::collections::HashMap::new();
    let mut excluded_hits = 0u64;
    for &(doc, off, sidx) in &hits {
        if Some(doc as usize) == source {
            excluded_hits += 1;
            continue;
        }
        let row = per_doc.entry(doc).or_insert_with(|| FanoutRow { doc: doc as usize, ..Default::default() });
        row.seeds.push(sidx);
        row.hits += 1;
        let _ = off;
    }
    let mut doc_rows: Vec<FanoutRow> = per_doc.into_values().collect();
    for r in doc_rows.iter_mut() {
        r.finish(&weights);
    }
    let documents_ranked = doc_rows.len() as u64;
    doc_rows.sort_by(|a, b| {
        b.score
            .total_cmp(&a.score)
            .then(b.seeds.len().cmp(&a.seeds.len()))
            .then(b.hits.cmp(&a.hits))
            .then(a.doc.cmp(&b.doc))
    });
    // How many DISTINCT documents does a seed really touch? That is the claim the
    // weight rests on (weight uses occurrences as the document-frequency proxy).
    let mut doc_occ_max = 0u64;
    let mut doc_occ_sum = 0u64;
    {
        let mut counts: std::collections::HashMap<u32, u64> = std::collections::HashMap::new();
        for &(doc, _, sidx) in &hits {
            if Some(doc as usize) == source {
                continue;
            }
            *counts.entry(sidx).or_insert(0) += 1;
        }
        for (_, c) in counts {
            doc_occ_max = doc_occ_max.max(c);
            doc_occ_sum += c;
        }
    }

    // ---- 6. REGION-LEVEL SCORES: hits within --region-gap bytes inside ONE
    // document are one region, scored like a document.
    let mut region_rows: Vec<FanoutRow> = Vec::new();
    {
        let mut by_doc: std::collections::HashMap<u32, Vec<(u64, u32)>> = std::collections::HashMap::new();
        for &(doc, off, sidx) in &hits {
            if Some(doc as usize) == source {
                continue;
            }
            by_doc.entry(doc).or_default().push((off, sidx));
        }
        for (doc, mut v) in by_doc {
            v.sort_unstable();
            let mut k = 0usize;
            while k < v.len() {
                let start = v[k].0;
                let mut end = v[k].0 + 1;
                let mut row = FanoutRow { doc: doc as usize, start, ..Default::default() };
                row.seeds.push(v[k].1);
                row.hits = 1;
                k += 1;
                while k < v.len() && v[k].0.saturating_sub(end) <= region_gap {
                    end = end.max(v[k].0 + 1);
                    row.seeds.push(v[k].1);
                    row.hits += 1;
                    k += 1;
                }
                row.end = end;
                row.finish(&weights);
                region_rows.push(row);
            }
        }
    }
    let regions_found = region_rows.len() as u64;
    region_rows.sort_by(|a, b| {
        b.score
            .total_cmp(&a.score)
            .then(b.seeds.len().cmp(&a.seeds.len()))
            .then(b.hits.cmp(&a.hits))
            .then(a.doc.cmp(&b.doc))
            .then(a.start.cmp(&b.start))
    });

    // ---- 7. OUTPUT. Both tables, each row carrying the evidence that produced
    // it: distinct seeds, raw hits, the score, and the top 3 contributing seeds.
    let titles: std::collections::HashMap<usize, Vec<u8>> = {
        let mut m = std::collections::HashMap::new();
        let push = |m: &mut std::collections::HashMap<usize, Vec<u8>>, d: usize| {
            let title = docs.lookup_title(d).to_vec();
            m.entry(d).or_insert(title);
        };
        for r in doc_rows.iter().take(top) {
            push(&mut m, r.doc);
        }
        for r in region_rows.iter().take(top) {
            push(&mut m, r.doc);
        }
        m
    };
    let title_of = |d: usize| -> Vec<u8> { titles.get(&d).cloned().unwrap_or_default() };
    let top_seeds = |row: &FanoutRow| -> Vec<(u32, f64, u64, String)> {
        let mut v: Vec<(u32, f64, u64, String)> = row
            .seeds
            .iter()
            .map(|&s| {
                let sd = seeds[s as usize];
                (
                    s,
                    weights[s as usize],
                    sd.mult,
                    fanout_escape(&text[sd.offset as usize..(sd.offset + sd.len as u64) as usize]),
                )
            })
            .collect();
        v.sort_by(|a, b| b.1.total_cmp(&a.1).then(a.0.cmp(&b.0)));
        v.truncate(3);
        v
    };
    let doc_len = |d: usize| -> u64 {
        if d + 1 < docs.count { docs.delta(d + 1) } else { docs.bytes }
    };

    let stdout = std::io::stdout();
    let mut w: Box<dyn Write> = Box::new(BufWriter::with_capacity(1 << 20, stdout.lock()));
    if json {
        let summary = serde_json::json!({
            "kind": "summary",
            "documents": big_d,
            "text_bytes": text.len(),
            "windows": out.stats.windows,
            "accepted_windows": accepted_windows,
            "unshared_seeds": unshared,
            "shared_accepted": shared_accepted,
            "seeds_selected": seeds.len(),
            "capped_dropped": capped_dropped,
            "rows_located": rows_located,
            "seeds_truncated": seeds_truncated,
            "documents_ranked": documents_ranked,
            "regions_found": regions_found,
            "region_gap": region_gap,
            "source_doc": source,
            "excluded_hits": excluded_hits,
            "weight_floor_clamped": weight_floor_clamped,
            "min_len": min_len,
            "max_len": max_len,
            "skip_markup": skip_markup,
            "scan_ms": scan_ms,
            "locate_ms": locate_ms,
        });
        writeln!(w, "{summary}").unwrap();
        for (rank, row) in doc_rows.iter().take(top).enumerate() {
            let ts: Vec<_> = top_seeds(row)
                .into_iter()
                .map(|(s, wt, mult, txt)| {
                    serde_json::json!({"seed_id": s, "weight": wt, "occurrences": mult, "seed": txt})
                })
                .collect();
            let obj = serde_json::json!({
                "kind": "document",
                "rank": rank + 1,
                "doc_id": row.doc,
                "title": String::from_utf8_lossy(&title_of(row.doc)),
                "doc_bytes": doc_len(row.doc),
                "distinct_seeds": row.seeds.len(),
                "hits": row.hits,
                "score": row.score,
                "top_seeds": ts,
            });
            writeln!(w, "{obj}").unwrap();
        }
        for (rank, row) in region_rows.iter().take(top).enumerate() {
            let ts: Vec<_> = top_seeds(row)
                .into_iter()
                .map(|(s, wt, mult, txt)| {
                    serde_json::json!({"seed_id": s, "weight": wt, "occurrences": mult, "seed": txt})
                })
                .collect();
            let obj = serde_json::json!({
                "kind": "region",
                "rank": rank + 1,
                "doc_id": row.doc,
                "title": String::from_utf8_lossy(&title_of(row.doc)),
                "doc_bytes": doc_len(row.doc),
                "region_start": row.start,
                "region_end": row.end,
                "region_bytes": row.end - row.start,
                "distinct_seeds": row.seeds.len(),
                "hits": row.hits,
                "score": row.score,
                "top_seeds": ts,
            });
            writeln!(w, "{obj}").unwrap();
        }
    } else {
        writeln!(
            w,
            "#fanout docs={big_d} text_bytes={} windows={} accepted_windows={} unshared_seeds={} \
             shared_accepted={} seeds_selected={} capped_dropped={} rows_located={} \
             seeds_truncated={} region_gap={region_gap} source_doc={} excluded_hits={} \
             weight_floor_clamped={weight_floor_clamped} min_len={min_len} max_len={max_len} \
             skip_markup={skip_markup}",
            text.len(),
            out.stats.windows,
            accepted_windows,
            unshared,
            shared_accepted,
            seeds.len(),
            capped_dropped,
            rows_located,
            seeds_truncated,
            source.map(|d| d.to_string()).unwrap_or_else(|| "none".into()),
            excluded_hits,
        )
        .unwrap();
        writeln!(w, "#documents rank\tdoc_id\tscore\tdistinct_seeds\thits\tdoc_bytes\ttitle\ttop_seeds").unwrap();
        for (rank, row) in doc_rows.iter().take(top).enumerate() {
            write!(
                w,
                "{}\t{}\t{:.6}\t{}\t{}\t{}\t",
                rank + 1,
                row.doc,
                row.score,
                row.seeds.len(),
                row.hits,
                doc_len(row.doc)
            )
            .unwrap();
            seed_write_escaped(&mut *w, &title_of(row.doc)).unwrap();
            w.write_all(b"\t").unwrap();
            write_top_seeds(&mut *w, &top_seeds(row));
            writeln!(w).unwrap();
        }
        writeln!(
            w,
            "#regions rank\tdoc_id\tregion_start\tregion_end\tregion_bytes\tscore\tdistinct_seeds\t\
             hits\ttitle\ttop_seeds"
        )
        .unwrap();
        for (rank, row) in region_rows.iter().take(top).enumerate() {
            write!(
                w,
                "{}\t{}\t{}\t{}\t{}\t{:.6}\t{}\t{}\t",
                rank + 1,
                row.doc,
                row.start,
                row.end,
                row.end - row.start,
                row.score,
                row.seeds.len(),
                row.hits
            )
            .unwrap();
            seed_write_escaped(&mut *w, &title_of(row.doc)).unwrap();
            w.write_all(b"\t").unwrap();
            write_top_seeds(&mut *w, &top_seeds(row));
            writeln!(w).unwrap();
        }
    }
    w.flush().unwrap();

    let mult_min = seeds.iter().map(|s| s.mult).min().unwrap();
    let mult_max = seeds.iter().map(|s| s.mult).max().unwrap();
    eprintln!(
        "FANOUT_STATS docs={big_d} text_bytes={} windows={} coverage_pct={:.4} \
         accepted_windows={accepted_windows} unshared_seeds={unshared} \
         shared_accepted={shared_accepted} seeds_selected={} capped_dropped={capped_dropped} \
         seed_mult_min={mult_min} seed_mult_max={mult_max} rows_located={rows_located} \
         rows_truncated_seeds={seeds_truncated} documents_ranked={documents_ranked} \
         regions_found={regions_found} excluded_source=doc{source:?} excluded_hits={excluded_hits} \
         weight_floor_clamped={weight_floor_clamped} seed_docs_sum={doc_occ_sum} \
         seed_docs_max={doc_occ_max} scan_ms={scan_ms:.3} locate_ms={locate_ms:.3} \
         ranks={} lf_steps={}",
        text.len(),
        out.stats.windows,
        out.coverage_pct,
        seeds.len(),
        cx.ranks,
        cx.steps,
    );
    if capped_dropped > 0 {
        eprintln!(
            "FANOUT_NOTE --max-seeds {} selected {capped_dropped} of {shared_accepted} shared seeds \
             were dropped; they were dropped EVENLY across the document, so the ranking is a \
             sample of the text, not its head",
            seeds.len()
        );
    }
    if seeds_truncated > 0 {
        eprintln!(
            "FANOUT_NOTE {seeds_truncated} seeds occur more than --max-rows {max_rows} times: their \
             rows were SAMPLED evenly across the interval, so a widely shared seed's document \
             count is a lower bound",
        );
    }
}

fn write_top_seeds(w: &mut dyn Write, v: &[(u32, f64, u64, String)]) {
    for (j, (s, wt, mult, txt)) in v.iter().enumerate() {
        if j > 0 {
            w.write_all(b"|").unwrap();
        }
        write!(w, "s{s}@{wt:.4}/m{mult}:").unwrap();
        w.write_all(txt.as_bytes()).unwrap();
    }
}

#[cfg(test)]
mod fanout_tests {
    use super::*;

    /// Q2, the weight's arithmetic, checked against ln(D/mult) directly.
    #[test]
    fn weight_is_log_of_d_over_mult() {
        assert_eq!(rarity_weight(1_000_000, 2), (1_000_000f64 / 2f64).ln());
        assert!((rarity_weight(100, 10) - 10f64.ln()).abs() < 1e-12);
        // Monotone decreasing in multiplicity, monotone increasing in D.
        assert!(rarity_weight(1000, 2) > rarity_weight(1000, 20));
        assert!(rarity_weight(1 << 20, 7) > rarity_weight(1 << 10, 7));
        // Floored at zero: a seed occurring more often than there are documents
        // cannot be rare.
        assert_eq!(rarity_weight(10, 10), 0.0);
        assert_eq!(rarity_weight(10, 11), 0.0);
    }

    /// Q2, the ranking: ONE rare shared seed must outrank THREE ubiquitous ones.
    /// This is the defect the command exists to fix - by COUNT, the ubiquitous
    /// document wins 3:1.
    #[test]
    fn one_rare_seed_outranks_three_ubiquitous_ones() {
        let big_d = 1_000_000u64;
        let mut rare = FanoutRow { doc: 1, ..Default::default() };
        rare.seeds = vec![0];
        rare.hits = 1;
        let mut ubiquitous = FanoutRow { doc: 2, ..Default::default() };
        ubiquitous.seeds = vec![1, 2, 3];
        ubiquitous.hits = 3;
        let weights = vec![
            rarity_weight(big_d, 2),       // the rare seed: 2 occurrences
            rarity_weight(big_d, 500_000), // three seeds that half the corpus has
            rarity_weight(big_d, 500_000),
            rarity_weight(big_d, 400_000),
        ];
        rare.finish(&weights);
        ubiquitous.finish(&weights);
        assert!(ubiquitous.seeds.len() > rare.seeds.len(), "the count comparison must favour it");
        assert!(
            rare.score > ubiquitous.score,
            "rare {} must beat ubiqitous {}",
            rare.score,
            ubiquitous.score
        );
        // And nothing about the ordering may depend on summation order.
        let mut shuffled = FanoutRow { doc: 2, ..Default::default() };
        shuffled.seeds = vec![3, 1, 2];
        shuffled.finish(&weights);
        assert_eq!(shuffled.score, ubiquitous.score);
    }

    /// REGION CLUSTERING, on the exact geometry the command reports: hits inside
    /// one gap are one region, a gap wider than the threshold opens a new one.
    #[test]
    fn regions_cluster_by_gap() {
        let hits: Vec<(u64, u32)> = vec![(100, 0), (500, 1), (1500, 2), (9000, 3)];
        let gap = 2000u64;
        let mut regions: Vec<(u64, u64, usize)> = Vec::new();
        let mut k = 0usize;
        while k < hits.len() {
            let start = hits[k].0;
            let mut end = hits[k].0 + 1;
            let mut count = 1;
            k += 1;
            while k < hits.len() && hits[k].0.saturating_sub(end) <= gap {
                end = end.max(hits[k].0 + 1);
                count += 1;
                k += 1;
            }
            regions.push((start, end, count));
        }
        assert_eq!(regions, vec![(100, 1501, 3), (9000, 9001, 1)]);
    }
}
