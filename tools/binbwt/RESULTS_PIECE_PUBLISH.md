# Piece collision and pipeline ownership (2026-10-31)

Base: `76feb73`; branch: `fix/piece-publish-order`.
Raw evidence: `/mnt/nvme2n1/erikg/piece-publish`.

The proposed out-of-order-processing cause is not supported by the code:
manifest-v2 reception already copies arbitrary completions into a bounded
window, returns their credits, and feeds the sink in increasing band order.
The run sink creates each piece job once from a monotonically increasing run
index. Band and piece boundaries are independent; their credits remain so.

The failed shared run had an older merge (PID 2758129, started 13:28 UTC)
and finish (2758128) alive alongside a later sorter (3084627) and finish
(3084631, started 15:24 UTC). All named `/mnt/nvme2n1/erikg/wbuild`.
The old merge's fd 2 still named `emit-tape.log (deleted)`, while the replacement
log ended in the reported fatal after reading band 28. This demonstrates
scratch reuse with live previous stages, but cannot retrospectively identify
which process wrote the colliding original file.

A controlled regression starts an old merge, replaces its piece directory,
finishes a second ordered stream into that path, and releases the old merge.
It reproduces the fatal without reordered input:

```
CROSS_PIECE_COLLISION path=/mnt/nvme2n1/erikg/piece-publish/collision-sorted/frag.pieces/p-0-0 existed_at_start=0 piece_runs=[0,65536) source_band=0 source_band_runs=[0,65536) pid=3310024
```

The production driver now holds an inherited advisory lock outside the scratch
tree; a live child retains it even if the driver exits and scratch is recreated.
It also polls every pipeline stage, so a failed merge or finish terminates the
remaining process groups instead of hiding behind a sorter credit wait. Native
standalone invocations still require fresh exclusive scratch. Lock files must
not be unlinked to bypass live ownership. Pre-fix orphans cannot acquire the new
lock retroactively; the four verified failed-run groups were terminated and
recorded in `failed-run-cleanup.json`.

## Validation already completed

- G1: 8 MB and 150 MB complete, all stage return codes zero, expected chi
  6,544,643 and 44,034,230; both products pass `cmp` against the retained
  `recursive-planner/release-*` baselines.
- SHA-256: 8 MB `2beb35a0d37a6ff6453b108c4d0a99e8eff4af91cf2875643ae6af86c9539f57`;
  150 MB `9ba2b1a11b02397e8b8a3992bb3366e933a35d1929b0849898178ecc7f31a756`.
- Native bundle rebuild succeeds. Cached finish used by the gates is identical
  to the rebuilt finish (`f4f9b5cb73fa6341867212c8ca021af5c15a989f5ea46c2a7e19c2bc4449627b`).
  The gate sorter source matches the release source digest; `digests.json`
  records the exact binaries. Gate merge is built from the changed source at O3;
  packaged merge is built at O2 and receives a separate small-product check.
- Nineteen chain tests pass, including inherited ownership through the actual
  phase wrappers, scratch deletion/recreation, and failure of each of the three
  stages while its peers wait. The full chain test command also exposes an
  unrelated existing failure in `derived_remap_is_order_preserving_per_corpus`.
  The same test fails on unmodified `76feb73` source: the 256-symbol fixture is
  rejected by the remap validator. `baseline-remap-test.log` records that check.
- Collision reproduction, out-of-order arrival/read-before-publish,
  multi-chunk release, any-credit admission, and all twelve abstract progress
  model configurations pass. The model is not a whole-pipeline certification.

## Large gates and stop evidence

G2 passes: all three stages exit 0, chi **2,676,929,445**, n=10,000,017,259,
r=3,474,889,847. Elapsed pipeline time: 1602.699 seconds.
Product SHA-256: `97dbf45dc6ad42aaf124a566061a818d0a7f1bc72cc91368b883546d71f899bf`.
There is no `CROSS_LCP_FATAL` in the gate logs.

G3 reaches the requested processing boundary: the merge completes band 401 and
enters band 402 (`phase=publish`, `window_first=400`). It has copied/released
408 bands and the sorter has published 415. No collision or permanent stall is
observed. The owner then terminates and reaps all three native stages with
SIGTERM (return codes -15); elapsed time is 1537.271 seconds.
The original Python runner's read-acknowledgement stop was deferred by pausing
only that monitor. A replacement monitor checked native liveness and the
stronger processing boundary, then resumed the runner for cleanup. No native
stage was paused. `g3_processing_stop` records the exact final state.

This is not a seconds-only latency pass: band 45 took **359.612 seconds** to
sort and caused a **324.005-second** credit wait. A one-second CPU sample showed
100 worker ticks; it subsequently completed without intervention and the run
advanced beyond band 401. The existing bounded admission/processing window
cannot hide an arbitrarily slow local sort forever. No capacity was grown.

The independently rebuilt packaged 150 MB gate also completes with all stages
zero and is byte-identical to its baseline. The 100 GB gate is intentionally a
prefix progress check, not a completed 100 GB product. The legacy LCP-slice
route is not qualified by these no-slice runs.

Machine-readable results, exact stage commands, binary/source digests, collision
diagnostic, and cleanup provenance are in
`evidence/piece-publish/summary.json`. The original writer's identity remains
unproven; the observed overlapping generations and controlled collision establish
the failure mode addressed by the ownership guard. The unrelated baseline remap
test failure remains unchanged.
