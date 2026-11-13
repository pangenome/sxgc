// Included in qlite to share the immutable index and per-worker navigation cache.
//
// SEEDS: adaptive exact-match seeding. Given a text T (an article or a fragment)
// that is NOT the corpus, emit substrings of T that are simultaneously SPECIFIC
// (long) and RECURRING (multiplicity inside a band). No embeddings, no
// tokenizer, no vector: the only primitive is the FM-index interval, i.e.
// backward search over the same query kernel `branches` and `mem` use.
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
// WHERE THE WINDOWS ARE, and why. The scan is CONTINUOUS by default: one window
// per byte of the text, `windows == text bytes`, `coverage_pct == 100`. An
// earlier revision walked a fixed stride from offset 0 and stopped the moment it
// had `--max-seeds` accepted seeds, so on a 177 KB article it examined 62 windows
// of 32 bytes - 1.1% of the document, all of it the lead section, where Wikipedia
// keeps its infobox/redirect/date templates. Seeds such as `{{About|the speci`
// were the honest output of that scan. A front-to-back walk that stops at K can
// only ever answer with the head of the document, so:
//
//   * the scan covers the WHOLE text (every position; `--stride S` is an opt-in
//     sampler for the old behaviour and is journaled as such),
//   * `--max-seeds K` is an OUTPUT cap, applied after the scan by walking the
//     accepted seeds at even index spacing, so the emitted set still covers the
//     document instead of being the first K found,
//   * identical content is deduplicated and an accepted seed contained in an
//     earlier one is dropped (continuous starts make both routine),
//   * `--skip-markup` (OFF by default) refuses windows overlapping Wikipedia
//     scaffolding, which recurs a few hundred times by construction and would
//     otherwise win the multiplicity target on its own.
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
    /// Windows refused because they OVERLAP a markup range (`--skip-markup`).
    windows_skipped_markup: u64,
    /// Seeds actually emitted (after the `--max-seeds` output selection).
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
/// (an earlier start cannot be contained in a later interval), and the test
/// collapses to `end <= max_end` where `max_end` is the running maximum of the
/// accepted intervals' far ends. That keeps a CONTINUOUS scan (one window per
/// byte) linear: without it, every window would walk every earlier seed, and a
/// contiguous run of identical text would emit the same seed at every offset.
fn seed_suppressed(
    max_end: u64,
    seen: &std::collections::HashSet<Vec<u8>>,
    text: &[u8],
    offset: u64,
    len: u64,
) -> Option<&'static str> {
    let end = offset + len;
    if end <= max_end {
        return Some("contained");
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

/// `hay[from..]` searched for `needle`, byte-exact. Linear, and only ever called
/// on the (comparatively rare) construct boundaries, never per window.
fn find_seq(hay: &[u8], needle: &[u8], from: usize) -> Option<usize> {
    if needle.is_empty() || hay.len() < needle.len() {
        return None;
    }
    let last = hay.len() - needle.len();
    let mut i = from;
    while i <= last {
        if &hay[i..i + needle.len()] == needle {
            return Some(i);
        }
        i += 1;
    }
    None
}

/// `find_seq` with ASCII case folding, for closing tags (`</REF>` exists).
fn find_seq_ci(hay: &[u8], needle_lower: &[u8], from: usize) -> Option<usize> {
    if needle_lower.is_empty() || hay.len() < needle_lower.len() {
        return None;
    }
    let last = hay.len() - needle_lower.len();
    let mut i = from;
    while i <= last {
        if hay[i..i + needle_lower.len()]
            .iter()
            .zip(needle_lower)
            .all(|(a, b)| a.to_ascii_lowercase() == *b)
        {
            return Some(i);
        }
        i += 1;
    }
    None
}

fn starts_seq(hay: &[u8], i: usize, s: &[u8]) -> bool {
    hay.len() >= i + s.len() && &hay[i..i + s.len()] == s
}

fn starts_seq_ci(hay: &[u8], i: usize, s_lower: &[u8]) -> bool {
    hay.len() >= i + s_lower.len()
        && hay[i..i + s_lower.len()]
            .iter()
            .zip(s_lower)
            .all(|(a, b)| a.to_ascii_lowercase() == *b)
}

/// A tag name must end at a delimiter, so `<reference>` is not `<ref>`.
fn tag_delim(c: Option<&u8>) -> bool {
    match c {
        None => true,
        Some(b) => matches!(b, b' ' | b'\t' | b'\n' | b'\r' | b'>' | b'/'),
    }
}

/// Opt-in markup ranges (`--skip-markup`).
///
/// Wikipedia scaffolding lives in a handful of constructs: `{{template}}`
/// (nested braces, including `{{{param|default}}}`), `<!-- comment -->`,
/// `<ref>...</ref>`, `<gallery>...</gallery>`, `<math>...</math>` and wikitables
/// `{| ... |}` (nested). They recur a few hundred times BY CONSTRUCTION, so a
/// scan that reads them spends its multiplicity budget on scaffolding rather
/// than prose. This scanner marks those byte ranges in one left-to-right pass; a
/// window that OVERLAPS a range is not started.
///
/// Deliberately simple, and the simplifications are stated rather than hidden:
/// ranges are contiguous, so nesting INSIDE a skipped range is swallowed by
/// jumping to the end of the construct; and an UNTERMINATED opener (a truncated
/// article) marks markup to the end of the text - which is what the text says.
fn markup_ranges(text: &[u8]) -> Vec<(usize, usize)> {
    let n = text.len();
    let mut out: Vec<(usize, usize)> = Vec::new();
    // Balanced two-byte opener/closer pairs (braces, tables).
    const PAIRS: [(&[u8], &[u8]); 2] = [(b"{{", b"}}"), (b"{|", b"|}")];
    // Tag constructs: `<name ...>` .. `</name>`, or a self-closing `<name ... />`.
    const TAGS: [(&[u8], &[u8]); 3] =
        [(b"<ref", b"</ref>"), (b"<gallery", b"</gallery>"), (b"<math", b"</math>")];
    let mut i = 0usize;
    while i < n {
        let mut jumped = false;
        for (open, close) in PAIRS {
            if !starts_seq(text, i, open) {
                continue;
            }
            let mut depth = 0usize;
            let mut j = i;
            let mut end = loop {
                if j + 1 >= n {
                    break n;
                }
                if text[j] == open[0] && text[j + 1] == open[1] {
                    depth += 1;
                    j += 2;
                    continue;
                }
                if text[j] == close[0] && text[j + 1] == close[1] {
                    depth -= 1;
                    j += 2;
                    if depth == 0 {
                        break j;
                    }
                    continue;
                }
                j += 1;
            };
            // `{{{param|default}}}` pairs as `{` + `{{...}}` + `}`: the pair count
            // closes after two of the three closing braces, so swallow the rest of
            // the brace run rather than leaving a stray `}` outside the range.
            if open == b"{{" {
                while end < n && text[end] == b'}' {
                    end += 1;
                }
            }
            out.push((i, end));
            i = end;
            jumped = true;
            break;
        }
        if jumped {
            continue;
        }
        if starts_seq(text, i, b"<!--") {
            let end = find_seq(text, b"-->", i + 4).map(|e| e + 3).unwrap_or(n);
            out.push((i, end));
            i = end;
            continue;
        }
        for (open, close) in TAGS {
            if !starts_seq_ci(text, i, open) || !tag_delim(text.get(i + open.len())) {
                continue;
            }
            let Some(gt) = find_seq(text, b">", i + open.len()) else {
                // An opener with no `>` at all: the tag runs off the text.
                out.push((i, n));
                i = n;
                jumped = true;
                break;
            };
            let self_closing = gt > 0 && text[gt - 1] == b'/';
            let end = if self_closing {
                gt + 1
            } else {
                match find_seq_ci(text, close, gt + 1) {
                    Some(c) => find_seq(text, b">", c).map(|g| g + 1).unwrap_or(n),
                    None => n,
                }
            };
            out.push((i, end));
            i = end;
            jumped = true;
            break;
        }
        if jumped {
            continue;
        }
        i += 1;
    }
    out
}

/// Markup ranges plus the amortized cursor a monotone window walk needs: window
/// starts only ever move forward, so the next overlapping range is found by
/// advancing one index, not by searching.
struct Markup {
    ranges: Vec<(usize, usize)>,
    bytes: u64,
    cursor: usize,
}

impl Markup {
    fn scan(text: &[u8], enabled: bool) -> Markup {
        let ranges = if enabled { markup_ranges(text) } else { Vec::new() };
        let bytes = ranges.iter().map(|(a, b)| (b - a) as u64).sum();
        Markup { ranges, bytes, cursor: 0 }
    }
    /// Does `[start, end)` intersect a markup range? Ranges are sorted and
    /// disjoint and `start` is non-decreasing across calls.
    fn overlaps(&mut self, start: usize, end: usize) -> bool {
        while self.cursor < self.ranges.len() && self.ranges[self.cursor].1 <= start {
            self.cursor += 1;
        }
        self.cursor < self.ranges.len() && self.ranges[self.cursor].0 < end
    }
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
/// Everything the continuous walk needs to know. `--target-mult` and the band
/// both collapse to (target, cap); the rest is placement. Shared by `xsa seeds`
/// (which ranks occurrences) and `xsa fanout` (which weights them by rarity),
/// so both commands run the SAME scan - a rarity ranking over a forked copy of
/// the solver would be evidence about the copy, not about the kernel.
struct ScanOpts {
    min_len: usize,
    max_len: usize,
    target: u64,
    cap: Option<u64>,
    nearest: bool,
    stride: usize,
    skip_markup: bool,
    /// Capacity hint for the accepted set (`--max-seeds`); the cap itself is
    /// applied by the caller, never here.
    reserve: usize,
    /// Suppress the per-window journal chatter (`SEEDS_MARKUP`,
    /// `SEEDS_PROGRESS`). A SERVED run answers one window after another in one
    /// process, so a line per window is noise; the totals stay in the
    /// `SEEDS_*_STATS` line and the per-window answer is on stdout.
    quiet: bool,
}

/// Result of one whole-document walk. Every window is accounted for in `stats`
/// (asserted by the caller), and the coverage figures are the union of the bytes
/// the windows actually READ.
struct ScanOut {
    seeds: Vec<Seed>,
    stats: SeedStats,
    markup: Markup,
    first_window: usize,
    last_window: usize,
    coverage_pct: f64,
    nearest_below: u64,
    width: usize,
    scan_ms: f64,
}

fn scan_text(cx: &mut Ctx<'_>, sigma: &[u8; 256], text: &[u8], o: &ScanOpts) -> ScanOut {
    let (min_len, max_len, target, cap, nearest, step, skip_markup) =
        (o.min_len, o.max_len, o.target, o.cap, o.nearest, o.stride, o.skip_markup);
    let planned = text.len().div_ceil(step);
    let mut cache = vec![None; max_len.max(1) + 1];
    let mut stats = SeedStats::default();
    let mut seeds: Vec<Seed> = Vec::with_capacity(o.reserve.min(4096));
    let mut seen: std::collections::HashSet<Vec<u8>> = std::collections::HashSet::new();
    let mut nearest_below = 0u64;
    // The far end of the longest accepted interval so far: with window starts
    // non-decreasing, an earlier interval can only contain a later one if its far
    // end reaches past the later one's, so one running maximum replaces a walk
    // over every accepted seed (which a per-byte scan could not afford).
    let mut max_end = 0u64;
    let width = max_len.min(text.len());
    let mut markup = Markup::scan(text, skip_markup);
    if !o.quiet {
        eprintln!(
            "SEEDS_MARKUP enabled={skip_markup} ranges={} bytes_skipped={} frac={:.6}",
            markup.ranges.len(),
            markup.bytes,
            markup.bytes as f64 / text.len() as f64,
        );
    }
    let scan = Instant::now();
    let mut first_window = usize::MAX;
    let mut last_window = 0usize;
    let mut covered = 0u64;
    let mut cov_start = 0usize;
    let mut cov_end = 0usize;
    let mut i = 0usize;
    while i < text.len() {
        stats.windows += 1;
        if !o.quiet && stats.windows % 10_000 == 0 {
            // A continuous scan of a whole article is minutes, not milliseconds:
            // say where it is so a long run is visible rather than silent.
            eprintln!(
                "SEEDS_PROGRESS windows={} of={planned} offset={i} of_text_bytes={} \
                 accepted_windows={} markup_skipped={} elapsed_ms={:.0}",
                stats.windows,
                text.len(),
                stats.windows_with_seed,
                stats.windows_skipped_markup,
                scan.elapsed().as_secs_f64() * 1e3,
            );
        }
        // `--skip-markup`: a window that OVERLAPS scaffolding is not started.
        // The bytes are then never read, so nothing from inside a template,
        // comment, ref, gallery, math block or wikitable can become a seed.
        let end = (i + width).min(text.len());
        if skip_markup && markup.overlaps(i, end) {
            stats.windows_skipped_markup += 1;
            i += step;
            continue;
        }
        // Union of the bytes actually READ: window starts are non-decreasing, so
        // one open run [cov_start, cov_end) is enough. This is what `coverage_pct`
        // reports - the fraction of the document the scan really looked at, which
        // is 100.0 only when every byte was inside some window.
        if cov_end < i {
            covered += (cov_end - cov_start) as u64;
            cov_start = i;
        }
        cov_end = cov_end.max(end);
        first_window = first_window.min(i);
        last_window = i;
        // The longest-occurring-prefix bisection answers NoMatch on its own, so
        // no `present` pre-filter is needed here.
        let mut chosen_lo = false;
        let w = seed_window(
            cx,
            sigma,
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
                match seed_suppressed(max_end, &seen, text, offset, len as u64) {
                    Some("contained") => stats.suppressed_contained += 1,
                    Some(_) => stats.suppressed_dedup += 1,
                    None => {
                        stats.windows_with_seed += 1;
                        max_end = max_end.max(offset + len as u64);
                        seen.insert(text[offset as usize..offset as usize + len as usize].to_vec());
                        seeds.push(Seed { offset, len, mult, l, r, gave_up, gave_up_mult });
                    }
                }
            }
        }
        // Clear the per-window cache: lengths are absolute (window-relative), so
        // a stale entry would be a wrong answer, not a slow one.
        for c in cache.iter_mut() {
            *c = None;
        }
        i += step;
    }
    let scan_ms = scan.elapsed().as_secs_f64() * 1e3;
    // Coverage is auditable, not asserted by prose: `coverage_pct` is the fraction
    // of the document that some examined window actually READ (union, not
    // density - a stride smaller than --max-len reads every byte), and the first
    // and last window starts say where the scan ran.
    covered += (cov_end - cov_start) as u64;
    let coverage_pct = (100.0 * covered as f64 / text.len() as f64).min(100.0);
    if first_window == usize::MAX {
        first_window = 0;
    }
    ScanOut {
        seeds,
        stats,
        markup,
        first_window,
        last_window,
        coverage_pct,
        nearest_below,
        width,
        scan_ms,
    }
}

pub fn seeds_command(raw: &[std::ffi::OsString]) {
    let args: Vec<_> = raw.iter().map(|a| a.to_string_lossy().into_owned()).collect();
    let container = args.first().unwrap_or_else(|| die(SEEDS_USAGE));
    let mut text_path: Option<std::ffi::OsString> = None;
    let mut docs: Option<String> = None;
    let mut json = false;
    let mut nearest = false;
    let mut skip_markup = false;
    let mut max_len = 64usize;
    let mut min_len = 12usize;
    let mut max_seeds = 64usize;
    let mut stride: Option<usize> = None;
    let mut limit = 8u64;
    let mut limit_given = false;
    let mut target_mult: Option<u64> = None;
    let mut max_mult: Option<u64> = None;
    let mut min_mult: Option<u64> = None;
    // ---- the warm/batch surface (see the WARM PATH section below).
    let mut serve = false;
    let mut jobs = 0usize;
    let mut rss_gb: Option<f64> = None;
    let mut cache_gb: Option<f64> = None;
    let mut anchors = false;
    let mut anchor_per_seed = SEEDS_ANCHOR_PER_SEED_DEFAULT;
    let mut windows_path: Option<String> = None;
    let mut windows_bin = false;
    let mut corpus_path: Option<String> = None;
    let mut windows_out: Option<String> = None;
    let mut window_len: Option<u64> = None;
    let mut window_stride: Option<u64> = None;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--jsonl" => json = true,
            "--nearest" => nearest = true,
            "--skip-markup" => skip_markup = true,
            "--serve" => serve = true,
            "--anchors" => anchors = true,
            "--windows-bin" => windows_bin = true,
            "-j" | "--jobs" => {
                i += 1;
                let v = args.get(i).unwrap_or_else(|| die("seeds: -j needs a value"));
                jobs = v.parse().unwrap_or_else(|_| die(&format!("seeds: invalid -j '{v}'")));
                if jobs == 0 {
                    die("seeds: -j must be positive");
                }
            }
            "--max-len" | "--min-len" | "--max-seeds" | "--stride" | "--limit" | "--docs"
            | "--target-mult" | "--max-mult" | "--min-mult" | "--rss-gb" | "--cache-gb"
            | "--anchor-max-per-seed" | "--windows" | "--corpus" | "--windows-out"
            | "--window-len" | "--window-stride" => {
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
                    "--limit" => {
                        limit = num("--limit");
                        limit_given = true;
                    }
                    "--target-mult" => target_mult = Some(num("--target-mult")),
                    "--max-mult" => max_mult = Some(num("--max-mult")),
                    "--min-mult" => min_mult = Some(num("--min-mult")),
                    "--rss-gb" => {
                        let g: f64 = v
                            .parse()
                            .unwrap_or_else(|_| die(&format!("seeds: invalid --rss-gb '{v}'")));
                        if !(g > 0.0) {
                            die("seeds: --rss-gb must be positive");
                        }
                        rss_gb = Some(g);
                    }
                    "--anchor-max-per-seed" => {
                        anchor_per_seed = num("--anchor-max-per-seed")
                    }
                    "--cache-gb" => {
                        let g: f64 = v
                            .parse()
                            .unwrap_or_else(|_| die(&format!("seeds: invalid --cache-gb '{v}'")));
                        if !(g > 0.0) {
                            die("seeds: --cache-gb must be positive");
                        }
                        cache_gb = Some(g);
                    }
                    "--windows" => windows_path = Some(v.clone()),
                    "--corpus" => corpus_path = Some(v.clone()),
                    "--windows-out" => windows_out = Some(v.clone()),
                    "--window-len" => window_len = Some(num("--window-len")),
                    _ => window_stride = Some(num("--window-stride")),
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
    // ---- WINDOW PLACEMENT. The default is EVERY position: a continuous scan of
    // the whole document, so no section can be starved by where the walk happens
    // to stop. `--stride S` is the optional override that samples every S-th
    // position instead (the pre-fix behaviour); it never changes the OUTPUT
    // selection, and the scan is complete either way.
    let step = match stride {
        Some(0) => die("seeds: --stride must be positive"),
        Some(s) => s,
        None => 1,
    };
    let params = SeedParams {
        min_len,
        max_len,
        target,
        cap,
        nearest,
        stride: step,
        skip_markup,
        max_seeds,
        anchor_per_seed,
    };
    // ---- THE SERVED FORMS (see the WARM PATH section below). Both take the
    // query text from somewhere else than the positional argument, so a
    // positional text is refused rather than silently ignored.
    if serve || windows_path.is_some() {
        if text_path.is_some() {
            die(if serve {
                "seeds: --serve reads its query text from stdin frames; the positional text \
                 argument does not apply (give one or the other)"
            } else {
                "seeds: --windows declares the query text; the positional text argument does \
                 not apply (give --corpus FILE for the bytes)"
            });
        }
        if serve && windows_path.is_some() {
            die("seeds: --serve (frames on stdin) and --windows (a declared window file) are \
                 alternative inputs; give one");
        }
        if windows_path.is_some() && corpus_path.is_none() {
            die(
                "seeds: --windows needs --corpus FILE: a window record declares (base, start, \
                 len) over a corpus, and the query bytes are read from it",
            );
        }
        if windows_path.is_none() && corpus_path.is_some() {
            die("seeds: --corpus only applies with --windows (see --help)");
        }
        if windows_out.is_some() || window_len.is_some() || window_stride.is_some() {
            die(
                "seeds: --windows-out/--window-len/--window-stride DECLARE a window set from a \
                 text file; they do not apply to --serve or --windows",
            );
        }
        if json {
            die(
                "seeds: the served forms answer with the anchor TABLE (TSV for --windows, \
                 fixed-width frames for --serve); --jsonl applies to the classic hit list",
            );
        }
        if limit_given {
            die(
                "seeds: --limit is the SAMPLE-POSITION cap of the classic hit list, not an anchor \
                 cap; use --anchor-max-per-seed",
            );
        }
        seeds_served(
            container,
            docs.as_deref(),
            &params,
            serve,
            windows_path.as_deref(),
            windows_bin,
            corpus_path.as_deref(),
            jobs,
            rss_gb,
            cache_gb,
        );
        return;
    }
    if windows_bin {
        die("seeds: --windows-bin only applies with --windows");
    }
    let Some(text_path) = text_path else { die(SEEDS_USAGE) };

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
    // ---- DECLARE the window set, if asked: `--windows-out` writes the boundaries
    // this run would use, so a later run can REPLAY them (`--windows`/--corpus)
    // instead of re-deriving them. The declaration is the geometry, not the text.
    if let Some(outp) = windows_out.as_deref() {
        let len = window_len.unwrap_or_else(|| {
            die("seeds: --windows-out needs --window-len L (and optionally --window-stride S)")
        });
        if len == 0 || len > SEEDS_WINDOW_MAX as u64 {
            die(&format!("seeds: --window-len must be 1..={SEEDS_WINDOW_MAX}, got {len}"));
        }
        let s = window_stride.unwrap_or(len);
        if s == 0 {
            die("seeds: --window-stride must be positive");
        }
        let count = declare_windows(text.len() as u64, len, s, outp);
        eprintln!(
            "SEEDS_WINDOWS_DECLARED out={outp} windows={count} window_len={len} \
             window_stride={s} text_bytes={} overlap={}",
            text.len(),
            if s < len { "yes" } else { "no" },
        );
    } else if window_len.is_some() || window_stride.is_some() {
        die("seeds: --window-len/--window-stride only apply with --windows-out FILE");
    }

    // `Reader::open`'s error IS the refusal (the shape + the rebuild
    // instruction): there is nothing to add and nothing to fall back to.
    let reader = Reader::open(container).unwrap_or_else(|e| die(&e));
    // ---- ANCHOR MODE, one window: the whole text is window 0. Same scan, same
    // output selection, same tie order and same pipeline as the served forms, so
    // the three paths agree row for row.
    if anchors {
        if json {
            die(
                "seeds: --anchors emits the anchor TABLE (TSV, one row per occurrence); \
                 --jsonl applies to the classic hit list",
            );
        }
        let ds = crate::docstarts::DocStarts::from_container(&reader);
        let docs_id = format!("m15:{}", ds.docs);
        if limit_given {
            die(
                "seeds: --limit is the SAMPLE-POSITION cap of the classic hit list, not an anchor \
                 cap; use --anchor-max-per-seed",
            );
        }
        let index = seeds_open_index(reader, 1, rss_gb, cache_gb);
        let sigma = index.reader.sigma;
        let started = Instant::now();
        let rchar0 = proc_io("rchar");
        let stdout = std::io::stdout();
        let mut w = BufWriter::with_capacity(1 << 20, stdout.lock());
        writeln!(w, "{}", anchors_header(&params, &docs_id)).unwrap();
        writeln!(
            w,
            "window_id\tquery_offset\tcorpus_doc\tcorpus_offset\tmatch_len\tmultiplicity"
        )
        .unwrap();
        let pool = if jobs > 1 {
            Some(
                rayon::ThreadPoolBuilder::new()
                    .num_threads(jobs)
                    .build()
                    .unwrap_or_else(|e| die(&format!("seeds: -j {jobs}: {e}"))),
            )
        } else {
            None
        };
        let mut cx = index.ctx();
        let wa = window_anchors(
            &index,
            &mut cx,
            &sigma,
            Some(&ds),
            0,
            text,
            &params,
            pool.as_ref(),
        );
        anchors_write_tsv(&mut w, &wa).unwrap();
        w.flush().unwrap();
        let secs = started.elapsed().as_secs_f64();
        let (hits, misses, decodes, _d) = index.cache_stats();
        eprintln!(
            "SEEDS_ANCHOR_STATS mode=file windows=1 text_bytes={} windows_scanned={} seeds={} \
             rows={} positions={} seconds={secs:.3} positions_per_s={:.1} lf_steps={} ranks={} \
             doc_absent={} hits={hits} misses={misses} decodes={decodes} rchar={} \
             peak_rss_gb={:.3} cap_gb={:.3} arena_bytes={} docs={docs_id} scan_ms={:.3} \
             locate_ms={:.3}",
            wa.text_bytes,
            wa.windows_scanned,
            wa.seeds,
            wa.rows.len(),
            wa.positions,
            wa.positions as f64 / secs,
            wa.lf_steps,
            wa.ranks,
            wa.doc_absent,
            proc_io("rchar").saturating_sub(rchar0),
            proc_peak_rss() as f64 / (1u64 << 30) as f64,
            rss_gb.map(|g| g).unwrap_or(0.0),
            index.cache_bytes(),
            wa.scan_ms,
            wa.locate_ms,
        );
        return;
    }
    let docs = discover_documents(container, docs.as_deref(), reader.n);
    if docs.is_none() && limit > 0 {
        eprintln!("SEEDS_NOTE docs=none: emitting plain corpus offsets");
    }
    let index = seeds_open_index(reader, 1, rss_gb, cache_gb);
    let sigma = index.reader.sigma;
    let n = index.n();
    let present: [bool; 256] = std::array::from_fn(|b| {
        let s = sigma[b] as usize;
        s != 0 && index.total[s] != 0
    });
    let planned = text.len().div_ceil(step);
    eprintln!(
        "SEEDS_READY n={n} present_bytes={} text_bytes={planned_windows} windows={planned} \
         mode={} target_mult={target} cap={} min_len={min_len} max_len={max_len} stride={step} \
         max_seeds={max_seeds} skip_markup={skip_markup} nearest={nearest}",
        present.iter().filter(|p| **p).count(),
        if band { "band" } else { "target" },
        cap.map(|c| c.to_string()).unwrap_or_else(|| "none".into()),
        planned_windows = text.len(),
    );
    // Identity is NOT required: a seed text is normally a FRAGMENT of the
    // corpus. Equal length is only worth a note, because that is usually the
    // corpus itself, where every window is its own longest match.
    if text_path.as_os_str() != "-" && text.len() as u64 == n.saturating_sub(1) {
        eprintln!("SEEDS_NOTE text length equals corpus_bytes: unexpected for a fragment");
    }

    let mut cx = index.ctx();
    let o = ScanOpts {
        min_len,
        max_len,
        target,
        cap,
        nearest,
        stride: step,
        skip_markup,
        reserve: max_seeds,
        quiet: false,
    };
    let ScanOut {
        mut seeds,
        mut stats,
        markup,
        first_window,
        last_window,
        coverage_pct,
        nearest_below,
        width,
        scan_ms,
    } = scan_text(&mut cx, &sigma, text, &o);

    // ---- OUTPUT SELECTION. The scan above is complete whatever `--max-seeds`
    // says; the cap is applied HERE, to the accepted set, and by walking it at
    // even index spacing so the emitted seeds still cover the document instead of
    // being the first K that happened to be found.
    let accepted_windows = seeds.len();
    let mut capped_dropped = 0u64;
    if accepted_windows > max_seeds {
        let take = max_seeds;
        let step_idx = accepted_windows / take;
        let picked: Vec<Seed> = (0..take).map(|j| seeds[j * step_idx]).collect();
        capped_dropped = (accepted_windows - take) as u64;
        seeds = picked;
    }
    stats.accepted = seeds.len() as u64;
    stats.lens = seeds.iter().map(|s| s.len).collect();
    stats.mults = seeds.iter().map(|s| s.mult).collect();

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
            if let Some(_d) = &docs {
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
            + stats.suppressed_contained
            + stats.windows_skipped_markup,
        "SEEDS_WINDOW_ACCOUNTING windows do not add up"
    );
    // Density: windows * width / text. Above 100% means the windows OVERLAP
    // (a continuous scan with --max-len 64 reads each byte 64 times).
    let examined_pct = (100.0 * (stats.windows as f64) * (width as f64) / text.len() as f64)
        .min(100.0);
    let skipped_pct =
        100.0 * stats.windows_skipped_markup as f64 / stats.windows.max(1) as f64;
    if stats.windows_skipped_markup == stats.windows {
        eprintln!(
            "SEEDS_NOTE every one of {} windows overlapped markup: the text is scaffolding \
             (or an unterminated construct runs to its end), so a zero-seed answer is expected",
            stats.windows
        );
    }
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
        "SEEDS_STATS windows={} coverage_pct={coverage_pct:.4} examined_pct={examined_pct:.4} \
         first_window={first_window} last_window={last_window} text_bytes={} \
         accepted_windows={accepted_windows} capped_dropped={capped_dropped} \
         windows_skipped_markup={} markup_skipped_pct={skipped_pct:.4} \
         markup_bytes_skipped={} markup_frac={:.6} tried={} accepted={} \
         mean_len={mean_len:.2} mult_histogram={} shorten_steps={} extend_steps={} \
         scan_ms={scan_ms:.3} locate_ms={locate_ms:.3} mode={} target_mult={target} cap={} \
         min_len={min_len} max_len={max_len} stride={step} no_match={} too_short={} \
         too_rare={} too_common={} suppressed_dedup={} suppressed_contained={} \
         nearest_below={nearest_below} positions={located} ranks={} lf_steps={}",
        stats.windows,
        text.len(),
        stats.windows_skipped_markup,
        markup.bytes,
        markup.bytes as f64 / text.len().max(1) as f64,
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

// ---------------------------------------------------------------- warm path
//
// `xsa seeds` was a ONE-SHOT command: every invocation paid the whole open (mmap,
// member directory, bounded m1 audit, m11 anchors, decoded-block arena) and every
// reported occurrence paid an LF walk to an m11 anchor. On a 100 GB container a
// 200-byte text therefore read gigabytes of container to print a few hundred
// bytes (NOTE_FROM_EMENDER_XSA_SEEDS_COST.md: 3.25 GB rchar, 3.3 M read(2), >5
// min). The three already-served commands (`mem --serve`, `branches --serve`,
// `consult`) fixed exactly this shape, and this section reuses their two devices
// instead of inventing a third:
//
//   FRAMING, ONCE. `--serve` reads u32-length-prefixed window frames from stdin
//   and writes u32-length-prefixed answer frames - `read_mem_request`'s encoding,
//   the one `mem --serve` already speaks. The container is opened, audited, and
//   its document structure resolved ONCE, before the first frame is read; no
//   framing or index check happens per window.
//
//   DECLARED WINDOWS. `--windows FILE --corpus FILE` replays a window set that
//   is DECLARED in the file - the XSWIN001/JSONL window stream `xsa consult`
//   already consumes - so a boundary is never inferred from content: window k is
//   corpus[base+start .. base+start+len) as the record says, refused by name if it
//   leaves the corpus or the container's text.
//
// WHAT A WINDOW COSTS is the ANCHOR TABLE, and the layers are not the same price.
// A seed's multiplicity is an INTERVAL WIDTH (O(1) once the scan has the
// interval): zero locates, exact, independent of -k/--limit. A corpus_offset is
// one LF walk to an m11 anchor - the one irreducible locate layer of this
// command - and `--rss-gb` sizes the decoded-block arena those walks read
// through (consult's knob, consult's split), which is why a served run's
// container reads are a function of the QUERY SET and not of the container.
// RESULTS_SEEDS_WARM.md is the price list.

/// Default OUTPUT cap on anchor rows per seed. `0` means EVERY occurrence of the
/// seed, which is one locate per occurrence (the expensive setting); the default
/// samples the interval at even SA-rank spacing, exactly as `--limit` samples the
/// classic hit list. Neither is a support threshold: see SEEDS_USAGE.
const SEEDS_ANCHOR_PER_SEED_DEFAULT: u64 = 8;
/// The framed window cap, the same 16 MiB `mem --serve` refuses past.
const SEEDS_WINDOW_MAX: usize = 16 * 1024 * 1024;
/// `#xsa-seeds-anchors` schema tag. Bump it when a column changes meaning.
const SEEDS_ANCHOR_SCHEMA: &str = "#xsa-seeds-anchors v1";

/// Every knob the scan needs, shared by the three input forms (a text file, a
/// framed stdin window, a declared window record) so no form can drift from
/// another: they run the SAME `scan_text` and the SAME output selection.
#[derive(Debug, Clone, Copy)]
struct SeedParams {
    min_len: usize,
    max_len: usize,
    target: u64,
    cap: Option<u64>,
    nearest: bool,
    stride: usize,
    skip_markup: bool,
    max_seeds: usize,
    anchor_per_seed: u64,
}

/// One chain-ready anchor: `query[query_offset .. query_offset+match_len)` occurs
/// at `corpus_off`. `mult` is the EXACT occurrence count (the BWT interval
/// width), never the number of rows a cap let through. `corpus_doc` is the
/// container's m15 document at `corpus_off`; `None` means the container carries
/// no m15 (SXI2 < v10) and the field is absent - a docid is never guessed from a
/// sidecar or from content.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct AnchorRow {
    query_offset: u32,
    match_len: u32,
    mult: u64,
    corpus_off: u64,
    corpus_doc: Option<u64>,
}

/// THE TIE ORDER, in one place because two runs must be byte-identical: rows are
/// ordered by (query_offset, corpus_offset, match_len, multiplicity). Seeds are
/// already emitted in increasing query offset; the sort makes the ORDER OF
/// OCCURRENCES canonical too, so a window's rows never depend on the order the
/// locate walks finished in (or on -j).
fn anchor_key(r: &AnchorRow) -> (u32, u64, u32, u64) {
    (r.query_offset, r.corpus_off, r.match_len, r.mult)
}

/// One window's anchor table plus the accounting that has to add up.
struct WindowAnchors {
    window_id: u64,
    text_bytes: u64,
    rows: Vec<AnchorRow>,
    windows_scanned: u64,
    seeds: u64,
    positions: u64,
    lf_steps: u64,
    ranks: u64,
    doc_absent: u64,
    scan_ms: f64,
    locate_ms: f64,
}

/// Run totals, summed in window order by the sink so they are identical whatever
/// `-j` is.
#[derive(Default)]
struct ServeTotals {
    windows: u64,
    text_bytes: u64,
    windows_scanned: u64,
    seeds: u64,
    rows: u64,
    positions: u64,
    lf_steps: u64,
    ranks: u64,
    doc_absent: u64,
    scan_ms: f64,
    locate_ms: f64,
}

impl ServeTotals {
    fn add(&mut self, w: &WindowAnchors) {
        self.windows += 1;
        self.text_bytes += w.text_bytes;
        self.windows_scanned += w.windows_scanned;
        self.seeds += w.seeds;
        self.rows += w.rows.len() as u64;
        self.positions += w.positions;
        self.lf_steps += w.lf_steps;
        self.ranks += w.ranks;
        self.doc_absent += w.doc_absent;
        self.scan_ms += w.scan_ms;
        self.locate_ms += w.locate_ms;
    }
}

/// The `<container>` element of `/proc/self/io` (or 0 when it is unreadable - a
/// receipt helper, never a gate). `rchar` is the COUNT of bytes this process asked
/// the kernel for with read-family calls; the container's block payloads are
/// `pread` (not page faults), so rchar is exactly "container read", and the gate
/// can require it to follow the query set instead of the container.
fn proc_io(name: &str) -> u64 {
    let s = std::fs::read_to_string("/proc/self/io").unwrap_or_default();
    for line in s.lines() {
        if let Some(v) = line.strip_prefix(name).and_then(|r| r.strip_prefix(':')) {
            return v.trim().parse().unwrap_or(0);
        }
    }
    0
}

/// Peak RSS of this process in bytes (VmHWM).
fn proc_peak_rss() -> u64 {
    let s = std::fs::read_to_string("/proc/self/status").unwrap_or_default();
    for line in s.lines() {
        if let Some(v) = line.strip_prefix("VmHWM:") {
            let kb: u64 = v
                .trim()
                .trim_end_matches(" kB")
                .trim()
                .parse()
                .unwrap_or(0);
            return kb * 1024;
        }
    }
    0
}

/// Open the index with the decoded-block arena the run paid for. `None` is the
/// historical 64 MB arena (`Index::open` is exactly this call). `--rss-gb` is
/// consult's budget split verbatim: a fixed arena of an eighth of the budget,
/// clamped to [4 MiB, 64 GiB], with the rest of the budget as the RSS cap the
/// governor bleeds the container mapping back to. The arena is FIXED at open and
/// `decoded_bytes` stays 0 - a budget split, not a materialisation.
fn seeds_open_index(
    reader: Reader,
    jobs: usize,
    rss_gb: Option<f64>,
    cache_gb: Option<f64>,
) -> Index {
    let Some(g) = rss_gb else {
        return Index::open(reader, jobs).unwrap_or_else(|e| die(&e));
    };
    let cap_bytes = (g * (1u64 << 30) as f64) as u64;
    let arena_bytes = match cache_gb {
        Some(c) => (c * (1u64 << 30) as f64) as usize,
        None => (cap_bytes / 8) as usize,
    }
    .clamp(4 << 20, 64usize << 30);
    let mut index = Index::open_with_cache(reader, jobs, arena_bytes).unwrap_or_else(|e| die(&e));
    index.set_rss_cap(cap_bytes);
    index
}

/// The anchor table of ONE window: the same scan, the same output selection, then
/// one locate per reported anchor row (bounded by `--anchor-max-per-seed`), then
/// the canonical sort. Nothing about the multiplicities comes from the located
/// rows: `mult` is the interval width the scan already had.
///
/// THE LOCATE LAYER IS PARALLEL ACROSS POSITIONS. One located position is one LF
/// walk to an m11 anchor (measured ~4,600 steps on the 100 GB heads), and a walk
/// touches BWT blocks at random, so the walks are independent and the only thing
/// that makes them cheap is doing many at once against the shared read-only index
/// and its decoded-block cache - exactly what `Index::locate_range` already does
/// for wide intervals. Positions are a function of their ROW, not of the order the
/// walks run in, so the parallel walk is bit-identical to the serial one and the
/// rows are sorted afterwards anyway (the documented tie order).
fn window_anchors(
    index: &Index,
    cx: &mut Ctx<'_>,
    sigma: &[u8; 256],
    ds: Option<&crate::docstarts::DocStarts>,
    window_id: u64,
    text: &[u8],
    p: &SeedParams,
    pool: Option<&rayon::ThreadPool>,
) -> WindowAnchors {
    assert!(!text.is_empty(), "SEEDS_EMPTY_WINDOW window={window_id}");
    let o = ScanOpts {
        min_len: p.min_len,
        max_len: p.max_len,
        target: p.target,
        cap: p.cap,
        nearest: p.nearest,
        stride: p.stride,
        skip_markup: p.skip_markup,
        reserve: p.max_seeds,
        quiet: true,
    };
    let (steps0, ranks0) = (cx.steps, cx.ranks);
    let out = scan_text(cx, sigma, text, &o);
    let scan_ms = out.scan_ms;
    let (scan_steps, scan_ranks) = (cx.steps - steps0, cx.ranks - ranks0);
    assert_eq!(
        out.stats.windows,
        out.stats.no_match
            + out.stats.too_short
            + out.stats.too_rare
            + out.stats.too_common
            + out.stats.windows_with_seed
            + out.stats.suppressed_dedup
            + out.stats.suppressed_contained
            + out.stats.windows_skipped_markup,
        "SEEDS_WINDOW_ACCOUNTING window={window_id}"
    );
    // THE SAME OUTPUT SELECTION as the classic path: `--max-seeds K` picks K of
    // the accepted seeds at even index spacing, so the emitted set covers the
    // window instead of being its first K accepted seeds.
    let mut seeds = out.seeds;
    if seeds.len() > p.max_seeds {
        let take = p.max_seeds;
        let step_idx = seeds.len() / take;
        seeds = (0..take).map(|j| seeds[j * step_idx]).collect();
    }
    let nseeds = seeds.len() as u64;
    // ONE WORK UNIT PER ANCHOR ROW, not per seed. A row is one LF walk (~80 ms of
    // block decodes on a 100 GB head), so the unit is coarse enough to amortise a
    // thread and the set is wide enough to fill the pool; unit-per-SEED was
    // measured 3x slower on a 4-window batch because one frequent seed (hundreds of
    // occurrences) then landed on one thread. `seed_sample` is the documented
    // even-SA-rank sampler; it is an OUTPUT cap on ROWS, and `mult` below stays the
    // exact width, so a capped row set can only ever be read as a cap.
    let mut units: Vec<(u32, u32, u64, u64)> = Vec::new(); // (qoff, len, mult, row)
    for s in &seeds {
        let want = if p.anchor_per_seed == 0 {
            s.mult
        } else {
            p.anchor_per_seed.min(s.mult)
        };
        let (sample, _truncated) = seed_sample(s.l, s.r, want);
        for row in sample {
            units.push((s.offset as u32, s.len, s.mult, row));
        }
    }
    let locate = Instant::now();
    let one_row = |cx: &mut Ctx<'_>, u: &(u32, u32, u64, u64)| -> (AnchorRow, bool) {
        let pos = cx.locate(u.3);
        let doc = match ds {
            Some(d) if pos < d.text_len => Some(d.rank(pos)),
            // A position at or past the declared text length is the virtual END
            // row: it has no document, and a docid is never guessed for it.
            _ => None,
        };
        let absent = doc.is_none();
        (
            AnchorRow {
                query_offset: u.0,
                match_len: u.1,
                mult: u.2,
                corpus_off: pos,
                corpus_doc: doc,
            },
            absent,
        )
    };
    let mut positions = 0u64;
    let mut doc_absent = 0u64;
    let mut steps = 0u64;
    let mut ranks = 0u64;
    let mut rows: Vec<AnchorRow> = Vec::with_capacity(units.len());
    if let Some(pool) = pool.filter(|_| units.len() > 1) {
        let pieces: Vec<(AnchorRow, bool, u64, u64)> = pool.install(|| {
            units
                .par_iter()
                .map(|u| {
                    let mut c = index.ctx();
                    let (s0, r0) = (c.steps, c.ranks);
                    let (row, absent) = one_row(&mut c, u);
                    (row, absent, c.steps - s0, c.ranks - r0)
                })
                .collect()
        });
        for (row, absent, st, rk) in pieces {
            positions += 1;
            doc_absent += u64::from(absent);
            steps += st;
            ranks += rk;
            rows.push(row);
        }
    } else {
        for u in &units {
            let (s0, r0) = (cx.steps, cx.ranks);
            let (row, absent) = one_row(cx, u);
            positions += 1;
            doc_absent += u64::from(absent);
            steps += cx.steps - s0;
            ranks += cx.ranks - r0;
            rows.push(row);
        }
    }
    rows.sort_unstable_by_key(anchor_key);
    let locate_ms = locate.elapsed().as_secs_f64() * 1e3;
    WindowAnchors {
        window_id,
        text_bytes: text.len() as u64,
        rows,
        windows_scanned: out.stats.windows,
        seeds: nseeds,
        positions,
        lf_steps: steps + scan_steps,
        ranks: ranks + scan_ranks,
        doc_absent,
        scan_ms,
        locate_ms,
    }
}

/// One anchor frame (serve mode): window id, row count, seed count, then fixed
/// 32-byte rows. Fixed width, no byte echo, no variable section - `mem --serve`'s
/// shape. `corpus_doc` is the all-ones sentinel when the container has no m15,
/// the same "absent" encoding consult's rows use.
fn anchor_frame_payload(w: &WindowAnchors) -> Vec<u8> {
    let mut b = Vec::with_capacity(16 + w.rows.len() * 32);
    b.extend(w.window_id.to_le_bytes());
    b.extend((w.rows.len() as u32).to_le_bytes());
    b.extend((w.seeds as u32).to_le_bytes());
    for r in &w.rows {
        b.extend(r.query_offset.to_le_bytes());
        b.extend(r.match_len.to_le_bytes());
        b.extend(r.mult.to_le_bytes());
        b.extend(r.corpus_off.to_le_bytes());
        b.extend(r.corpus_doc.unwrap_or(DOCID_ABSENT).to_le_bytes());
    }
    b
}

/// The TSV header: the parameters a reader needs to interpret the rows, and
/// NOTHING that varies between two identical runs (no path, no timing), so the
/// determinism gate can hash the whole stream.
fn anchors_header(p: &SeedParams, docs: &str) -> String {
    format!(
        "{SEEDS_ANCHOR_SCHEMA} mode={} target_mult={} cap={} min_len={} max_len={} stride={} \
         max_seeds={} anchor_max_per_seed={} docs={docs} tie_order=query_offset,corpus_offset,\
         match_len,multiplicity",
        if p.cap.is_some() { "band" } else { "target" },
        p.target,
        p.cap.map(|c| c.to_string()).unwrap_or_else(|| "none".into()),
        p.min_len,
        p.max_len,
        p.stride,
        p.max_seeds,
        p.anchor_per_seed,
    )
}

/// Write one window's rows in the table's column order. `corpus_doc` is `-` when
/// the container has no m15: marked absent, never guessed.
fn anchors_write_tsv(w: &mut dyn Write, wa: &WindowAnchors) -> std::io::Result<()> {
    for r in &wa.rows {
        match r.corpus_doc {
            Some(d) => write!(w, "{}\t{}\t{d}", wa.window_id, r.query_offset)?,
            None => write!(w, "{}\t{}\t-", wa.window_id, r.query_offset)?,
        }
        writeln!(w, "\t{}\t{}\t{}", r.corpus_off, r.match_len, r.mult)?;
    }
    Ok(())
}

/// Declare a window set over a text file: `--windows-out FILE` with
/// `--window-len L [--window-stride S]` writes JSONL window records (the schema
/// consult reads: doc, base, start, len) that tile the text with the DECLARED
/// geometry. `doc` is the window index - seeds does not interpret it (the
/// anchor's corpus_doc comes from m15 at the located offset) - and the file is
/// what makes replay possible: `--windows FILE --corpus TEXT` re-reads these
/// boundaries instead of inferring new ones.
fn declare_windows(text_bytes: u64, len: u64, stride: u64, out: &str) -> u64 {
    assert!(len > 0 && stride > 0, "SEEDS_DECLARE zero geometry");
    let f = File::create(out).unwrap_or_else(|e| die(&format!("seeds: create {out}: {e}")));
    let mut w = BufWriter::with_capacity(1 << 16, f);
    let mut off = 0u64;
    let mut id = 0u64;
    while off < text_bytes {
        let n = len.min(text_bytes - off);
        writeln!(
            w,
            "{{\"doc\":{id},\"base\":0,\"start\":{off},\"len\":{n}}}"
        )
        .unwrap();
        id += 1;
        off += stride;
    }
    w.flush().unwrap();
    id
}

/// THE SERVED LOOP both input forms run on: windows arrive in order (stdin
/// frames, or the declared records of a window file), each is scanned and its
/// anchor rows are located - IN PARALLEL inside the window, on the rayon pool
/// `-j` sizes - and written before the next window is read. There is no window
/// queue, no result re-ordering and no second thread of our own: the answer's
/// order is the input's order by construction, and the parallelism lives where
/// the cost is (one locate per anchor row, thousands of block decodes each).
fn seeds_loop<P, S>(
    index: &Index,
    sigma: &[u8; 256],
    ds: Option<&crate::docstarts::DocStarts>,
    params: &SeedParams,
    pool: Option<&rayon::ThreadPool>,
    mut produce: P,
    mut sink: S,
) -> ServeTotals
where
    P: FnMut() -> Option<Vec<u8>>,
    S: FnMut(&WindowAnchors) -> std::io::Result<()>,
{
    let mut totals = ServeTotals::default();
    let mut scan_cx = index.ctx();
    let mut window_id = 0u64;
    while let Some(text) = produce() {
        // A zero-length window is refused here, by name: it has no seed to report
        // and a silently empty answer would be a lie.
        if text.is_empty() {
            die(
                "seeds: zero-length window: every window must carry at least one byte (clients \
                 must not send empty frames)",
            );
        }
        if text.len() > SEEDS_WINDOW_MAX {
            die(&format!(
                "seeds: window of {} bytes exceeds the {SEEDS_WINDOW_MAX}-byte cap",
                text.len()
            ));
        }
        let wa = window_anchors(
            index,
            &mut scan_cx,
            sigma,
            ds,
            window_id,
            &text,
            params,
            pool,
        );
        sink(&wa).unwrap_or_else(|e| die(&format!("seeds: write: {e}")));
        totals.add(&wa);
        window_id += 1;
    }
    totals
}

/// `xsa seeds <container> --serve -j N` and `xsa seeds <container> --windows FILE
/// --corpus FILE`: one process, one open, many windows. Both are the SAME
/// computation on the same scan; they differ only in where the query bytes come
/// from and in the answer's encoding.
fn seeds_served(
    container: &str,
    _docs_dir: Option<&str>,
    p: &SeedParams,
    serve: bool,
    windows_path: Option<&str>,
    windows_bin: bool,
    corpus_path: Option<&str>,
    jobs: usize,
    rss_gb: Option<f64>,
    cache_gb: Option<f64>,
) {
    let started = Instant::now();
    let rchar0 = proc_io("rchar");
    let syscr0 = proc_io("syscr");
    let jobs = if jobs == 0 { 1 } else { jobs };
    // `Reader::open`'s error IS the refusal (the shape + the rebuild
    // instruction): there is nothing to add and nothing to fall back to.
    let reader = Reader::open(container).unwrap_or_else(|e| die(&e));
    let n = reader.n;
    let text_len = n.saturating_sub(1);
    // The locate layer is parallel ACROSS THE POSITIONS OF A WINDOW (see
    // window_anchors), so `-j` sizes the rayon pool the walks run on - the same
    // knob `xsa query` has. One thread is the honest serial baseline; a pool that
    // is already installed (a library caller, or `xsa query`) is left alone.
    // AN EXPLICIT POOL, not the global one: `build_global` fails silently once any
    // rayon call has initialised the default pool, which would make `-j 1` a lie
    // and `-j 16` unmeasurable. `-j 1` (the default) means NO pool, i.e. the serial
    // baseline the rate gate compares against.
    let pool = if jobs > 1 {
        Some(
            rayon::ThreadPoolBuilder::new()
                .num_threads(jobs)
                .build()
                .unwrap_or_else(|e| die(&format!("seeds: -j {jobs}: {e}"))),
        )
    } else {
        None
    };
    // THE DOCUMENT STRUCTURE, resolved ONCE from the container's m15. m15 is
    // REQUIRED by the one container shape, so every row's doc field is exact -
    // there is no "absent, never guessed" arm to fall into.
    let ds = crate::docstarts::DocStarts::from_container(&reader);
    let docs_id = format!("m15:{}", ds.docs);
    let index = seeds_open_index(reader, jobs, rss_gb, cache_gb);
    let sigma = index.reader.sigma;
    // THE OPEN IS THE ONLY VERIFICATION, and it has already happened: the m1 audit
    // ran in `open`, the m11 anchor header was checked, and m15 was parsed and
    // range-checked. Nothing below re-verifies the index; frames only carry bytes.
    let arena_bytes = index.cache_bytes();
    let cap_bytes = rss_gb.map(|g| (g * (1u64 << 30) as f64) as u64).unwrap_or(index.rss_cap());
    eprintln!(
        "SEEDS_SERVE_READY mode={} jobs={jobs} n={n} text_len={text_len} docs={docs_id} \
         container_bytes={} arena_bytes={arena_bytes} cap_bytes={cap_bytes} target_mult={} \
         cap={} min_len={} max_len={} stride={} max_seeds={} anchor_max_per_seed={} \
         open_ms={:.3}",
        if serve { "serve" } else { "windows" },
        std::fs::metadata(container).map(|m| m.len()).unwrap_or(0),
        p.target,
        p.cap.map(|c| c.to_string()).unwrap_or_else(|| "none".into()),
        p.min_len,
        p.max_len,
        p.stride,
        p.max_seeds,
        p.anchor_per_seed,
        started.elapsed().as_secs_f64() * 1e3,
    );

    let stdout = std::io::stdout();
    let mut out = BufWriter::with_capacity(1 << 20, stdout.lock());
    let totals = if serve {
        // ---- framed stdin: u32 length + bytes per window, u32 length + payload per
        // answer. The same frame reader `mem --serve` uses, including its 16 MiB
        // refusal.
        let stdin = std::io::stdin();
        let mut input = stdin.lock();
        seeds_loop(
            &index,
            &sigma,
            Some(&ds),
            p,
            pool.as_ref(),
            move || {
                read_mem_request(&mut input)
                    .unwrap_or_else(|e| die(&format!("seeds --serve: request frame: {e}")))
            },
            |w: &WindowAnchors| {
                let b = anchor_frame_payload(w);
                out.write_all(&(b.len() as u32).to_le_bytes())?;
                out.write_all(&b)?;
                out.flush()
            },
        )
    } else {
        // ---- declared windows over a corpus: the byte ranges are the file's, not
        // an inference. Each window's text is `pread` into one bounded buffer (the
        // cap is the frame cap) and handed to the same pipeline.
        let wpath = windows_path.unwrap();
        let cpath = corpus_path.unwrap();
        let mut src = WindowSource::open(wpath, windows_bin);
        let corpus = File::open(cpath).unwrap_or_else(|e| {
            die(&format!("seeds: open corpus {cpath}: {e}"))
        });
        let corpus_len = corpus
            .metadata()
            .unwrap_or_else(|e| die(&format!("seeds: stat corpus {cpath}: {e}")))
            .len();
        eprintln!(
            "SEEDS_WINDOWS source={wpath} format={} corpus={cpath} corpus_bytes={corpus_len} \
             container_text_bytes={text_len}",
            if windows_bin { "bin" } else { "jsonl" },
        );
        let header = anchors_header(p, &docs_id);
        writeln!(out, "{header}").unwrap();
        writeln!(
            out,
            "window_id\tquery_offset\tcorpus_doc\tcorpus_offset\tmatch_len\tmultiplicity"
        )
        .unwrap();
        let ds_ref = &ds;
        let totals = seeds_loop(
            &index,
            &sigma,
            Some(ds_ref),
            p,
            pool.as_ref(),
            move || -> Option<Vec<u8>> {
                let w = src.next()?;
                // The window's byte range, resolved the way consult resolves it and
                // REFUSED when it cannot be resolved (never inferred).
                let (base, whole_doc) = if w.base == BASE_ABSENT {
                    let d = ds_ref;
                    if w.doc >= d.docs {
                        die(&format!(
                            "seeds: window doc={} is past the container's {} documents (m15)",
                            w.doc, d.docs
                        ));
                    }
                    (d.select(w.doc), w.whole_doc)
                } else {
                    (w.base, false)
                };
                let len = if whole_doc {
                    // the whole document: to the next start (or the text end)
                    let d = ds_ref;
                    let end = if w.doc + 1 < d.docs { d.select(w.doc + 1) } else { d.text_len };
                    end.checked_sub(base).unwrap_or_else(|| {
                        die("seeds: whole-document window has a start past its end")
                    })
                } else {
                    u64::from(w.len)
                };
                if len == 0 || len > SEEDS_WINDOW_MAX as u64 {
                    die(&format!(
                        "seeds: window len={len} outside 1..={SEEDS_WINDOW_MAX} (declared in the \
                         window file)"
                    ));
                }
                let start = base + w.start;
                if start + len > corpus_len {
                    die(&format!(
                        "seeds: window [{start},{}) leaves the {corpus_len}-byte corpus {cpath}",
                        start + len
                    ));
                }
                let mut buf = vec![0u8; len as usize];
                std::os::unix::fs::FileExt::read_exact_at(&corpus, &mut buf, start)
                    .unwrap_or_else(|e| die(&format!("seeds: read corpus {cpath}: {e}")));
                Some(buf)
            },
            |w: &WindowAnchors| {
                anchors_write_tsv(&mut out, w)?;
                out.flush()
            },
        );
        totals
    };
    let secs = started.elapsed().as_secs_f64();
    let rchar = proc_io("rchar").saturating_sub(rchar0);
    let syscr = proc_io("syscr").saturating_sub(syscr0);
    let (hits, misses, decodes, drops) = index.cache_stats();
    let pos_per_s = totals.positions as f64 / secs;
    eprintln!(
        "SEEDS_SERVE_STATS mode={mode} windows={windows} text_bytes={text_bytes} \
         windows_scanned={windows_scanned} seeds={seeds} rows={rows} positions={positions} \
         seconds={secs:.3} positions_per_s={pos_per_s:.1} lf_steps={lf_steps} \
         lf_steps_per_position={lps:.1} ranks={ranks} doc_absent={doc_absent} hits={hits} \
         misses={misses} decodes={decodes} drops={drops} rchar={rchar} syscr={syscr} \
         peak_rss_gb={rss:.3} cap_gb={cap:.3} arena_bytes={arena_bytes} jobs={jobs} \
         scan_ms={scan_ms:.3} locate_ms={locate_ms:.3}",
        mode = if serve { "serve" } else { "windows" },
        windows = totals.windows,
        text_bytes = totals.text_bytes,
        windows_scanned = totals.windows_scanned,
        seeds = totals.seeds,
        rows = totals.rows,
        positions = totals.positions,
        lf_steps = totals.lf_steps,
        ranks = totals.ranks,
        doc_absent = totals.doc_absent,
        rss = proc_peak_rss() as f64 / (1u64 << 30) as f64,
        cap = cap_bytes as f64 / (1u64 << 30) as f64,
        scan_ms = totals.scan_ms,
        locate_ms = totals.locate_ms,
        lps = totals.lf_steps as f64 / totals.positions.max(1) as f64,
    );
}

const SEEDS_USAGE: &str = "usage:\n  \
    xsa seeds <container> <text-file|-> [--target-mult T] [--max-len L] [--min-len m] \
[--max-seeds K] [--stride S] [--skip-markup] [--docs DIR] [--limit N] [--jsonl] [--nearest]\n  \
    xsa seeds <container> <text-file|-> --max-mult M [--min-mult m] ...   (band mode)\n  \
    xsa seeds <container> --serve -j N [--rss-gb G] [--cache-gb G]   (frames on stdin/stdout)\n  \
    xsa seeds <container> --windows FILE --corpus FILE [--windows-bin] -j N   (declared windows)\n  \
    xsa seeds <container> <text-file> --anchors [-j N]   (the anchor table, one window)\n  \
    xsa seeds <container> <text-file> --windows-out FILE --window-len L [--window-stride S]\n  \
  EMITS exact substrings of the text that are SPECIFIC and RECURRING: at every window \
position the seed LENGTH is solved so the multiplicity lands on --target-mult (default 50).\n  \
THE THREE INPUT FORMS run the SAME scan and answer the same rows: one text file (one window), \
a DECLARED window set (--windows), or framed stdin (--serve). The served forms open the \
container, audit it and resolve member m15 ONCE and then answer window after window in one \
process, which is the difference between a cold per-invocation command and an index that is \
resident for the whole transcript.\n  \
WINDOW PLACEMENT. --flow: the default is a CONTINUOUS scan of the text (one window per byte, \
windows=<text bytes>); --stride S samples every S-th POSITION inside a window (an opt-in \
sampler for the pre-fix behaviour, journaled as such). Neither has anything to do with where a \
WINDOW begins: in the served forms a window boundary is DECLARED - by one u32-length-prefixed \
frame on stdin, or by one record of the --windows file (XSWIN001 binary or the JSONL schema \
doc, base, start, len of `xsa consult`), never inferred from content. A window that leaves the \
corpus, or whose byte range cannot be resolved, is refused by name. --windows-out DECLARES a \
window set over a text file (--window-len L, --window-stride S) so a later run can REPLAY it.\n  \
THE ANCHOR TABLE (--anchors, and the only output of the served forms) is the chain-ready set \
of anchor rows, one per reported occurrence:\n  \
    window_id  query_offset  corpus_doc  corpus_offset  match_len  multiplicity\n  \
corpus_doc comes from the container's member m15 (the document START of the located offset) and \
is '-' when the container carries no m15 - a docid is never guessed. TIE ORDER: rows are sorted \
by (query_offset, corpus_offset, match_len, multiplicity), so two runs - and any -j - produce a \
byte-identical table. Serve mode sends the same table as fixed-width frames (u64 window_id, u32 \
rows, u32 seeds, then rows of u32 query_offset, u32 match_len, u64 multiplicity, u64 \
corpus_offset, u64 corpus_doc with all-ones = absent), one u32-length-prefixed frame per window.\n  \
CAPS ARE NOT SUPPORT THRESHOLDS. --max-seeds K, --limit N and --anchor-max-per-seed M are \
OUTPUT CAPS: they bound how many seeds are reported, how many sample positions a classic row \
carries, and how many anchor ROWS a seed contributes. The multiplicity reported is always the \
exact BWT interval width of the seed, so a capped table still tells you the true support \
(--anchor-max-per-seed 0 reports EVERY occurrence, one locate per occurrence).\n  \
--max-len default 64, --min-len default 12, --limit default 8, --anchor-max-per-seed default \
8, --max-seeds default 64. --skip-markup drops windows overlapping {{template}}, \
<!--comment-->, <ref>, <gallery>, <math> or {| ... |} scaffolding (OFF by default). --rss-gb G \
caps this process's resident set and sizes the decoded-block arena (an eighth of G) that the \
locate walks read through; --cache-gb G pins the arena alone. -j N sizes the thread pool the \
locate layer runs on (-j 1 is the serial baseline). See RESULTS_SEEDS_WARM.md for the layer \
cost table.";

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

    /// Build a text from (bytes, is_markup) pieces and compare the scanner's
    /// ranges against the table. No scanner code is reused: the expectation is
    /// the table itself, including the two cases a naive scanner gets wrong -
    /// nested `{{ }}` and `}}` / `{{` / `|}` INSIDE a comment.
    fn markup_table(parts: &[(&[u8], bool)]) -> (Vec<u8>, Vec<(usize, usize)>) {
        let (mut text, mut want, mut off) = (Vec::new(), Vec::new(), 0usize);
        for (bytes, is_markup) in parts {
            if *is_markup {
                want.push((off, off + bytes.len()));
            }
            text.extend_from_slice(bytes);
            off += bytes.len();
        }
        (text, want)
    }

    #[test]
    fn markup_scanner_marks_each_construct_and_nests_correctly() {
        let (text, want) = markup_table(&[
            (b"prose ", false),
            (b"{{Infobox|nested={{flag|US}}|x=y}}", true),
            (b" and ", false),
            (b"<!-- a }} {{ |} comment -->", true),
            (b" then ", false),
            (b"{| class=wikitable\n|-\n| a || b\n|}", true),
            (b" then ", false),
            (b"<ref name=x>citation</ref>", true),
            (b" then ", false),
            (b"<ref name=y />", true),
            (b" then ", false),
            (b"<gallery>\nFile:A.jpg\n</gallery>", true),
            (b" then ", false),
            (b"<math>x^2</math>", true),
            (b" then ", false),
            (b"{{{param|default}}}", true),
            (b" end", false),
        ]);
        assert_eq!(markup_ranges(&text), want);
    }

    #[test]
    fn markup_scanner_ignores_near_misses() {
        // `<reference>` is not `<ref>`, text braces are not templates, and a
        // construct that is not opened is not skipped.
        for text in [
            &b"a <reference>x</reference> b"[..],
            &b"a }} b { c |" [..],
            &b"a > b | } c"[..],
            &b"plain prose only"[..],
        ] {
            assert!(markup_ranges(text).is_empty(), "{:?}", String::from_utf8_lossy(text));
        }
        // An UNTERMINATED opener marks markup to the end of the text: that is
        // what a truncated article says.
        let text = b"prose {{unclosed and it runs off the end";
        assert_eq!(markup_ranges(text), vec![(6, text.len())]);
        // A tag with no `>` at all runs off the text too.
        let text = b"prose <ref name=q and no close";
        assert_eq!(markup_ranges(text), vec![(6, text.len())]);
    }

    #[test]
    fn markup_cursor_only_advances_and_answers_overlap() {
        let (text, _) = markup_table(&[
            (b"prose ", false),
            (b"{{a}}", true),
            (b" prose prose prose ", false),
            (b"{{b}}", true),
            (b" prose", false),
        ]);
        let mut m = Markup::scan(&text, true);
        assert_eq!(m.ranges, vec![(6, 11), (30, 35)]);
        assert_eq!(m.bytes, 10);
        // window starts are non-decreasing, as in the scan loop
        assert!(!m.overlaps(0, 6)); // ends exactly where the first block starts
        assert!(m.overlaps(5, 7)); // overlaps the first block
        assert!(!m.overlaps(11, 20)); // starts exactly at the block's end
        assert!(m.overlaps(30, 40)); // the second block
        assert!(!m.overlaps(35, 40));
        // disabled: nothing is marked, so nothing is ever skipped
        let mut off = Markup::scan(&text, false);
        assert_eq!((off.bytes, off.ranges.len()), (0, 0));
        assert!(!off.overlaps(5, 7));
    }
}

#[cfg(test)]
mod seeds_anchor_tests {
    use super::*;

    /// THE TIE ORDER, checked as an order: the sort key must be total on the
    /// fields the header promises, and it must put occurrences of one seed in
    /// corpus order regardless of the order the walks returned them.
    #[test]
    fn anchor_rows_sort_by_the_documented_tie_order() {
        let r = |q: u32, off: u64, len: u32, mult: u64| AnchorRow {
            query_offset: q,
            match_len: len,
            mult,
            corpus_off: off,
            corpus_doc: None,
        };
        let mut v = vec![r(5, 90, 12, 3), r(1, 70, 12, 3), r(5, 10, 12, 3), r(1, 70, 11, 9)];
        v.sort_unstable_by_key(anchor_key);
        assert_eq!(
            v.iter().map(|x| (x.query_offset, x.corpus_off, x.match_len)).collect::<Vec<_>>(),
            vec![(1, 70, 11), (1, 70, 12), (5, 10, 12), (5, 90, 12)]
        );
    }

    /// The serve frame is FIXED WIDTH and carries the absent-document sentinel:
    /// a client can walk it without a schema and can never read a missing docid as
    /// document 0.
    #[test]
    fn anchor_frames_are_fixed_width_and_encode_absent_docs() {
        let wa = WindowAnchors {
            window_id: 3,
            text_bytes: 5,
            rows: vec![
                AnchorRow { query_offset: 1, match_len: 2, mult: 7, corpus_off: 9, corpus_doc: Some(4) },
                AnchorRow { query_offset: 1, match_len: 2, mult: 7, corpus_off: 11, corpus_doc: None },
            ],
            windows_scanned: 1,
            seeds: 1,
            positions: 2,
            lf_steps: 0,
            ranks: 0,
            doc_absent: 1,
            scan_ms: 0.0,
            locate_ms: 0.0,
        };
        let b = anchor_frame_payload(&wa);
        assert_eq!(b.len(), 16 + 2 * 32);
        assert_eq!(&b[..8], &3u64.to_le_bytes());
        assert_eq!(u32::from_le_bytes(b[8..12].try_into().unwrap()), 2);
        let doc0 = u64::from_le_bytes(b[16 + 24..16 + 32].try_into().unwrap());
        let doc1 = u64::from_le_bytes(b[16 + 32 + 24..16 + 64].try_into().unwrap());
        assert_eq!(doc0, 4);
        assert_eq!(doc1, DOCID_ABSENT, "an absent document must be the sentinel, not 0");
        // the same rows in TSV carry '-' for the absent field
        let mut out: Vec<u8> = Vec::new();
        anchors_write_tsv(&mut out, &wa).unwrap();
        let s = String::from_utf8(out).unwrap();
        assert_eq!(s, "3\t1\t4\t9\t2\t7\n3\t1\t-\t11\t2\t7\n");
    }
}
