# C-term attack lane — report (THEORY LANE, /tmp/sxgc-laneC2)

Task: kill the cell-identification term C in the O(r + parse + C) suffixient-array
construction: given (rlbwt + C + run-end samples + direct-law phi-pieces with PLCP
samples + PFP artifacts), compute the per-(run,piece) cell extremes — equivalently
the restricted PSV/NSV at all run-boundary rows — in o(n) total work.

All numbers below are gated against brute (full SA/LCP/PSV/NSV) in the same process.
New files: tools/c_probe_common.py, c_probe_gate.py, c_probe_r1*.py, c_probe_r2_r1e.py,
c_probe_r3.py.  No existing file was modified.  Nothing was committed.

--------------------------------------------------------------------------------
## 0. Baseline + a correction to the premise (c_probe_gate.py)

The baseline reproduces MODEL v3 exactly: with DIRECT-LAW pieces (piece continues
at p iff PLCP[p-1] == PLCP[p]+1) and the event set boundary ∪ both per-cell extremes,
FM chi is reproduced on 124/124 of the fmTexts battery and on every aligned text.
The numpy SA used throughout is validated against the gated pure-python SA (60/60).

CORRECTION (important): with the correct direct-law pieces the number of nonempty
(run,piece) cells P is NOT O(r).  Measured P/n and P/r:

    random-4-20k   : P = 20,001  = 1.00n  (1.34r)
    satellite-18k  : P =    509           (1.40r)
    HOR-nested     : P =    323           (1.39r)
    duplicates-600k: P = 595,474 = 0.99n  (40.5r)
    dup+unique-120k: P = 119,941 = 1.00n  (2.23r)
    random-bin-20k : P = 19,994  = 1.00n  (2.02r)
    random-4-200k  : P = 199,998 = 1.00n  (1.33r)

So the previously recorded "P <= 3r" (RESEARCH.md, measured under the *phi-condition*
piece definition) does not carry over to MODEL v3: with direct-law pieces, P is
Theta(n) on random-like and duplicate-heavy text.  The v3 cell-extreme event set is
therefore itself a Theta(n) object in those regimes, and "compute the cell extremes"
cannot be the O(r) target.  What we actually need is the weaker, correct target:
restricted PSV/NSV AT BOUNDARY ROWS (for the FM sweep), which may be preserved by a
much smaller restricted set.  Note also (measured): the v3 cell set does not even
reproduce PSV/NSV exactly at every boundary row — it can differ by one answer at a
handful of rows while still yielding the same chi (satellite 1, HOR 2, random-bin 1).
The right correctness target is the sweep result (chi and the witness set), not
pointwise PSV/NSV equality.

--------------------------------------------------------------------------------
## 1. R3 — THE INVERSE MAP (position -> row): SOLVED, gated 100%

This is the lane's headline result.  The forward map (pfpds::pfp_sa_support:
row -> class -> wavelet range_select -> saP -> occurrence start - m.len) is
invertible with structures that are all natural PFP artifacts:

    row(pos):
      if pos >= |T|:                       row = pos - |T|     (the W dollar rows)
      else:
        o    = the phrase occurrence containing pos      (rank/select on b_p)
        s    = the first occurrence start > pos with s - pos >= W, else n
               (near-end-of-phrase positions skip boundaries closer than W:
                the build loop only creates classes for suffix length >= W)
        p_i  = (s's occurrence index) - 1                [s = starts[p_i+1]]
        mlen = s - pos
        pid  = parse[p_i]
        ds   = select_b_d(pid) + (len_phrase(pid) - mlen)   (dictionary offset)
        cls  = cls_of_ds[ds]        (dictionary-suffix-start -> class index)
        k    = isaP[p_i+1] - 1, except p_i = |parse|-1 -> k = isaP[0] - 1
        row  = boundaries[cls] + rank of k in class_rows(cls)

Query cost: O(log) (rank/select, one bisect / wavelet range_rank).
Space: O(parse + dict + |M|); the ds->class table is O(dict); class_rows is the
wavelet range_rank in the real implementation (O(parse log)); isaP is O(parse).
In a real implementation the wavelet range_select/rank replace the materialized
class row lists, so the structures stay O(parse + dict + |M| bits/ints).

GATE (resolve_inv(resolve(i)) == i for EVERY machinery row):

    random-4-20k    ok=  20010 bad=0 unresolved=0 / 20010
    satellite-18k   ok=  18510 bad=0 unresolved=0
    HOR-nested      ok=  18370 bad=0 unresolved=0
    duplicates-600k ok= 600010 bad=0 unresolved=0
    dup+unique-120k ok= 120010 bad=0 unresolved=0
    random-bin-20k  ok=  20010 bad=0 unresolved=0
    random-4-200k  ok= 200010 bad=0 unresolved=0

Conventions pinned empirically (each was a real trap; recorded so nobody re-derives
them wrongly): the W dollar rows; the "first boundary at distance >= W" rule; the
cyclic last-phrase handling (k = isaP[0]-1); ds uses the dictionary phrase length
(len_phrase, not span+... — an off-by-W here cost a full debug cycle).

VERDICT R3: ALIVE and SOLVED.  Position-space structures are now enumerable, and
the primitive "row -> position" (forward, gated earlier) + "position -> row"
(inverse, gated here) are both O(log) from PFP artifacts.  This also removes the
dependency on LF-walks for turning positions into rows.

--------------------------------------------------------------------------------
## 2. R1 — PIECE-TAIL / boundary-anchored candidates: PARTIALLY ALIVE

Structural measurements first (all gated):

* Answer closure size |{PSV[i], NSV[i] : i boundary}| / r:
    0.67 (random-4-20k/200k), 0.69 (satellite), 0.69 (HOR), 0.74 (duplicates),
    0.67 (dup+unique), 0.97 (random-bin).   => the ANSWER SET is O(r).
* Closure rows' POSITION distance to the nearest piece boundary:
    p50 = 0, p90 = 1..2 on all texts; p99 up to 4; but the MAX is 300 (duplicates)
    and 301 (HOR), 294 (dup+unique), 8 (random-bin).
* Closure rows that ARE run-end rows (whose SA we already hold as samples):
    80% random-4, 73% satellite, 86% HOR, 23% duplicates, 68% dup+unique,
    62% random-bin, 80% random-4-200k.
* Closure row-gap |answer - query| in ROW order: p50 = 2 (1 for random-bin,
    50 for duplicates); p90 = 8..20 (352 duplicates); p99 up to 6,025 (satellite)
    and max 149,249 (duplicates).  So answers are usually row-close but the tail is
    unbounded.

Candidate rules tested (rows selected, then a stack pass; checked for exact PSV/NSV
at boundary rows and for chi preservation):

  A: boundary ∪ run-ends                       size 1.25-1.78r   NOT chi-exact
  B: A ∪ piece-boundary position-window d      size 1.33-3.10r   chi-exact at
                                                d=1..5 for all but duplicates
  C: boundary ∪ both cell extremes (= v3)      size 1.34r-40.8r  chi-exact, but
                                                40.8r ≈ n on duplicates
  D: boundary ∪ run-ends ∪ row-window w ∪ position-window d    <-- BEST

Rule D results (exact PSV/NSV / chi / size):

    text              D(d=1,w=2)          D(d=1,w=8)          D(d=3,w=8)
    random-4-20k      exact  1.34r        exact  1.34r        exact  1.34r
    satellite-18k     exact  1.42r        exact  1.52r        exact  1.52r
    HOR-nested        2 bad,chi ok 1.48r  2 bad,chi ok 1.89r  exact  1.91r
    dup+unique-120k   39 bad 1.92r        exact  2.23r        exact  2.23r
    random-bin-20k    29 bad 2.01r        exact  2.02r        exact  2.02r
    random-4-200k     1 bad,chi ok 1.33r  exact  1.33r        exact  1.33r
    duplicates-600k   196 bad CHIBAD 4.1r 138 bad CHIBAD 11.7r 3 bad,chi ok 12.0r
                      (needs D(d=3,w=128): chi ok at 34.6r; exact only at 40.8r ≈ n)

VERDICT R1: ALIVE for every tested regime EXCEPT duplicate-heavy text, where it
degenerates to Theta(n) (no o(n) rule among the families tested).  The blockers,
stated exactly, from the counterexample data:

  (i) long pieces.  Piece lengths for duplicates-600k: p50=2, p90=298, p99=597,
      max=898.  The 925 closure rows at position-distance > 10 all live deep inside
      these pieces (examples: d=300 NSV q_row=331545 LCP=603 -> ans_row=331546
      LCP=600 pos=171300 in piece [171000,171894]; d=299 PSV q_row=466518 LCP=302
      -> ans_row=466512 LCP=301 pos=491699 in [491400,491998]).  Within a piece PLCP
      decreases by 1 per position, so a long piece spans a long interval of LCP
      values and its interior supplies answers for thresholds deep inside it.
  (ii) run-ends (samples) do not capture duplicates: only 23% of closure rows are
      run-end rows there (vs 73-92% elsewhere).
  (iii) row-gaps are unbounded: max 149,249 rows for duplicates (p50=50).

So for the web-text-like regime (duplicate-heavy), no o(n) restricted set was found;
the exact set is Theta(n).  Everything else — including the DNA-degenerate regimes
(satellites, HOR) and random text at 20k/200k — has an O(r)-sized exact (or
chi-exact) candidate set, enumerable in position space via R3 with O((r + d·iv)·log)
work.  Note the same regime split found by the earlier gap-probe: duplicates are the
recurring obstacle, now for the C-term as well.

--------------------------------------------------------------------------------
## 3. R2 — per-run position extremes / LF-image recursion: NOT ALIVE standalone

The LF-image identity is exact and trivial (LF is a bijection on a run's rows, and
SA[k] = SA[LF(k)] + 1), so min/max over a run = 1 + min/max over its image interval.
But it does not close on samples.  Measured, for each run: how far (in row offset)
the argmin/argmax SA row sits from the run's ends, and how often an extreme is
AT a run end (i.e. a row whose SA we already have, or the run's first row):

    text              offsets p50 p90 max     extremes AT a run end
    random-4-20k      0 1 6                    7279/8000  (91%)
    satellite-18k     0 1 6000                 656/728    (90%)
    HOR-nested        0 1 5939                 428/464    (92%)
    duplicates-600k   11 47 252                2643/8000  (33%)
    dup+unique-120k   0 2 14                   6528/8000  (82%)
    random-bin-20k    0 2 9                    6235/8000  (78%)
    random-4-200k     0 1 5                    7362/8000  (92%)

So for most texts a run's extreme is attained at one of its two end rows (≈90%),
but the residue is unbounded (satellite offsets to 6000; duplicates p90=47).  The
recursion "descend to the image interval" maps the question to an ARBITRARY row
interval [L, L+len) whose own min/max is not derivable from O(polylog) samples:
measured extremes can sit deep inside intervals, and there is no local rule
(the offset distributions above are exactly that statement, one level down).
No closed form, no bounded-probe rule: R2 as a standalone route is dead.  Its
measurement ("≈90% of run extremes are at run ends") is nevertheless a useful
ingredient for hybrid heuristics (it is why rule D's run-ends component helps).

EXACT OBSTRUCTION (R2): "min/max of SA over an arbitrary row interval" is a
range-min/range-max query over a permutation with no compressed representation
among the available structures.  Samples give O(r) point values; the parse gives
O(log) point values (forward and inverse); neither gives interval extremes, and
the measured offset distributions show interval extremes are not concentrated at
interval ends or at sampled rows.

--------------------------------------------------------------------------------
## 4. Summary verdict

  R3 (position -> row):      ALIVE — SOLVED, gated 100% on all 7 battery texts.
                             O(log) query, O(parse + dict + |M|) space.
                             Unblocks position-space enumeration for everything else.
  R1 (candidate sets):       PARTIALLY ALIVE — O(r)-sized exact/chi-exact candidate
                             sets exist and are enumerable (rule D) for random text
                             (20k, 200k), satellites, HOR, mixed duplicate+unique;
                             DEAD for duplicate-heavy text (blocker: long pieces +
                             non-sample answers + unbounded row gaps; exact set is
                             Theta(n) there).
  R2 (run extremes/LF rec.): NOT ALIVE standalone (unbounded interval extremes;
                             no bounded-probe rule).  Measurement retained as an
                             ingredient.

Net effect on the O(r + parse + C) claim: C can be reduced to O((r + iv)·polylog)
for the non-duplicate regimes via rule D + R3, but NO o(n) rule was found for the
duplicate-heavy regime (which is exactly the web-text regime and the regime that
already forced the parse pivot in the gap analysis).  The honest revised target is
therefore conditional on the text family, and the duplicate regime needs either a
new idea (e.g. an RMQ/"min-LCP over row interval" structure built from the PFP
classes, or a duplicate-aware piece decomposition) or must accept the Theta(n)
candidate set.

Recommended next probes (concrete):
  1. RMQ route: can "min LCP over a row interval" be answered from the PFP class
     structure?  Within a class, row ranges correspond to k-ranges and the positions
     are starts[p_i+1] - mlen (mlen FIXED per class) — so a class-internal min over a
     row range reduces to a min over the occurrences p_i in a k-range of PLCP at
     (starts[p_i+1] - mlen).  Worth attacking: it may give O(log) range-min, which
     would make NSV/PSV exact in O(r log^2) with no candidate-set guesswork.
  2. Duplicate-aware pieces: measure whether the long pieces of duplicate-heavy text
     align with repeated-paragraph boundaries (they appear to: p90 piece length 298
     vs 300-byte paragraphs), and whether answers then live at paragraph boundaries.
  3. Rule D's minimal (w,d) as a function of (r, iv, max-piece-length): fit a rule
     and test on HPRC-scale data if a lane ever gets there.

--------------------------------------------------------------------------------
## 5. Duplicate-aware follow-up: the long pieces ARE the copies (measured)

Executed measurement for report idea 2 (duplicates-600k, 40 distinct 300-byte
paragraphs x 50 shuffled copies):

  * piece length histogram (top): 1 -> 3547, 2 -> 2443, 3 -> 1213, 4 -> 460,
    298 -> 354, 299 -> 352
  * 962 / 9656 piece starts lie exactly on the 300-byte copy grid
    (expected by chance ~32 => ~30x enrichment)
  * 945 / 1766 long pieces (length >= 50) start exactly on the copy grid

So the long pieces that block rule D are precisely the repeated-paragraph spans:
their length (~298-299) is the paragraph length and their starts are copy starts.
A duplicate-aware candidate rule therefore does not need to guess d: it can take the
candidate positions from the PHRASE-OCCURRENCE structure (every copy's span is a
run of parse occurrences; its start positions are already in the parse), enumerate
the copy interiors, and resolve their rows with R3.  Whether the answer rows inside
copies can be derived from the corresponding rows of the FIRST copy (a copy-conjugacy
argument, the same idea that motivated the parse pivot) is the natural next question;
the present lane establishes that the blocking structure is copy-aligned, not diffuse.

## 6. Files

  tools/c_probe_common.py   gated baseline (numpy SA validated 60/60; build();
                            chi_full(); chi_events_v3(); batteries)
  tools/c_probe_gate.py     MODEL v3 gate (124/124) + battery table
  tools/c_probe_r1.py       candidate-set exactness checks + closure measurements
  tools/c_probe_r1b.py      distance windows (first cut)
  tools/c_probe_r1c.py      windows via the R3 inverse
  tools/c_probe_r1f.py      run-ends + windows (rules A/B/C)
  tools/c_probe_r1g.py      hybrid rule D (row-window + position-window + run-ends)
  tools/c_probe_r2_r1e.py   closure row-gap/LCP-drop + per-run extremes
  tools/c_probe_r3.py       INVERSION from PFP artifacts (R3) — 100% gated
  tools/C_LANE_REPORT.md    this report
