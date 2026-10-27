# Concurrent wire gates

150 MB audit arm: finish started first, order and emit immediately after it.
CROSS_V2_PIECE_WINDOW=21; sink writer credits remain unchanged. The whole-run
allocated peak is 406,822,912 bytes (2.712152729n); live pieces+BWT peak is
1.675373215n. Required container SHA, chi 44034231, runs 57279187, closure
0/0/0, walk_fallbacks=0, independent full STREAM_VS_BWT and all seven
selftests pass. See the reconciled per-component table in 150-audit-v2.
The column and ISA are external read-only audit inputs and excluded from
new scratch; this is explicitly not a measurement of generating them.

The first attempt was terminated and all children reaped after a /proc/io
exit race. The rerun handles that race by checking process exit status;
permission errors on live processes still fail. Failure-path integration
also proves a dead emitter reaps the waiting finish and sorter.

Two blockers invalidate the requested total-space prediction:

* The existing BWT is consumed only in the final m1 replay, after the sweep,
  not concurrently with the pieces. xsa_finish.cpp:5676 waits for its full
  length and replays it at 5705; deletion is at 5793 after container publish.
  Concurrency bounds pieces, but does not bound this BWT to a window.
* Piece endpoints are suffix **corpus positions**, not suffix ranks. The
  captured 150 MB run 27262976 has length 6, head SA 90585844, tail SA
  116952685. Head SA + length - 1 is 90585849, which is wrong. Rank closure
  cannot supply SA[tail_rank]. The named case is preserved in
  tail-sa-counterexample.json; no tail value was substituted or silently
  reconstructed. A rank-addressable SA or equivalent position resolver
  would be needed before removing this column.

No C++ sources have been changed, no guard weakened, no run above 150 MB
launched for this change. The >10 GB hold remains in force.
