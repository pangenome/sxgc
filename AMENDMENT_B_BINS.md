# AMENDMENT B (THE BINS / SUB-N SCRATCH) — spec before dispatch

Status: FROZEN. Completes TAPE_VISION.md's BIN stage for the PAIRS
phase — the one phase that dominates both time and space, and the one
the bin design never reached. Operator order: small-scale proof FIRST;
the machine must be fast AND operate in less than N space.

## Motivation (measured, at the 100 GB launch gate)

The 100 GB projection: level-1 pairs ~95.5e9 records materialized in
full — ~2.29 TB buckets + 2.67 TB columns + 0.49 TB run_start + 1.6 TB
pieces ≈ 7 TB live scratch against 5.2 TB free. At pile scale this is
~80 TB class: untouchable. The machine currently writes a large
multiple of N in scratch; the law of this amendment is that it writes
LESS than N — live scratch bounded by the census-drawn bin size, not
by the corpus.

## B1 — Census-drawn pairs bins

The parse's census (already computed, already rides the parse) draws
bins over phrase-ID ranges, equalized by pair mass — never first-byte,
never sampled (A5's law, applied to pairs). Bin count is set so ONE
bin's working set fits the scratch budget with margin.

## B2 — One bin at a time: build → sort → emit → DELETE

Per bin: build that bin's pairs, sort (the W7 kernels), emit its
contribution, and DELETE the bin's inputs as part of the phase —
deletion is machine behavior, not housekeeping. Outputs stream in key
order; a bin boundary is a commit boundary (A3 — no merge, ever).

## B3 — The cross-bin arrival exchange

Arrivals destined for other bins exchange CSR-style, bounded by the
per-bin working set (the vision's design). No global arrival table.

## B4 — THE SCRATCH_LAW (a receipt with teeth, like DIAL_LAW)

Peak live scratch bytes journaled per phase per arm
(max_live_scratch receipt, from statvfs deltas + per-phase accounting).
Asserted: peak live scratch <= the declared scratch budget at every
scale — violation dies with numbers.

OPERATOR'S CLAUSE (the scale-invariance of the law itself): the
scratch budget is NOT a free knob. It is <= 2xN at every scale — the
100 MB arms run under a ~200 MB budget, the 1 GB arms under ~2 GB,
100 GB under ~200 GB, the pile under ~2.6 TB. The census must draw
enough bins to fit. If a component cannot scale down (a fixed-size
pool, a run-ahead, a buffer sized independent of N), the receipt
must NAME IT — the SS_SCRATCH_PEAK receipt carries a per-component
breakdown (pairs/bins, columns, pieces, pool run-ahead, other) so the
fat is visible, not just the total — and the die() fires if the
component, not just the sum, breaks the budget. 100 MB must work in
100 MB.

## B5 — Level-2 stays a census decision

No machine-mode change here. The level-2 census rides the 100 GB run;
its yield decides the pile's level (the 29.86x condensation measured
at 100 MB / 1 GB / 3 GB).

## Verification — small scale FIRST (operator order)

1. Sandbox (100 MB), all three dial arms: byte-identity SHA
   6325c843…f341 unchanged + the wall table (bins vs W-machine) + THE
   NEW RECEIPT: peak live scratch, old vs new, per arm. Space must be
   a number, like time.
2. G0 fixtures (31a764b6…), then 1 GB / 3 GB identity vs the certified
   containers.
3. The 100 GB launch package returns to the operator: walls AND the
   measured scratch curve, before any launch.

## Non-goals

No container format changes. No merge machinery (bin order is commit
order). One mode. The pairs comparator's std::function wrap cost and
the emit resolve-path costs are separate work items, journaled, not
smuggled into this amendment.
