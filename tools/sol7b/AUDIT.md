# sol7b structural audit (base 21ad74b)

Disposition: route (c), no production protocol change. The requested speedup is
NOT achieved. Neither proposed route currently supplies the exact checkpoints
needed to partition the existing serial comparison trace. This is a statement
about the audited implementations, not an impossibility result for parallel merge.

## Route (a): anchors are not execution checkpoints

`bit6/cross_lcp_merge.cpp:2306` defines `BlockRec { q, lo, hi; }`.
`anchor_blocks_ext` records a chunk-local suffix occurrence interval and the
anchor position. `repair_ext` sorts the affected per-side position-overlay
slices. These records contain neither global merged ranks nor run counts,
per-side walk cursors, heap layout, pending run, or open seam-class state.
They are discarded before the cross merge. The hash checkpoints at :1727
are prefix fingerprints, also not execution checkpoints. The finish-side
repair checkpoints concern an already emitted run stream.

Consequently the supplied 76.9 s anchor/repair time is NOT a measured heap
reconstruction cost. No such reconstruction is implemented. Obtaining the
old heap state at an arbitrary band boundary by replaying the prefix performs
the work we are required to partition, and preserves the serial critical path.

## Route (b): exact rows, different work and missing global run bases

The existing planner (:2950-3040) does reconstruct exact **row** ranges from
repaired sides. It binary-searches the merged rank of elements of the widest
side, using rank searches in every other side. Its boundary need not equal
the requested rank and need not close a BWT run. This is exact range partitioning,
not a sampled heuristic; it guarantees row coverage but not serial heap state.

Each shard initializes its heap with hPush in side order, truncates each side
at its shard end, and removes exhausted sides early. The serial walk instead
retains the continuing fronts and its history-dependent heap layout.
Planning adds charged comparisons; heap comparisons also change. Even an
oracle supplying exact per-side cursors for free would not make this loop
reproduce the serial comparison trace. Deterministic work across worker counts
can be obtained with a FIXED logical shard plan, including at P=1, but that
establishes a new baseline and does not preserve the required numeric totals.

Historical commit 7a42011's `ParallelSparseRuns::Shard` writes local RLE to
`runs-ID.spool`; its consumer waits for `done` and reduces shards in order.
`finish()` stitches the first/last seam-target flanks after joining. Its
count/replay path supplied global run bases. It cannot simply attach to BPG1:
local shard EOF is not global EOF, a BWT run can cross any number of shards,
and seam captures may cross a boundary before their after-flank is known.
SequentialV2 contains a single ordered row counter, pending run, class table,
and output stream. Returning S>1 alone would race all of these and fail its
explicit beginShard bounds check. `admission()` also currently returns one.

## Smallest practical change to unblock parallelism

1. Permit a one-time work rebaseline, with all planning/heap work honestly
   counted; require identical totals across P using a fixed logical shard plan.
2. Use the exact row-range planner once, independent of physical worker count.
   Parallel producers publish bounded, ordered row slabs plus explicit range
   completion to one ordered reducer. Keep the reducer as sole owner of
   SequentialV2, run coalescing, seam captures, global band numbering, BPG1,
   and BPT1. Row slabs retain target positions needed for seam capture;
   historical compressed run endpoints alone lose interior seam targets.
3. Keep per-producer queues bounded and reserve progress for the earliest
   unconsumed range. Do not wait for full-range completion before draining a
   bounded queue. Never interpret a range's end as global terminal or clamp
   readiness at that end. Only all-range coverage and validated final flanks
   permit BPT1. Abort wakes every queue/flow waiter. Retry needs stable range
   and slab identities with commit acknowledgements; retries cannot replay
   rows already committed to the reducer.

This preserves the downstream fixed-band protocol and single traversal of
assigned rows, but adds an upstream transport and may leave the reducer as a
new bottleneck. It has not been implemented or performance-gated here.
A run-compressed transport with sparse seam events could reduce traffic but
needs its own model, especially for classes spanning many producer seams.

Alternative under the original numeric-work constraint: supply genuine exact
heap checkpoints generated without a serial prefix replay, including heap
layout and continuing fronts (do not truncate comparisons at shard ends), plus
output-prefix/run/seam state. No audited anchor provides those checkpoints.
Generating them by walking once is not a launch-wall fix.
