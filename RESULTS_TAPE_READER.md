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
