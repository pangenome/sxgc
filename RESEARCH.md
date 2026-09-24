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
