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
