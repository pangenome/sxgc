// Included in qlite to share the immutable index and per-worker navigation cache.
//
// SEEDS: adaptive exact-match seeding. Given a text T (an article or a fragment)
// that is NOT the corpus, emit substrings of T that are simultaneously SPECIFIC
// (long) and RECURRING (multiplicity inside a band). No embeddings, no
// tokenizer, no vector: the only primitive is the FM-index interval, i.e.
// backward search over the same v6 lite kernel `branches` and `mem` use.
//
// THE ONE MONOTONICITY THIS FILE IS BUILT ON. For a fixed text offset i, let
// `mult(i, m)` be the number of corpus occurrences of `T[i..i+m)`. Prefix
// occurrence is downward closed, so
//
//     mult(i, m) is NON-INCREASING in m:
//     m1 < m2  =>  mult(i, m1) >= mult(i, m2).
//
// (Every occurrence of `T[i..i+m2)` contains an occurrence of its prefix
// `T[i..i+m1)`, so the shorter string occurs at least as often. Widening a
// match can only lose occurrences.)
//
// Consequences, and why the code below looks the way it does:
//
//   * A SHORTENING loop guarded by `mult > M` can never satisfy its own guard:
//     shortening raises multiplicity. The specification's step 2 ("while
//     multiplicity(seed) > M: shorten one byte from the far end") has its
//     inequality inverted; the loop it describes diverges and would reject every
//     window in which it ran. What the band [min_mult, M] actually asks for is
//     the LONGEST seed whose multiplicity has come UP to min_mult without
//     overshooting M - i.e. shorten while mult < T (too specific), then refuse
//     the window if the achieved multiplicity is above the cap.
//
//   * Because the property is monotone, the length is SOLVED FOR, not searched
//     linearly: bisect m in [min_len, L0] for the longest m with
//     mult(i, m) >= T. That is O(log L) interval descents per window (each
//     descent is one rank step per byte), not an O(L^2) shortening walk.
//
//   * Every bisection probe is checked against the monotonicity it assumes
//     (SEEDS_MULT_NOT_MONOTONE): if the bracket ever contradicts itself, the
//     reader is wrong, not the text, and we abort.
//
// Two multiplicity modes share one core:
//
//   --target-mult T          (DEFAULT PATH) the fan-out you want; the length is
//                            solved for. Emit the longest seed with mult >= T
//                            (the closest approach to T from above); `--nearest`
//                            may instead emit the one-byte-shorter seed whose
//                            mult < T when that is closer to T in log space, and
//                            says which it chose.
//   --max-mult M [--min-mult m]  the band mode: T = min_mult (default 2) drives
//                            the same solve, then mult <= M is required. A band
//                            window whose achieved multiplicity exceeds M has NO
//                            length in band (shorter is more frequent still) and
//                            is counted `too_common`.
//
// Window outcomes are counted separately and exactly, because they mean
// different things: `no_match` (not even one byte of the window occurs),
// `too_short` (the longest occurring prefix is below --min-len, so there is no
// length to probe), `too_rare` (even at --min-len the multiplicity is below T:
// the window is more unique than the target), `too_common` (even at the longest
// occurring prefix the multiplicity is above the target/cap: the window cannot
// be pinned at this length).
//
// Nothing here is materialized to disc: the text is read through a windowed
// mmap (or one bounded buffer for stdin) and never written anywhere; there is no
// per-suffix position column, and the seed's sample positions come from the
// existing locate walk (LF to an m11 anchor) plus the document sidecars.

/// A seed: `text[offset..offset+len)`, occurring `mult` times at BWT rows [l,r).
/// `gave_up` is the length one byte LONGER than the emitted one, whose
/// multiplicity first fell below the target - the length at which the solve gave
/// up being specific enough. 0 means "the window itself ended there" (the
/// emitted length is the longest occurring prefix, so no longer prefix exists).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Seed {
    offset: u64,
    len: u32,
    mult: u64,
    l: u64,
    r: u64,
    gave_up: u32,
    gave_up_mult: u64,
}

/// Per-window outcome, counted by name. `Seed` carries the solved length.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Window {
    NoMatch,
    TooShort,
    TooRare,
    TooCommon,
    Seed { len: u32, mult: u64, gave_up: u32, gave_up_mult: u64 },
}

/// Exact accounting for one `xsa seeds` run. Every window lands in exactly one
/// counter, and `windows == no_match + too_short + too_rare + too_common +
/// windows_with_seed`, which is asserted before the journal line is printed.
#[derive(Default)]
struct SeedStats {
    windows: u64,
    no_match: u64,
    too_short: u64,
    too_rare: u64,
    too_common: u64,
    windows_with_seed: u64,
    accepted: u64,
    suppressed_dedup: u64,
    suppressed_contained: u64,
    /// Interval descents performed while solving the length (the "shorten"
    /// probes of the specification; one descent = `len` rank steps).
    shorten_steps: u64,
    /// Interval descents performed to find the longest occurring prefix.
    extend_steps: u64,
    lens: Vec<u32>,
    mults: Vec<u64>,
}

/// Occurrence count of `text[start..start+len)` from the interval, cached per
/// length. `cache[len]` is `None` until probed; width 0 means "does not occur".
/// Caching matters: the longest-prefix bisection and the multiplicity bisection
/// overlap, and a probe is O(len) rank steps.
fn seed_probe(
    cx: &mut Ctx<'_>,
    sigma: &[u8; 256],
    cache: &mut [Option<(u64, u64)>],
    text: &[u8],
    start: usize,
    len: usize,
    calls: &mut u64,
) -> (u64, u64) {
    if let Some(v) = cache[len] {
        return v;
    }
    *calls += 1;
    let n = cx.idx.reader.n;
    let (mut l, mut r) = (0u64, n);
    let mut k = start + len;
    let mut empty = false;
    while k > start {
        k -= 1;
        let (nl, nr) = extend_left(cx, l, r, sigma[text[k] as usize]);
        if nl == nr {
            empty = true;
            break;
        }
        l = nl;
        r = nr;
    }
    // (0,0) is an unambiguous empty marker: a real interval has width >= 1.
    let v = if empty { (0, 0) } else { (l, r) };
    cache[len] = Some(v);
    v
}

/// Solve one window starting at `start`.
///
/// `target` is the multiplicity the seed must reach; `cap` is the optional band
/// ceiling (a seed above it is not a seed); `nearest` may return the length one
/// byte past the solve when it is closer to `target` in log space.
///
/// TOO_COMMON has one precise meaning and it is NOT "the achieved multiplicity
/// overshot the target": a solve that SHORTENS can land on a length whose
/// multiplicity jumped well above the target (the ladder 12:336, 13:51, 14:38
/// with target 50 correctly answers 13 at 51). It means the window cannot be
/// made specific enough - the LONGEST occurring prefix is already above the
/// ceiling, so every admissible length is more frequent still. In target mode
/// the ceiling is the target itself (there is no length left to lengthen into);
/// in band mode it is the band's --max-mult.
fn seed_window(
    cx: &mut Ctx<'_>,
    sigma: &[u8; 256],
    cache: &mut [Option<(u64, u64)>],
    text: &[u8],
    start: usize,
    max_len: usize,
    min_len: usize,
    target: u64,
    cap: Option<u64>,
    nearest: bool,
    stats: &mut SeedStats,
    chosen_lo: &mut bool,
) -> Window {
    let lim = max_len.min(text.len() - start);
    // The empty string "occurs" at every boundary; the bisection below needs a
    // true lower anchor, and length 0 is that anchor without a rank step.
    cache[0] = Some((0, cx.idx.reader.n));
    // ---- 1. LONGEST OCCURRING PREFIX, by bisection on prefix occurrence.
    // occ(m) > 0 is downward closed in m, so the largest occurring m is found
    // in O(log lim) descents instead of a linear walk.
    let (mut lo, mut hi) = (0usize, lim);
    while lo < hi {
        let mid = (lo + hi + 1) / 2;
        let (l, r) = seed_probe(cx, sigma, cache, text, start, mid, &mut stats.extend_steps);
        assert!(l <= r, "SEEDS_INTERVAL_INVERTED len={mid} l={l} r={r}");
        if r > l {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    let l0 = lo;
    if l0 == 0 {
        return Window::NoMatch;
    }
    if l0 < min_len {
        return Window::TooShort;
    }
    // ---- 2. SOLVE THE LENGTH for the target multiplicity.
    // mult(i, m) is non-increasing in m, so Q(m) = (mult(i, m) >= target) is
    // true on a PREFIX of [min_len, l0] and false above it: bisect for the
    // longest true m. Probes are recorded and re-checked for monotonicity.
    let mut probes: Vec<(usize, u64)> = Vec::with_capacity(8);
    let mut probe = |cx: &mut Ctx<'_>, m: usize, steps: &mut u64| -> u64 {
        let (l, r) = seed_probe(cx, sigma, cache, text, start, m, steps);
        let w = r - l;
        probes.push((m, w));
        w
    };
    stats.shorten_steps += 1;
    let mut q_hi = probe(cx, l0, &mut stats.shorten_steps);
    if q_hi >= target {
        // The longest occurring prefix is already at or above the target: it is
        // the most specific seed available at this offset.
        let seed = Seed {
            offset: start as u64,
            len: l0 as u32,
            mult: q_hi,
            l: cache[l0].unwrap().0,
            r: cache[l0].unwrap().1,
            gave_up: 0,
            gave_up_mult: 0,
        };
        // The solve did not have to shorten: the window is at its own longest
        // occurring prefix, so an overshoot can only mean "too common to pin".
        return seed_finish(seed, target, cap.or(Some(target)), stats, probes);
    }
    // Q(l0) is false and Q(m) for small m is what we are looking for.
    let (mut lo, mut hi) = (min_len, l0);
    let mut q_lo = probe(cx, min_len, &mut stats.shorten_steps);
    if q_lo < target {
        // Even the shortest admissible seed is more unique than the target.
        check_monotone(&probes, target);
        return Window::TooRare;
    }
    while hi - lo > 1 {
        let mid = (lo + hi) / 2;
        let w = probe(cx, mid, &mut stats.shorten_steps);
        if w >= target {
            lo = mid;
            q_lo = w;
        } else {
            hi = mid;
            q_hi = w;
        }
    }
    check_monotone(&probes, target);
    // `lo` is the longest length reaching the target from above; `hi == lo+1` is
    // the length at which the solve gave up.
    let (pl, pr) = cache[lo].unwrap();
    let mut seed = Seed {
        offset: start as u64,
        len: lo as u32,
        mult: q_lo,
        l: pl,
        r: pr,
        gave_up: hi as u32,
        gave_up_mult: q_hi,
    };
    if nearest && q_hi > 0 {
        // Compare in log space: multiplicity spans orders of magnitude, so
        // "closer to T" means closer in ln(mult).
        let d_above = (q_lo as f64).ln() - (target as f64).ln();
        let d_below = (target as f64).ln() - (q_hi as f64).ln();
        if d_below < d_above {
            let (nl, nr) = cache[hi].unwrap();
            seed.len = hi as u32;
            seed.mult = q_hi;
            seed.l = nl;
            seed.r = nr;
            seed.gave_up = 0;
            seed.gave_up_mult = 0;
            *chosen_lo = true;
        }
    }
    seed_finish(seed, target, cap, stats, probes)
}

/// The band/target acceptance rule, applied to a solved seed. Kept in one place
/// so the target path and the band path cannot drift apart.
fn seed_finish(
    seed: Seed,
    target: u64,
    cap: Option<u64>,
    _stats: &mut SeedStats,
    probes: Vec<(usize, u64)>,
) -> Window {
    check_monotone(&probes, target);
    if let Some(m) = cap {
        if seed.mult > m {
            // Shorter is more frequent still, so NO length of this window is in
            // the band: not a seed, counted by name.
            return Window::TooCommon;
        }
    }
    assert!(
        seed.mult >= 1,
        "SEEDS_EMPTY_INTERVAL offset={} len={}",
        seed.offset,
        seed.len
    );
    Window::Seed {
        len: seed.len,
        mult: seed.mult,
        gave_up: seed.gave_up,
        gave_up_mult: seed.gave_up_mult,
    }
}

/// Assert the monotonicity the bisection assumes: across every probed length,
/// multiplicity must be NON-INCREASING in length. A contradiction is a reader
/// bug and aborts with both lengths named.
fn check_monotone(probes: &[(usize, u64)], target: u64) {
    if probes.len() < 2 {
        return;
    }
    let mut v = probes.to_vec();
    v.sort_unstable();
    for w in v.windows(2) {
        let ((m1, q1), (m2, q2)) = (w[0], w[1]);
        assert!(
            q1 >= q2,
            "SEEDS_MULT_NOT_MONOTONE len {m1} has mult {q1} but the longer len {m2} has mult {q2}\n  \
             (multiplicity must be non-increasing in length; target={target})"
        );
    }
}

/// Containment suppression and content dedup, in one place. Seeds arrive in
/// increasing text offset, so a new seed can only be contained in an EARLIER one
/// (an earlier start cannot be contained in a later interval).
fn seed_suppressed(
    emitted: &[(u64, u64)],
    seen: &std::collections::HashSet<Vec<u8>>,
    text: &[u8],
    offset: u64,
    len: u64,
) -> Option<&'static str> {
    let end = offset + len;
    for &(a, b) in emitted {
        if a <= offset && end <= b {
            return Some("contained");
        }
    }
    if seen.contains(&text[offset as usize..end as usize]) {
        return Some("dedup");
    }
    None
}

/// Escape only the TSV delimiters (as `mem`/`query` do for titles). Seed bytes
/// are arbitrary corpus bytes; the text is UTF-8 in practice but nothing here
/// assumes it.
fn seed_write_escaped(w: &mut dyn Write, bytes: &[u8]) -> std::io::Result<()> {
    for &b in bytes {
        match b {
            b'\t' => w.write_all(b"\\t"),
            b'\n' => w.write_all(b"\\n"),
            b'\r' => w.write_all(b"\\r"),
            b'\\' => w.write_all(b"\\\\"),
            _ => w.write_all(&[b]),
        }?;
    }
    Ok(())
}

/// Evenly spaced sample rows across [l,r): when the seed is more frequent than
/// `limit`, adjacent SA rows very often share a document, so the FIRST `limit`
/// rows would report one title. Spacing is deterministic (same input, same
/// rows) and documented in SERVING.md.
fn seed_sample(l: u64, r: u64, limit: u64) -> (Vec<u64>, bool) {
    let width = r - l;
    let take = width.min(limit);
    if take == 0 {
        return (Vec::new(), width > 0);
    }
    let step = width / take;
    let rows = (0..take).map(|j| l + j * step).collect();
    (rows, take < width)
}

pub fn seeds_command(raw: &[std::ffi::OsString]) {
    let args: Vec<_> = raw.iter().map(|a| a.to_string_lossy().into_owned()).collect();
    let container = args.first().unwrap_or_else(|| die(SEEDS_USAGE));
    let mut text_path: Option<std::ffi::OsString> = None;
    let mut docs: Option<String> = None;
    let mut json = false;
    let mut nearest = false;
    let mut max_len = 64usize;
    let mut min_len = 12usize;
    let mut max_seeds = 64usize;
    let mut stride: Option<usize> = None;
    let mut limit = 8u64;
    let mut target_mult: Option<u64> = None;
    let mut max_mult: Option<u64> = None;
    let mut min_mult: Option<u64> = None;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--jsonl" => json = true,
            "--nearest" => nearest = true,
            "--max-len" | "--min-len" | "--max-seeds" | "--stride" | "--limit" | "--docs"
            | "--target-mult" | "--max-mult" | "--min-mult" => {
                let flag = args[i].clone();
                i += 1;
                let v = args
                    .get(i)
                    .unwrap_or_else(|| die(&format!("seeds: {flag} needs a value")));
                let num = |what: &str| -> u64 {
                    v.parse().unwrap_or_else(|_| die(&format!("seeds: invalid {what} '{v}'")))
                };
                match flag.as_str() {
                    "--docs" => docs = Some(v.clone()),
                    "--max-len" => max_len = num("--max-len") as usize,
                    "--min-len" => min_len = num("--min-len") as usize,
                    "--max-seeds" => max_seeds = num("--max-seeds") as usize,
                    "--stride" => stride = Some(num("--stride") as usize),
                    "--limit" => limit = num("--limit"),
                    "--target-mult" => target_mult = Some(num("--target-mult")),
                    "--max-mult" => max_mult = Some(num("--max-mult")),
                    _ => min_mult = Some(num("--min-mult")),
                }
            }
            other if other.starts_with('-') && other != "-" => {
                die(&format!("seeds: unknown option {other}\n{SEEDS_USAGE}"))
            }
            _ => {
                if text_path.is_some() {
                    die("seeds: exactly one text file (or '-' for stdin) per invocation");
                }
                text_path = Some(raw[i].clone());
            }
        }
        i += 1;
    }
    let Some(text_path) = text_path else { die(SEEDS_USAGE) };
    // ---- guards, refused by name (S3).
    if max_len == 0 || max_len > PAT_LEN_MAX {
        die(&format!("seeds: --max-len must be 1..={PAT_LEN_MAX}, got {max_len}"));
    }
    if min_len == 0 {
        die("seeds: --min-len must be positive (a zero-length seed occurs everywhere)");
    }
    if max_len < min_len {
        die(&format!(
            "seeds: --max-len {max_len} is smaller than --min-len {min_len}: no length is \
             admissible, so the run could not emit anything"
        ));
    }
    if max_seeds == 0 {
        die("seeds: --max-seeds must be positive");
    }
    if target_mult.is_some() && (max_mult.is_some() || min_mult.is_some()) {
        die(
            "seeds: --target-mult and the band (--min-mult/--max-mult) are alternative modes; \
             give one. The band mode solves for --min-mult and refuses seeds above --max-mult",
        );
    }
    if max_mult.is_some() != min_mult.is_some() {
        die("seeds: band mode needs both --min-mult and --max-mult (or neither)");
    }
    let band = max_mult.is_some();
    let (target, cap) = if let (Some(mx), Some(mn)) = (max_mult, min_mult) {
        if mn < 1 || mn > mx {
            die(&format!("seeds: band needs 1 <= --min-mult {mn} <= --max-mult {mx}"));
        }
        (mn, Some(mx))
    } else {
        let t = target_mult.unwrap_or(50);
        if t == 0 {
            die("seeds: --target-mult must be positive");
        }
        (t, None)
    };
    let stride = stride.unwrap_or_else(|| (max_len / 2).max(1));
    if stride == 0 {
        die("seeds: --stride must be positive");
    }

    // ---- text: a windowed mmap for a path, one bounded buffer for stdin. Never
    // written anywhere; the text is read only at the window positions.
    enum TextSrc {
        Mapped(Mmap),
        Owned(Vec<u8>),
    }
    impl TextSrc {
        fn bytes(&self) -> &[u8] {
            match self {
                TextSrc::Mapped(m) => m,
                TextSrc::Owned(v) => v,
            }
        }
    }
    let from_stdin = text_path.as_os_str() == "-";
    let text = if from_stdin {
        let mut v = Vec::new();
        std::io::Read::read_to_end(&mut std::io::stdin().lock(), &mut v)
            .unwrap_or_else(|e| die(&format!("seeds: stdin: {e}")));
        TextSrc::Owned(v)
    } else {
        let p = std::path::PathBuf::from(&text_path);
        let f = File::open(&p).unwrap_or_else(|e| die(&format!("seeds: {}: {e}", p.display())));
        // An empty file is refused below, by name, on the bytes - the same guard
        // as stdin, so the two sources cannot disagree.
        match unsafe { Mmap::map(&f) } {
            Ok(m) => TextSrc::Mapped(m),
            Err(e) => die(&format!("seeds: mmap {}: {e}", p.display())),
        }
    };
    let text = text.bytes();
    if text.is_empty() {
        die(
            "seeds: empty text: walking it yields zero windows, and answering zero seeds would \
             be a silently empty result rather than a refusal",
        );
    }

    let reader = Reader::open(container).unwrap_or_else(|e| {
        die(&format!(
            "{e}\n  supported: SXI2 v6 lite (members m1,m4,m5,m7,m10,m11)\n  \
             migration: a legacy container must be rebuilt with `xsa build`."
        ))
    });
    let docs = discover_documents(container, docs.as_deref(), reader.n);
    if docs.is_none() && limit > 0 {
        eprintln!("SEEDS_NOTE docs=none: emitting plain corpus offsets");
    }
    let index = Index::open(reader, 1).unwrap_or_else(|e| die(&e));
    let sigma = index.reader.sigma;
    let n = index.n();
    let present: [bool; 256] = std::array::from_fn(|b| {
        let s = sigma[b] as usize;
        s != 0 && index.total[s] != 0
    });
    eprintln!(
        "SEEDS_READY n={n} present_bytes={} text_bytes={} mode={} target_mult={target} \
         cap={} min_len={min_len} max_len={max_len} stride={stride} max_seeds={max_seeds} \
         nearest={nearest}",
        present.iter().filter(|p| **p).count(),
        text.len(),
        if band { "band" } else { "target" },
        cap.map(|c| c.to_string()).unwrap_or_else(|| "none".into()),
    );
    // Identity is NOT required: a seed text is normally a FRAGMENT of the
    // corpus. Equal length is only worth a note, because that is usually the
    // corpus itself, where every window is its own longest match.
    if text_path.as_os_str() != "-" && text.len() as u64 == n.saturating_sub(1) {
        eprintln!("SEEDS_NOTE text length equals corpus_bytes: unexpected for a fragment");
    }

    let mut cx = index.ctx();
    let mut cache = vec![None; max_len.max(1) + 1];
    let mut stats = SeedStats::default();
    let mut seeds: Vec<Seed> = Vec::with_capacity(max_seeds.min(4096));
    let mut emitted: Vec<(u64, u64)> = Vec::with_capacity(max_seeds.min(4096));
    let mut seen: std::collections::HashSet<Vec<u8>> = std::collections::HashSet::new();
    let mut nearest_below = 0u64;
    let scan = Instant::now();
    let mut i = 0usize;
    while i < text.len() && stats.accepted < max_seeds as u64 {
        stats.windows += 1;
        // The longest-occurring-prefix bisection answers NoMatch on its own, so
        // no `present` pre-filter is needed here.
        let mut chosen_lo = false;
        let w = seed_window(
            &mut cx,
            &sigma,
            &mut cache,
            text,
            i,
            max_len,
            min_len,
            target,
            cap,
            nearest,
            &mut stats,
            &mut chosen_lo,
        );
        match w {
            Window::NoMatch => stats.no_match += 1,
            Window::TooShort => stats.too_short += 1,
            Window::TooRare => stats.too_rare += 1,
            Window::TooCommon => stats.too_common += 1,
            Window::Seed { len, mult, gave_up, gave_up_mult } => {
                let offset = i as u64;
                let (l, r) = cache[len as usize].unwrap();
                assert!(r - l == mult, "SEEDS_CACHE_MULT_MISMATCH offset={offset} len={len}");
                if chosen_lo {
                    // --nearest took the seed BELOW the target: said once per
                    // seed, so a below-target multiplicity can never look like a
                    // solver bug in the output.
                    nearest_below += 1;
                }
                match seed_suppressed(&emitted, &seen, text, offset, len as u64) {
                    Some("contained") => stats.suppressed_contained += 1,
                    Some(_) => stats.suppressed_dedup += 1,
                    None => {
                        stats.windows_with_seed += 1;
                        stats.accepted += 1;
                        stats.lens.push(len);
                        stats.mults.push(mult);
                        emitted.push((offset, offset + len as u64));
                        seen.insert(text[offset as usize..offset as usize + len as usize].to_vec());
                        seeds.push(Seed {
                            offset,
                            len,
                            mult,
                            l,
                            r,
                            gave_up,
                            gave_up_mult,
                        });
                    }
                }
            }
        }
        // Clear the per-window cache: lengths are absolute (window-relative), so
        // a stale entry would be a wrong answer, not a slow one.
        for c in cache.iter_mut() {
            *c = None;
        }
        i += stride;
    }
    let scan_ms = scan.elapsed().as_secs_f64() * 1e3;

    // ---- output + evidence positions.
    let stdout = std::io::stdout();
    let mut w: Box<dyn Write> = Box::new(BufWriter::with_capacity(1 << 20, stdout.lock()));
    let mut located = 0u64;
    let locate = Instant::now();
    for (sidx, s) in seeds.iter().enumerate() {
        let (rows, truncated) = seed_sample(s.l, s.r, limit);
        let mut positions = Vec::with_capacity(rows.len());
        let mut docinfo: Vec<(usize, u64, Vec<u8>)> = Vec::with_capacity(rows.len());
        for row in &rows {
            let p = cx.locate(*row);
            positions.push(p);
            if let Some(d) = &docs {
                let (d_id, off, title) = d.lookup(p).unwrap_or_else(|e| die(&e));
                docinfo.push((d_id, off, title.to_vec()));
            }
        }
        located += rows.len() as u64;
        let seed_bytes = &text[s.offset as usize..s.offset as usize + s.len as usize];
        if json {
            let mut obj = serde_json::json!({
                "seed_id": sidx,
                "text_offset": s.offset,
                "length": s.len,
                "occurrences": s.mult,
                "seed": String::from_utf8_lossy(seed_bytes),
                "taken": rows.len(),
                "truncated": truncated,
                "positions": positions,
                "mode": if band { "band" } else { "target" },
                "target_mult": target,
                "gave_up_len": if s.gave_up == 0 { serde_json::Value::Null } else { serde_json::json!(s.gave_up) },
                "gave_up_mult": if s.gave_up == 0 { serde_json::Value::Null } else { serde_json::json!(s.gave_up_mult) },
            });
            if let Some(d) = &docs {
                let list: Vec<_> = docinfo
                    .iter()
                    .zip(positions.iter())
                    .map(|((d_id, off, title), p)| {
                        serde_json::json!({
                            "doc_id": d_id,
                            "offset": off,
                            "title": String::from_utf8_lossy(title),
                            "pos": p,
                        })
                    })
                    .collect();
                obj["docs"] = serde_json::json!(list);
            }
            writeln!(w, "{obj}").unwrap();
        } else {
            // seed_id, text_offset, length, occurrences, seed, gave_up_len,
            // gave_up_mult, taken, truncated, positions, titles.
            write!(w, "{sidx}\t{}\t{}\t{}", s.offset, s.len, s.mult).unwrap();
            w.write_all(b"\t").unwrap();
            seed_write_escaped(&mut *w, seed_bytes).unwrap();
            write!(w, "\t{}\t{}\t{}\t{}", s.gave_up, s.gave_up_mult, rows.len(), truncated)
                .unwrap();
            for (j, p) in positions.iter().enumerate() {
                let sep = if j == 0 { '\t' } else { ',' };
                if let Some(_d) = &docs {
                    let (d_id, off, _) = &docinfo[j];
                    write!(w, "{sep}{d_id}:{off}:{p}").unwrap();
                } else {
                    write!(w, "{sep}{p}").unwrap();
                }
            }
            if docs.is_some() {
                w.write_all(b"\t").unwrap();
                for (j, (_, _, title)) in docinfo.iter().enumerate() {
                    if j > 0 {
                        w.write_all(b"|").unwrap();
                    }
                    seed_write_escaped(&mut *w, title).unwrap();
                }
            }
            writeln!(w).unwrap();
        }
    }
    let locate_ms = locate.elapsed().as_secs_f64() * 1e3;
    w.flush().unwrap();
    assert_eq!(
        stats.windows,
        stats.no_match + stats.too_short + stats.too_rare + stats.too_common
            + stats.windows_with_seed
            + stats.suppressed_dedup
            + stats.suppressed_contained,
        "SEEDS_WINDOW_ACCOUNTING windows do not add up"
    );
    let mean_len = if stats.lens.is_empty() {
        0.0
    } else {
        stats.lens.iter().map(|l| *l as f64).sum::<f64>() / stats.lens.len() as f64
    };
    let mut hist: std::collections::BTreeMap<u64, u64> = std::collections::BTreeMap::new();
    for m in &stats.mults {
        *hist.entry(*m).or_insert(0) += 1;
    }
    let hist = hist
        .iter()
        .map(|(m, c)| format!("{m}:{c}"))
        .collect::<Vec<_>>()
        .join(",");
    eprintln!(
        "SEEDS_STATS windows={} tried={} accepted={} mean_len={mean_len:.2} \
         mult_histogram={} shorten_steps={} extend_steps={} scan_ms={scan_ms:.3} \
         locate_ms={locate_ms:.3} mode={} target_mult={target} cap={} min_len={min_len} \
         max_len={max_len} stride={stride} no_match={} too_short={} too_rare={} too_common={} \
         suppressed_dedup={} suppressed_contained={} nearest_below={nearest_below} positions={located} ranks={} lf_steps={}",
        stats.windows,
        stats.no_match + stats.too_short + stats.too_rare + stats.too_common
            + stats.windows_with_seed,
        stats.accepted,
        if hist.is_empty() { "-".into() } else { hist },
        stats.shorten_steps,
        stats.extend_steps,
        if band { "band" } else { "target" },
        cap.map(|c| c.to_string()).unwrap_or_else(|| "none".into()),
        stats.no_match,
        stats.too_short,
        stats.too_rare,
        stats.too_common,
        stats.suppressed_dedup,
        stats.suppressed_contained,
        cx.ranks,
        cx.steps,
    );
}

const SEEDS_USAGE: &str = "usage:\n  \
    xsa seeds <container> <text-file|-> [--target-mult T] [--max-len L] [--min-len m] \
    [--max-seeds K] [--stride S] [--docs DIR] [--limit N] [--jsonl] [--nearest]\n  \
    xsa seeds <container> <text-file|-> --max-mult M [--min-mult m] ...   (band mode)\n  \
  Emits exact substrings of the text that are SPECIFIC and RECURRING: for each window \
  start the seed LENGTH is solved so the multiplicity lands on --target-mult (default 50).\n  \
  --max-len default 64, --min-len default 12, --stride default max-len/2, --max-seeds \
  default 64, --limit default 8 sample positions.";

#[cfg(test)]
mod seeds_tests {
    use super::*;
    #[test]
    fn sample_rows_are_evenly_spaced_and_bounded() {
        // A zero-width interval cannot happen for an emitted seed (asserted at
        // the call site), and it samples nothing rather than claiming a truncation.
        let (r, trunc) = seed_sample(100, 100, 4);
        assert!(r.is_empty() && !trunc);
        let (r, trunc) = seed_sample(0, 10, 4);
        assert_eq!(r, vec![0, 2, 4, 6]);
        assert!(trunc);
        let (r, trunc) = seed_sample(5, 8, 8);
        assert_eq!(r, vec![5, 6, 7]);
        assert!(!trunc);
    }
    #[test]
    fn monotonicity_check_rejects_an_inverted_probe() {
        check_monotone(&[(10, 5), (20, 2), (30, 1)], 2);
        let r = std::panic::catch_unwind(|| check_monotone(&[(10, 1), (20, 4)], 2));
        assert!(r.is_err(), "an increasing multiplicity must abort");
    }
}
