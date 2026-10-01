# Chunk merge construction: premise audit (2026-09-30 UTC)

## Verdict

The proposed incremental-chi invariant is **false** under the repository's
finite-text suffixient definition, even for two document-aligned chunks. A
nonwitness in the old chunk can become a required maximal coverage class after
an append. The independently built cyclic suffix order of a chunk can also
change after concatenation, even when each chunk ends at a `0x1e` document
boundary and has no empty documents. A stable interleaving of per-chunk BWT
rows therefore cannot be assumed to yield the monolithic cyclic BWT.

These are correctness blockers for the exact proposed merge and pruning rules,
not a proof that every possible BWT concatenation algorithm is impossible.
No chunk-merge builder or yeast/fragment gate is claimed here. Neither corpus
was read or modified.
The referenced `bit6/sxi_logs/token-space/deps/` libsais vendor directory is
also absent from this worktree; no unverified library copy was introduced.

## Exact chi counterexample

The checked definition is `requirements`, `coversAt`, `covSet`, `ScopeLe`, and
`IsMax` in `lean/Sxgc.lean`. For a right-maximal context `w` and right
extension `c`, requirement `(w,c)` is covered by an old position `x` when
`wc` is the suffix of the length-`x` text prefix. A position is a witness
class when its requirement-coverage set is nonempty and inclusion-maximal;
`chi_eq_maxClasses` counts distinct such classes.

Let `s=0x1e`, `A=(1,2,2,s)`, and `B=(1,s)`. Both are document-aligned.

| Position in A | Coverage in A | Coverage in A+B |
| ---: | --- | --- |
| 2 | `{(epsilon,2)}` | `{(epsilon,2),(1,2)}` |
| 3 | `{(epsilon,2),(2,2)}` | `{(epsilon,2),(2,2)}` |

In A, position 3 strictly dominates position 2. In A+B, the second `1` has
right extension `s`, while the first has right extension `2`; `1` becomes
right-maximal and `(1,2)` becomes a new requirement covered **only by old
position 2**. Position 2 is now a distinct maximal class. The exact class
counts are `chi(A)=3`, `chi(A+B)=5`. Thus "new witnesses appear only in the
added chunk" and "old witnesses can only be dominated away" both fail.
The changed condition is a right-extension fork, not merely a merged BWT run
boundary. Append can also remove a right-maximal context that was maximal
only because it was a suffix of A.

## Cyclic BWT merge counterexample

Let `A=(1,1,s,1,s)` (two nonempty documents) and `B=(2,1,1,s)`.
`gcd(freq(A))=1`, so A is aperiodic; this is not a periodic tie case.
With the repository's cyclic-rotation comparator and position tie-break, the
rows starting in A have local SA order `[0,3,1,4,2]`. In the cyclic SA of
`A+B`, those **same rows** occur in order `[0,1,3,2,4]`. A stable merge of
the two local ordered row lists cannot produce the latter. A correct
concatenation merge must revise affected within-chunk row order and their BWT
predecessors. A generic BWT collection merge that only interleaves existing
rows lacks this operation.

## Safer design requirement

1. Specify and prove an exact BWT **concatenation** operation for the cyclic
   text convention, including rows whose comparison crosses a chunk boundary,
   repeated separators, primary-row changes, and periodic ties. A balanced
   tree alone does not establish the `O(R log k)` cost: the number of affected
   rows and the cost of finding them require bounds and measurement.
2. Build or update the LF move map, SA-value phi intervals, sparse anchors,
   and criterion-C escape data from the corrected merged BWT. Endpoint and
   anchor values need exact SA recovery; a naive LF walk can cost O(n).
3. Re-derive chi after **each** corrected merge from the merged text's
   SA/LCP order or an equivalent verified LCE oracle and phi recovery.
   The move map and sparse anchors alone do not supply LCP. The existing
   `bit6/slim_lce.hpp` illustrates why an LCE source is separately needed.
   Only after a proof of how requirements change could local chi updates
   replace this full merge-round derivation. Criterion C and its periodic
   escape semantics remain mandatory for phi use.

## Gates and measurements

`monotonicity_probe.py` implements the finite-text definitions directly and
has no corpus I/O. `test_monotonicity_probe.py` checks both counterexamples,
independently brute-forces the minimum hitting-set sizes, and enumerates 180
separator-aligned two-chunk pairs; 26 revive an old nonwitness.

| Gate | Status | Measured wall | Peak RSS |
| --- | --- | ---: | ---: |
| Three premise regression tests, 180 small pairs | PASS | 0.06 s | 14,336 KiB |
| 20+ synthetic merged-output byte comparisons | NOT RUN; no correct builder | — | — |
| 9.7 KB PFP byte-identity fixtures | NOT RUN; no correct builder | — | — |
| yeast235, about 20 chunks, chi 85,404,336 | NOT RUN | — | — |
| pile fragment, about 16 chunks, chi 306,164,765 | NOT RUN | — | — |

The retained monolithic counts in the task are targets, not observations from
this lane. The older PFP and BCR measurements are not chunk-merge timings.

## Scale arithmetic, not a performance projection

For the requested 1.31 TB / 1,310-chunk tree, there are 11 rounds and 1,309
pair merges. The per-round pair counts are `655,327,164,82,41,20,10,5,3,1,1`.
If a corrected algorithm truly costs O(R) per round, the formal total is
O(R log 1310), at most about 11 passes over merged run counts. No measured
merge throughput, run-count evolution, peak RAM, or wall time exists from
this lane, so numeric time and memory projections would be invented. The
requested under-900-GB gate is unverified.

Chunk merge may eventually compose with BCR for boundary repair or as a
single-chunk base case. The evidence here does not support superseding BCR.
The claimed `O(R log k)` merge and monotone chi update must be revised before
the long corpus gates are worth launching.

## Round 2 follow-up (2026-10-01 UTC)

The banked `bit6/third_party/libsais/token-libsais.c/.h` is now available.
It supplies 32-bit suffix-array/BWT functions, which suffice for a roughly
68 MB chunk. This resolves the dependency noted above. The corrected task
also requests a fresh monolithic fragment reference; that is a separate
reproducibility gate and cannot certify a chunk merge by itself.
The round 2 plan derives chi once from the final merged structure; the
earlier recommendation to rederive it at each merge was a conservative
correctness route, not a required invariant for this revised plan.

The specified run-granularity primitive still has a fatal correctness
obstruction. Its operation is a stable interleave of the two input BWT run
orders. The aligned, nonperiodic example above is stronger than an SA-order
objection: **its final BWT byte sequence is not any stable interleave of the
two input BWT byte sequences**. The independent exhaustive dynamic-programming
gate in `../chunk-merge-v2/test_interleave_obstruction.py` checks all possible
character-level stable interleavings (a superset of run-level ones):
The fixed fixture fails, as do 7 of 20 deterministic, separator-aligned
synthetic two-chunk collections.

| Text | Cyclic BWT bytes (hex) | Runs |
| --- | --- | ---: |
| A = `06 06 1e 06 1e` | `1e 1e 06 06 06` | 2 |
| B = `07 06 06 1e` | `07 06 1e 06` | 4 |
| A+B | `07 1e 06 06 1e 1e 06 06 06` | 5 |

The target prefix forces `07` from B, `1e` from A, and `06` from B.
At the next target `06`, both inputs' next bytes are `1e`; the merge is
already impossible. This direct prefix proof does not depend on samples,
tie handling, rank implementation, or computational limits.

Since no character-level interleave exists, per-symbol run-boundary
rank/select arithmetic cannot produce the required concatenated cyclic BWT
by interleaving these runs. The issue is the ordinary `0x1e` separator and
single cyclic text convention: suffix comparisons cross the chunk seam, so
both order and predecessor bytes can change. BCR or ropebwt2 collection
insertion uses different endmarker/ordering semantics and does not prove this
merge. An exact concatenation algorithm must be specified and gated before
claiming the requested builder, fragment equality, or 100x contrast.

The requested scale arithmetic is: 1.31 TB / 50 GB = 26.2 chunks, hence
27 if a final partial chunk is counted, five tree levels; at 1 GB, 1,310
chunks and eleven levels. These are counts only, not throughput projections.

### Round 2 work in this lane

`bit6/chunk_frontend.cpp` now streams the original file once, maps each byte
as it arrives, ends chunks at `0x1e`, and uses the banked libsais to sort
each chunk's cyclic rotations. It writes a versioned RLE plus run-head and
run-tail local SA samples; it cannot merge those chunks under the specified
primitive. Its tiny independent oracle passes 20 generated aligned
collections, two periodic collections, a nonidentity in-stream remap, and a
no-clobber check. `bit6/third_party/libsais/libsais.h` is the compatibility
include needed because the banked C source includes `libsais.h` but the
banked header is named `token-libsais.h`.

The long fragment regeneration and 16-chunk sort completed under
`vendor/chunk-merge-v2/`, with detailed timings in
`../chunk-merge-v2/REPORT.md`. Fresh reference Gate 0 passed exactly:
normalized runs = **397,723,010**, chi = **306,164,765**. The buffered
chunk frontend produced 16 document-aligned artifacts for all 1,082,130,213
bytes in 3:57.26 wall, 2,309,920 KiB peak RSS; its local run counts sum to
420,913,952. The monolithic six-stage wall sum was 4,789.07 s, with a
28,251,580 KiB largest stage RSS. These are valid front-end/reference
measurements, not a completed merge or 100x throughput contrast. The exact
merge, 20 merged-versus-monolithic gates, merged chi, and independent review
remain open because the specified stable run interleave fails the byte-level
gate above.
