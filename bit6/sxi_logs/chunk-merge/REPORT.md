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
