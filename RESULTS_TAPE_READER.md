# Tape-reader gate results

Branch `feat/tape-reader`; production source is
`xsa/runtime/tools/binbwt/tape_sort.cpp`. The stale `tools/binbwt` copy is untouched.

## Implemented design

Initial requests are seeded by the radix partition. Follow-ups carry the exact
positions from their tied classes, never deep prefixes in the matcher. Thin
classes receive byte windows. Classes predicted to exceed the window-plane
bound, or surviving their window, receive per-class dictionaries and phrase
sequences; the slice is that phrase index. The shared pass computes phrase
boundaries; ordering uses the byte-walking phrase comparator (including its
exact-period path), widening and bounded residual LCE. Trigger collisions are
journaled as `prefix_relations`; prefix-freeness is not an exactness theorem.

## Corrections and evidence

- Removed the inherited wall-time watchdog and its signal delivery. Anonymous
  memory refusal exits with numbers; matcher allocation refuses at 1,048,576
  nodes before allocating the next node.
- Positional indexing reserves once per pass instead of reallocating and copying
  all accumulated positions per request.
- The matcher consumes radix-prefix lookahead beyond `KEY_N`, but admits only
  starts below `KEY_N`. Root-child matcher depth is one, not zero.
  On docsep with 100,000-row units, the original reader lost positions
  6,291,431 and 6,291,432 because their prefixes ended beyond its scan boundary.
  Symbol-zero multiplicity was already correct; the emitter detected the loss.
- Docsep, binary source `c355c10`: corpus 6,291,456 bytes; 6,291,457 completed
  rows; 116 units; 30.086 s process wall; `SS_DONE`; brute-force SA oracle
  **0 mismatches / 6,291,457 rows**. Threads 24, envelope 96,000,000,000 bytes.
  Evidence: `/mnt/nvme2n1/erikg/TAPEREADER/gate-docsep/result.json`.

The reproducible fixture runner is `gates/tape_reader_gate.py`. It requires
`SS_DONE`, converts the packed unit stream to the oracle format, and preserves
logs and JSON receipts. Runs are detached with `setsid nohup`, without timeouts.
Scratch is `/mnt/nvme2n1/erikg/TAPEREADER`.

## Pending gates / limitations

The remaining fixtures, P1 and P2 are not yet attested. No pile-wall projection
is justified before their measured sweep, sort and finish rates exist. The
product driver currently still invokes the old scatter-sort/concat route;
container identity must exercise the new reader explicitly, not accidentally
compare two old-route builds. Docsep's 6 passes versus two maximum sort rounds
also exposes scheduler batching that the P1 accounting must measure.

## Full fixture gate (green)

Native source `4a72ee1`, 24 threads, 96,000,000,000-byte envelope,
100,000-row units. All eight named 6 MiB fixtures (including `refuse`) and the
16 KiB collision fixture completed with `SS_DONE`: **50,348,041 rows,
1,175 units, zero SA mismatches**. The seven original nonperiodic fixtures
match their saved brute-force oracle streams byte for byte. `refuse` matches
the completed brute-force oracle compiled with GNU parallel sorting and
`OMP_NUM_THREADS=24`; its comparator is unchanged. The collision is directly
oracle-green at 16,385 rows with **4,112 prefix relations** journaled.

Full commands, row denominators, SA hashes and receipts are committed in
`gates/tape_reader_evidence/fixtures.json`. Additional period-1/31/257 boundary
cases are oracle-green (30,017 rows total). `cargo test --release -j 24`:
106 passed, 0 failed, 1 ignored. Cargo still exercises the existing product
route; the native gates above exercise the new reader.

The triggerless periodic case now uses a bounded candidate period (at most
2,048 bytes), certified against every byte during the existing census. The
sorter orders phase ranks and finite lengths, with exact short-tail handling;
no deep per-row walk or widening of empty streams is needed. `refuse` uses a
5-byte period and one extraction pass. The bound now tracks exact live bytes
by unit and request class, charges follow-up reservations before allocating,
and refuses a batch that would otherwise wait while retaining its own
unconsumed groups. New builds observe `stop.requested` from the memory monitor.

P1 is next. Its harness records identical resource flags and the distinct
native output plumbing explicitly: main uses scatter-sort plus concat; the
reader emits pieces directly. This is a native-stage comparison, not yet a
comparison of two integrated `xsa build` routes.
