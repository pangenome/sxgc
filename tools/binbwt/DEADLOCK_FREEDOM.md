# Tape pipeline: progress proof and implementation boundary

**Status: the complete pipeline is NOT certified deadlock-free.** This change
implements bounded out-of-order SA transport for `--no-lcp-slices`, read-time
piece credit, subdivision and instrumentation. The legacy LCP-slice route still
has a sorter-to-finish credit edge and timed credit growth. Its removal requires
a further wire-format change described below. Passing small product gates does
not discharge that proof obligation.

## Required architecture

The target pipeline has exactly two bounded queues:

```
sorter -- band queue (K) --> merge -- piece queue (P) --> finish
```

A band envelope contains all information needed to turn that band into output,
including its LCP data when enabled. A piece envelope contains all information
needed to read that piece, including the corresponding LCP span and closed seam
metadata. There must be no independent sorter-to-finish credit queue.

Ownership invariants:

1. The sorter reserves a publication credit before admitting a job. An admitted
   job can sort and write without acquiring another credit. It releases its
   arena after writing; it never holds an arena while waiting to claim a new
   job. Publication does not require the claim mutex.
2. Sorter bands and output pieces are different partitions. A piece can span
   multiple bands and a band can span multiple pieces. Band credit must never
   depend on closing or publishing a piece. The merge reads any available
   completion in its admitted key window. It
   copies the complete envelope into reserved private storage, deletes the
   input, and returns its credit **before** entering downstream publication.
3. The finish copies a complete piece and returns its credit immediately after
   reading. Subsequent processing uses private storage. It cannot wait for
   another input before returning a credit for an input it already read.
4. Every queue item is independently readable. In particular, consuming a piece
   cannot require a future, credit-blocked piece or a future sorter LCP slice.
5. Reorder storage is reserved for the complete admitted window, including the
   earliest missing key. A later completion cannot take that reservation.
   Completion reception may be out of order. Processing into the run sink is
   strictly in key order: `reorder[band - windowFirst]` is processed exactly
   once before advancing `band`. Piece writer completion may be out of order,
   but each immutable job owns a unique run interval and piece pathname.
6. End-of-stream and failure notification do not require a data-queue credit.
   Finite local work and eventual scheduling/I/O completion are assumptions of
   progress. A crashed process or permanently failed disk is outside deadlock
   freedom and must produce a separate failure result.

## Run ownership and failure propagation

One live pipeline generation owns each canonical scratch path. `xsa build`
acquires an exclusive nonblocking advisory lock on the sibling
`.<scratch-name>.xsa-build.lock` before opening the journal or starting stages.
The lock descriptor is inherited by all stages and wrappers, so killing the
driver does not permit a replacement run to reuse their paths. Keeping the
lock outside the scratch tree also covers deletion/recreation of that tree.
Never unlink the lock file while a stage is alive. These guarantees apply to
launches through `xsa build`; standalone native stage invocations must use fresh,
exclusive paths. Older binaries without this lock must be stopped before reuse.

The owner polls every live stage, rather than joining the sorter before observing
the merge and finish. Any nonzero exit drops the remaining stage handles, kills
their private process groups, and reaps their wrappers. A merge failure therefore
cannot leave the sorter waiting indefinitely for a credit that will never arrive.
Failure handling is separate from the finite-work progress argument below.

The October 31 piece collision was observed with an older merge and two finish
processes sharing the failed run's paths; the old merge still held its deleted
log open. A controlled scratch-replacement regression reproduces the collision
with an ordered input stream. The existing reorder algorithm is retained.
`CROSS_PIECE_COLLISION` reports the path, whether it existed at process entry,
the attempted piece run interval, source band, and that source band's run
interval through the attempted publication. This last interval is a prefix:
future runs in the same source band may not have been processed yet.

## Bounded-buffer argument

For a single queue of capacity C, let q be the number of readable items, with
`0 <= q <= C`. A producer blocked on enqueue implies `q == C`; therefore the
consumer has an item and can read it without waiting for the producer. A
consumer blocked on dequeue implies `q == 0`; therefore the producer has free
capacity. Full and empty are mutually exclusive for positive C. Thus the two
ends cannot simultaneously wait on one another through this queue.

For the three-stage chain, a merge blocked on a full output queue may temporarily
stop reading its input. This does **not** mean that every consumer is always
runnable whenever its input is nonempty. Follow the dependency forward: the
finish has an available piece and no downstream stage. Similarly, follow empty
input dependencies backward to the sorter. The underlying stage graph is a
path; any directed cycle on a path would require opposing edges on an adjacent
pair. The full/empty exclusion above rules that out, provided invariants 2--4
hold. Local resource locks must not add edges that bypass this argument.

Reserved sorting jobs are not yet readable queue items. Consequently the claim
“all K reservations occupied implies the consumer has an item” is false. The
additional invariant is that every unready reservation belongs to an admitted
job that can finish without another queue credit. If a window lacks an item,
its earliest missing job is either runnable or already published. Sorting jobs
cannot wait on later jobs, the output sink, or the claim mutex. This closes the
reservation case without calling a timer part of the proof.

Unrestricted completion order plus a bounded reorder array alone is insufficient:
if K later keys fill the array and the earliest key was never admitted, a cycle
is possible. Admission and a reserved slot for every current-window job are
necessary, not optional optimizations.

## Implemented SA transport

The no-slice sorter publishes manifest version 2: `n 2 K row_capacity`.
It atomically publishes `window_<first>` descriptors before launching a wave.
Each descriptor names a consecutive key range containing at most K bands.
The merge scans all completion markers in that range, reads each available
payload, and acknowledges it before waiting for missing completions. It emits
that range only after all its payloads are private. Empty bands and short final
windows use the same lifecycle. Version 1 remains readable for compatibility.

Admission reclaims **any** consumed reservation, rather than waiting for
`band_(next-K).consumed`. A slow sort therefore does not prevent other freed
credits from admitting work. The window may still wait for its earliest key
before emission: bounded memory cannot promise arbitrary reordering without
such a limit. That is finite local work, not itself a circular wait.

The packed reorder bound is fixed at startup: the packaged sorter's existing
staging allowance (`budget.stage`, at most 2 GiB), communicated as a row capacity
in the manifest. Equivalently K nominal transport slots each have
`row_capacity / K` rows at eight bytes per row; variable-size bands share the
aggregate allowance. Descriptors are shortened using actual scatter row counts,
so sampled partition skew cannot silently overfill that bound. A single band
exceeding it is refused before sorting. The merge independently enforces the
aggregate bound before allocating each payload. `SS_REORDER_WINDOW` journals
actual rows, bytes, limit and ratio. The previous attempt to use the sorter's
nominal quantile size refused the 143,604,756-row band 55 at 10 GB: nominal
quantile size is not a maximum, and cannot support a memory proof.

This is additional merge-private storage; it is not the sorter's arena. The
packaged sorter's existing `RowGate` and `SS_TAPE_BUDGET` continue to account for
its actual arena separately. A combined whole-process-budget qualification
remains necessary; the standalone sorter uses its RAM setting for this bound.

Band boundaries now come from a positional stride sample with multiplicity.
Within each wave the planner groups 24-byte prefixes, packs to 80% of the row
target, and recursively extends only oversized equal-prefix nodes by 24 bytes.
Finite suffixes compare by length when a key window reaches EOF; no padding
symbol is used. Depth 192 is the backstop: an oversized nonanalytic leaf emits
`TAPE_LEAF_AT_CAP`, and actual bands exceeding 110% of the target fail before
sorting. `TAPE_DEPTH_HISTOGRAM` and `TAPE_ROW_BOUND` expose both checks.

A repeated symbol is analytic only when a direct corpus check proves that its
entire prefix class belongs to the single-symbol suffix ending at EOF. Runtime
also checks every member against that proved region. Such rows are already in
position order from scatter and are reversed, with `order=analytic`. Position
descending means shorter suffixes first under this machine's finite comparator.
Agreement among sampled bytes alone does not prove terminality. All other bands
use the full comparator. There is no elapsed-time split heuristic or runtime
subdivision; each wave scatters once against its planned variable-depth fences.

These changes bound rows, not suffix-comparison cost. Long internal repetitions
can still make a bounded band slow. A very large analytic leaf can also exceed
the strict row assertion; analytic order does not waive the memory bound.

Band-credit waits now emit periodic diagnostics (default 1800 seconds;
`XSA_TAPE_WAIT_S` override), never terminate the producer or increase capacity.
`SS_WAIT_DONE` records even sub-two-second waits. Packaged per-band sort timing
now includes the actual sort, which the previous counter accidentally omitted.

## Existing piece lookahead and local writers

The actual piece reader has a weaker invariant than independent readability:
`ready_band(b)` may wait for seam metadata through `b + extent`, where
`extent < P`. With b the earliest unconsumed piece, all pieces through
`b + P - 1` have publication credit. `BinBandSink::closeBand` checks an active
class's next-band extent **before** entering `v2_flow_wait`; a class that would
outgrow that horizon is refused instead of waiting for an impossible credit.
Thus a full credited window contains the metadata needed to consume its first
piece, or the producer has already reported failure. This is the full-window
progress invariant, not the stronger claim that every nonempty queue permits
an immediate read. Failure propagation by the pipeline owner is still required.

The merge's local writer slots are distinct from disk piece credits. A writer
pops its job while holding the mutex, then writes piece and progress files
without waiting for finish or taking another slot. Its final critical section
returns the slot. The publication thread releases the mutex before waiting for
disk credit. Therefore writer-slot dependencies end at finite I/O; they do not
add a reverse dependency to the publication thread.

## Remaining proof blockers

- LCP slices are still produced by sorter and consumed directly by finish.
  Their row-credit growth and timeout logic remain in the legacy route. A piece
  spans runs, not sorter bands, so returning an individual slice at read time
  alone does not remove the cross-stage cycle.
- Sort-time thresholds do not bound the time of one suffix comparison, one key
  sort, or a complete band. No seconds-level latency guarantee follows from
  deadlock freedom.

To finish the design, move LCP ownership to the merge, attach LCP spans to the
same piece envelope, and allow bounded partial-piece publication when a complete
run-count piece would need more input than the reserved row budget. Finish must
stage each envelope independently of subsequent metadata, returning its credit
before reconstruction. Both the fixed-run-count assumptions and seam-class
capture checks need corresponding changes and product gates. Removing the
legacy growth timer first would merely expose its remaining cycle.

## Validation

`test_tape_progress_model.py` exhausts all reachable schedules of a seven-band
abstract pipeline for K=1..4 and P=1..3, including arbitrary sort completion and
reserved reorder windows. It deliberately does not model the legacy LCP side
channel or real seam metadata; it cannot certify those implementations.
`test_tape_any_credit.py` returns only band 1 credit and requires band 2 to
publish while band 0 remains unread. `test_tape_reorder.py` withholds band 0 and requires band 1 to be read and released
first. `test_tape_release.py` checks multi-chunk read-before-publish and empty
bands. `test_tape_split.py` compares suffix order and BWT symbols to an independent
finite-suffix oracle on uniform, periodic, mixed and random inputs.
`test_tape_wait.py` requires a diagnostic while the process stays alive.
`tape_redesign_gate.py` records commands, process exits, product hashes, waits,
splits and top-five completed-band sort times; it reaps its own process groups
on interruption. See `RESULTS_DEADLOCK_REDESIGN.md` for observed gates and limits.
