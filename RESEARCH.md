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

## CORRECTION to negative #8 (supervisor error, recorded): the killed syng
## run was NOT the anomalous one — it was the healthy retry, mid-flight

Facts corrected: the 2d9.5h process I killed was the RETRY (launched
2026-09-24 with -t 32 --parallel-dictionary): its dictionary phase SUCCEEDED
(61 min, 151,318,172 unique syncmer nodes — the retry FIXED the original
single-core dictionary anomaly). It then spent 56.5 h in the per-sequence
phase: silent (one log line total), single-core despite -t 32, no
checkpoint, output only written at completion. I killed it based on STALE
evidence from the original run, and the wrapper's `| tail -5; echo $?`
plumbing masked the kill as "SYNG EXIT: 0" (tail's exit code, not impg's).

Amended assessment: the experiment's real defect is OBSERVABILITY (no
progress signal, no resume, unknown ETA — could be hours or months).
Bridge point 2 remains retired-unmeasured per user decision; the yeast
point stands. IF revisited: restart with -v 2 on a 20-50-sequence SLICE
first to measure the per-sequence rate before committing a full run
(the only disciplined route to this data point). The original dictionary
anomaly is RESOLVED (parallel build works); the per-sequence phase is
the unmeasured part.

## CORRECTION #3 (user-driven, 2026-09-26): the |M| verdict measured the
## WRONG OBJECT — lce_support is Theta(P + dict), NOT Theta(n)

Code-read of pfpds::pfp_lce_support (all 150 lines): its ENTIRE query
path touches only parse structures (rank/select on b_p, pars.p, isaP,
lcpP, rmq_lcp_P — Theta(P)) and dictionary structures (select_b_d,
isaD, lcpD, rmq_lcp_D, length_of_phrase — Theta(dict)). M, b_bwt, w_wt
appear NOWHERE — they are r-pfbwt TEXT-BWT construction machinery that
the LCE primitive never queries. The probe (and the 3.74 TB projection,
and "Theta(r) refuted") measured pf_parsing-THE-OBJECT because its ctor
demands build_b_bwt_and_M; the construction never used those parts.
The parse-space LCP route was killed by a measurement artifact.

Second artifact: the "pfp++ slow at scale" red flag was SYNG
contamination (syncmer-GBWT tool, different beast; its own retry built
the k10 dictionary in 61 min parallel). pfp++ itself built the k10
parse without incident — it is on disk.

Corrected state: LCE supports = Theta(P + dict) (the accepted classes);
at k10 approx 20-30 GB; at 466 approx 300-700 GB raw (P ~ 7-16e9
phrases), feasible esp. with compressed isaP/lcpP. Seeds = r polylog
LCE queries from parse space. The construction claim upgrades back to
O((r + parse + dict) polylog) after one pfp++ pass — parse door is the
FRONT door again. LESSON recorded: profile the DEPENDENCY SET of the
primitive, not the RAM of the object its constructor forces you to
build. Immediate action: k10 standalone-LCE dump (build only the true
dependencies), gate .agg byte-identical, measure REAL component sizes.

## 2026-09-26 — BUILD DIRECTIVE LOCKED (user): the memory-lean parse-space
## construction IS the product — yeast first, then humans

THE TARGET (the "this we're building"): chi/sA construction over 466-class
corpora with peak RAM <= 200 GB (hard ceiling 300), in parse time and
space, via:
- standalone LCE: only lce_support's true Theta(P + dict) dependency set
  (M/b_bwt/w_wt never built or loaded)
- fingerprint-LCE on the parse (sampled Karp-Rabin suffix hashes,
  tau ~ P/r; direct-parse-read VERIFICATION makes answers deterministic;
  no saP/isaP/lcpP materialized anywhere, not even on disk)
- streamed .agg sweep (row-order chunks; -32 B/run)
- fixed-width parse ids; compressed SA samples; delta-compressed chi output
Memory ledger at 466 (projected, to be measured): seed phase ~120-160 GB;
sweep phase ~90 GB streamed (~170 unstreamed); retained index ~45-55 GB.
62 B/run observed today; retained floor ~18 B/run.

THE LADDER (law): yeast first (all gates minutes; .agg oracle + chi =
85,404,240 + witness-set equality standing), then k10 (chi = 1,627,063,183;
RAM watch live), THEN the 466/human decision with measured constants.
laneN (running) = the standalone-LCE baseline; the fingerprint+streaming
lane fires on its output and re-gates byte-identity at each rung.

PROOF EXPECTATIONS (user set them): we will NOT have Lean proof of most
of this build machinery (fingerprints, streaming, IO). Warrant grading
stands as designed: PROVEN = characterization, scan=FM, consumer side;
DIFFERENTIAL-GATED = this entire build path (byte-identity gates at every
rung, the measurement IS the warrant); the Phi-decode bridge stays as the
one formal rung worth keeping on the consumer side. Never claim above
warrant.

## 2026-09-26 — FORMAL QUEUE EXTENDED (user directive: develop proofs);
## k10's job pinned (measured human constants, not correctness)

Two new Lean statements queued into the formal batch (proof of the
ALGORITHM; code stays differential-gated per the weld architecture):
1. FINGERPRINT-LCE DETERMINISM: if direct parse reads confirm l matching
   phrases at (p,q) and phrase l+1 differs, then LCE_parse(p,q) = l —
   the verification step removes all probability from the statement.
2. LCE COMPOSITION: parse-LCE + dict-LCE compose to text-LCE (the
   structure of lce_support's query path, as a lemma) — together with
   the piece law and Phi-decode it gives "seeds determine aggregates,"
   the construction theorem's formal skeleton.

K10'S ROLE (pinned, answer to "what is k10 for"): NOT another correctness
rung (yeast settles correctness). Its product = measured human constants
that make the 466 decision arithmetic: avg phrase length on real human
DNA (-> P_466), dict growth (-> dict_466), the RAM curve at 18x yeast
with lean structures resident. The newline-joined k10 parse (laneN,
building now) delivers the first of these. Rationale recorded: after
three projection-corrections this week, 466 commitments run on measured
constants only.

## 2026-09-26 — 466 walk KILLED (user decision: no free-oracle-bait needed)

Killed pid 286620 at ~14 h of its projected 38 h. Rationale (user's
correct audit): the oracles that matter for the future human rung —
chi = 2,249,968,075 and the 18 GB witness set chi_h466.sA — are ALREADY
on disk. h466.agg was a debugging convenience regenerable on demand
(pay the 38 h then, only if needed); the sweep it would have fed is
already validated at k10 (R = 1.86e9, real human DNA, exact). ~3,650
core-hours were not worth a convenience. 245 GB + 96 cores freed;
watcher stopped. The pilot ladder's 466 rung via the old route is
RETIRED; the 466 construction will happen via the slim build with its
own gates when the program reaches the human rung.

## 2026-09-26 — STANDALONE-LCE lane committed; dict structures are the
## new space hog (correction to correction #3); v5 head-samples MEASURED

bit6/chi_rspace_dump.cpp +326/-29: --resolve-ri4 mode — builds ONLY
lce_support's true deps (|M|=0, no b_bwt/w_wt, no pfp_sa_support, saD
freed after build), resolves positions from .ri4's own per-run SA
samples via LF walk + XANC anchor fallback (mirrors xsa's gated s_at).
Supervisor re-verified from main-tree source: battery byte-identical.

GATES: G0 7/7 battery byte-identical (incl. duplicates-600k), flat spot
512/512. G1 k10 full byte-identity INFEASIBLE AS-BUILT (position
resolution = LF walk from TAIL samples: 2,156 steps/run-head avg, max
933,995 — the mega-run degeneracy; ~333 core-days) -> substituted
200,003-run sampled gate vs h10.walk.agg: 0 saFirst/saLast mismatches,
flat spot 512/512 at 30 Gbp. THE MEASURED JUSTIFICATION FOR V5 HEAD
SAMPLES: with head-SA samples, resolution is O(1)/run-head and k10
full byte-identity becomes feasible (~104 min construction + ~83 min
queries). V5 head-sample field: ADOPTED on this measurement.

THE BIG FINDING (corrects correction #3's constant): the route's space
is Theta(|D|) with |D| ~ 0.18n on k10 human — dict structures measured
3.3 B/symbol -> ~3.1 TB projected at 466 as-built (isaD 1.53 + lcpD
0.77 + dict text 0.26 = 83%). The no-M win was real but ~2x, not
asymptotic: the DICTIONARY of real human DNA is a fat fraction of the
text. Error bar: |D|/n at 466 unknown (may halve with more shared
content). ANSWER ALREADY DESIGNED: fingerprint-LCE applies at BOTH
levels — the dict-LCE queries are also ~2r known-in-advance questions;
fingerprints on the dictionary text replace isaD/lcpD/rmqD (O(|D|/tau)
~ O(r)), with dict text read directly only for block verification.
This two-level fingerprint design is the slim build lane's brief.

Cost table (k10 baseline): peak 164 GB, structures ~105 GB (vs walk
178.6 GB); dict build 6,265 s; parse 244 s; b_p 539 s. The k10
newline-joined parse: 333,531,723 phrases in 27.4 min, 10 GB RAM
(pfp++ FAST on human — syng contamination fully confirmed dead).

Walk SIGTERM mystery (laneN flagged): RESOLVED — it was the supervisor's
kill per user decision (ledger 5436ee0), not external.

Artifacts: tools/sample_decode_probe.py, walk_decode_probe.py,
bit6/gate_logs/G0_G1_gate.log, k10 parse h10rl_pfp.{parse,dict}.

## 2026-09-26 — SLIM LANE (astra) first pass: G0 GREEN 8/8; streamed sweep
## is a monster win; TWO honest corrections recorded

DELIVERED (supervisor re-verified from main tree): bit6/slim_lce.hpp
(two-level verified fingerprints, sparse boundaries, bounded dict caching),
slim mode in the dumper, --stream-agg chunked sweep in xsa, gate tooling.
- G0: 8/8 battery byte-identical (resident AND streamed dictionary).
- STREAMED SWEEP AT YEAST (over baseline .agg): chi = 85,404,240 correct,
  witness byte-identity + set equality — in 15.36 s / 4 MiB peak RSS
  (vs 99 s / 6 GB resident: ~6x faster, ~1500x less RAM).
- Slim structure-only build at yeast: 21.56 s / 3.3 GB peak.
- 466 projection: 191.9 GB with streamed dict + 8x-class sampling —
  UNDER the 200 GB target, ~8 GB margin, conditional.

CORRECTION #4 (supervisor design overreach, lane caught it): deterministic
two-sided direct verification of an l-phrase match costs O(l) reads —
the O(tau) per-query bound holds only for the probabilistic hash-jump
PROPOSAL. The O(tau)-deterministic claim in the slim design is RETIRED.
Honest form: deterministic-exact with measured average verification
length (seed adjacent-row LCEs average ~n/chi/avg-phrase phrases —
small); probabilistic O(tau) available as an option with a second
independent hash (collision^2). The Lean "VerifiedLCE determinism"
statement is UNAFFECTED (correctness, not cost).

CORRECTION #5 (data finding, lane): /tmp/laneY/yp2new.ri4 is a runs-only
artifact — ALL 100,904,881 SA samples are INF placeholders (supervisor
verified: 499/500 INF u64s in the sample region; ri4_from_rle.cpp built
it from the rlbwt pair, which carries no SA). k10/battery .ri4s have
real samples — which is why resolve worked there. ARCHITECTURAL FIX:
slim mode should resolve positions from the PARSE it already holds
(parse-resolve is the committed, yeast-gated machinery; at 466 the parse
exists by construction). The .ri4 sample table becomes irrelevant to
the slim dump; the sweep needs only run structure (PROVEN by the yeast
streamed sweep passing on the placeholder .ri4).

G1 resumes with: --resolve-parse in slim mode, G0 re-gate both paths,
yeast end-to-end, G2 final cost table, G3 fail-loud.

## 2026-09-26 — CORRECTION #6 (astra caught supervisor overreach #2 of the
## slim arc): the legacy parse-resolver REQUIRES M/b_bwt/w_wt — the two
## pass-2 constraints were incompatible

Audit (bit6/gate_logs/slim/pass2-resolver-audit.log): pfp_ds_vendor's
sa_support.hpp resolve path directly requires the forbidden structures
(lines 55-66) — they ENCODE the row->position mapping; the parse alone
gives position->phrase, not row->position. The "--resolve-parse without
M/b_bwt/w_wt" instruction was unsatisfiable and astra refused to
fabricate oracle-derived samples. Correct call.

ADJUDICATION: the slim build's position machinery is --resolve-ri4
(SA samples + LF walk + anchors — laneN's battery-gated path, which
astra's own pass-1 G0 gated 8/8). v5 head-SA samples (ALREADY ADOPTED,
measured justification: 2,156-step walks at k10) make it O(1)/run-head
at scale. For yeast G1: yp2new.ri4 is runs-only (all-INF samples) ->
produce a REAL-samples .ri4 for the pfp2 chain via one teralcp_chi
--samples walk over the EXISTING yp2t.lcp_index (2.48 GB, on disk;
~1 h at yeast scale) — the old world produces the v5-class artifact
the new world consumes, once. Then slim G1 = --ri4 yp2real.ri4
--resolve-ri4 --slim -> byte-identity -> sweep -> chi = 85,404,240.

## V5 PROVENANCE CLARIFICATION (user caught muddled framing): SA samples
## are a FRONT-END OUTPUT — TeraLCP never runs in the end-state, not even
## once per corpus

The pass-3 yeast teralcp_chi walk is LEGACY REMEDIATION ONLY: the pfp2
chain's .ri4 was born runs-only (converter had no SA; the samples file
was a by-product of the production walk we retired). One-time fix for a
pilot-era artifact, nothing more — never validation, never architecture.

END-STATE (binding): the pfp++/RLBWT build — the single mandatory pass —
EMITS RLBWT + SA samples (head+tail, the v5 field) + anchors together,
using its internal construction structures legitimately at build time.
The slim construction consumes (RLBWT + samples + parse + dict):
fingerprints -> LCE -> aggregates -> sweep -> chi/sA. NO TeraLCP, NO
lcp_index, NO separate O(n) walk, EVER. The seed pass's original job
(LCP values) died to fingerprint LCE; its lingering second job (samples)
belongs to the front-end. Iteration-2 chains (PFP-BWT over 466) ship
samples from the build directly.

## 2026-09-26 — THE SLIM BUILD IS GREEN AT YEAST (supervisor-verified):
## the full parse-space chi/sA construction, gated end-to-end

ALL GATES GREEN (astra pass 3; supervisor re-ran the full construction
from main-tree sources): G0 battery 8/8 byte-identical; G1 YEAST
END-TO-END — slim dump 17.5 min / 4.81 GB peak at 3.34 Gbp -> .agg
BYTE-IDENTICAL to the standing baseline -> chi = 85,404,240 from BOTH
sweep modes (streamed 11.3 s; resident 105 s), witness set-equality vs
chi_yeast_pfp2.sA TRUE both; G3 fail-loud: 8/8 injected mismatches
rejected (exactness does not depend on hash luck).

THE MEASURED BOUNDS TERMS (the Lean theorem's other side):
- verification length: 15.33 phrases/seed AVERAGE, 22,262 max —
  the Sum-l term, measured.
- resolve: 278M walks / 2.1B LF steps / 64M cache hits, 0 failures
  (bounded exact LF cache; v5 head samples make it O(1)/run-head).
- 466 projection refined: 206.0 GB (v5 heads + LF cache) /
  162.2 GB (no-LF path, unimplemented). The 200 GB target is met by
  the no-LF path; the LF path is 3% over — name it honestly, the no-LF
  path is the follow-up lever.

ARCHITECTURE CLAIM NOW GATED (not projected): one pfp++ pass (29 min
at 30 GB human) emits RLBWT + samples + parse + dict; the construction
then runs entirely in parse space — fingerprints at both levels, no
M/b_bwt/w_wt, no suffix arrays, no LCP arrays, no lcp_index, no O(n)
after the read. Index cost tracks NOVELTY, not bulk.

Files: bit6/slim_lce.hpp, chi_rspace_dump.cpp (--slim complete),
xsa/src/main.rs (--stream-agg), tools/slim_* suite, SLIM_*.md reports,
gate_logs/slim/ (3 passes).

## 2026-09-26 — PUSHED (all 40 session commits to github.com:pangenome/sxgc,
## ea2ae0a..c82718d); k10 SLIM HUMAN PILOT running; v5 = ONE index file

V5 CONTAINER DECISION (user directive: minimal representation, one file):
the QUERY-SIDE artifacts bundle into a single v5 container — RLBWT +
run table + head/tail SA samples + anchors + the (delta-compressed)
chi set. That is THE index: one file, ~62 GB raw at 466 (~40-45 GB
compressed). BUILD-side artifacts (parse, dict, fingerprints, .agg)
stay separate by design — construction-time inputs, not the product.
Two lives: the index (ships, queries) and the crane (builds).

K10 SLIM PILOT (running, proc k10-slim-pilot): slim dump on the real-
human chain (h10new2.ri4 + laneN's h10rl_pfp parse) -> .agg vs the
production walk baseline h10.walk.agg -> streamed sweep -> chi ==
1,627,063,183 + set equality vs chi_h10.sA. The resolve phase pays
the known 2,156-steps/head LF tax (tail samples) — measured as part of
the pilot; v5 head samples are the adopted fix.

## 2026-09-26 — THE PILE recon (user's web-scale corpus, sequenced after
## the human rung): /mnt/nvme2n1/erikg/pile.txt — 1.31 TB web text

USER SEQUENCING: k10 pilot (running) -> 466 human -> THE PILE.

RECON (supervisor probes, ~1.1 GB read): 1.31 TB, 207 distinct byte
values, English-dominant natural UTF-8 text. Local 32-gram duplication
1-7%; cross-window 64-gram overlap ~1-2/8000; SYSTEMATIC block scan:
5 duplicate 4KB blocks of 263,520 (0.002%) across 540 sparse windows.
VERDICT: unique-dominant single-snapshot-style text — the ADVERSARIAL
regime, not the showcase. Expected n/r ~ 5-50 (roadmap's caveat line),
so a pile chi-index would run ~3-25% of text size (30-300 GB) — real
compression with random access (a legitimate FM-class artifact at
1.31 TB), but NOT the 500x novelty-priced pangenome story.

The pile's scientific value: the graceful-degradation test (high
entropy, sigma=207, no mega-runs, short natural-text LCEs — the
opposite stress from satellites). PLAN when its turn comes: slice-first
(50-100 GB slice, one day) to measure r/n, P, dict on real pile text
BEFORE any full-scale commitment — the syng lesson, generalized.

## 2026-09-26 — THE LEAN REFERENCE IMPLEMENTATION LANDS: the construction
## capstone PROVEN (kernel-verified; the "proof of it" delivered)

lean/SxgcBuild.lean (1,324 lines, 0 sorries, 0 native_decide) — the full
chain, axioms = propext/Classical.choice/Quot.sound on every theorem
(supervisor-audited):
- textLCEvia_correct: the LCE composition theorem — parse-space phrase-
  walk-with-id-jumps = ground-truth lcpOf on any well-formed parse.
- parseTriplesOf_eq: the parse-built triples stream IS triplesOf T —
  THE CONSTRUCTION WELD.
- parseScan_eq: the parse-based scan = the production scan.
- parseChi_eq (CAPSTONE): (parse scan).length = chi T, conditional on
  the four named pillars as EXPLICIT hypotheses (hcover=O1, hnd=O2,
  hmax=O3, hdisj=O4) — the honest conditional form: given the open
  pillars, the parse-space construction computes chi. Not hidden,
  named.
- VerifiedLCE_determinism: the named two-sided determinism wrapper.
lean/BuildEval.lean: the differential battery — 45 runs (15 texts x
k in {2,3,5}), parse-triples = triplesOf TRUE, parse-scan = scan TRUE,
|parse-chi| = chi oracle TRUE, negative control (wrong parse) FALSE
(non-vacuity). Mandatory duplicates text included. GLM also fixed a
real fuel-wiring flaw in the executable during the proof (mutual-def
lexicographic termination; supervisor-reviewed at ~line 860).

HONEST NOT-STARTED (next formal lane's queue): (1) step-count bounds
for textLCEvia/textLCEwalk — the work-accounting inequality whose
measured side we already have (15.33 phrases/seed avg, 22,262 max);
(2) the fingerprint-hash section (krs/chunkHash/fpParseLCE) — the
Rust oracle is now textLCEvia, only hash plumbing remains; (3) O2
adjudication (Nodup below saturation). In-Lean chi oracle = the
powerset minimality oracle (tiny texts only; scale oracle stays Rust/
sA — documented in HANDOFF).

## 2026-09-26 — BOUNDS PROVEN (astra lane): explicit step accounting
## with constants; the hash-scheme weld; O2 adjudicated with warrant

lean/SxgcBounds.lean (+ BoundsEval/BoundsAxioms/O2Eval): 32 kernel-
checked theorems, 0 sorries, axioms restricted to
propext/Classical.choice/Quot.sound (supervisor-audited from main tree).

THE BOUNDS THEOREMS (the retired O(tau) claim's honest replacement):
- explicit indexed-primitive step accounting over the reference
  implementation; reference per-query constant 8, total-work constant 16;
  fingerprint-scheme per-query constant 12, total-work constant 24 —
  bounds of the shape C*(1 + tau + l_i) per query with the verification
  length l_i EXPLICIT (correction #4's honest form, now a theorem).
- numerical spot checks pass (e.g. answer 12, steps 112, bound 180).
- HASH-SUBSTITUTION WELD: the sampled-hash + direct-read-verification
  scheme = the proven textLCEvia oracle, hash kept abstract (the SCHEME
  is proven, not a hash function). The Rust LCE primitive's semantics
  are now welded: Rust = Lean scheme = proven oracle.

O2 ADJUDICATION (statement-lock, with warrant): 729/729 bounded
battery entries (127 distinct texts), ZERO Nodup violations below
saturation; the saturation witness reproduces (duplicate emissions
above MAXINT). O2_bounded (hsat side-condition) is LOCKED as the
successor statement per house rules — remains OPEN (unproved), named
honestly. Note (lane's own residual): saturation refutes arbitrary-
stream Nodup; a TEXT-DERIVED counterexample for unbounded O2 is not
established — the retired-false record says "presumably false"; the
measured warrant covers the bounded form only.

HONEST SCOPE (lane-recorded): bounds are indexed-primitive accounting
(not Lean list runtime); fixed-block jumps modeled (production uses
binary search); reference counts not proved identical to yeast
telemetry; compiled C++/Rust refinement bridges remain the weld's
differential half.

## 2026-09-26 — THE HUMAN RUNG IS GREEN: k10 slim pilot lands
## (the parse-space construction validated on real human DNA)

SUPERVISOR-VERIFIED (main-tree rebuild): the k10 slim construction on the
30 GB human chain (865 contigs, R = 1,859,825,801, r/n = 16) —
- .agg BYTE-IDENTICAL to the production walk baseline h10.walk.agg
  (supervisor cmp'd the artifacts directly)
- chi = 1,627,063,183 EXACT + numpy sorted-set equality vs chi_h10.sA
- FULL PILOT: 1h36m49s, 73.98 GB peak — vs take-1's ~3-DAY projection.
  The head-SA sidecar (v5 field, transitional) collapsed resolve:
  437.5 s with ZERO LF steps (O(1)/run-head, exactly as designed).
- Yeast re-gate with sidecar: chi = 85,404,240, resolve 272 s -> 34.8 s.
- G0: 8/8 battery, sidecar AND fallback paths byte-identical.
- REAL BUG FOUND AT HUMAN SCALE, fixed in-lane: collection-LCP
  newline-boundary bug (the k10 multi-string corpus exposed it; the
  fix is in the committed diff, gated by the same byte-identity).

466 PROJECTION UPDATED (honest): 213.9 GB with raw heads + LF;
170.0 GB on the no-LF path (UNIMPLEMENTED — the named lever if the
200 GB ceiling is hard; the 214 number is the as-built commit).

THE PROGRAM STATE: the slim construction is now validated at yeast
(3.34 Gbp) AND k10 human (30 Gbp) — parse-space, no M/b_bwt/w_wt, no
suffix arrays, no lcp_index, O(1) resolve, both chains byte-identical
to production oracles. Remaining to 466: the 466 parse (pfp++ over
1.4 TB, ~day-scale by k10 extrapolation) + the ~214 GB construction
run. Sequencing per user: k10 (DONE) -> 466 -> the pile.

## 2026-09-26 — CORRECTION #7 + THE LAW (user): NO O(n) WALKS, EVER, EVEN
## ONCE — everything in parse/r-space; the phi-inverse route

The supervisor's "no way around the walk" was WRONG. The user's principle
is now law: if a build step costs O(n), we've lost.

THE PHI-INVERSE TRICK (supervisor-derived under user pressure): head
position of run j = SA-successor of the PREVIOUS run's tail position =
phi^-1(tail_pos[j-1]). The phi permutation is stored in the lcp_index as
O(r) intervals (the move structure) — a sorted index over the interval
images gives phi^-1 in O(log r)/query. ALL head samples computable in
O(r log r), ~30 GB RAM, MINUTES-TO-HOURS AT 466 — no O(n) scan, no walk.
The 466 walk launched earlier is KILLED; this replaces it. The lcp_index
is a PILOT-ERA SUNK ARTIFACT being mined for its O(r) phi table; nothing
O(n) is re-run. The 466 gate goes end-to-end: chi = 2,249,968,075 +
witness set vs chi_h466.sA.

FOR FUTURE CHAINS: the PFP-BWT front-end ALREADY emits SA samples
natively (.ssa output; the k10 pilot produced h10ss_pfp.ssa) in its one
mandatory pass — the v5 provenance clause is satisfiable with existing
tooling. No chain ever needs a walk.

EXTRACTOR GATE: k10's walk-derived head-sa sidecar (on disk) is the
BYTE-IDENTITY gate for the phi^-1 extractor before it touches 466.

## 2026-09-27 — THE WELD IS CLOSED + PHI-INVERSE PROVEN (correctness lane)

lean/SxgcPhi.lean (19 theorems, 0 sorries, axioms = standard trio):
headFromTail (THE EXTRACTOR'S CORE LEMMA: adjacent BWT rows -> head
position = phiInv(previous tail position) — the SA-successor algebra),
phiInv_phi (permutation inverse law), list-level interval-lookup
correctness (the extractor's binary-search semantics at the function
level). Phi battery: 0 mismatches across 381 positions.

THE WELD DIFFERENTIAL (the verification-ladder rung from the user's
original Lean-first directive — now EXECUTED AND GREEN): the executable
Lean reference (parseChi machinery, #eval) vs the Rust slim pipeline on
the 8 battery texts: 8/8 FULL WITNESS AGREEMENTS (sorted-list equality,
multiplicity preserved — stronger than set equality), aggregate files
byte-identical, coordinate conversion documented. "The implementation
follows the proven model" is now a GATE RESULT, not a claim.

Honest scope: the compiled binary-search refinement stays outside the
proof scope (differential-gated as always); the list-level semantics
are proven. Protected files (Sxgc/SxgcBuild/SxgcBounds) unchanged.

FORMAL LEDGER NOW: characterization, validity, scan=FM, parse weld,
LCE composition, capstone (conditional on named O1-O4), determinism,
bounds with constants, hash-scheme weld, phi-inverse extractor, AND
the implementation-vs-model differential — all green. OPEN: O1,
minimality (O2-O4 outright), O2_bounded, chi-necessity lower bound.

## MODEL DOCTRINE (user, 2026-09-27): astra = implementing/testing/correcting
## errors and bugs; job-running and gate management = glm-5.3 / flash
## models. (Matches the evidence: astra excelled at design-judgment work;
## process-running is cheap-model territory.)

## 2026-09-27 — PHI-INVERSE EXTRACTOR: ALL GATES GREEN AT ALL THREE
## SCALES; YEAST IS THE FIRST-EVER WALK-FREE SOUP-TO-NUTS CORPUS

SUPERVISOR-VERIFIED (main-tree rebuild; supervisor initially raised a
FALSE ALARM on the baseline column by misreading the .agg as row-major
— it is COLUMN-MAJOR: 12-byte header + four 8R columns; corrected and
confirmed):
- YEAST (G0): THE FIRST COMPLETE WALK-FREE RUN — phi^-1 sidecar ->
  slim dump -> streamed sweep -> chi = 85,404,240, numpy witness
  equality, ZERO LF steps, head byte-identity vs the walk-derived
  saFirst column (100,904,881 values, supervisor-cmp'd).
- K10 (G1): ALL 1,859,825,801 heads BYTE-IDENTICAL to the walk-derived
  sidecar — 751 s / 64.8 GB (vs the walk's 38 h: 165x, no O(n)).
- 466 (G2): all 2,739,735,806 heads in range, 38,551 boundary anchors
  matched (239 interior unchecked — noted), 830 s / 99 GB.
  h466.head_sa EXISTS at /tmp/laneS/h466.head_sa — the 466 slim run's
  position input is READY; only the parse remains.
- G3 correctly skipped (parse incomplete; pfp++ untouched).
- .ssa audit: localized HEAD+TAIL front-end emission change is
  feasible (the permanent from-scratch fix — queued engineering).

The extractor (bit6/phi_inverse_heads.cpp, -fopenmp): mines the sunk
lcp_index's O(r) phi table, sorted-image binary search, O(log r)/head.
Files: phi_inverse_check.py, phi_inverse_pipeline.py, README,
ACCEPTANCE, gate logs.

## 2026-09-27 — FORMAT DECISION + SEQUENCING DIRECTIVE (user): .sxi, and
## the from-scratch ladder — k10 then 466, from the AGC stream, in-format

NAME: .ri5 was the wrong name (r-index lineage, component-named). THE
FORMAT IS .sxi — "suffixient index", magic SXI1, VERSIONED INTERNALLY so
the filename never changes again. ONE FILE: RLBWT + run table + head &
tail SA samples + anchors + the delta-compressed chi set. The .ri4
family stays loadable (pilot-era artifacts); everything born after the
format lane writes .sxi.

SEQUENCING DIRECTIVE (user, supersedes the sunk-artifact 466 route):
1. Fix the format: front-end .ssa HEAD+TAIL emission (r-pfbwt,
   localized per the audit) + the .sxi container writer + the xsa
   .sxi loader. [astra engineering lane]
2. THEN k10 FULLY FROM SCRATCH in .sxi format, from the AGC stream —
   the complete one-pass recipe at 30 GB: front-end -> .sxi -> slim ->
   chi == 1,627,063,183. This is ALSO the RAM slice-measurement for
   the PFP-BWT build profile before the 466 commitment.
3. THEN 466 fully from scratch in .sxi from the AGC stream (the
   running h466rl_pfp parse feeds it — same bytes). chi == 2,249,968,075.
The 466 milestone IS the pure build; no inherited-artifact shortcut.

## ACCEPTANCE TEST DIRECTIVE (user, 2026-09-27): xsa build is tested by
## running it at ALL THREE SCALES in exactly its final UX form (text/AGC
## input -> .sxi), supervisor-gated against the production oracles:
## yeast (chi 85,404,240), k10 (1,627,063,183), 466 (2,249,968,075).

## Correction #8 (supervisor, self-caught): the 466 walk kill was never verified.
The "walk killed" claim (turn of the syng retreat) killed the parent shell only;
teralcp_chi_agg ran orphaned (init-reparented) for ~2h15m at 267 GB RSS, contending
with pfp466 for disk before a second kill (verified this time: pids gone, RAM freed).
House rule reinforced: every kill is followed by a ps of the target pids + RSS delta.

## MILESTONE: yeast born fully from scratch via `xsa build` (G0/G1/G2 GREEN, commit follows).
Endpoint-tap lane (user decision A) landed: 27-line vendored patch on r-pfbwt (upstream
untouched, .ssa byte-identical, .ssa_t = run-tail SA values the merge already computes);
xsa build pipeline real (parse -> front-end -> slim -> sweep -> internal chi gate -> .sxi,
fail-closed). Yeast: xsa build --text -> yeast.sxi (1.80 GB, chi=85,404,240 embedded,
delta-chi 12.99%); fresh heads byte-equal /tmp/laneS/yp2.phi.head_sa and ENTIRE fresh .ri4
byte-equal yp2real.ri4 (908 MB) - oracles gated, never consumed. Per-stage yeast profile:
rpfbwt 624s/8.8GB, slim 833s/3.7GB, sweep 15s/6MB, total ~26 min, ZERO LF walks.
k10 from scratch RUNNING (self-gated: expect-chi 1,627,063,183 + heads/tails byte gates;
also the 466 RAM slice measurement). Honest limits recorded: upstream empty-chunk .ssa
merge bug worked around by chunk config (not fixed); newline-joined vs BCR collection
ordering to be adjudicated by k10's real byte gates.

## 466 pfp++ parse COMPLETE (h466rl_pfp): 15,517,244,887 phrases, dict 89,908,623
## phrases / 18.52 Gbp total length, 10h35m wall, 28.7 GB peak RSS, exit 0. Rate
## doubled after the walk-orphan kill freed the disk (finished ~3h early). File
## set matches k10's exactly; feeds the 466 xsa build, which is QUEUED behind
## k10 green (k10's per-stage RAM profile parameterizes the 466 run).

## AGC PATH PROVEN AT YEAST SCALE (xsa build --agc yeast235.agc -> yeast235_agc.sxi,
## 1086s, published). chi = 85,350,673 EXACTLY the ledger's yeast235.rl chain number -
## the .rl chain was revlines-convention all along; the Sep-17 '$'-separated yeast235.txt
## was older archaeology. Full journal in bit6/sxi_logs/yeast235_agc-*.jsonl: agc2flat
## --revlines --upper -> parse -> l2 -> patched rpfbwt -> endpoints -> [gate-heads,
## gate-runs-tails byte-equal vs --text control build] -> slim -> sweep -> write ->
## validate -> publish. First run (chi-only) fail-closed on the multi-string interlock
## as designed; control-build endpoints then gated the rerun. Multi-string interlock
## semantics: endpoint byte gates required; oracle-free equivalence = --text control.

## YEAST RE-ADJUDICATED CLEAN (repair lane): chi 85,404,240 and ALL five embedded
## members unchanged through the certified pad-removal path - the published
## yeast.sxi is contract-valid as-is. Slim cyclic-LCE fix landed (1.3M comparisons).
## Multi-string seam repair remains: the local scheme has a proven Theta(n) wall on
## adversarial (constant-run) classes; DESIGN DECISION: class-based seam repair
## (reorder seam-equivalence classes via cyclic-LCE sorting, r-space when classes
## are small - yeast235 seam measured 6,893 rows = 0.007% of R) + LAWFUL fail-loud
## refusal for pathological corpora (the law is preserved by refusing, not by
## walking). Derivation: bit6/sxi_logs/sep-convention/repair/DERIVATION.md.
## Also open: sweep emits n+1 witness for cyclic 0x1E input (legacy sentinel
## convention, xsa/src/main.rs ~1012).

## SEAM REPAIR LANDED: first multi-string canonical publication. yeast235.sxi
## (0x1E cyclic contract) PUBLISHED, chi = 85,404,336 - exactly +96 above the
## single-string yeast corpus (85,404,240) with k=9,901 records: the FIRST
## MEASURED boundary-delta identity instance (~1 witness per ~103 separators).
## Old 85,350,673 resolved as legacy-convention (the cross-chain mystery pair
## was two conventions of near-identical corpora). Yeast regression: chi +
## all five members unchanged (certified path intact). Separator suite 47/47,
## 2,214 endpoint cases, constant-run refusal gate (adversarial classes REFUSE
## loudly per policy). Witness n+1 fixed (zero-based cyclic). Residual flag:
## container k semantics inconsistent (k=1 byte-strings vs records 9,901) -
## query lane to pin. Open: k10 canonical + 466 under the repaired frame.

## QUERY PRODUCT LANDED (supervisor-verified): xsa is now a pangenome searcher.
- xsa mems --sxi ... --reads fq|fa|gz -j N: bounded-memory rayon streaming, MEM(len,
  qstart, name, offset, strand); 46K+ MEMs brute-verified (23 fixtures + yeast 1,720).
- name+offset annotation via boundary array; container k = named records (yeast235
  k=9901, members unchanged); native SXI loader byte-identical aggregates to RI4 path.
- --mode auto: fasta/fastq/agc => dna (revcomp, strand), --text => generic; explicit
  overrides; fail-loud on non-IUPAC query bytes in dna mode.
- xsa serve: POST /query /ms /batch, GET /stats; HTTP byte-identical to CLI.
- --verify-text-sample N: in-flight ground-truth witness audit vs the materialized
  text (32/32 at gates; corrupted text blocks publication) - the 466 gate.
- Terminal-byte check generalized (any separator flows to the endpoints adapter).

## ROPEBWT3-COMPAT OUTPUT LANDED (supervisor-verified): xsa mems --out ropebwt3 -
## one line per SMEM (qname, qstart, qend, hit_count [+ name:strand:pos tokens,
## forward-normalized]), --mem all-MEMs, capped sampling, --gap/--cov companion
## modes. ORACLE: real ropebwt3 built from source; 23 parity fixtures agree on
## SMEM sets, counts, forward-position multisets, gaps, coverage; 756 SMEM
## intervals + 44,583 occurrences brute-verified; native mode + six members
## unchanged. Documented divergences recorded in the acceptance note.

## CHI-NECESSITY RECON COMPLETE (laneY, GLM background researcher) - the theory
## frontier is now MAPPED. Findings (all web-verified, sources in
## lean/LOWER_BOUND_PLAN.md):
## 1. VIRGIN TERRITORY CONFIRMED: NO space lower bound proportional to chi (nor
##    even r) is proven ANYWHERE for ANY pattern-matching operation class - every
##    published r/chi-index is an upper bound; "optimal" claims are time-only.
## 2. THE LINKAGE (flagged inference): the chi-floor is NOT free-standing - it is
##    equivalent up to logs to the OPEN chi-vs-delta*log(n) placement (Navarro
##    CPM 2023 already does MEMs in O(delta log n) words). If chi = O(delta log n)
##    always, the natural-tier chi-floor is provable family-specific; if not, the
##    Omega(chi)-floor for MEMs is REFUTED (a negative theorem, also publishable).
## 3. PROVABLE NOW (S1 bridge): any correct locate-one oracle's emitted positions
##    form a suffixient set => chi <= |emitted| - mechanical from proven theorems;
##    the first rung. family_counting (pigeonhole) elementary. Attack 1 ranked:
##    witness-perturbation family + answer-function counting => Omega(chi log(n/chi))
##    bits, family-specific first, #eval-batterable against brute-chi.
## 4. HONEST CALIBRATION: a proven natural-tier floor = ~0.56 B/run at yeast -
##    underwrites only ~4% of the 12.7 B/run engineering floor (the rest is SA
##    samples/RLBWT representation, not chi). Delta-coded witnesses measured
##    8-22 bits each: already within ~1.6-4x of the honest theory floor.
## 5. CORRECTION #9 (supervisor, lane-caught): the r-index is Gagie-Navarro-
##    Prezza (J.ACM 2020), NOT "Belazzougui-Canovas-Navarro" as my lane brief
##    said. The researcher refused the silent fix and recorded it.
## NEXT: bridge+family_counting in Lean (mechanical), #eval go/no-go battery for
## the perturbation family, full-paper read of the collapse theorem boundary
## (does count/locate have a delta-space index? - decides final statement form),
## measure delta/gamma on yeast+k10 slices.

## TWO MATH LANES BANKED - the theory ledger advances materially:

### 1. O2_bounded PROVEN OUTRIGHT (laneW, GLM-5.3-high) - A CAPSTONE PILLAR CLOSES.
Via a strictly stronger stream theorem, scanAux_nodup (lean/SxgcNodup.lean): ANY
distinct-sa, position-bounded, lcp-bounded triple stream scans Nodup. Ten invariants;
boundedness used EXACTLY ONCE (blocks re-arm at run-length-1 boundaries - the precise
spot unbounded streams duplicate). O2 assembles byte-for-byte from it; its own positive-T
hypothesis is unused (stronger theorem). Axioms clean; Sxgc.lean byte-untouched.
Capstone parseChi_eq pillar ledger: O1 (grinding, laneX) / O2 (PROVEN) / O3 (clean,
empirical) / minimality (open). The stale no-proof note in SxgcBounds is superseded by
O2_bounded_true (measurement owner to annotate).

### 2. Seam-repair IDENTITY HALF PROVEN (lean/SxgcSeam.lean): rot_agree, 
identity_outside_classes, agreement_with_nonseam, rotation_cmp_total - the frame-level
core of DERIVATION.md is now theorems. Reordering half statement-locked with measured
warrant (yeast235: 7 classes, 13,503 rows, byte-identical to the dense oracle).

### 3. Boundary-delta statement-locked (boundaryDelta_bounded) with honest obstruction
(position-shift + coverage-coupling between the class characterization and insertion).

### 4. THE FLOOR PROGRAM STARTS COMPOSING (laneY, GLM-5.3-high; lean/LowerBound.lean):
- S1 BRIDGE PROVEN, kernel-checked: emitted_suffixient + chi_le_of_oracle - ANY correct
  locate-one oracle's emitted positions form a suffixient set, chi <= |emitted|. The
  first theorem connecting ARBITRARY INDEXES to chi.
- family_counting PROVEN (pigeonhole, fixed decoder, no classical axioms): pairwise-
  incompatible correct answers force one index per family member, |F| <= 2^(s+1)-1.
- ATTACK-1 BATTERY: GO (reformulated honestly) - strict chi-invariance REFUTED and fully
  characterized (chi = 2k + #{i>=2: p_i >= 1}; junction/marker classes coincide at p=0),
  but the uniform bound 2k <= chi <= 3k-1 holds on all grids - which is what the floor
  needs: family_counting + chi_fam_bounds (next Lean target, battery-pre-validated) +
  marker injectivity => Omega(chi log(n/chi)) modulo bookkeeping.
All eval runners unchanged-green incl. Main BIT 1B GATE 511/511.

## LM-OVER-CHI-INDEX RUNG - DESIGN SPEC (user-directed record; conversation 2026-09-28)
The experiment rung, now measurement-designed. Core thesis: memorization externalized
into the index (which the bridge theorem says any continuation-answering system must
consult); network capacity spent on policy (choice among forks), not on rulebook storage.

### The three-mode generation loop (the resolution of "how does the net react"):
The index delimits reaction points EXACTLY. Per generated position, backward search gives:
- FORCED (the overwhelmingly common case; chi ~ 0.85*r << n means most contexts admit
  ONE next byte): mask dictates; the net passes the byte through - no compute needed.
- FORK (at witness-neighborhood decision points): the fork set C = distinct next-chars of
  the MS/SA interval, each with counts + witness (doc, offset) annotations; the net
  set-encodes the candidates and CHOOSES. Its expressive freedom is chi-shaped.
- OFF-CORPUS (MS length collapses to 0: the walk left every known continuation): free
  emission until re-entry; the index marks novelty exactly.
Training = learning the FORK policy (+ when to leave and re-enter).

### Architecture (v1 dumb-first, per ablation discipline):
byte-level small RNN (100M-4B range), input = [chosen byte ; fork-set features]
(candidates, log counts, doc-label features; set-encoder or padded); hard mask to C.
Hybrid extension (ablation-gated): sparse attention over PINNED corpus pointers -
the walk's committed witnesses (doc, offset) - a rotating/circular bounded pin buffer.
ATTENTION = QUERY EMISSION: a head emits offsets into the corpus; the index returns the
bytes there ("what happened next at this witness"); those bytes enter the state. Keys/
values are not stored in the context window - they live in the corpus, retrieved by
position. This unifies attention and index lookup.

### Reading fragments (MEM-reasoning mode):
The matched MEM content needs no re-reading (it equals the probe). What is fed back =
the witness CONTINUATIONS: the next m bytes after each MEM's target, per-MEM encoded,
then chosen among. Ops space: byte-fork, mem-copy, mem-with-edits, jump-to-doc,
emit-new. Novelty detection is exact (MS boundary). 

### Training signal for the op policy (free supervision):
Haplotype pairs (and pile near-duplicate families) ARE supervised MEM-op traces: the op
sequence reconstructing haplotype B from A is an alignment; mine with the MEM machinery.
DNA mode: op-traces = variant recombination, annotated by nature.

### Stages:
0: sweep the pile .sxi -> per-position fork sets/MS/witness flags (server = dataloader:
   POST /batch). 1: pile pretraining, hard-masked next-byte, candidate featurization.
2 (the headline): swap indexes under the same weights (web -> other web -> DNA pangenome);
   fast re-adaptation = the net learned domain-agnostic fork navigation.
Scaling law: net size x index presence; flattening curve = memorization-tax thesis.

### Baselines: pure n-gram (fork histogram), pure net (no index), k-NN retrieval
(Infini-gram replication), net+index. Prediction (falsifiable): index-augmented
agreement concentrates at witness-dense regions; stratify metrics by witness density.
Pile labels (pile_set_name) = per-component analysis for free.

### Open: GPU inventory for the byte-level 1.31TB run; v1 = dumbest version (RNN +
mask + features); pointer-attention only if ablation demands it.

## O1 REDUCED (laneX, GLM-5.3-high): O1 iff O1_maxHit (every inclusion-maximal coverage
## class contains an emitted position) - a single named statement now carries the pillar.
## FACTORIZATION PROVEN: O1 iff RunEdgeHit AND RunEdgeDominate. RunEdgeHit = cheapest
## next target (purely semantic: every maximal class contains a run-edge position; needs
## only row/position alignment lemmas over proven saOrder/lcpOf facts). RunEdgeDominate
## = blocked on the emission-set's semantic identity - the SAME pending endpoint-rule/
## exact range-min model the v3 events refutation hit at scale (no crisp invariant yet).
## New proven en route: covSet_ne_of_mem_positionsT (the exists_max_above side condition
## now unconditional), covering_given_stream_of_maxHit, maxHit_iff_reps, maxHit_in_class,
## runEdgeHit_of_maxHit + maxHit_of_runEdge. Statement-locked with differentials:
## 0 counterexamples across 729-binary/243-ternary/structured batteries; sorry ledger
## unchanged; Bit 1B gate GREEN. Full analysis: lean/O1_OBSTRUCTION.md.

## CHI_FAM_LOWER PROVEN (laneY, GLM-5.3-high): 2k <= chi(famText) - the floor program's
## family bound, lower half kernel-checked (92 declarations, 0 errors). The engine:
## notreq_of_unique_window (unique window => no requirement) with two instantiation
## engines (marker + block-boundary pinning); req_word_shape (every requirement word is
## eps or a pure run); chi_ge_letters general lemma. Upper bound: full structure proven,
## two marked cover-case stubs remain (fam_run_occ_le, fam_cover) - plan in the battery.
## Composition skeleton statement-locked: fam_forced_incompat, fam_oracle_witness,
## floor_theorem_shape. Toolchain discoveries recorded (no-Mathlib workarounds, the
## getElem_idx_congr index-congruence lemma). Bit 1B gate GREEN throughout.

## Correction #10 (supervisor, self-caught, pre-commit): the RunEdgeHit lane crashed
## mid-assembly; a STALE-CACHE `lake build` reported green while the file had 5 real
## errors (type mismatch, failed rewrite, unknown tactic, unsolved goals). Explicit
## `lake build SxgcRunEdge` exposed them. NOTHING was banked. House rule added:
## kernel-check claims require an explicit-target rebuild (or #print axioms on a fresh
## import), never a cached full-package green. Lane revived with the error list.

## RUNEDGEHIT PROVEN OUTRIGHT (laneX, revived after crash; final assembly closed by
## supervisor - two list-associativity fixes): Sxgc.runEdgeHit_true, kernel-checked,
## axioms exactly {propext, Classical.choice, Quot.sound} (fresh-import audit), explicit-
## target build green, Main BIT 1B GATE GREEN, zero sorries in SxgcRunEdge.lean.
## LEDGER EFFECT via the proven factorization (O1 iff RunEdgeHit AND RunEdgeDominate):
## O1 now reduces to RunEdgeDominate ALONE - the endpoint-rule/exact-range-min obstruction
## is the pillar's entire remaining content. Lean file: SxgcRunEdge.lean (783 lines,
## namespace Sxgc). Correction #10's strict-verification rule enforced throughout
## (explicit-target rebuild + fresh-import #print axioms; the crashed lane's cached
## green claim was false, the real proof needed 2 more fixes).

## Cover-case lane: INCOMPLETE on the proofs, DELIVERED on the warrant. The battery's
## emitted values match the recorded chi-law exactly (chi-linearity (2,2,2)->(16,8,8);
## 2k <= chi <= 3k-1 on every emitted member) - the family bound's empirical warrant is
## now confirmed on live data. But fam_run_occ_le and fam_cover still carry their marked
## stubs: chi_fam_bounds upper half remains OPEN. Lane's "complete and banked" claim was
## overstated; recorded here per discipline. Refire: narrow two-lemma lane.

## CHI_FAM_BOUNDS COMPLETE (narrow lane, GLM-5.3-high; strict chain: explicit build rc=0,
## fresh-import axioms {propext, Classical.choice, Quot.sound}, Main BIT 1B GATE GREEN):
## 2k <= chi(famText k L ps) <= 3k-1 KERNEL-CHECKED (Sxgc.LowerBound.Fam.chi_fam_bounds).
## Both cover cases closed: fam_run_occ_le (window localization + half-grid squeeze +
## marker contradiction; axioms {propext, Quot.sound} - cleaner than allowed) and
## fam_cover (full case analysis: single-letter covers + three run-continuation
## families incl. the div/mod boundary trichotomy - omega cannot see through i*(L+1)
## products; every cross-block bound needs a typed Nat.succ_mul bridge). Assembly:
## coverSet/mem_coverSet/coverSet_length/chi_fam_upper/chi_fam_bounds. Proof-engineering
## findings recorded for future lanes. The floor program's central object: MEASURED
## ((2,2,2)->(16,8,8) battery) and PROVEN. The 3 skeleton statements
## (fam_forced_incompat, fam_oracle_witness, floor_theorem_shape) remain locked,
## awaiting exactly this theorem for assembly.

## THE FIRST SPACE LOWER BOUND FOR A PATTERN-MATCHING CLASS - PROVEN (family-specific):
## Sxgc.LowerBound.Fam.fam_floor_chi, kernel-checked, axioms {propext, Classical.choice,
## Quot.sound}, explicit build rc=0, Main BIT 1B GATE GREEN. EVERY fixed-decoder index
## answering locate-one on all (L/2+1)^k half-grid members with s-bit indexes satisfies
## s + 1 >= chi * log2(n/chi) / 3   (chi in [2k, 3k-1] by chi_fam_bounds; n = k(L+1)).
## Bridge (chi <= |emitted|) -> counting pigeonhole (fam_floor_half_grid, grid Nodup via
## nodup_flatMap_of_disjoint, famText_inj_grid) -> chi-translation (floor_theorem_shape
## as locked). The recon's ranked Attack 1 executed end-to-end in ~36h from plan to
## kernel: witness-perturbation family + answer-function counting = the natural tier.
## STATEMENT-LOCK EVENT (adjudication pending user): the third skeleton fam_oracle_witness
## as originally locked is REFUTABLE (counterexample k=1,L=2,s=1 documented in its
## docstring: the smallness must be concluded, not assumed) - the intended content is
## proven as fam_floor_half_grid; fam_floor_chi does NOT depend on it; revision proposal
## recorded (add the smallness hypothesis or replace conclusion with the inequality).
## The floor is FAMILY-SPECIFIC; the general rung and the chi-vs-delta*log(n) linkage
## remain the open frontier (recon: equivalence up to logs). Proof-engineering notes
## retained (omega nonlinear-atom limits, typed Nat.succ_mul bridges, getElem transport).

## K10 BORN FROM ZERO - PUBLISHED (astra lane, supervisor-verified). k10.sxi at
## /mnt/nvme3n1/erikg/sxgc-k10sep/from-zero-p_fwd37s/ (33.96 GB): n=30,151,407,545,
## R=1,859,825,862, CANONICAL chi = 1,627,067,257 (embedded, delta-coded to 1.65 GB).
## TWO complete independent fresh builds (A control, B gated); complete publications
## byte-identical. Root cause of the endpoints failure: the pipeline INVOCATION left the
## adapter's terminal arg at its stale 0x0A default while the canonical text's row-0 char
## is 0x1E (supervisor static analysis had the right check, wrong level - live
## instrumentation found it at row 0). Adapter now validates/infers the terminal +
## handles clean and straddled padding boundaries with refusal diagnostics; 7 new
## regression checks; all prior gates green (2,214 cyclic cases, 47 separator, yeast +
## yeast235 byte-equal to accepted controls). k10 seam: 7 classes / 1,161 rows repaired
## (r-space at 1.86e9 runs). Fresh-build peak RSS 133.9 GB (THE 466 PARAMETER).
## MEASURED DELTA: canonical 1,627,067,257 - historical BCR 1,627,063,183 = +4,074
## (O(k)-scale for k=865; same-frame witness attribution UNPROVED - recorded honestly;
## the boundary-delta identity theorem remains the theory's open answer).
## NOTE: k10.sxi records k=1 (byte-strings) for --text raw input - correct per pinned
## semantics (no names sidecar); AGC/fasta/fastq paths record named records.

## Correction #11 (user directive, supervisor overrode it): I launched the 466 run
## file-based AFTER the user had asked "why would we materialize 1.4tb" and after the
## FIFO streaming was proven byte-identical - because free disk made materialization
## *feasible*. The user's directive was about the RIGHT pipeline, not the feasible one.
## BUILD KILLED + scratch removed. STANDING DIRECTIVE (now policy): xsa build NEVER
## materializes text to disk - the pipeline's only O(n) object is the mandatory read,
## in flight (FIFO); audit ranges come from the archive; scratch is pure r-space.
## The streaming lane (a0627df9) is implementing it; 466 fires STREAMED behind its
## gates. The four preflight lessons from the killed run remain valid (durable tools
## path, same-filesystem rule, the banked agc2flat --sep, interlock amendment).

## STREAMED PIPELINE LANDED (supervisor-verified from main tree): xsa build --agc no
## longer materializes - archive -> FIFO -> pfp++ (single writer, broken-pipe/SIGTERM
## lifecycle), names emitted same-pass, --verify-text-sample audit fetches ranges FROM
## THE ARCHIVE (bounded range access; corruption blocks publication); legacy --materialize
## kept as a forensic flag. GATES: full yeast235 streamed build vs control - ALL FIVE
## core members + parse + dict byte-identical, chi=85,404,336 both, 32/32 archive
## witnesses, no collection.txt created; battery member-equality/archive-range/corruption
## gates pass from main tree; FIFO lifecycle gate; committed regressions green. Peak RSS
## 9.06 GB at yeast. Interlock amendment re-applied to the banked pipeline (lane copy
## predated it - caught at bank time). STANDING DIRECTIVE SATISFIED: the pipeline's
## only O(n) object is the mandatory read, in flight.

## Correction #12 (user-caught, throughput unmeasured): the 50 MB FIFO "proof" validated
## byte-correctness ONLY - no rate. The streamed 466 run measured ~2.4 MB/s (pfp++ at
## 0.66 of its 16 allocated cores; the single pipe serializes pfp++'s seek-parallel
## chunk reader) = ~6 days for 466. KILLED at 1.5h. Also: --threads 16 was carried from
## a crowded-machine k10 context - meaningless on the idle box (the user asked the right
## question; threads were never the constraint). FIX (user's directive, engineered):
## GHOST FILE over the AGC - a seekable view (FUSE or equivalent) where pfp++'s parallel
## readers get regions served ON DEMAND from the archive; every read is an archive query;
## nothing text-sized on disk, ever. GATES: yeast235 ghost-file build byte-identical AND
## measured parse throughput >= file-based/2 at -j 16 - correctness AND rate, both.

## DIRECTIVE (user): patch pfp++ to read AGC DIRECTLY - no FUSE (an environment privilege,
## not a foundation: unavailable in sandboxes/HPC; not a reliable product base). The AGC
## reader is written INTO pfp++ as a first-class input mode. Institutional form: FORK the
## tool under the pangenome org, edit there, submit a PR back upstream so the author
## benefits. POLICY: no more carried .patch files in this repo - vendored changes live in
## pangenome-org forks with upstream PRs (the rpfbwt tap patch gets the same treatment).
## The mandatory read becomes "read the archive" natively; canonical extraction rule
## (revlines, --upper, 0x1E) inside the reader; thread-parallel range serving.

## PFP++ READS AGC NATIVELY - LANDED (two lanes, supervisor-verified): the parse stage
## now consumes the archive directly (pfp++ -t <archive> --agc --agc-names; canonical
## extraction - uppercase reversed contigs, archive order, 0x1E - inside the reader;
## FIFO fallback; --materialize forensic-only). GATES: full yeast235 native publication,
## five core members byte-identical + 32/32 archive witnesses; all 3.34 GB canonical
## bytes identical vs agc2flat; 20x100MB HPRC windows byte-identical at 466 scale;
## measured reader throughput 440-535 MB/s (73-87% of file-based; optimum j16 at HPRC);
## MEASURED 466 PARSE ETA: 6.63 h (upstream parse loop is serial; the reader feeds it
## at 509 MB/s at j16 - supervisor's 48-min extrapolation was wrong, lane's is right).
## Vendored durably at vendor/pfp-agc-fork (reproducible via tools/build_pfp_agc.sh +
## the committed patch; agc lib refresh-bio/agc@e67e3fc, static). Upstreamable: clean
## optional CMake (PFP_ENABLE_AGC), README, PR draft in bit6/sxi_logs/pfp-agc/.
## Interlock amendment re-applied at bank (lane copies keep predating it).

## DISTRIBUTION HARDENING LANDED (supervisor-verified): tools/MANIFEST.sha256 (75
## artifacts), strict preflight hash verification (single-byte tool corruption rejected
## before any scratch/output; --allow-drift is debug-only), tools/build_all.sh (one
## fresh-checkout build: in-repo C++ + patched pfp++ + patched rpfbwt + pinned HTSlib +
## agc; upstream.lock.json pins), journal provenance (every build binds manifest + tool
## hashes + output SHA256). GATES: 7/7 battery through the sealed toolset; yeast235
## PUBLISHED a third time from a fresh checkout (chi=85,404,336, 32/32 archive audit,
## no materialization) - three independent toolsets, one answer; 336 protected binaries
## (the RUNNING 466's tools) hash-identical before/after. DESYNC IS NOW IMPOSSIBLE TO
## MISS. Interlock amendment re-applied at bank for the THIRD time - lane copies keep
## predating it; root fix queued (lanes must start from main's current pipeline).

## CHI-AS-ML-BACKEND SPEC BANKED (researcher lane, GLM-5.3-high; code-cited, hardware-
## probed, honest): docs/LM_CHI_BACKEND_SPEC.md. HEADLINE FINDINGS:
## (1) THE 0x1E ALIGNMENT: Emender's DocumentStreamDataset ALREADY uses 0x1E as its
##     document delimiter - the same byte the .sxi reserves as record separator. Model
##     byte stream and index multi-string convention are ONE contract, natively.
## (2) THE HOOKS EXIST: LadderLM.forward already supports loss_mask/reset_before/
##     doc_boundaries - masked fork-conditioned pretraining needs ZERO loss-machinery
##     changes; fork features enter via one new projection GEMM onto the embedding.
## (3) COMPUTE REALITY (live probe): this box has >=7x RTX 6000 Ada 48GB (336 GB VRAM);
##     Emender's own 1.273B anchor = 0.973 bpb on The Pile in 23 days on ONE such GPU.
##     The full staged plan is feasible HERE at slice scale; full 1.31 TB byte-level
##     pretraining is honestly a cluster job - and the stratified falsification test
##     does NOT need terabyte scale to be decisive.
## (4) ARCHITECTURE VERDICT: pins ride BESIDE the NDM delta memory (the 32x32 store is
##     content-addressed, not a pointer store); the delta-correction write is literally
##     a prediction-error write = the natural fork-policy carrier (interpretation,
##     labeled, must be measured not asserted).
## (5) 12 open questions recorded (off-corpus eval has no ground truth by definition;
##     op-credit assignment; 0x1E dual-use policy; doc-id embeddings could poison the
##     stage-2 transfer test).
## Stage 0 = pile slice sweep (also measures the missing pile constants). QUEUED behind
## paper + human index. Lean follow-up identified: CorrectContinuation statement-lock
## (mechanical analog of CorrectLocateOne) - hardens the bridge's LM form.

## PAPER DRAFT BANKED (GLM-5.3-high lane + supervisor compile check): paper/main.tex
## (623 lines) + refs.bib (11 verified entries, r-index correctly attributed per
## correction #9) + FACTS.md (claim->artifact map; a "deliberately NOT claimed" list).
## Statement-lock discipline carried into prose: formal statements quoted in exact Lean
## forms (O2/scanAux_nodup discharged; O1 reduced to RunEdgeDominate with RunEdgeHit
## proven; fam_floor_chi verbatim with hypotheses; the refuted skeleton recorded
## visibly); every number sourced to a log path; the honest bounds (no O(tau) claim
## preserved); the 0.19-0.56 B/run proven-floor calibration included. \hprcchi
## placeholder (4 occurrences) ready for the 466 drop. COMPILED by supervisor (tectonic;
## 3 fixes: \Bits math-mode wrapper, bib note underscores, note math) - 0 errors.

## THE CONTINUATION BRIDGE PROVEN (lane, supervisor-verified strict chain): lean/LM.lean
## (9 theorems, 0 sorries, axioms {propext, Quot.sound}): CorrectContinuation defined
## (requirement-driven form); emitted_suffixient_of_cont + chi_le_of_cont_oracle (bridge
## analog, mechanical as predicted); AND THE LM-FACING FORM OUTRIGHT:
## chi_le_of_cont_oracle_distinct - any correct continuation oracle must CONSULT >= chi
## DISTINCT positions - plus chi_le_of_realized_cont (the fixed-decoder index form).
## The memorization-externalization thesis now has a kernel-checked warrant. The
## CorrectContinuation => CorrectLocateOne implication also proven (continuation is the
## strictly weaker demand, so the new bridge subsumes the old). Identified follow-on:
## the fam_floor_chi counting analog for continuation oracles (bridge shapes now
## identical - mechanical).

## CARGO-VENDOR LANDED (supervisor-verified): `cargo install xsa` = the complete product.
## 9.92 MB crate (under the crates.io 10MB limit - the lane built a vendor-compaction
## tool to fit); all 8 stage tools compiled in-crate (xsa/build.rs + build_tools.py);
## install 5m17s; works with ZERO env vars, source trees, or prebuilt tools; battery
## 7/7 THROUGH THE INSTALLED BINARY; drift-rejection intact (corruption fails before
## scratch); installation provenance (SOURCES.sha256.json + embedded stage hashes);
## relocation-proof. SNAPSHOT CATCH at bank: the lane had snapshotted laneV's polluted
## agc2flat (FUSE-era ghost/fuser leftovers, never banked) instead of main's native-
## reader version - caught by test_cargo_sources, fixed by regenerating snapshots from
## main (package_xsa_sources.py). Interlock amendment re-applied (4th time).

## 466 FRONT-END SCALE BOUNDARY FOUND: rpfbwt died std::bad_alloc after 1h31m at ONLY
## 36.3 GB RSS (750+ GB free, heuristic overcommit, commit limit nowhere near) - at the
## 'computing SA of the dictionary' phase. 466's dictionary = 89.9M phrases / 18.52 Gbp
## total length > 2^32 - the first corpus past a 32-bit length/offset path (k10's dict
## fit under; n and dict-length both crossed). Projected RAM correction: front-end RAM
## is DICTIONARY-driven, not R-driven (my R-similar extrapolation was wrong).
## The 6.6h parse is RETAINED (15.5G phrases + 17.75 GB dict in the work dir) - debug
## iterations cost minutes. Fix = another upstreamable fork patch (32-bit overflow in
## the dict-SA path); then the milestone reruns FROM ZERO per doctrine.

### Correction #13 — supervisor misdiagnosis of the 466 front-end failure
Claimed: a 32-bit length/offset wrap in rpfbwt's dictionary-SA path (dict > 2^32, k10 under).
Also misread /usr/bin/time "1:31.36" as 1h31m - it was 91 seconds.
Truth (lane's instrumented reproduction, LD_PRELOAD allocation tracer): the failing request was a
LEGITIMATE 148,895,781,360-byte allocation (18,611,972,670-entry dictionary SA x 8 bytes) denied by
RLIMIT_AS = 149,000,000,000 - THE PIPELINE'S OWN ADDRESS-SPACE CEILING, set on every child by
sxi_pipeline.py since the distribution lane. The dictionary SA alone exceeds it. No overflow exists;
the lane correctly REFUSED to fabricate the upstream 64-bit patch I commissioned.
Repair: `xsa build --address-space-gb N` (default stays 149; journaled; inherited hard limit respected).
Gated: 3 tiny builds at default/850GB/inherited-1GB -> byte-identical indexes (chi=1401); invalid rejected.
Lesson: before proposing a code bug at scale, instrument the actual failing request - and read time -v
format before converting units. Infrastructure lesson: a failed codex run's sandbox teardown killed the
detached validation tree ~20 min later (0-byte .time files = SIGKILL signature); relaunch validation as a
supervisor-owned process.

### Near-miss (supervisor, logged for honesty): accidental 69 GB materialization
While preparing the 466 validation relaunch I invoked the pipeline's AGC *prepare* verb
(agc2flat --revlines --upper --sep 1e -o collection.txt) believing the audit stage needed the flat
listing. It is the MATERIALIZED-path verb: 69 GB of flat text written before the 900s timeout killed
it; deleted immediately. The streaming audit never opens the text path when given --agc/--names
(sxi_text_audit.cpp line 24: `if(!archive){open...}`), and names.tsv is metadata-only from the parse.
Lesson restated: the streaming path has NO prepare stage; audit boundaries come from AGC + names.tsv.
A metadata-only listing (ragc-ffi or agc2flat --ghost) is available if ever needed.

### Address-space default removed (user decision)
A fixed 149 GB default ceiling was itself the 466 killer; requiring the user to budget address
space up front is wrong product behavior. New contract: NO self-imposed ceiling by default;
--address-space-gb N is an optional explicit cap; lower inherited hard limits always apply.
The milestone rerun is therefore knob-free: xsa build --agc ... -o hprc.sxi --threads N --verify-text-sample K.
Gate: default(null-cap)/850GB/inherited-1GB tiny builds -> byte-identical indexes (chi=1401).

## 466 FRONT-END DONE; SEAM-LCE POLICY BOUNDARY FOUND (validation chain paused at endpoints)
Front-end PASS at 466 (largest dictionary SA ever built): wall 10h13m, peak RSS 505.9 GB,
n = 1,403,221,068,491 (1.40 Tbp), R = 2,739,737,289 (vs k10 R=1.86e9: 47x text, 1.47x runs).
parse.rlebwt 10.96 GB + .ssa/.ssa_t published; tap-count and tap-structure gates PASS.
Endpoints stage then FAILED LOUD after 2h40m (peak 192 GB): CYCLIC_SEAM_REFUSED - an LCE
verification at the cyclic seam needed MORE than the polylog-work policy
(max(1000, bit_width(n)^3) = 68,921 probes ~ 6 MB at parse granularity; 466-haplotype
near-identity makes multi-MB collinear identity routine). Fail-loud worked as designed: no
silent O(n). The SlimFingerprint verifier is already exactness-optimal (galloping search,
then direct verify; phrase-ID equality amortizes ~90 symbols per comparison); the POLICY
BOUND is what is miscalibrated for pangenome scale, not the algorithm.
Fix direction: justified sublinear budget (per-seam + total-work accounting, journaled
actuals, fail-loud preserved), exact LCE value in the refusal message; k10/yeast regressions
must republish identical seam deltas (+4,074 / +96).

## PARSE-MEMORY LANE RESULTS (banked; k10 gate re-owned by supervisor after worktree reaping)
Opt-in memory front-end (M32/M64 width selection - pinned gsacak has no M5 ABI; L1 DA derived
from phrase-boundary ranks; L1 ISA released after construction; colex sort without copied
dictionary symbols). YEAST GATE: all four frontend files byte-identical, peak RSS 9.032->5.762 GB
(-36.2%), small runtime cost. 24 randomized dictionary A/B tests identical. Disk-SA probe works
(hash-matching) but ~51x slower under a 32 MiB cgroup cap; NOT a production fallback yet.
466-redo projection: ~355 GB peak (was 506; 466 actually used 4-byte LCP entries).
WEB-TEXT REGIME (pile-frag slices, THE pile-rung datum): w10/p100 D/n = 1.08 CONSTANT (dictionary
larger than text); w20 worse (1.18); w3/p5 halves D/n (0.51) but parse explodes and live set still
9.3 TB. Naive 24D workspace at pile scale = 33.9 TB -> IN-RAM dictionary-SA construction is
fundamentally infeasible for web text at any sane parameter. Also R/n ~ 0.39 at 100 MB slice:
the pile will sit in the unique-dominant regime (chi/n a large fraction) - the paper's contrast
case, and a construction-regime change (external-memory SA or a non-PFP front-end) is REQUIRED
for the pile rung. Infrastructure lesson #3: completing a run reaps its worktree INCLUDING
long-running gates inside it - never leave gates in a worktree past lane exit; hand them to the
supervisor. k10 memory gate (lost with the worktree) is being re-run from main as a
supervisor-owned process.

## PARSED-SPACE FRONT-END BANKED: O(parse) RAM PROVEN (yeast gate byte-exact at 2.30 GB peak)
User architecture ("all operations in parsed space") implemented and PROVEN at yeast scale:
disk-backed phrase store (parser: 123.7 MiB RSS!), phrase-aligned external suffix runs with
sequential tournament merge, bounded paged dictionary access. Yeast front-end: all four outputs
byte-identical, peak RSS 2.30 GB (was 9.03 stock / 5.76 in-RAM-optimized), wall 32m06s, under
prlimit 3.5 GB. Small cyclic fixture through full audit/write/validation: PASS.
ANSWER TO THE USER'S CORE QUESTION: construction memory floor is O(parse); the limiter moves out.
Full-pile blockers, measured honestly:
(1) DISK: SA/LCP runs cost 6x D (not 2-3x D hoped) -> ~30.1 TB overlapping frontend files +
    ~15.2 TB slim arrays vs 10.98 TB free. Infeasible on this box without D-shrink or external slim.
(2) WIDTH: projected distinct-phrase and LF-run counts exceed uint32 downstream interfaces
    (consistent with the R~5e11 > 2^32 flag from the remap lane) - widening is a pile prerequisite.
(3) PARSE SPEED: external phrase store parses at ~4.4-6.8 MB/s (83.2 h projected for 1.31 TB) -
    ~100x slower than the in-RAM DNA parse (440-535 MB/s); needs batching/prefilter engineering.
NOTE the interaction: the syncmer-trigger lane (GPT-6-SOL, running) attacks D directly - any D/n
reduction shrinks both the 6xD disk footprint and the dictionary sort; the two lanes compose.
Gates b-d (k10 byte-identity, pile-frag end-to-end, slice datum) not run; recipes recorded.

## SYNCMER-TRIGGER HYPOTHESIS FALSIFIED (GPT-6-SOL lane; user hypothesis tested honestly)
Closed-syncmer triggers (Durbin syng selection rule) implemented as drop-in PFP trigger
(pipeline + fork patch; mod-p stays default). MEASURED at matched density: web D/n 1.070->3.283
(1%), 0.472->2.411 (19%); yeast chrI 0.258->1.108 (1%), 0.056->0.339 (19%). Dictionary GREW ~3x.
MECHANISM (the finding): mod-p DECOUPLES trigger density from phrase length (w=10 window, 1% rate,
mean phrase 112); syncmers COUPLE them - 1% density needs K~260, and every trigger drags its whole
k-mer, flooring phrases at ~k-s+1 (measured mean 360, max 512: the bounded-gap tail cut works
exactly as Edgar's guarantee says, but it floors every phrase). No (k,s) is simultaneously
sparse-trigger and short-phrase. LONG-PHRASE-TAIL ELIMINATION ALONE IS NOT SUFFICIENT.
VALIDATED FREE: chi invariance proven at byte level - chrI end-to-end through the full pipeline
(50-chunk front end, endpoints, slim, sweep, writer) produced BYTE-IDENTICAL chi files across
trigger schemes (2,093,235 entries, SHA 027155db...). Chi is construction-agnostic, empirically.
Also: w>10 integration blocker caught by fail-loud (endpoints/slim hardcode w1=10; pipeline now
refuses long-window parses early). Pile path remains: parsed-space external construction with
mod-p w10/p100 (D/n=1.08 stands as the web-text constant).

## THE WINDOW IS THE KNOB: w=10 was never tuned - D/n collapses 10-100x at longer windows
The syncmer lane's (w,p) density sweeps (side-effect data, on disk in bit6/sxi_logs/syncmer/):
WEB 1.08 GB slice: w10/p100 D/n=1.070 -> w20/p8 0.155 -> w40/p12 0.069 -> w80/p3 0.028 ->
w160/p8 0.013 -> w200/p3 0.0106. DNA chrI: w10/p1 0.835 -> w40/p3 0.102 -> w80/p5 0.031.
Mechanism: web repetition is BOILERPLATE-SCALE (templates, markup, near-duplicate pages) - a 10-byte
window triggers inside novel text (phrases short and mostly distinct); a 200-byte window rides over
duplicated spans (phrases long, shared, wildly repeated: 44,432 distinct phrases for 1.08 GB).
Consequence: at pile scale D ~ 0.011 x 1.31 TB ~ 14-17 GB -> the dictionary SA fits IN RAM
(3x8Bx17e9 ~ 400 GB) - no external machinery needed for the front-end; parse ~25 GB; phrase IDs
uint32-safe; the 83h external-parse path unnecessary (stock in-RAM parse at ~500 MB/s ~ 45min-1h).
The earlier 'web text has no compression' conclusion was an ARTIFACT OF w=10 (correction of framing).
Construction-side only: R, chi, and query semantics are text-intrinsic (byte-proven via the syncmer
chrI smoke). UNVERIFIED before pile adoption: (1) slice representativeness (is the fragment
near-duplicate-heavy? re-sweep 2-3 fresh pile.txt regions); (2) downstream w-parameterization -
endpoints/slim hardcode w1=10 (known blocker, now the gate to a 100x payoff); (3) L2 retune;
(4) R ~ 5e11 run-count widening still required regardless.

### Pile provenance note (user): pile.txt documents are SHUFFLED
Doc-level shuffle (fragment collapse already rules out byte-level). Implications: (1) the D/n
collapse at long windows cannot be adjacency/duplicate-proximity - the repetition is INTRA-document
boilerplate (templates, markup, page structure) - the most layout-independent kind; (2) slices are
IID at document level -> consistent fresh-region sweep results = the strongest representativeness
evidence; (3) boundary-spanning phrases must draw from a small shared pool (doc starts/tails are
boilerplate): fragment D at w200 = 11 MB total across 177,753 boundaries proves the pool is small
under random adjacency - the adversarial test passed by construction. Chi and R of the pile are
defined for the shuffled corpus AS SHIPPED (document order is part of T; provenance only).

### Correction #14 - supervisor misread: "window-is-the-knob 100x collapse" RETRACTED
The web-density.csv collapse table (D/n 0.0106 at w200) was a MISCOMPUTED/mislabeled side-effect
CSV from the syncmer lane (it contradicts that same lane's own acceptance measurements, and its
columns were syncmer (K,S), not mod-p (w,p)). The long-window verification lane's fresh 3-region
sweep of the REAL pile (proper parses, 1 GB slices, begin/middle/end): syncmer D/n = 3.10-4.43
UNIFORMLY - long-window syncmers are BAD on the pile, consistent across regions (representativeness
confirmed for the shuffled pile). TRUE mod-p facts on web text: w3/p5 D/n=0.47 (best known),
w10/p100 1.07-1.08, w20/p100 1.18 (longer window makes mod-p WORSE, not better).
The genuinely open question (mod-p w40-200/p3-12 on real regions) was NOT in the fresh sweep;
supervisor is measuring it directly. Lesson reinforced: side-effect CSVs are not findings;
cross-check against the producing lane's own acceptance numbers before announcing.

### WEB-TEXT PARAMETER QUESTION CLOSED (supervisor mod-p sweep, fresh region @110GB offset, 1.08 GB clean slice)
mod-p D/n: w3/p5 0.51 (parse 0.76n) | w10/p100 1.08 | w20/p8 3.24 | w40/p5 8.73 |
w80/p5 16.70 | w160/p8 20.76. LONG WINDOWS EXPLODE the dictionary on heterogeneous web text
(longer phrases = longer DISTINCT phrases); the long-window idea is dead in both trigger families
(syncmer 3.3, mod-p up to 20.8). The user's original instinct was correct: TIGHTER grammar wins.
PILE PLAN (settled): mod-p w3/p5 (D ~ 0.5n), sharded-parallel parse (friend's architecture:
block-split with w-1 overlap, per-worker local dictionaries, sort-based external dedup, remap -
fixes the 83h sequential external parse; matches the Diaz-Dominguez SPIRE 2025 partition+merge
philosophy), parsed-space external front-end (banked), uint32 widening (R~5e11 regardless).
w-parameterization of endpoints/slim (running lane) is load-bearing for w3.
Token-space PFP (BPE) remains the bigger-shrink option: 4-5.7x sequence reduction, aligns with
the LM-over-chi backend; chi becomes tokenizer-conditioned (provenance decision).

## ==================== CHI(HPRC-466) = 2,250,211,129 ====================
### Validation-only (from the retained parse; from-zero milestone rerun pending). hprc.validation.sxi
### = 52,175,300,288 bytes at /tmp/rpfbwt-64-466/. chi/n = 0.1604%, chi/R = 0.8214 (inside the
### proven [0.82,0.88] band). n = 1,403,221,068,491; R = 2,739,737,289; strings = 38,790 contigs.
### Paper macro \hprcchi filled; results row complete.

SEAM-LCE RESOLUTION (the full arc, measured): old cap 68,921 refused the FIRST dict-level seam LCE
at 110,001 symbols; the TRUE max seam LCE = 18,180,977 text symbols (18.2 MB of collinear haplotype
identity - pangenome near-identity is real and large). Corrected policy: one work unit = one
comparison / one hash probe (probeLimit-capped; tau-bounded reads uncharged) / one directly-verified
symbol; per-verify cap min(2^16, structure size); shared total budget min(2^32, max(10^6,(P+D)/8)),
atomic + journaled. Total seam work 426,807,960 / 4,266,152,237 budget = 10.0%. Classes 5,836,
rows repaired 274,952, exact=1. THE LAW intact: all 2.74e9 boundary rows resolved directly,
zero LF walks; audit 1000/1000 samples verified against the AGC.
En-route correction: the prior draft charged hash-probe reconstruction PER SYMBOL (tau-x inflation,
984,245 units on a 267-byte fixture) - caught by the dense integration gate, fixed before any 466 claim.
Regression gates green: yeast endpoints+slim byte-identical to accepted publication (the anchor);
k10 A/B byte-identical (finding: the k10 pilot parse is pre-0x1E-era text, 0x0A terminal - head
samples diverge from k10.sxi member 3; canonical-k10 anchoring impossible from it, yeast carries it).
Ledger incident: the accidental agc2flat prepare verb ALSO truncated work-dir names.tsv to zero
(prepare-verb sidecar clobber) - silently broke the audit; restored from the original work dir.
Banked with the w1-parameterization merge (long-window lane, 3-way): w1 now flows through
pipeline/endpoints/seam/slim in BOTH trees, defaults w10, w10 byte-identity gated.

### Correction #15 - supervisor brief error, caught by lane fail-loud
The SXI2 brief gated the retained pile-frag .sxi (REMAPPED corpus: n=1,082,130,213,
chi=306,164,765, audit 128/128) against the FILTERED corpus's chi (306,164,941, n minus
229 forbidden bytes, from the long-window lane). Two different corpora, both correct, both
already attributed in this ledger. The lane refused to publish on the mismatch - correct.
Also caught: tail SA is NOT head+length (within-run SA values are arbitrary in suffix order);
the published compact form requires the FULL Nishimoto-Tabei machinery: LF move map PLUS the
SA-value interval mapping (phi at run granularity) - Movi 2 compresses only the LF part.
Sizing carried over: fragment members ~0.552 GB before the locate map (consistent with the
~0.6-0.7x-text target). Relaunching with per-artifact gates and the corrected design requirement.

## SXI2 v2 REVIEW: periodic-cyclic phi blocker found (fail-loud held; hybrid escape mandated)
The v2 oracle (phi_oracle.py): 28,362 aperiodic locate cases PASS; 126 PERIODIC CYCLIC cases FAIL -
run-tail SA-value mapping returns wrong successors under local periodicity (satellite arrays, repeated
padding) - the cyclic-frame manifestation of classic LF periodicity. Shipped SXI1 samples never hit it
(locate reads explicit values). Design direction (v3): HYBRID ESCAPE - phi for aperiodic runs + explicit
side-list for periodic-ambiguous runs, with a stated criterion. THEORY CONNECTION: the open O1 problem
(RunEdgeDominate; RunEdgeHit PROVEN in SxgcRunEdge.lean) is precisely about run-edge rules - the
phi-correctness criterion may be O1-adjacent and Lean-provable. EF chi member: 0.144 GB ideal at fragment.
Evidence: bit6/sxi_logs/sxi2-v2/ (DESIGN.md, RESULTS.md, phi oracle + counterexamples, size probe).

## UPSTREAM pfp++ BUG FOUND (explains the original pile refusal): signed-char gate
Upstream pfp_algo.cpp text path: `char c = record->seq.s[seq_it]; if (c <= DOLLAR_PRIME)` with
SIGNED char - any byte >= 128 (negative) trips the gate. The error message says "bytes <= 5" but
the actual behavior ALSO rejects all UTF-8 high bytes - the pile's original refusal was mostly
HIGH BYTES, not the rare 0-5 bytes (229 low bytes vs abundant UTF-8). Our remap fork already
replaced the path with unsigned-char handling; main's dev-tree xsa bundles the fixed toolchain.
Upstream-PR material: fix the comparison (uint8_t) or the message. Also third k10-gate SIGKILL
logged (same phase, crowded-window OOM both prior times; relaunched on the now-empty box, watch
armed). 8 GB pile trend build launched (slice8.txt, clean, offset 900 GB region): decisive test
of the user's scaling hypothesis - does chi/n improve with 8x corpus, or hold at the 1 GB floor
(baseline: filtered-fragment chi/n = 0.28293; within-fragment D/n was flat 500MB->1GB).

## SXI2 v3: CRITERION C FOUND, ORACLE-VERIFIED, REAL CORPORA NEED ZERO ESCAPES
Criterion C (sufficient for phi on a run): BWT symbol-frequency gcd == 1, OR the run's SA-value
domain is a singleton. Oracle: all 126 v2 counterexamples + 88,569 exhaustive ternary texts +
2,000 periodic stress cases PASS under C+escape. Reference escape codec implemented + tested.
EMPIRICAL: yeast (R=100.9M), k10 (R=1.86e9), remapped pile-frag (R=397.7M) ALL have gcd 1 ->
escape-list size ZERO in every real corpus. The periodic pathology is synthetic-only (so far).
Remaining: production implementation (writer/loader/query port/gates/sizes) - v4 mandated to
implement, not re-review. Banked: bit6/sxi_logs/sxi2-v3/ (DESIGN.md criterion + proof sketch,
oracle, criterion probe, escape codec).

## SXI2 v4 BANKED (production substrate + honest space-gate failure)
Implemented and small-scale-gated: SXI2 publish-only converter (bit6/sxi2_write.cpp), dual-format
xsa loader, EF chi decoder, phi locate, escape fallback. Synthetic MEM + HTTP byte parity PASS
(aperiodic phi + periodic nonempty-escape + exact EF chi); SXI1 regression + SXI2 differential PASS;
all three chi header gates verified. SPACE GATE FAILED LOUD (correct refusal): the v4 codec retains
raw head+tail samples AND adds 24 B/run phi records - the compact form AUGMENTED instead of REPLACING
(projected yeast 3.81 vs 1.80 GB SXI1; fragment 14.77 vs 7.02; 466 phi member alone 65.75 GB).
v5 mandate: phi REPLACES samples - sparse anchors (8B per 2^k runs) + few-bits/run NT interval
members; compact LF move map as a STORED member (not rebuilt from decoded runs); O(log log n)
predecessor, not O(log R) binary search. Also fixed: SOURCES.sha256.json regenerated for the 8
runtime/src entries changed by the seam+w1 merges and v4 (vendoring discipline caught the drift).

## SXI2 v5 SHIPPED: full-scale compact artifacts, all correctness gates green (bits/run target unmet)
Published: yeast235.sxi2 1.414 GB (0.783x SXI1), pile-frag.sxi2 5.197 GB (0.741x), k10.sxi2
28.373 GB (0.835x) - durable copies at /mnt/nvme3n1/erikg/sxi2-v5/. Raw head/tail members GONE
(phi replaces samples - the v4 architecture bug fixed). GATES ALL GREEN at full scale: space
preflight = achieved exactly; chi EXACT (every decoded value compared to SXI1); native MEM
byte-parity (423/4/205 records) + HTTP byte-parity at all three scales. WARM QUERY WIN:
HTTP median latency SXI2 vs SXI1 - yeast 377.9 vs 5076.7 ms, k10 2.93 vs 10.23 ms, frag ~par.
UNMET: phi+LF members at 94.4-107.0 bits/run vs the 5-12 NT target (packed fields, not the
permutation structure - the remaining 10x is succinct-structure engineering); O(log R)
predecessor; cold load worse than SXI1 (k10 48.4 GB RSS / 14 min vs 31.1 GB / 3.4 min - reader
still builds symbol-run lists). 466 projection >= 42.83 GB (better than SXI1's 52, stretch 6-8
needs the few-bits form). v6 candidate: NT few-bits permutation representation + O(log log)
predecessor + cold-load path.

## 8 GB TREND DATA (first cross-scale web-text measurement; user scaling hypothesis CONFIRMED)
R/n: 0.3676 (1 GB fragment) -> 0.3501 (8 GB slice, offset 900 GB region) - the ratio IMPROVES
~1.2% per doubling, monotone, no cliff. The 1 GB fragment is the worst case as the user argued
("as long as there is any repetition it should get better and better"). Extrapolated to the
full 1.31 TB pile: R/n ~ 0.32, chi/n ~ 0.25 (via chi/R ~ 0.77). Also: the trend corpus
initially cut mid-document (terminal 0x20 = SPACE) -> the seam machinery REFUSED correctly:
predecessors_before_zero=678,349,792 candidate spaces, class_size 1.39e9 vs limit 2.9M,
CYCLIC_SEAM_REFUSED - the cyclic contract ENFORCES document-aligned corpora. Supervisor
error (bad slice cut), caught by policy; slice truncated at last 0x1E (dropped 34,675 bytes),
aligned rebuild running for the true 8 GB chi.

## K10 MEMORY GATE PASS (third attempt; completes the parse-memory lane's gates)
k10 front-end, memory path vs stock oracle: ALL FOUR OUTPUTS BYTE-IDENTICAL (.rlebwt/.meta/.ssa/
.ssa_t). Peak RSS 116.4 GB -> 76.3 GB (-34.5%; yeast was -36.2% - consistent). Wall 2:03:30 vs
2:04:07 (+0.3% - free). Parse-memory lane now FULLY GATED at both scales; the 466-redo projection
(~355 GB peak vs 506 as-built) is supported by two independent scale gates. Note: two prior gate
attempts were OOM-killed in crowded windows - third attempt on the quiet box ran clean.
ALL MERGED PIECES NOW GATED IN MAIN: seam policy (yeast+k10+466 chain), w1 parameterization (w10
byte-identical), memory path (yeast+k10), remap (fragment end-to-end), address-space (3 tiny
ceilings identical), no-default-ceiling. Sealed-toolset refresh is the only remaining mechanical
step before the 466 from-zero milestone.

### SEALED TOOLSET v2 REFRESHED + MILESTONE LAUNCH
tools/build_all.sh sealed-tools-v2: 83 artifacts (was 75; now incl. sxi2_write), manifest
verified 74e077b6... All gated pieces in one toolset: seam policy, w1 parameterization,
remap pfp++, address-space defaults, SXI2 converter. THE FROM-ZERO 466 MILESTONE launches:
xsa build --agc HPRC_r2_assemblies_0.6.1.agc -o hprc.sxi --threads 48 --verify-text-sample 10000
(sealed-tools-v2; stock rpfbwt - the memory path stays opt-in; peak ~506 GB projected, box
accommodates; the number must reproduce the validation chi = 2,250,211,129 EXACTLY).

## BCR FRONT-END v1 BANKED (route validated at correctness level; scale structure is v2's work)
Single-read, reverse-stream BCR prototype with run-head+run-tail SA sample maintenance during
insertion; writes all four PFP-compatible files (drop-in for the chain). GATES PASS: 33 collections
vs independent cyclic-SA oracle (all four files); cross-producer PFP byte-identity incl. a REMAPPED
corpus (entries changed=12) - TWO DIFFERENT CONSTRUCTIONS, IDENTICAL ARTIFACTS. Parse-free slim
design doc included. HONEST LIMIT: flat-run arrays => quadratic construction; yeast/fragment scale
gates not run. Projections (fragment-density): 1 TB slice - PFP 25 TB vs BCR >=5.15 TB, and SAMPLES
DOMINATE BCR (2 samples/run x R=0.35n) - the same whale as SXI1. v2 mandate: ropebwt2-style blocked
run structure (O(log) insert/rank) + SPARSE-ANCHOR sample maintenance with phi recovery (dynamic
move structure line) - the SXI2 lesson applied to construction. Full pile remains external/output-
bound either way (theorem: chi ~ 0.25n); slices 10-50 GB work with full samples, 100 GB+ needs sparse.

## BCR FRONT-END v2 BANKED (blocked AVL run tree; scale gates running detached, adopted)
Independently implemented blocked AVL RLBWT builder (bit6/bcr_frontend_v2.cpp): O(log) rank/insert,
ASAN/UBSAN-clean, 33 oracle cases + both PFP byte-identity gates + 50KB v1 differential PASS.
OPEN ITEMS (honest): (1) endpoint recovery is currently O(n log r) full-LF-cycle - sparse-anchor
O(R) recovery is v3; (2) yeast235 + remapped-fragment scale gates LAUNCHED DETACHED by the lane and
ADOPTED by the supervisor (watcher armed; fragment auto-queued after yeast). Projections (tree
memory at fragment density): 100 GB slice 612-897 GB (under the 900 cap, barely); 1 TB 6.1-9 TB
(external required - as expected, output-bound). No checkpoint/restart for long gates yet.

### BCR v2 source recovered (retention patch VALID for unstaged lane changes; note the pattern)
Sol-lane retention patches are empty when the lane STAGES files, valid when it leaves changes
UNSTAGED. v2's 27 KB patch applied cleanly; bcr_frontend_v2.cpp (13 KB) compiles warning-free.
The detached scale gates (yeast + fragment) write verdicts to /tmp/bcr-v2-gates-2ec315bd/
(outside the reaped worktree); supervisor watcher armed. Infrastructure lesson #4 recorded.

## 8 GB ALIGNED TREND BUILD: chi lands; the web-text scaling law is measured
chi(8 GB aligned slice) = 2,261,916,360; n = 8,388,474,069; chi/n = 0.2696 (was 0.28293 at 1 GB -
4.7% relative gain at 8x corpus). R/n 0.3501 (was 0.3676). CRITICAL INVARIANT: chi/R = 0.7703 vs
0.7697 at 1 GB - CONSTANT across scales; the ratio improvement is pure BWT-run economy, the fork
fraction never moves. Web-text scaling law (measured at two scales): ~1.2% per doubling, chi/R ~ 0.77.
Extrapolation to the 1.31 TB pile: R/n ~ 0.32, chi/n ~ 0.25. The 1 GB fragment was the worst case
(user's prediction, confirmed twice). Also: parse-speedup lane banked - partitioned dict CORRECT
(byte-identity at -j 1/16/48 on yeast + full 8 GB slice) + rehash fix (264->41ms) gives web 1.4x;
PROFILING FINDING: the plain-text parse loop is SERIAL in this fork (zero lock contention measured)
- the text scan, not the dictionary, is the text-path bottleneck; moot for the pile (BCR route),
and the AGC/pangenome path was already parallel.

### 8 GB TREND BUILD PUBLISHED (full pipeline from zero, exit 0, wall 5.9h)
slice8.sxi = 52.55 GB at /mnt/nvme2n1/erikg/sxgc-trend/ - the second published web-text index.
Chain: parse -> front-end -> endpoints -> slim -> sweep (chi = 2,261,916,360) -> 100-sample
streamed audit (PASS) -> write -> validate -> PASS. All from one command, no knobs. Per-stage
journal at logs2/. Summary: TWO published web-text scales now exist (1 GB frag: chi=306,164,765;
8 GB slice: chi=2,261,916,360; chi/n 0.283 -> 0.2696; chi/R constant 0.770).

## TOKEN-SPACE MEASUREMENT: the naive 10x is NOT real alone (1.83x measured); composes with v6
Document-aware BPE (4.10 bytes/token) on the fragment: token R'/n' = 0.728 (vs byte 0.368 - runs
are 2x DENSER per token), token chi'/n' = 0.520 (vs 0.283). The merge-hypothesis FAILED: BPE
tokenization VAPORIZES run structure - a 100K-symbol near-uniform alphabet makes the BWT almost
run-free (first-symbol entropy governs runs; bytes' small skewed alphabet is what gave 0.368).
Net absolute: R' = 0.178n vs byte 0.368n (2.07x fewer runs) - hence total index 7.02 -> 3.83 GB
(1.83x, incl. a 132.8 MB EF byte-offset table). Pile projection: ~4.5 TB at 290 Gtokens - NOT a
rescue alone. HOWEVER the levers COMPOSE: samples dominate BOTH routes (always the samples);
tokens cut sample COUNT 2.07x, the v6 few-bits form cuts sample COST 16B -> ~5-12 bits/run.
Composed estimate for the fragment: rleBWT ~240 MB + move-members ~120-290 MB + EF chi ~220 MB +
offset table 133 MB = ~0.7-0.9 GB - the user's half-of-text goal (0.54 GB) is in reach at the
composition. MEM semantics honestly limited: token MEMs != byte MEMs; in-token matches
unrecoverable; offset table gives boundaries only. Token-space is the LM-substrate query space.

### Correction #16 - the v5 code was never actually banked (staged-files retention trap, AGAIN)
The v5 retention patch was 0 BYTES (the lane staged its changes; staged files produce empty
retention diffs - the SAME trap that lost the syncmer and BCR v1 worktrees), and my bank's
"V5-APPLIED" echo was fooled: git apply on an empty patch exited through without error under
2>/dev/null, and my verification test used STALE v4 binaries (/tmp/sxi2_write_v4_o3) so it
green-lit the wrong code. Main has been carrying the v4-era writer (format version 2) while the
v5 artifacts on nvme3n1 are version 3 - caught by the v6 lane's honest format audit. FIXED NOW:
v5 code recovered from the still-live worktree (writer verified version 3, cargo release build
green, writer compiles clean). The verification lesson, restated with teeth: a bank is not
verified until the BINARY IT PRODUCES is checked against the artifact it claims to be.

### v6 PREFLIGHT BANKED (recovered from retention patch - the unstaged-lane pattern works)
Explicit-association cost: 25.146 / 27.124 / 29.350 bits/run (yeast / fragment / k10);
optimistic totals 575.9 MB / 1.982 GB / 11.58 GB - the corrected v6-2 targets (2.4-2.6x over v5).
The 12-bit two-list form is information-theoretically impossible (association lost); a BWT-derived
association is the open research question, deferred.

## SXI2 v6-2 SHIPPED AND BANKED (format version 4; verified by binary this time)
All gates green at all three scales: chi exact (85,404,336 / 306,164,765 / 1,627,067,257),
native MEM byte-parity (423/4/205 records), HTTP byte-parity, format differential, escape + bounded
MEM checks. Achieved: yeast 1.010 GB (v5 1.414), fragment 3.656 GB (v5 5.197), k10 20.24 GB
(v5 28.37) - 29% off v5, ~52% off SXI1. Version 4 derives LF starts from the run stream (removed
from storage); phi association remains the whale at 63-72 bits/run vs the preflight floor 25-30
(optimistic totals 576 MB / 1.98 GB / 11.6 GB still available). Warm throughput within noise
(yeast 382 ms, frag 0.36 ms; k10 3.47 vs 2.93 ms - slightly slower, noted). Bank path: unstaged
retention patch (165 KB, applied clean), version-4 writer verified by built binary, cargo green,
small gates re-run from main. NEXT (optional v6-3): the association representation gap 63-72 ->
25-30 bits/run; composed with token-space = the half-of-text goal still in reach.

## WEBTEXT ARCHITECTURE DECISIONS (user, end of day 2)
(1) CORPUS CONTRACT: BYTE SPACE for the pile (token route banked as measurement + LM-substrate
option; not the default). (2) ADOPT A/B/C/D ALL: A = current shipped chain (milestone running);
B = MOVE-NATIVE (BCR maintaining move map + sparse anchors, NO sample materialization; decoupled
phi-LCE slim; chi computed FROM the move form; one representation end-to-end); C = Movi 2 as
external benchmark/verification oracle only (cross-check vs their HPRC-466 numbers); D = direct
SA+LCP chi derivation as the slice-scale third construction route AND standing cross-verification
oracle (the token-space lane's cyclic-SA oracle already proves the pattern).
(3) THE NEXT-LEVEL DESIGN (user's ropebwt3-style proposal, confirmed natural): CHUNK-MERGE
progressive construction - per-chunk in-RAM indexes built by the SIMPLEST method (direct SA + linear
chi derivation, no PFP/BCR needed per chunk) then progressive ropebwt-style BWT merges WITH move-map
maintenance, chi maintained INCREMENTALLY: chi is MONOTONE-COMPACT under merges (adding text only
dominates old witnesses away, never re-requires them; new witnesses only in the added chunk;
fork conditions recomputed only at merged boundaries). Never sorts the whole thing; merges are
O(r) pairwise in a balanced tree (O(R log k) total). Matches the Diaz-Dominguez SPIRE 2025
"merging big BWTs" motivation with our chi twist.

## CHUNK-MERGE PREMISE AUDIT: two shortcuts are FALSE (counterexamples, brute-verified)
(1) CHI IS NOT MONOTONE-COMPACT under concatenation: appending text can REVIVE old dominated
nonwitnesses. Counterexample (brute-verified minimum cover): A=(1,2,2,0x1e), B=(1,0x1e) ->
chi(A)=3, chi(A+B)=5, old position 2 goes dominated -> REQUIRED. 26/180 aligned chunk-pair
tests showed revivals - REVIVAL IS COMMON, not a corner case. Verified against lean/Sxgc.lean
coverage definitions. My monotone-pruning claim was WRONG; incremental chi maintenance by
pruning alone is impossible.
(2) CYCLIC-FRAME ROW INSTABILITY: appending B reorders suffixes WITHIN chunk A (rows [0,3,1,4,2]
-> [0,1,3,2,4] on the counterexample) - cyclic suffix comparison runs through the wrap into the
appended material, so independently sorted chunk rotations cannot be merged by stable
interleaving, and chunk-local chi candidates are not reusable as-is.
THE ARCHITECTURE SURVIVES, CORRECTED: (a) the BWT merge itself is unaffected (BCR/ropebwt merges
interleave by LF steps, not by stable-order assumption); (b) per-chunk products are BWT +
samples-for-insertion, NOT chunk chi; (c) chi is derived ONCE from the FINAL merged move map via
the decoupled phi-LCE slim (or re-derived per round at O(R) - still no full-corpus sort).
New theory datum for the paper: witness REVIVAL under concatenation - the suffixient set is
genuinely dynamic, with both shrinkage and local growth. Also relevant to the LM-over-chi story
(incremental corpus growth has non-monotone state).

## LEAN FLEET ROUND 1 - SEAM LANE RESULT (dsv4 native, first deepseek bank)
Seam order core BANKED (sorry-free, lake green, additions only): lexLt strict order lemmas,
cycCmp irrefl/asymm/trans + trichotomy via rotation_cmp_total => the cyclic comparison is a
STRICT TOTAL ORDER on rows, class sort well-defined and unique; compRel/seam-class structure
(comparable rows are exactly seam rows; non-seam rows are singleton classes). STATEMENT
CORRECTION #17 (fleet catch): seamRepair_reorders_in_classes was VACUOUS as written (its
comparability hypothesis is unused; its conclusion IS rotation_cmp_total) - proven as
seamRepair_reorders_in_classes_from_total and demoted; the SUBSTANTIVE half needs a
prefix-interval/trie class-partition model + the r-space spliced-output equivalence (both
statement-locked in lean/FLEET_SEAM_REPORT.md). Fleet lesson: fresh-worktree lanes pay a
~5min Sxgc.lean baseline compile; seed the olean cache next round.

## LEAN FLEET ROUND 1 COMPLETE - the theory ledger moves (2026-10-02)
Fleet: astra (O1 campaign, exploratory) + 3x deepseek-4.1-flash native grind (criterion C,
revival, seam). ALL LANES DELIVERED. SORRY LEDGER: 2 live -> 1 (minimality lower half, the
Lemma 34 tie-breaking, now with saturation-evidence + a newly identified duplicate-family
datum under small scan caps).
(1) O1 COVERING CLOSED (astra): runEdgeDominate_true (RunEdgeDominate proven unconditionally,
SxgcRunEdge.lean sorry-free) + the RunEdgeHit -> maxHit -> domination -> covering assembly;
covering_given_stream now a THEOREM (was the 1913 sorry). The scan provably emits a
suffixient set - the O1 half of the main theorem is done. Axiom audit: standard three only.
BIT 1B gate 511/0 GREEN.
(2) CRITERION C: 2 of 3 obligations proven (gcdOne_not_isPower + singleton branch); the
order-preservation bridge (primitive => distinct rotations => affine interval map) is
statement-locked, Lyndon-Schutzenberger-shaped, next round's target.
(3) WITNESS REVIVAL FORMALIZED: 17 sorry-free theorems; revival (position 2: dominated ->
required) AND destruction (position 1: required -> dominated) both machine-checked; chi A=3,
chi J=5. FLEET SHARPENING (correction #18 territory, lane catch): my brief's claim 'no lemma
chi T1 <= chi (T1++T2)' was WRONG as a refutation target - the example does not refute the
COUNT inequality (3 <= 5) and a 40k-pair search found NO chi-decreasing concatenation. NEW
OPEN QUESTION: is chi-count monotone under concatenation? (If yes: chunk-merge gets a free
lower-bound invariant and the paper gets a cleaner statement.) What IS proven: witness-SET
non-monotonicity (both directions) - the merge architecture conclusion (derive chi once at
the end) stands on the set-level fact.
(4) SEAM order core banked earlier (daad4e1). Infrastructure lessons: retention patches
miss UNTRACKED files (revival file survived only because the lane wrote into main cwd);
fresh-worktree lake baseline ~5min (seed olean cache next round); deepseek-4.1-flash cannot
route through codex (use native runner + model override).

## CORRECTION #18 + THEORY CLOSURE: chi-COUNT MONOTONICITY REFUTED (fleet round 2, both barrels converged)
The open question from fleet round 1 is CLOSED - REFUTED, machine-checked in Lean, found
independently by both lanes within ~90 minutes of being posed:
(1) COUNTEREXAMPLES (Lean, sorry-free, SxgcChiMono.lean): chi_append_decreases (T1=[1,30,3,4,30,
3,30,4,30,5,1,30], T2=[3,30]: chi 7 -> 6); chi_single_symbol_document_decreases (even the
single-document append-unit of the BCR/merge world violates it); chi_prefix_extension_decreases.
Oracle hammer's minimal contract-valid witness: A=1<SEP>2<SEP>2<SEP>1<SEP> (4 docs, chi=4),
B=2<SEP> (chi(A+B)=3) - DELTA = -1; every violation found across ~20M pairs is exactly -1.
(2) WHY THE 40K SEARCH MISSED IT: it tested only SINGLE-document texts (body++[SEP]) - that
family is genuinely safe (8M further pairs, 0 violations). Violations require MULTI-document A
(docs(A)<=2 safe over 2.7M pairs; minimal violators have 4 documents). The document contract
SEP recurs at every doc end, so it is NOT a unique trailing delimiter - the safe condition fails.
(3) MECHANISM: append can ERASE a 'terminal-context' requirement - a context right-maximal
only as a SUFFIX of A - demoting an old maximal class with NO compensating creation.
Demotion-driven, never revival-driven (independent of round 1's revival result).
(4) POSITIVE SURVIVORS (Lean-proven + oracle-hardened): monotone when the trailing symbol is
unique (a genuine delimiter); fresh symbols; disjoint content alphabets; suffix-preserving
appends; doubling cannot decrease.
(5) ARCHITECTURE CONSEQUENCE: no cheap incremental chi-count invariant exists; ANY prune-based
incremental chi maintenance is UNSOUND (now by Lean-checked counterexample, not just set-level
revival). The 'derive chi once from the final merged structure' decision STANDS, STRENGTHENED.
Fleet pattern validated again: pose sharp question -> parallel hammer+proof -> closed same night.

## INFRASTRUCTURE LESSON #5 (supervisor self-inflicted): the cleanup sweep removed the byte-remap
lane worktree holding the ONLY copy of the pile-frag PFP four-file front-end (untracked vendor
outputs; its retention patch was 0 bytes - staged-lane pattern). Banked reports referenced those
files (byte-remap/REPORT.md points at the dead path). RULE: before removing any completed lane's
worktree, grep banked reports for references to its untracked outputs and rescue them first.
RECOVERY: the fragment reference is regenerable from the retained corpus
(/home/erikg/sxgc-piletest/pile-frag.txt, n=1,082,130,213) in ~1h via the banked pipeline;
chi must print 306,164,765. libsais now BANKED in-repo (bit6/third_party/libsais) - it had been
living untracked in /tmp all along (three lanes reported it absent before one finally said why).

### LESSON #5 AMENDED (user correction): do NOT hoard big intermediate artifacts at all
The original lesson said "rescue untracked artifacts before sweeping worktrees" - WRONG DIRECTION.
The real problem was retention: a 27 GB front-end + 7 GB SXI1 for a 1 GB input is not an asset,
it is a liability that almost cost us a false sense of loss. THE RULE: retain (1) code, (2)
journals, (3) input corpora, (4) the small canonical product set (the published artifacts the
paper claims: hprc.sxi, slice8.sxi, the v6-2 containers, the sealed tools) - on managed nvme
paths. EVERYTHING intermediate is derived and regenerable on demand: banked reports must
reference ARTIFACT RECIPES (input path + command + expected chi), never artifact paths.
Sweep intermediates freely. The old pipeline's artifact bloat is itself a symptom of the
samples whale (the front-end carries 16B x 2/run sample arrays) - the chunk-merge/move-native
architecture does not produce these intermediates at all, so the pattern dies with the whale.

## BCR V2 YEAST GATE: ALL FOUR FILES BYTE-IDENTICAL (verdict completed by supervisor)
The 14h single-core yeast235 build (n=3,336,986,769) finished all outputs at 03:47Z; the gate
script was SIGKILLed at the finish line by the round-1 chunk-merge lane's sandbox teardown
(infrastructure lesson #1 pattern, ~20min lag; 0-byte .time) before its own cmp step ran.
Supervisor ran the verdict directly: rlebwt + rlebwt.meta + ssa + ssa_t ALL byte-identical to
the retained PFP reference. THE BCR V2 RUN-MERGE SEMANTICS ARE PROVEN at yeast scale:
two constructions (PFP front-end; blocked-AVL BCR insertion), one artifact, four files,
zero byte differences. Speed verdict stands as measured: one-at-a-time insertion is
untenable at scale (14h/3.3Gbp single-core) - the chunk-merge builder (round 2, running)
is the O(runs) answer. NOTE: the queued pile-frag gate in run_gates.sh points at the dead
worktree path (my cleanup) - mooted by the recipe rule; the fragment reference regenerates
in chunk-merge round 2's Gate 0.

## V7 MODEL VERDICT (witness-restricted run structure): NOT a standalone search index - negative, measured
The user's idea (search directly on chi + move, dropping witness-free runs) was modeled by census +
simulation + projection, not built. FINDINGS: (a) CENSUS: witness-free runs are 18.4-25.6% of R
(yeast 20.6%, frag 25.6%, k10 18.4%); witnesses map to run heads OR tails (head-witness runs 54%/
66%/47% of R); (b) OPTIMISTIC SPACE: with charged skeleton (bitvector+rank+C-array), v7 would be
832 MB / 2.81 GB / 17.0 GB - a real ~20-23% under v6-2 at stored size; (c) THE KILL: search
routing fails - 99-100% of real MEM queries (1001 per corpus) hit intermediate backward-search
steps inside witness-free intervals; the skeleton cannot supply rank there. Making routing work
via packed LF starts pushes the pile projection to 5.861 TB vs v6-2's 5.378 TB baseline - the
fix costs more than the win. CONCLUSION: the rleBWT's witness-free runs are LOAD-BEARING for
backward search; chi rides the artifact, it does not drive it. v6-2 remains the container shape.
The statement-locked Lean target (witness-anchored search) is recorded in the journal for any
future hybrid scheme. One month of speculative building avoided by one hour of modeling.

## CHUNK-MERGE ROUND 2 + THE MERGE OBSTRUCTION (theory datum, merge family #3)
GATE 0: regenerated fragment reference prints chi=306,164,765, R=397,723,010, n=1,082,130,213
EXACT - the lost-artifact incident is fully recovered and the fragment number is now
reproduced from zero twice. The per-chunk libsais front end: 16 document-aligned chunks in
237s at 2.3 GB peak, 254 exhaustive + 20 synthetic collections pass, 16/16 byte-identity
between serializers. Sum of local runs 420.9M > merged R 397.7M (expected: merges absorb).
OBSTRUCTION (the lane proved it with a legal separator-aligned fixture): the cyclic BWT of
A++B is NOT a stable interleave of BWT(A) and BWT(B), even at byte/run granularity - 7/20
synthetic aligned pairs obstruct. Merge family falsifications now: (1) stable chunk-internal
ROW order (round 1); (2) incremental chi maintenance (revival/demotion, Lean); (3) stable
run INTERLEAVE as the merge itself (this). The correct merge must recompute CROSS-BOUNDARY
order: rotations of A continue into B (cyclic frame), so interleaving points depend on
cross-LCPs, not on either input's internal order. OPEN RESEARCH TARGET: a run-granularity
cyclic-BWT merge with cross-LCP semantics (the O(r) prize; publishable if it exists).
PRAGMATIC GATE PATH meanwhile: batch-BCR insertion at CHUNK granularity (rotation-by-
rotation within each 68 MB chunk, ~17 min/chunk measured class) finishes the fragment gate
in ~4.5h - proves the pipeline end-to-end while the O(r) merge stays open. Pile waits on
the real merge, not on the gate.

## WITNESS-ANCHORED LOCATE SHIPPED + THE INTERVAL-WITNESS LIMITATION (measured, honest)
xsa mems --first: chi edge bitmap (2 bits/run sidecar: 25.2 MB yeast / 99.4 MB frag - ~2% of
artifact) + stored phi recovery. NEITHER production route enumerates occurrences or walks LF.
GATES: 1000/1000 correct decisions both corpora; 872/872 (yeast) and 778/778 (frag) returned
positions text-verified as true occurrences. SPEED: yeast first-position 0.513 ms vs full
locate 5.671 ms (11x); frag 0.369 vs 0.410 ms (frag full-locate is already near-warm-floor).
LIMITATION (the lane caught the supervisor's imprecise corollary BEFORE banking): 'P's BWT
interval contains a witness row' is FALSE in general - chi members are END-positioned (requirement
ends), occurrence STARTS depend on |P|; the static bitmap misses 390/870 yeast / 425/778 frag
occurring probes. The toehold fallback covers every miss with no enumeration. This does NOT
falsify covering_given_stream (the theorem is about requirement coverage at ends); the wrong
step was assuming coverage implies interval-row witnessing for arbitrary-length patterns.
The honest corollary to state in the paper: find-one-position is witness-anchored for the
end-positioned requirement; the start mapping needs |P|, hence toehold rescue for ~45% of probes.

## THE 466 FROM-ZERO MILESTONE IS DONE - THE PAPER NUMBER IS UNQUALIFIED
chi(HPRC-466) = 2,250,211,129 (N=1,403,221,068,491, R=2,739,737,289, 38,790 strings) - FROM ZERO,
one knob-free command (sealed-tools-v2 xsa build --agc HPRC_r2_assemblies_0.6.1.agc --threads 48
--verify-text-sample 10000), 27.15 hours wall. THE MILESTONE ARTIFACT IS BYTE-IDENTICAL TO THE
INDEPENDENT VALIDATION BUILD: hprc.sxi == hprc.validation.sxi, sha256
2aca539e3747414e447372b9554f14613335162fd07c00c03599a1948198a8fb, 52,175,300,288 bytes each.
TWO full 1.4 Tbp constructions, identical 52 GB artifacts, identical chi. Every number in the
paper about 466 is now from-zero reproducible: chi/n = 0.1604%, chi/R = 0.8214 (inside the proven
band). The audit gate (10000-sample text verification) passed in-build. This is the largest
suffixient-set construction ever, and it is byte-reproducible.

## CHUNK-MERGE V3 GATE: COMPLETE - ALL GATES PASS
The webtext construction route is proven END-TO-END: 16 libsais chunks (268.78s) -> batch-BCR
merge (20,599s) -> FOUR FILES BYTE-IDENTICAL to the monolithic PFP reference (cmp PASS x4) ->
endpoints -> slim (397,723,010 heads resolved DIRECTLY, head_lf_steps=0 - THE LAW held through
the merged structure) -> CHI = 306,164,765 (N=1,082,130,213, R=397,723,010) DERIVED FROM THE
MERGED STRUCTURE, exact. sA byte-size gate PASS (2,449,318,120). HONEST TABLE (banked,
TABLE.md): at fragment scale monolithic PFP (4,789s) beats chunk+batch-BCR (20,868s) - as
predicted; the route's value is bounded-memory construction + the runway to the open O(runs)
cross-LCP merge; the parallel merge tree (~4x wall at 16 chunks, next lane) closes part of the
gap. FOUR constructions have now produced identical fragment numbers: PFP monolithic,
PFP regenerated, BCR small-scale differential, and merged chunks.
Incident note: the finish needed two one-line recoveries - the endpoints' dictionary path
(symlinked reference parse) and the raw-ri4 guard regression (the witness-locate rescue
clobbered the v2 lane's guard fix; re-applied, lesson: file-level rescue copies must diff
against current main first, not blanket cp).

## SXI2 V6-3 COMPLETE: FORMAT VERSION 5 SHIPPED AT ALL THREE SCALES (all gates green)
The implicit-exception phi encoding (singleton-run tails are implicit; v stored only at
non-singleton edges, ranked by a bitmap): pile-frag 5,196,757,368 -> 2,606,290,008 bytes
(0.502x of v6-2!), phi 42.25 bits/run (from 63.36) - AND WARM HTTP FASTER: 0.325 ms vs
0.362 (the exception path short-circuits the common case). yeast: 1.414 -> 0.924 GB
(0.653x, phi 59.19), warm 440 vs 382 ms (+15%). k10: 28.37 -> 20.22 GB (0.713x, phi 71.96),
warm 4.61 vs 3.47 ms (+33%). ALL GATES GREEN at all three scales: native MEM byte-parity,
bounded MEM parity, HTTP byte-parity, chi exact. The SINGLETON RATE is the knob: pile-frag
71% singletons -> 42.25 bits/run (approaching the 25-30 floor); yeast/k10 pay more per
exception. HONEST TRADE: v5 = 29-50% smaller; warm latency improves where singletons
dominate (webtext), regresses modestly where they do not (DNA). Lane history: the original
lane died at provider capacity mid-gates (work rescued via retention patch + 3-way merge
with the run-width widening - Edge.run u64, phi inverse u64, implicit_v kept). The 25-30
floor remains open (exceptions coding is the next lever).

## MERGE-TREE GATE COMPLETE: ALL GATES PASS - the tree is proven, and honestly priced
TREE MERGE: 16 chunks through 4 pairwise levels -> FOUR FILES BYTE-IDENTICAL to the monolithic
PFP reference (CMP_PASS x4); CHI = 306,164,765 DERIVED FROM THE TREE-MERGED STRUCTURE, exact
(CHI_PASS, sA bytes 2,449,318,120). FIVE constructions have now produced the identical fragment
number and artifact family.
HONEST PERFORMANCE (TABLE.md): tree 19,597s vs serial 20,599s = 1.05x - the parallel tree at
16 chunks is nearly serial, because the TOP LEVEL dominates: level 4 (final pair merge) alone ran
16,063s of the 19,597s. Level walls: L1 818s (8 pairs), L2 846s (4), L3 1,869s (2), L4 16,063s (1).
The tree is a SCALING answer, not a 16-chunk speedup: at k chunks the lower levels parallelize
wide and the wall converges to top-merge + spread; at k=16 there is almost nothing to hide.
PILE PROJECTION (linear, conditional): 26x50GB chunks -> ~1.30e6s fully-parallel level sum,
top level ~7.4e5s; AND the blocker: final-level BCR state projects ~330 GB (over the 64 GB
line) - a memory-bounded BCR state is REQUIRED before any 50 GB attempt. The batch-BCR route
at pile scale is thus bounded by BOTH the top-merge wall AND memory - reinforcing that the
O(runs) cross-LCP merge (cost model banked: 23.7 symbols/run conditional) is the real pile path;
the tree proves the semantics that merge will inherit.

## CORRECTION #19 + V6-4 NEGATIVE (the fleet catches the supervisor again): the permutation is incompressible
My 12.29-bit delta-entropy measurement was a SAMPLING ARTIFACT (unique-sample entropy ~ log2(5000));
the lane's exact gamma-Golomb accounting proves the run permutation is at flat-code cost on real
corpora (yeast 28.16, pile-frag 30.35 = flat log2(r); consistent with the 0.507 coin-flip
increasing-fraction) - the BWT scrambles the permutation to genuine randomness. 'Permutation
compresses >2x' is RETRACTED; the 42.25 bits/run v6-3 association is closer to the true floor
than the preflight's 25-30 (which partly assumed the same compressibility). Remaining unmeasured
levers: exception-value coding and adaptive models - both speculative, neither scheduled.
SHIPPED ANYWAY: format version 6, codec 120 (gamma-Golomb with never-regress fallback to 119) -
zero-cost capability insurance for any future corpus with compressible permutation; three codec-path
differentials PASS, three-scale gates PASS (ratios 1.000, byte-parity identical, phi unchanged
59.19/42.25/71.96). v6-3 REMAINS THE SHIPPED CONTAINER. Container effort now redirects to the
cross-LCP merge (the pile's actual blocker) per the lane's own recommendation - agreed.

## ROUND 5 (the relaunch after the infra repair): paper refresh + minimality round 4
(1) PAPER REFRESHED: main.tex now carries the from-zero unqualified 466 result (byte-identical
second construction, sha256 2aca539e...), five constructions of the fragment number, tonight's
theory additions with Lean file citations, the v6-3 container results at the honest ~42 bits/run
floor, the witness-anchored locate, and the negative-results paragraph. 194 insertions; braces
and environments balanced (pdflatex unavailable on this box - compile check pending upstream CI
or a local texlive install).
(2) MINIMALITY ROUND 4 (partial, real): O2 (NO DUPLICATES) IS NOW AN UNCONDITIONAL THEOREM for
every physically realizable text (length <= 2^63-1) - scan_nodup_of_length_le in SxgcNodup.lean,
no premises, kernel-checked. The exact saturation boundary is characterized: the scan's fixed
reset cap binds only beyond 2^63-1. The remaining obligations for the minimality lower half are
now the MINIMAL precise set: O3_maximal + O4_distinct (plus an astronomic-texts-only O2 lock),
with the assembly lemma minimality_lower_of_O3_O4 PROVEN - the last sorry now reduces cleanly
to two statement-locked lemmas. Full lake build green (6 jobs).

## THE CROSS-LCP MERGE IS DONE - THE PILE'S BLOCKER HAS FALLEN (round 5, glm-5.3 lane)
bit6/cross_lcp_merge.cpp (999 lines, verify-by-binary: builds from main). THE ALGORITHM
(refined beyond the design lock): merged vs local order differs only on PREFIX pairs; every
short side is an ANCHOR whose occurrence block is found by backward search on the chunk's own
BWT (walk stops when the block drops below 2 members); merged order = local order with anchor
blocks sorted in place by merged key - exact, with block splits handled naturally. Dense prefix
fingerprints over M.M, 16-symbol checked fast path, galloping probes + bisect, FULL direct
verification of every proposal (a hash disagreement aborts - caught two real bugs in dev).
Periodic chunks (tied classes wrapping position 0) handled BCR-safe. ~150k brute selftest cases.
GATES ALL GREEN: (a) 26+ synthetic collections byte-identical vs direct BCR incl. periodic/
all-equal; (b) FULL 16-chunk fragment: four files byte-identical (SHA-256) under BOTH tree and
serial schedules, chi = 306,164,765 exact, sA 2,449,318,120 bytes; (c) COST: TREE 1,331.4s vs
banked 19,596.8s batch-BCR = 14.7x FASTER; serial 2,991.9s vs 20,599s = 6.9x; exactly 1.00
comparisons/position, 17.9 verified symbols/symbol, 2.1 probes/comparison, peak RSS 27.5 GB.
HONEST CAVEATS: per-position amortized O(1) (runs genuinely split) - the merge is linear with
tiny constants, not O(r); fully-periodic large inputs slower (still exact); the finish sequence
currently uses the reference parse for slim LCE (the pile needs the parse-free decoupled slim -
the remaining piece); artifacts at /home/erikg/cross-lcp-run deletable per the recipe rule.
PILE PROJECTION: 1.31e12 positions x ~18 symbols ~ 2.3e13 verified symbols ~ hours-to-a-day on
48 cores at 27-100 GB-class RSS per pair. THE PILE'S REMAINING LIST: parse-free slim at scale +
the full widened-run battery. Construction itself: SOLVED.
