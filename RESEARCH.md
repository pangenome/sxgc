# Research notes — sxgc

## 0. Production-path update (substrate pivot)

χ derivation from **compressed-space LCP via TeraLCP thresholds** is now the
sA **production path**: TeraLCP's `--thr-pfp` 5-byte threshold files feed the
one-pass suffixient scan (rung 4a.3; yeast χ gate = 85,404,240), replacing
the PFP-era component route for production. The in-house PFP machinery
remains the byte-gated yeast reference and the χ-legacy path. The χ_tag
research below is unchanged by this pivot — its open problems (Def. 9-analog
for annotated suffix arrays, (char, tag)-pair construction) are now framed
against the RLBWT/TeraLCP substrate instead of the PFP scan.

## 1. Tag-suffixed sA (near-term, engineering): graph-space anchors at χ scale

The sA already samples χ positions — one occurrence per right-extension. Each
AGC contig maps into the HPRC v2 graph via its path (local sidecars:
`hprc-v2.0-mc-chm13.pansn.traversal.bed`, `pansn.chrompos.bed`,
`covering_set.bed`). Carry each sA entry's graph coordinate alongside its text
position:

```
sA entry x -> (text position, graph (node, offset, strand) via path bed)
```

Result: all MEMs + **one graph-space anchor each** at χ-scale footprint
(~15-25 GB @ HPRC v2) — the tier-1.5 that otherwise needs the 86 GiB tag
array. Dedup set not included (that's Tier 2 + tags). No new theory; sidecar
extension only. Build as part of Phase 6's sidecar work.

## 2. Open question: χ_tag — a suffixient characterization of the tag array
*(the "one more round" idea)*

**Motivation.** The tag array (WABI 2025) is run-compressed document listing:
BWT interval -> distinct graph locations. It is the least compressible layer
of the stack: 11.1 B tag runs @ HPRC v2 (86 GiB) vs 2.53 B BWT runs — tags
change faster than the BWT. The χ idea applied to it: for every right-maximal
string w and every graph location g that an extension of w leads to, sample
ONE BWT position capturing the (extension -> g) pair. Call the minimum such
set **χ_tag** — a suffixient set over the (context, tag) product.

**Query shape.** MEM -> binary search over χ_tag + oracle LCP -> distinct
graph locations directly — the dedup set without the full tag array and
without LF-walking every occurrence.

**Hypothesis.** χ_tag is bounded by *graph-branching* contexts (where tagged
traversals diverge) — tracking graph nodes/edges and variant density, not
total sequence. Given tag runs already stay flat with 5x sequence (9.86B ->
11.1B), χ_tag plausibly sits far below 11.1B runs — potentially the smallest
graph-annotated seed artifact measurable, and a **new repetitiveness measure
for variation graphs** (the χ <= 2r̄ line's natural follow-up: χ, χ_tag over
annotated BWTs).

**Open problems (write down honestly).**
- Coverage condition differs from Def. 9: requirements are (right-extension ->
  *distinct tag*); tags are a path-dependent annotation, not an alphabet. Need
  a Def. 9-analog for annotated suffix arrays and a construction in the
  streamed PFP scan (the (BWT char, tag) pair is computable in the same pass).
- Non-monotonicity likely carries over from our right-extension experiments
  (chi can *decrease* under text appends — baab+`a`: 3->2): chi_tag under
  graph edits (adding haplotypes/paths) is probably non-monotone too, so
  dynamic maintenance is open.
- Lower bounds: is chi_tag <= chi * (something like branching factor)? Relation
  to the smallest string attractor of the tagged text?

**First experiment.** On the HPRC v2 3-sample smoke chroms: compute the exact
(context, tag) pair count (brute force over suffix-tree edges with bed-derived
tags) vs tag-run count vs chi; extrapolate the ratio to the full v2 graph to
bound chi_tag's plausible size before building anything.

## Status

- [ ] Tier 1.5: graph-space anchors via path beds (Phase 6 sidecar work)
- [ ] chi_tag brute-force experiment on smoke chroms
- [ ] chi_tag: formal definition (annotated-suffix-array Def. 9-analog)
- [ ] streamed PFP construction emitting (char, tag) pairs

## Bit-2: r-space construction of chi — the scatter problem, formal program (2026-09-24)

North star: construct chi from the rlbwt + phi structure in O(r) TIME.
Search was closed in r-space by the r-index; chi closed the MEASURE;
r-space CONSTRUCTION is the missing chapter. Either resolution closes it:
an O(r) algorithm, or an impossibility theorem in a stated model.

### The structural fact (the handle)

PLCP along text order is piecewise-LINEAR with O(r) pieces, slope -1
per piece: within phi-interval I starting at text position st,
lcp(pos) = PLCPsamples[I] - (pos - st). (This is exactly lcp_step in
teralcp_chi — already load-bearing in production.)

### The reduction (what needs proving in Bit-2)

chi-selection = per-run interior LCP minima. On the piecewise-linear
structure, the minimum of a run r's rows restricted to one interval I
is attained at the LARGEST text position of r's rows inside I (the
piece decreases). Therefore:

  chi is computable in O(r) time  <=>  the extreme points
  E(r, I) = argmax{ pos in run r's rows within interval I }
  over all intersecting (r, I) pairs are derivable in O(r) time
  (value + position, without enumerating run rows).

The O(n) cost of the current walk hides ENTIRELY in the scatter: a
run's rows are distributed across phi-intervals in a pattern that
encodes the run/DAWG structure. The question is whether that incidence
compresses.

### Theorem targets, in order

- Bit 1b (prerequisite, sorry-elimination): prove `covering_given_stream`
  and `minimality` — the LCP-maxima characterization (Lemma 34 analog)
  connecting the one-pass scan's selected set to Def. 9's suffixient
  property. This is the foundation both targets stand on.
- Bit 2a (formalize the handle): define phi-intervals over (rlbwt,
  PLCPsamples), prove lcp(pos) equals the piecewise-linear evaluation,
  and prove the reduction: interior-min selection equiv. per-(r, I)
  extreme points. Pure definitional work; Lean-suitable end to end;
  executable-testable on small texts (Bit-1 pattern: #eval verification).
- Bit 2b (the open theorem, two doors):
  (A) constructive: exhibit an O(r)-time procedure computing E(r, I)
      — verify the bound and the correctness in Lean; ship it in
      teralcp_chi as --rspace; the walk's 38 h collapses.
  (B) impossibility: prove that row-enumeration is necessary in a
      stated model (e.g., access to lcp only via interval queries and
      run boundaries) — formalize the STATEMENT in Lean, prove on
      paper; redirects to O(r log r) / sampled chi.

Both doors publish. Lean's role: 1b and 2a fully formal; 2a makes the
scatter question precise enough to attack; 2b(A) fully verifiable if
we win; 2b(B) statement-formal, proof on paper.

### Motivation, recorded verbatim as the product claim

"the whole web fits on my laptop, searched in real time": FineWeb-EDU
~10 TB -> ~1 TB index (measured ratios) -> a fat laptop's SSD; queries
already microsecond-class (gated); what O(r) construction buys is the
REFRESH: quarterly re-sort/rebuild of a 10 TB corpus in hours instead
of the ~week the O(n) walk implies at that scale. Byte-alphabet
substrate (specced, ROADMAP 5) + O(r) construction + this query engine
= the laptop-web stack, complete.

### Conjecture (2026-09-24, session): O(r log r) chi via the FM route

Working bet, sharpened by the slice-2 negative result (chi selection is
NOT a per-row predicate -> any r-space route must be global): the
FM/PSV-NSV characterization (sA/suff-set-src/fm.cpp; reproduces the scan
exactly on all tested texts) is the declarative spec to target. If
PSV/NSV structure of the LCP stream can be computed from the
run-compressed representation (piecewise-linear PLCP pieces + run
boundaries + range-minima over the O(r) pieces), a divide-and-conquer
suggests O(r log r) — the plausible landing zone between the O(n) walk
and the possibly-unreachable exact O(r). Slice 3 (in flight) formalizes
the scan<->FM equivalence and states the r-space bridge lemma. If the
equivalence closes, the O(r log r) question becomes a well-posed
complexity question about PSV/NSV over run-compressed streams — attack
with the piecewise-linear handle (Bit 2a) + RMQ machinery.

## Bit-2 measurements (2026-09-25 night): the scatter is O(r); LF-images are
## free on balanced texts, degenerate on mega-runs (tools/scatter_probe.py,
## tools/lf_image_probe.py; brute SA/LCP, fm.cpp port, production phi law)

Three measurements, all at <=500k brute-verified scale:

1. THE SCATTER IS LINEAR. #(BWT-run x phi-interval) incidence pairs
   P = ~1.2r on EVERY family (random bin 1.33-1.38r, random 4-letter
   1.23-1.26r, periodic satellites 1.24-1.28r, (ab)^k 1.15r, run-length
   1.2r), flat in n. The answer set of any per-pair chi algorithm is
   O(r)-sized. The pieces themselves are O(r) (production phi law
   phi(p-1) != phi(p)-1; slope law verified exact, law-bad=0 everywhere;
   an earlier stronger text-adjacency condition was WRONG and inflated
   pieces to Theta(n) on repetitive texts — condition corrected).

2. BIT 2a AS STATED IS FALSE (negative result, caught by experiment).
   "FM witness = per-(run, piece) argmax text position" fails for ~22%
   of witnesses on every family. The FM selection is decided in ROW
   order (PSV/NSV over the LCP stream; witnesses are run-HEAD rows),
   not text order. Restatement required before Lean grinding:
   chi_from_run_heads.

3. LF-IMAGE RESOLUTION IS TWO-REGIME. LF maps a run's rows onto a
   consecutive row block computable in O(1) from C + run lengths
   (.ri4 data, no walking); a run-end sample at (or a toehold walk from)
   the image's end resolves the run's tail position (image start -> head).
   Measured aggregate walk cost over all runs:
     random-4 100k/500k:  0.67r / 0.66r total, max walk 7/8, 75% instant
     random-bin-100k:     5.99r total, one max-walk=n orbit (2-letter
                          rotation effect), 49.9% instant
     satellite-100k:     518.79r, max walk = n   (r=771: mega-runs)
     (ab)^50k:           25,001r, max walk = n   (r=4: total degeneracy)
   Reading: balanced texts (n/r = O(1)) get a TRUE O(r) construction
   shape; the blowup is carried entirely by MEGA-RUNS — the same objects
   that broke 466 locate (walks to 393,160 steps). P stays 1.2r even on
   satellites: the ANSWER is always O(r); position-RESOLUTION cost is
   what degenerates. HPRC (n/r = 512) is a mixed regime.

DECISIVE NEXT MEASUREMENT: port the walk simulation into xsa (pure
r-space work on the .ri4: C, runs, samples, optional --anchors) and run
at k=10 (R=1.86B) and 466 (R=2.74B): sum-walk/r with and without the
anchor table. Anchors terminate exactly the walks that cross string
starts — the same class as the 466 locate bads. If 466 sum-walk/r is
O(1)-ish, the O(r) construction is real for pangenomes and the theorem
program is: (i) LF-image lemma (image blocks O(1) from C+runs), (ii)
chi_from_run_heads (restated Bit 2a), (iii) balanced-case O(r) theorem,
(iv) mega-run handle (the named open problem — periodicity structure of
long runs is the candidate lead; anchors are the engineering mitigation).
Worst case is never worse than production's Theta(n) walk: mega-runs
are few; hybrid = LF-image for the many + walk for the few.

## Bit-2 model v2 (2026-09-28): the events conjecture — MEASURED GREEN

Negative result #4 (cheap catch): FM with PSV/NSV restricted to BOUNDARY
rows undercounts chi (~half on binary texts) — interior rows genuinely
interpose in the next/previous-smaller-value decisions.

The corrected two-layer model, both layers now measurement-backed:
- LAYER 1 (provable): every FM witness is a run-BOUNDARY row (head or
  tail); from the fmSpec port, candidates are only ever set at boundary
  rows ip in {i-1, i}.
- LAYER 2 (the events conjecture — 5/5 GREEN: random-bin/4-letter
  n=500/2000 + satellite-1200): FM with PSV/NSV restricted to the event
  set B = {boundary rows} UNION {(run, phi-piece) pair-extreme rows}
  computes chi EXACTLY. Boundary-only fails; boundary+extremes matches
  full chi everywhere tested.

Consequence: chi is a function of an O(r)-sized event set (|B| <=
2r + P, P ~ 1.2r measured). The O(r) construction reduces to:
  (a) positions/LCPs of event rows — pair-extreme positions are the
      E(r,I) derivation (LF-image route resolves run heads/tails at
      0.66r on balanced texts; pair-extremes are interior rows — the
      remaining derivation core, Bit-2b door);
  (b) a stack sweep over events in row order — O(|B|).
Worst-case fallback: O(r log r) D&C over the pieces with P = 1.2r
crossings. Lean scaffold updated (ScatterOofR <= 2r after measuring
1.2r; chi_from_psvnsv RETIRED as measured-false on both sides;
witnesses_at_boundaries + lf_image_consecutive + chi_from_events
stated, chi_from_events gated on the Python 5/5 differential).

## Bit-2 measurements RE-DERIVED with a CORRECT SA (2026-09-29)

MEASUREMENT-TOOL BUG (negative result #5, caught by Lane C's Lean
differential): tools/scatter_probe.py + lf_image_probe.py prefix-doubling
suffix_array assigned DISTINCT initial ranks to equal characters — wrong
SA for every text with repeats. All 2026-09-25/28 Python probe numbers
were artifacts. Fixed (dense tie-aware ranks), verified 150/150 vs
brute incl. heavy-repeat stress; both files carry the lesson.

Corrected numbers (same batteries, correct SA):
- SCATTER: P/r in [1.25, 2.02] (binary ~2.0, 4-letter ~1.33, satellites
  ~1.3, run-length 1.25). P = O(r) SURVIVES; ScatterOofR restated <= 3r.
  P/chi ~ 1.4-2.5. blocks/run ~ P/r (per-run intervals still isolated).
- NEW CLEAN FACT: #phi-intervals = r EXACTLY on every text tested
  (random/satellite/repeat/run-length, n to 100k). Theorem candidate:
  phi-interval breaks <-> BWT run boundaries, 1:1, for the 0-terminated
  single-string convention.
- PIECE LAW: law-bad = 0 everywhere, re-verified (production law exact).
- BIT-2a "22% FALSE" WAS ALSO AN ARTIFACT: with the correct SA the FM
  witnesses sit at per-(run,piece) argmax-SA rows (argMIN text position
  = the cell's LCP-MAXIMUM) with only 0-2 exceptions per text, and the
  exceptions are off-by-one/boundary rows and sentinel-adjacent prefix
  rows. The original text-order argMAX statement had the DIRECTION
  wrong; the corrected direction is nearly exact. Model v3: restate,
  characterize the residual convention, re-battery.
- LF-IMAGE: random-4 0.66-0.67r (unchanged), random-bin 1.99r (was
  5.99r under the bug — the giant orbits were artifacts), max walk
  9-16 on balanced texts. Mega-run degeneracy CONFIRMED REAL:
  satellite-100k 395r (max walk 98,992), (ab)^k 133,330r. Two-regime
  split stands with clean constants.
- EVENTS (Layer 2): with correct SA and the argmax-SA extreme
  convention, 121/124 (smallest failure T=[2,2,2,1,1]); boundary-only
  still fails. Near-miss, not theorem; residuals same class as the
  witness-argmax off-by-ones. v3 task: nail the boundary/off-by-one
  convention.

Lean ledger after Lanes B+C landed (review-as-squash, this commit):
PROVEN: chi_le_of_suffixicient (B), lf_image_consecutive (C).
SORRY 6: chi_eq_maxClasses (A in flight), covering_given_stream (needs
SA/LCP theory: lcpOf correctness, saOrder sortedness), minimality
(half proven; lower half needs covering + maxClassCount),
fm_equivalence (route FOUND: the event-bridge — scan and fmSpec emit
the IDENTICAL (char,pos) event multiset, 3,226 texts 0 mismatches),
witnesses_at_run_edges (corrected Layer 1, 0/729, provable),
chi_from_events (open door). Statements corrected: ScatterOofR <= 3r,
witnesses boundary-only RETIRED (false), isRunEdge restatement locked.

Depth-ladder appendix (correct SA, 2026-09-30): nested satellites at
repetition depth 1-4 measure P/r = 1.34-1.39, iv = r exactly — the
nested-sat P/r = 3.20 in the 500k battery was the SA bug's last
artifact (that process had loaded the pre-fix module). No
depth-dependent scatter inflation through depth 4; ScatterOofR <= 3r
comfortable. Lane A landed both targets the same night: chi_eq_maxClasses
PROVED (privacy lemma: distinct maximal-class representatives have
DISJOINT witness sets), plus chi_le via the ordered-blocks head bound.
Ledger after all three lanes: PROVEN chi_eq_maxClasses,
chi_le_of_suffixient, lf_image_consecutive; minimality half-proven;
remaining: covering_given_stream (needs lcpOf correctness + saOrder
sortedness), minimality lower half, fm_equivalence (event-bridge
route), witnesses_at_run_edges (corrected L1), chi_from_events (open
door, 121/124).

## Bit-2 cost model v3 (2026-10-01): the gap conjecture — measured, and the
## human-text obstacle found where predicted (tools/gap_probe.py)

CORRECTION (from working the lockstep accounting honestly): lockstep
block-walking saves NOTHING asymptotically over the production walk —
unresolved members are not contiguous, so block-splits track member
resolutions one-for-one; both are the O(n) walk in different clothes.
The ONLY structural saving is JUMPABLE GAPS:

    construction cost = O(r + sum of NON-JUMPABLE sample-gap lengths)

Walk identity (validated 50/50): walk depth == position - sample,
i.e. every toehold walk is a linear scan of a sample gap in text space.

THE REFINED CONJECTURE: sum |non-jumpable gaps| = O(r).  Battery:
- random-4/bin 20k: 0.34r / 0.95r (trivial: n ~ r)
- satellite-18k: 0.29r — the 17,994-byte mega-gap is PERIODIC
  (period 3): cycle-jumpable. HOLDS spectacularly.
- HOR-nested (alpha-sat analog): 0.34r — 17,755-byte gap period 301.
  HOLDS.
- repeat-ab: 0.00r after a terminator-pollution fix (the text is
  maximally periodic).
- **duplicates-600k (the HUMAN-TEXT analog: 40 unique paragraphs x 50
  shuffled copies): 39.91r = 0.976n — the conjecture FAILS by 40x.**
  Copies are non-periodic: pure periodicity does not compress
  duplicate-driven repetitiveness.  Exactly the regime web text lives
  in (boilerplate, quotes, licenses — copies, not satellites).
- dup+unique 120k: 1.31r (mixed: fine because r is high).

UNIFIED THEORY OF GAPS (the construction's cost model):
  gaps come in three kinds —
  (1) PERIODIC (DNA satellites, HOR): cycle-jump, cost O(period);
      validated mechanism (walk identity + period detection).
  (2) DUPLICATE (human text): a copy's walk is row-space-CONJUGATE to
      the first copy's walk — the phrase-jump.  This is exactly the
      structure PFP already parses (duplicates = repeated phrases;
      PFP parse of duplicate-heavy text ~ O(distinct phrases) ~ O(r)).
      We own the machinery (pfp++ heritage, the sA PFP chain).
  (3) IRREDUCIBLE (random-ish): gaps are tiny anyway; sum ~ O(r).
  Theorem target: cost = O(r + sum periods + sum phrase costs), and
  both jump families cover the measured degenerate cases.
NEXT: (a) 466 gap scan (running: .ri4 samples + anchors + flat reads,
  pure O(r) work) — gap quantiles + top-gap periodicity at 2.74B runs;
  (b) prototype the cycle-jump engine on the battery (satellite: gaps
  resolve in O(period) — validate positions vs brute);
  (c) prototype the phrase-jump on the duplicates battery (PFP parse
  of the duplicate text; positions via phrase-index arithmetic);
  (d) then yeast; 466 only for confirmation.

## Bit-2 unification (2026-10-01): the taxonomy dissolves into the parse

CORRECTION (user's call, conceded): the three-kind gap taxonomy was
handwavy — gap_probe's "non-jumpable" was really "non-PERIODIC under the
only test implemented." Duplicates were never unjumpable; the probe was
too weak. The 39.9r number measured the test, not the data.

THE UNIFIED REPLACEMENT, measured (tools/parse_probe.py):

    text            n       r     parse   =r     (old 'nonper')
    satellite-18k   18500   369    3      0.01   140
    HOR-nested      18360   227    66     0.29   86
    duplicates-600k 600000  14835  7195   0.49   585161
    dup+unique-120k 120000  58948  1273   0.02   61013
    random-20k      20000   15045  199    0.01   4951

PARSE LENGTH is small in EVERY regime measured — satellites parse to
three phrases; the "obstacle" duplicates text parses to 0.49r. ONE
mechanism covers both degenerate families: phrase-occurrence
coordinates (the parse lists every occurrence's position; positions
inside an occurrence = base + offset, native PFP-BWT machinery).

THE PIPELINE REALIZATION: the adopted v3 chain is PFP-BWT — the parse
ALREADY EXISTS as a build-time artifact. The chi/sA construction should
CONSUME it (positions via occurrence coordinates, O(1)/event; LCPs via
the pieces; the event sweep on top) instead of re-walking the text:
the 37.7h O(n) walk becomes an O(r + parse) pass with no new
lower-level machinery — we own every piece (pfp++, rpfbwt, .ri4,
pieces, anchors).

Cost model, final form: construction = O(r + parse) given the build-time
parse + rlbwt + pieces; both parameters measured small across all
regimes. Open items unchanged: the 121/124 event-predicate convention,
and the parse-granularity resolution algorithm (PFP internals; prototype
next at yeast). v5 FREEZE GATE: only after the yeast prototype proves
which structures are actually consumed (anchors decided; parse sidecar
now likely; pieces).

## Literature placement + the PFP bridge (2026-10-01, checked before claiming)

The suffixient-array construction frontier (as of the checked literature):
- Cenzato et al.: linear-time, compressed-space; testing sets; online
- Olivares & Navarro (SPIRE 26): first linear one-pass PRACTICAL
- Urbina (arXiv 2607.00204): SUBLINEAR — O(n log sigma / sqrt(log n) +
  min(r, rbar) log^eps n) — the n-term remains
- Navarro et al. 2025: chi <= 2r PROVEN; near-tightness paper exists

PFP literature: build-time substrate only — BWTs (Boucher et al. WABI18/
AMB19), SA, suffix trees, BWT merging, recursive PFP, pangenome aligners,
higher-order parse-size theory (DCC 2026); one query-side use (FM-index
acceleration via word parsing, WABI 2023). Field's own assessment: PFP
"lacking good worst-case guarantees" — parse size is parameter- and
text-dependent; no theorem parse = O(r).

THE GAP (our lane, checked twice, proper survey still owed at paper
time): no work connects PFP to suffixient arrays, and nobody RETAINS
the parse as a coordinate sidecar for secondary constructions. The
mechanism claim: row -> (phrase occurrence, offset) -> position, from
parse structures, O(1)/event — the sA construction then runs in
O(r + parse) from compressed artifacts with NO n-term.

MEASURED, real scale (k=10 pilot parse, on disk from the PFP pilot):
parse = 333,531,726 occurrences; parse/r = 0.18; avg phrase 90 bp;
parse/n = 0.011. Battery: 0.01-0.49r. Both parameters small in every
regime measured.

THEORY FRAMING (the "parts stand in for each other" observation):
witnesses (chi), phi-pieces (iv = r exactly), BWT runs (r), and PFP
phrases (parse) are all CONTEXT-BOUNDARY SAMPLERS of one underlying
object — each samples the text where a comparison-context changes
(right-extension contexts for chi; SA-predecessor chains for pieces;
left-contexts for runs; repeated w-contexts for phrases). Measured
ratios: chi/r in [0.82,0.88], P/r 1.2-2, iv/r = 1.0, parse/r 0.18
(human scale). The unification is not an analogy: it is why parse
coordinates resolve witnesses.

## MODEL v3 EXACT (2026-10-01): 124/124 — the predicate is CLOSED

The 121/24 residual, dissected on T=[2,2,2,1,1]: TWO convention bugs,
both mine, both now fixed and measured:
1. PIECES must be defined DIRECTLY by the law (a piece continues at p
   iff PLCP(p-1) = PLCP(p)+1). The phi-parallel-shift condition
   (phi(p-1) = phi(p)-1) was an approximation that happens to hold on
   the large batteries but glues wrong pieces on ascending-chain texts
   (phi(p) = p+1: no descending chain exists at all -> pieces are
   singletons). Direct-law pieces: slope law holds by construction;
   piece count <= 2r+2 on all 124 texts (theory-bound shape).
2. The event set needs BOTH per-cell extremes: witnesses sit at the
   cell argmax-SA row (LCP-max), interposers enter at the argmin-SA row
   (LCP-min). One extreme is not enough (maxsa-only: 121/124).

With both fixes: FM with event-restricted PSV/NSV reproduces chi
EXACTLY, 124/124 (729-family battery + satellites + repeats +
[2,2,2,1,1]). The sweep is: event set size <= 2r boundary + 2P extremes
(P<=3r) = O(r); one stack pass; chi and the witness set both fall out.
Lean statement-lock for chi_from_events v3: pieces-by-law, both-extreme
event set, restricted-FM = chi.

REDUCTION STATEMENT (frozen, the paper's core): given the compressed
artifacts (rlbwt + C + run-end samples + direct-law phi-pieces + PFP
artifacts), the smallest suffixient set sA is constructible in
O(r + parse) time and O(r + parse) space. Parse is the named open
parameter (measured 0.18r at human pangenome scale, <= 0.5r on all
batteries incl. duplicates; no worst-case bound known — Boucher
question). Both doors publish: parse = O(r polylog) closes the no-n-term
construction; a construction lower bound separates parse from r.

## 466 gap scan RESULTS (2026-10-01): the real hard case is DIVERGED
## repeats — exact cycle-jump would have FAILED at 466; the parse pivot
## is validated by the data itself

Gap quantiles (2,739,736,836 resolving positions = samples + anchors):
  median 1 | p90 154 | p99 13,412 | p99.9 48,995 | p99.99 115,858 |
  max 18,168,834
Top-50 gaps: 6.8M-18.2M positions, clustering in two stream regions
(~0.72B, ~1.07B) — centromeric alpha-satellite arrays in the complete
HPRC assemblies.
CRITICAL FINDING: the top-20 gaps are NOT EXACTLY PERIODIC — periods
up to 4096 rejected; near-match fraction at candidate periods (171/342/
1717/3434) only 0.26-0.49 (chance is 0.25). Real alpha-satellite
monomers are 20-30% DIVERGED: alignable, not byte-identical.

CONSEQUENCES, honest:
1. The exact-periodicity cycle-jump (my first mechanism) would have
   failed at full scale. The synthetic-satellite experiments (exact
   (ATC)^k) were an idealization; real satellites are diverged.
2. The PARSE is the covering mechanism for the real hard case: PFP
   needs shared w-mers, which diverged monomers still produce
   (~10% of 10-mers survive 25% divergence); k=10 already measured
   parse/r = 0.18 including centromeres.
3. Cost shape at 466: top-20 gaps = 215M positions = 7.8% of r —
   even a pure walk over the worst gaps is sublinear in r; the median
   position is depth 1. The walk's worst case is concentrated exactly
   where the parse lives.
4. The user's duplicates instinct generalized further than human
   text: diverged repeats (approximate copies) are the universal hard
   class — and phrase-jump is the right jump for them.

## LANE 1 GREEN (2026-09-25): parse-coordinate resolution engine —
## row-exact, O(log)/row, no walking (tools/parse_resolve_proto.py)

The primitive that replaces toehold-walking exists and is brute-gated:
resolve(row) -> text position from PFP artifacts ALONE (dictionary +
parse + class boundary structures), verified against the brute SA on
EVERY row of every battery text:
- random-4-20k: 20,010/20,010 rows exact
- satellite-18k: 18,510/18,510 exact (|parse| = 4 — the whole periodic
  text parses to FOUR occurrences, all rows still resolve in O(log))
- HOR-nested, dup+unique-120k: all rows exact
- duplicates-600k: all 14,700 run-boundary rows + 100 random exact
Per-resolution cost: O(log |M|) bisect + O(1) array reads; no LF walk,
no SA, no text access. Conventions pinned in the file ($^w linear text,
colex wavelet semantics, phrase-start arithmetic — two off-by-ones found
and fixed by the agent via brute calibration; recorded).

Both components of the construction are now gated independently:
  (1) the event model: exact 124/124 (direct-law pieces + both cell
      extremes + restricted FM sweep -> chi);
  (2) the resolution primitive: row-exact on the full battery.
Remaining: the END-TO-END ASSEMBLY (events -> resolve -> pieces -> LCP ->
sweep -> sA), gated at battery scale vs the fm oracle, then multi-string
adaptation (anchors as events — the locate-fix law), then yeast.

## ASSEMBLY + C-LANE RESULTS (2026-09-25): the construction works on 4
## regimes; TWO claims die honestly; R3 (pos->row) SOLVED

END-TO-END ASSEMBLY (tools/chi_rspace_proto.py, gated): full r-space
pipeline (runs -> resolve run edges -> cells -> piece-law LCPs ->
restricted sweep -> sA) gives EXACT SET EQUALITY with the FM oracle on
random-4-20k, satellite-18k, HOR-nested, dup+unique-120k. The
piece-law LCP interface and the resolver are exact on EVERY row of
every text. The assembly is real.

TWO CLAIMS RETIRED (counterexamples, recorded honestly):
1. Model-v3-universal ("events = boundary + both cell extremes suffice")
   is FALSE: on duplicates-600k the true PSV interposer at boundary row
   574179 is an INTERIOR row of a size-3 cell — one witness missed
   (13004 vs 13003, missing value 184398, no spurious). The 124/124 was
   an artifact: on random-4-20k ALL 20,001 cells have size 1.
2. P = O(r) (scatter small) is FALSE under DIRECT-LAW pieces: cells are
   Theta(n) on random (P = n) and duplicates (P = 595,474 = 40.5r).
   The earlier P <= 3r was measured under the superseded phi-condition
   pieces. Correctness target: the SWEEP RESULT, not pointwise PSV/NSV.

R3 SOLVED (tools/c_probe_r3.py, gated 100%): resolve_inv(pos) ->
row from PFP artifacts in O(log) — the row<->position bijection now
exists in BOTH directions from parse structures. Gated on EVERY row of
every battery text incl. duplicates.

RULE D (C-term, random/satellite/HOR/mixed regimes): candidate set =
boundary U run-ends U row-window w=8 U position-window d (enumerated
via R3): EXACT at sizes 1.33-2.23r (d=1, w=8) — C is dead for these
regimes; construction O((r + iv) polylog) there.
R2 DEAD STANDALONE: LF-image recursion needs range-min/max of SA over
arbitrary row intervals of a permutation — no representation in the
available structures. But 78-92% of per-run extremes ARE run-end rows
(the v4 samples) — hybrid ingredient retained.

DUPLICATES REGIME (web text): still no o(n) rule — blocked by long
pieces (p90=298: the long pieces ARE the 300-byte copy spans; 962/9656
piece starts exactly on the copy grid, ~30x enriched), 23% run-end
capture, row-gaps to 149K. TWO LEADS: (a) copy spans are enumerable
from the parse directly (the piece structure IS the copy structure) —
duplicate-aware candidates need no d-guessing; (b) the class-internal
RMQ reduction — within a parse class, row ranges <-> k-ranges with
m.len fixed, so class-internal min over a row range may reduce to min
over occurrences — if it closes: exact NSV/PSV in O(r log^2).

## THE COUNTEREXAMPLE IS CLOSED (2026-09-25): endpoint-rule sweep EXACT
## on all 7 regimes; the formal pillars reduced to named obligations

EMPIRICAL (tools/endpoint_*.py, ENDPOINT_REPORT.md, gate GREEN from
main tree): the whitelist replaced by exact range-min semantics —
class blocks (min from 2 endpoint rows, each resolve O(log) + piece
law) -> segment tree over |M| block-minima -> exact PSV/NSV -> FM
sweep. RESULT: exact SET equality with the FM oracle on all 7 regimes
incl. duplicates-600k at 13,004/13,004 (the missed witness found;
zero spurious).

HONEST HYPOTHESIS CORRECTION (the agent's, retained verbatim in spirit):
the raw endpoint law is NOT a universal data law (interior dips exist;
the seductive 0-violations number was partly carried by cross-block
boundary values). The OPERATIVE law that holds — exhaustively, 1.5M
query-parts checked, 0 violations — is: for every threshold tau the
sweep queries and every block in range, if the block contains any
sub-tau row then an endpoint row is sub-tau too. Plus a never-wrongly-
skip fallback (skip iff endpoint-min >= tau AND tau <= block mlen).

ARCHITECTURE + COST: no whitelist, no O(n) slices, no SA, no text.
New honest third parameter |M| (dictionary-suffix classes): 1.3r
(random) .. 50r (satellite — singleton degeneration where rule D at
1.5r is the better rule). Sweep probes 5.7r-104r by regime. NET:
min(endpoint-sweep, rule D) is O(r)-constant on every regime measured,
exact, gated end-to-end. The construction story at battery scale:
CLOSED.

FORMAL (formal lane 2): no pillar fully closed, but all three reduced
to single named obligations with the reductions PROVEN (8 new
theorems; sorry ledger unchanged at 5; all gates green):
- covering_given_stream  <=>  (O1) domination: every position is
  scope-dominated by some scan emission
- minimality              <=>  (O2)-(O4): no duplicates, maximality,
  distinct classes
- fm_equivalence         <=>  the event-bridge multiset equality
  (route: per-character candidate lifecycles, style in-file)
(O1)-(O4) statement-locked: 0 counterexamples on 729 + 320 stress
texts. Next formal lane: the event bridge first (needs no coverage
theory), then (O1)-(O4) as the crystallized characterization problem.
The provable core the endpoint rule rides on: the classic sorted-
suffix identity min LCP over (a,b] = LCP(S_a, S_b) — statement-lock
candidate for the successor of chi_from_events.

NEXT: the YEAST FULL TEST — all inputs ready (parse 34.1M occ, .ri4,
lcp_index pieces, chi oracle 85,404,240). Requires the Rust port
(Python cannot sweep 100.9M runs) + the multi-string convention.

## TeraLCP code-reading verdict (2026-09-25): O(r) space, O(n) time —
## the n-term has one address, and we own its replacement parts

Read from the source (/home/erikg/TeraTools/src/include/TeraLCP/TeraLCP.h):
1. Its own comment (line ~738): "4 O(n) traversals for construction +
   1 O(n) for minLCP" — compressed SPACE is the design goal, time never
   was (they weigh optimizations by added-% of the O(n) traversals).
2. The O(n) is ONE phase: ConstructPhiAndSamples — an OpenMP loop
   walking EVERY suffix position to build the Phi move structure +
   samples, ISA checkpoints every n/r positions. All other phases
   (ConstructPsi from rlbwt, aux/repair, ComputePLCPSamples) loop over
   run/interval-sized arrays.
3. TeraLCP never consumes the parse: input = rlbwt (r-sized); the
   -othresholds flags EMIT pfp-compatible files downstream. r-space in,
   n-time middle, r-space out.

THE OBVIOUS WIN (user's call, confirmed): the Phi structure is
per-INTERVAL data (<= 2r+2 intervals: a start + pointer values each);
the n-walk exists only to DISCOVER the intervals position-by-position.
Our gated parse machinery (resolve + resolve_inv, O(log) both ways)
builds the same structure per-interval: enumerate starts -> resolve
endpoints -> fill pointers = O(r polylog) replacing O(n).
THE GATE: start enumeration (lane M2, running). If a 100%-containing
O(r+parse) candidate set exists, M3 builds the pieces in parse space,
partition-identical to brute — and the ENTIRE pipeline (BWT, samples,
pieces, parse, chi/sA) becomes r+parse time with exactly ONE Omega(n):
the single pfp++ text read. Build and refresh become the same claim.

## PIECES IN PARSE SPACE: GREEN (2026-09-25) — the last n-term has a
## replacement, correctness-gated end to end (tools/teralcp_m*.py)

M1 (confirmation): TeraLCP time is linear in n at fixed r (8x n -> 7.9x
time, r~300): the O(n) verdict from source-reading, now also measured.

M2 GREEN — THE CANDIDATE STRUCTURE (the gate): the direct-law piece
starts are CONTAINED in the resolved BWT run-boundary rows:
    C = { resolve(row i) : BWT[i] != BWT[i-1] }  union  {N-1},
|C| = r EXACTLY. Exhaustive: 511 small texts, 0 containment failures;
all 7 battery texts: containment 1.000. Negative note: the phi-parallel
break set is NOT safe (misses 22/255 ascending-chain texts) — the
run-boundary set is the right structure. Elegant closure: this is
EXACTLY the resolution work the chi/sA sweep already does for run
edges — pieces and sweep share one O(r) resolution budget.

M3 GREEN — PIECES BUILT FROM PARSE COORDINATES ALONE: candidates ->
parse-space PLCP (via resolve/resolve_inv, phi(p) = resolve(resolve_inv
(p)-1)) -> starts + samples. Partition IDENTICAL to brute direct-law
on all 7 texts; samples exact to the value; slope law clean (1000
spot-checks/text); parse-space LCP 0/400 vs brute. No text scan, no
brute PLCP.

THE HONEST COST CAVEAT (the one remaining engineering item): LCP CALL
COUNT is O(r) (~2-3r) — the parse-time SHAPE is right — but the
prototype's phrase-walk LCP primitive costs O(lcp value)/call, so
periodic-text totals exceed n (satellite: 188n steps). The fix is the
STANDARD PFP-LCP primitive (dictionary LCP + parse ISA + bounded
within-phrase compare -> O(polylog)/call), giving O(r polylog) total.
Named, standard, portable — the only thing between the build and
"one Omega(n) total (the pfp++ read)".

Claim ledger: piece sidecar constructible from parse coordinates alone
— CORRECTNESS GATED; wall-clock parse time pending the PFP-LCP
primitive port.

## Verification-ladder rung (user's call, 2026-09-25): implementation-vs-
## Lean-spec differential — the missing weld

Honest status: the construction implementation (resolve/resolve_inv,
endpoint sweep, parse-space pieces) is NOT proven; its warrant is brute
differential gating (row-exact, set-exact, partition-identical). The
mathematical core is proven in Lean; the spec pillars (O1-O4, event
bridge) are open with a lane queued.

Why differentials-first was right THIS time: four false models died to
batteries in hours each this week (boundary-only, events-5/5 artifact,
argmax direction, both-extremes whitelist); Lean-first would have
formalized corpses. The convention-bug class (SA bug, orientations,
off-by-ones, the yeast trailing-newline seam) is however exactly what
a formalized spec eliminates — the user's methodology point stands.

THE MISSING RUNG (added to the ledger): once O1-O4 + the event bridge
are proven, RUN THE LEAN SPEC AS CODE on the full battery (the #eval
machinery already executes it) and diff the Rust construction against
it. Ladder from bottom to top: (1) brute differentials = ground truth;
(2) Lean theorems = the model; (3) implementation-vs-Lean-spec
differential = the weld that makes "the implementation follows the
proven model" a gate, not a hope. The convention-seam class (pfp file
formats, dollar padding) stays differential-gated by design — it is
engineering, not mathematics.

## 2026-09-25 — YEAST FULL TEST GREEN: the r-space χ/sA construction at
## real scale, EXACT (the session's headline claim banked)

`xsa chi-rspace` (Rust) + `chi_rspace_dump` (C++ aggregates) constructed
χ/sA on real yeast — 3.34 Gbp, R = 100,904,881, multi-string (9,901
'!'-joined), erasure regime — from compressed artifacts only: **no text
scan, no SA, no LF walks.**

**χ = 85,404,240 — EXACT; witness SET EQUALITY vs the production
oracle chi_yeast_pfp2.sA (numpy sorted-compare True).** Main-tree
re-verification by supervisor: dumper rebuilt from committed source
(aggregate output byte-identical to the lane's), calibration 512/512
zero-bad at ROW_OFF=10, LCE cross-checks 1,190,377 mismatches 0.

Cost split at yeast: chi-rspace core = **99 s wall / 6.0 GB RSS**
(r-dominated); dumper pass = 901 s / 29.7 GB (the residual O(n)
lcp_index read — this is the piece the PFP-LCP primitive port retires;
after it, the only Ω(n) left in the system is the single pfp++ text
read). Battery chain (bit6/chi_rspace_battery_chain.sh): 7/7 texts,
G1 (aggregates == production --triples) + G2 (Rust chi/witness-set ==
oracle) green, including duplicates-600k.

**The 2.9% bad-rows mystery resolved as provenance, not machinery:**
two complete yeast chains on disk had been cross-mixed — y2new.ri4 +
y2.lcp_index = the yeast235.rl.txt chain (χ = 85,350,673), while the
parse + gate 85,404,240 = the yeast_pfp2.txt chain. The lane built the
pfp2 chain's missing .ri4 from its rlbwt pair (ri4_from_rle.cpp,
battery-byte-identical converter). The file-end-seam hypothesis:
REFUTED by the discriminators (round-trip / row-direction /
LF-consistency all ran clean on the correctly-paired chain). Negative
result #7 recorded: scale-only debugging reached a WRONG hypothesis
(one chain) before the data forced the right one (two chains).

Chain of custody for the gate value: the syng-table 85,404,240 is the
yeast_pfp2 (PFP-chain, '!'-joined) χ; 85,350,673 is the BCR-chain
('\n'-joined revlines) χ. Both are correct for their respective text
objects — they are different texts (separator structure differs).

Files: bit6/chi_rspace_dump.cpp, ri4_from_rle.cpp, sa_decode.cpp,
chi_rspace_battery_chain.sh, YEAST_LANE_REPORT.md; xsa/src/main.rs
(+102: the chi-rspace subcommand).

## 2026-09-26 — FmJoint milestone + STATEMENT EVENT: fm_equivalence retired-false, bounded successor locked

Lane (strong model, 3 revivals) drove FmJoint.lean — the joint induction
backing the event-bridge — to ZERO errors, ZERO sorries. The load-bearing
`coupled_step_inv` is fully proven across its case lattice, including a
genuine invariant repair en route (FM ALWAYS overwrites when vi > v; the
old "FM holds" claim was wrong; repaired form re-validated on 740 random
streams, 0 disagreements). File-level O3: adjudicated CLEAN (0 violations
on 729/1092/1364 + 3-chunk stress) — stands as-is; the O3 "event" was a
FmJoint-internal lemma shape (disjunctive fix = ordinary proof
engineering, not statement-lock territory — SCOPE RULING recorded).

STATEMENT-LOCK EVENT (8th): unbounded `fm_equivalence` REFUTED AS STATED.
Saturation counterexample, supervisor-verified and now repo-resident
(lean/counterexamples/saturation_refutation.lean): with lcps strictly
above MAXINT (a model-arifact regime; every real text has lcp ≤ |T| <
2^63), scan's running-min saturates and emits spurious DUPLICATE
positions — scan [94,95,95,96,96,97] vs fmSpec [94,95]. Boundary SHARP:
at MAXINT exactly and below, both machines agree. (Note: O2/Nodup also
fails in that regime — recorded for the next lane's adjudication.)
SUCCESSOR locked: `fm_equivalence_bounded` (hsat : ∀ t ∈ triplesOf T,
t.lcp ≤ MAXINT.toNat) — the operative form, vacuous side-condition for
real texts. Sorry ledger: 5 (composition unchanged: one retired-false
record swapped for its bounded successor).

Remaining for fm_equivalence_bounded (enumerated in FmJoint.lean
HANDOFF): boundary-step assembly, non-boundary row step, final flush,
main induction over JointInv, then the bounded statement. FmJoint.lean
committed (1744 lines incl. HANDOFF; supervisor typecheck clean).

## 2026-09-26 — PFP-LCP PORT GREEN: TeraLCP OFF THE CRITICAL PATH
## (the lcp_index is dead for this consumer)

Key insight (smaller than briefed): the dumper's ONLY lcp_index use was
topLCP = PLCP at the run-head row = LCP(SA[a-1], SA[a]) — the classic
adjacent-row LCP — computable directly as clamp(LCE_sup(resolve_row(a-1),
resolve_row(a))) via the vendored pfpds::pfp_lce_support (dictionary RMQ +
parse ISA/RMQ + rank/select, polylog/call, NO phrase-walk loop; the
O(LCP-value) walk existed only in the M3 Python prototype). The lcp_index
was REDUNDANT for this consumer. bit6/chi_rspace_dump.cpp now takes
--ri4 --parse ONLY (PFP artifacts, no --lcp-index).

Gates (supervisor re-verified from main-tree sources):
- G0: 997,000 positions, 7 battery texts, 0 mismatches vs lcp_index
  (ALL positions, incl. duplicates-600k)
- G1: .agg byte-identical 7/7 vs lcp_index baseline
- G2: yeast .agg byte-identical (3,228,956,204 B); xsa chi-rspace ->
  chi = 85,404,240, witness set-equality vs oracle True. NO lcp_index
  anywhere in the chain.
- G3: 841.6 s / 25.3 GB vs 901.0 s / 28.4 GB baseline.

HONEST COST CAVEAT (the next decision, in bit6/PORT_HANDOFF.md): the
dump still calls pf_parsing::build_b_bwt_and_M() — O(n)-bit b_bwt
allocation + |M| = 317,476,865 ≈ 0.1n entries (~9 of the 14.5 min at
yeast). That is PFP-index CONSTRUCTION (one-off parse-build work), not
a per-query scan; the query phase itself is O(r polylog). The "only
Omega(n) is the single pfp++ text read" claim requires folding this
construction into the pfp++ parse build and persisting the artifacts
(the same RETAIN-THE-PARSE doctrine). Until then the honest statement:
no lcp_index, no TeraLCP, one foldable O(n)-space construction phase.

Also recorded: ft30/sat4 fixtures UNUSABLE for pfp gates (regenerated
parses don't match their .ri4 texts — the same cross-chain mispairing
class as the yeast lane; baseline also core-dumped). Excluded, not
counterexamples.

Files: bit6/chi_rspace_dump.cpp (+91/-16), PFP_LCP_PORT_REPORT.md,
PORT_HANDOFF.md.

## 2026-09-26 — PILLAR CLOSED: fm_equivalence_bounded PROVEN (sorry ledger 5 -> 4)

The FM-equivalence pillar is now KERNEL-VERIFIED in its operative (bounded)
form: the scan machine and the fmSpec machine emit the same suffixient set
for every text with lcps ≤ MAXINT (vacuous side-condition for real texts).
#print axioms: propext, Classical.choice, Quot.sound ONLY — no sorryAx;
supervisor additionally PURGED three native_decide axioms the lane had used
for the trivial (0:Int) ≤ MAXINT (native_decide trusts code generation,
not the kernel — replaced with decide, kernel-checked). House rule going
forward: flagship theorems admit no native_decide axioms.

Proof architecture (merged into lean/Sxgc.lean, +2973/-13; FmJoint.lean
deleted after verbatim merge): bound_coupled (per-char boundary assembly
over the sorry-free coupled_step_inv/coupled_step_notinv), bound_mem (the
emission-accounting MEM iff, incl. the overwrite-without-emit
impossibility via the prevSmaller witness vs the armed NSV), joint_stream
(lockstep induction carrying JointInv + running-min alignment), and the
top-level assembly. Statement byte-identical to the locked form.

Sorry ledger now 4: O1 covering_given_stream (the largest open pillar),
minimality, chi_from_events (retired-at-scale record kept deliberately),
witnesses_at_boundaries_FALSE_AS_STATED (retired-false record).

NEXT FORMAL QUESTION (lane flagged, needs supervisor adjudication before
any edit): O2/Nodup unbounded is presumably FALSE AS STATED — the
saturation counterexample shows scan emitting DUPLICATE positions above
MAXINT. If O2 is next, it likely takes an hsat-bounded successor like
fm_equivalence did.

Lean-without-Mathlib traps recorded in lean/ASSEMBLY_LANE_REPORT.md:
cases-on-getFM substitutes without iota-reduction (use simp only [h]);
pair-projection through an unreduced ite is not defeq.

## 2026-09-26 — PFP-INDEX FOLD GREEN (yeast); the 466 binding constraint
## is SPACE, not time — retention decision recorded

The dump phase no longer builds the PFP query index in-process:
bit6/pfp_index_build.cpp (standalone "parse -> XPF1 v2 index" tool,
per-component sizes) + --pfp-index load mode in chi_rspace_dump.cpp
(chunked M load, fail-loud cross-checks, FNV digests over the logical
values of all ten structures). Two vendored headers gain a documented
defer-build ctor (pfp.hpp, dictionary.hpp); five byte-identical to
upstream. Two self-caught bugs BEFORE gates: M block written 2x
oversize (reading past buffer — byte-gates would have passed on UB),
tellp() after close().

GATES (supervisor re-verified from main-tree binaries): G1 load == build
byte-identical on battery incl. duplicates-600k; G2 yeast: lane index
(8.460 GB) -> load-dump 204 s / 19.5 GB RSS -> .agg BYTE-IDENTICAL to
the standing baseline -> chi = 85,404,240, set-equality True; G2d
mispaired index dies loudly both directions. G3: construction 536-626 s
-> ~37 s (15-17x); whole dump 820-913 s -> 204-347 s; RSS 25.3 -> 19.5 GB.

RETENTION DECISION (supervisor):
- YEAST: retain (8.46 GB trivial; every re-dump is 204 s + no build).
- 466: DO NOT RETAIN. Index projected 2.60 B/symbol -> 3.74 TB
  (M 1.57 TB, isaD 845 GB, lcpD 423 GB, w_wt 365 GB, b_bwt+b_p 344 GB)
  vs 1.9 TB free on the volume; AND load-path RSS is linear in n
  (19.5 GB at yeast -> ~8.4 TB at 466) vs 1 TB RAM: the pf_parsing
  route CANNOT RUN at 466 on this machine, load mode or build mode.
  The 66 h -> 6 h refresh projection is therefore not actionable at
  466 on this hardware without an external-memory LCE design (named
  open item: chunked/offline M answering ~R polylog queries, external
  RMQ; OR the minimal-subset question — the dump's only parse-side
  query is topLCP, a smaller persistent structure may exist).
- 466 CONFIRMATION ROUTE: the lcp_index path (O(r) space, streaming
  O(n) time) ALREADY RAN at 466 — h466.lcp_index (82.5 GB) is on disk.
  The port removed --lcp-index; resurrect it from history behind an
  explicit compatibility flag for the 466 run: one streaming O(n)
  pass (already paid once, reusable per re-dump) + the r-space
  chi-rspace core (~160 GB RSS at 466, fits). This is the honest
  466 cost statement until/unless an external-memory LCE exists.

Honest scope note: the "only Omega(n) is the single pfp++ text read"
endgame claim holds ARCHITECTURALLY (index is build-time, persistable,
polylog-queryable) but NOT YET on this machine at 466 scale.

## CORRECTION (2026-09-26, user caught it): "can't run 466 here" was WRONG

The retention-decision entry above conflated the pf_parsing OPTIMIZATION
route with the only route. Precise statement: the 466 confirmation is
O(r)-SPACE IN RAM and RUNS FINE on this machine (1 TB) — TeraLCP is
O(r)-space streaming (it already ran at 466; the 82.5 GB lcp_index is
on disk), the dump streams it at O(1) RAM, chi-rspace needs ~160 GB.
The only O(n)-SPACE structure is inside the NEW pf_parsing/LCE route
(a yeast-class refresh optimization, NOT on the 466 critical path).
The O(n) term at 466 is TIME ONLY (one streaming pass, already paid).

What TRUE O(r)-space-AND-time would need: an O(r)-space LCE oracle for
the ~2r query points. Three routes: (1) MEASURE |M| scaling — the
3.74 TB projection assumed Theta(n); at yeast Theta(n) and Theta(r)
are indistinguishable (n/r = 33); at 466 (n/r = 526) they diverge:
Theta(n) -> |M| ~ 1.1 TB, Theta(r) -> ~70 GB. The k10 466 parse is on
disk, so the index-build probe (700 GB ulimit, self-terminating) gives
the verdict cheaply. (2) External-memory/offline LCE (sort R queries,
stream M once, O(r) RAM). (3) r-index LCE from the literature (O(r)
space, polylog query). Lanes fired: 466 confirmation (lcp_index route)
+ |M| scaling probe.

## 2026-09-26 — k10 FULL-PIPELINE GREEN (R=1.86e9); 466 walk running;
## |M| probe verdict: Theta(r) REFUTED at k10 ratio (probe killed to protect walk)

K10 GATE (lane K, supervisor re-verified --agg-out from main-tree source):
walk (--agg-out in teralcp_chi, byte-identical .agg vs committed dumper
8/8 battery) 1:01:56 / 178.6 GB; xsa chi-rspace 43:19 / 111.7 GB;
chi = 1,627,063,183 EXACT + sorted-set equality vs chi_h10.sA. The r-space
sweep is now gated at R = 1.86e9 (18x yeast). Another cross-chain trap
caught pre-mortem: h10new2.ri4 = newline-joined 865-contig chain,
h10ss_pfp = '!'-joined single string — pairing would have been wrong;
lane gated on the walk route instead.

466 WALK (route A): pid 286620, launched 05:25:45Z, ~38 h projected,
245 GB RSS at 15 min, watcher armed on the DONE marker; then sweep
(~1.5-2 h, ~160 GB) -> gate chi == 2,249,968,075 + set equality vs
chi_h466.sA. Pre-confirmations green (n, runs, strings, sidecar,
run-chars == bwt.heads pairing).

|M| PROBE VERDICT (supervisor decision): killed by pid at 210 GB RSS /
2h19m on the 30 GB k10 corpus — it had already BREACHED the Theta(r)
prediction ceiling (~47 GB for M + ~150 GB total) and was still
climbing; extrapolation to 466 ~10+ TB. Theta(r) refuted at the k10
ratio; the pf_parsing/parse-space LCE route is a yeast/k10-class
solution only (this machine). The 466 refresh path = retained sidecars
(.agg + .ri4 + head samples per the v5 decision) + pure sweep —
door-independent, O(r) space/time. Parse-space LCE remains the paper's
conditional no-n-term construction claim, with BOTH conditions now
measured adverse at 466 scale on this hardware (supports Theta(n)-class;
pfp++ front-end cost at terabase unresolved — the k10 syng build's 40+h
dictionary phase is the red flag to diagnose).

Files: bit6/teralcp_chi.cpp (+35: --agg-out CRA1 sidecar), bit6/
H466_LANE_HANDOFF.md (state, cost tables, next commands).

## 2026-09-26 — v5 (.ri5) freeze spec drafted (pending 466 gate) + Lean/LCE division recorded

V5 FORMAT (freeze AFTER the 466 gate lands — do not churn mid-gate; every
field below is justified by a gate that ran):
- IN: anchors native (620 KB, no sidecar); run-HEAD SA samples alongside
  tail samples (the saFirst fix — proven needed by the 466 walk route;
  kills the walk dependency for any future re-aggregation); chi set as a
  delta-compressed sibling (raw 18 GB u64 at 466 -> compress; positions
  cluster on run structure; measure the ratio at freeze time).
- OUT (build scaffolding, deletable after the sweep): .agg, lcp_index,
  XPF1 index. Parse = dev-side artifact only where it exists.
- UNTOUCHED: rlbwt, run table, names sidecar.

LEAN/LCE DIVISION (architectural, recorded): the Lean formalism models
the CONSUMER of the (bwt, lcp, sa) stream — machines, classes, pieces,
equivalence — and deliberately sits one level above the seed/LCE
producer (nothing in Sxgc.lean knows phrases/dictionaries/LCE). Mirrors
the implementation: consumer proven, producer = open corner
(differential-gated). NEXT LEAN RUNG when the pillars land: the
Phi-interval decode lemma (within a run interval, PLCP(row) =
seed[head] - steps) — the formal bridge from O(r) seeds to the n-row
stream; completes "chi/sA from r seeds" on the consumer side in Lean
and connects our theorem side to the move-structure machinery formally.

## 2026-09-26 — syng k10 bridge run ABANDONED (negative result #8)

Killed by pid after 2d 9.5h: single-core (101% CPU), 101 GB RSS, on the
~30 GB human-subset corpus, with NO output materialized (no h10.syng
file) — past every core-hour estimate, cause UNDIAGNOSED (tool scaling
vs bug; not worth diagnosing now). Bridge ratio point 2 (syncmer-dict
count at human scale) retired-unmeasured; point 1 (yeast) stands. syng
as a line stays open on other experiments, but this run is dead.

Related recorded red flag (unchanged): any pfp++/syng-class front-end at
466 scale has unresolved practical constants — the reason the parse
door's BUILD cost, not just its support space, stays on the adverse
side of the ledger on this machine.
