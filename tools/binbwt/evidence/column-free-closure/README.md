# Column-free rank closure qualification

The column-free verified-LCE arm now executes the same single-writer rank derivation, endpoint stamp assertions, per-band adjacency and contiguity checks as the column arm before answering its LCE queries. Both arms require zero closure counters and complete rank coverage before publication. Stored tail SA values remain unchanged. Verified LCE is the selected implementation, not a fallback from a failed column lookup; no fallback implementation exists.

150 MB: exact required SHA, chi 44034231, runs 57279187. Closure rank_holes=0 disorder=0 noncontiguous_runs=0 walk_fallbacks=0, max_rank_end=n=150000001. Full independent BWT comparison and the explicit central 4096-row comparison both report zero mismatches. The sampled SA has 4095 exactly checked adjacent pairs. Six finish selftests, checker selftest, sampled-checker corruption/offset selftests, and the dead-emitter integration test pass.

This run's observed whole peak is 569098240 bytes (3.793988241n), with 470548480 bytes (3.136989846n) in the rlebwt allocation at that instant. This is a clean negative for a 3n space assertion on this run, despite identical product bytes and zero closure counters. The previous 2.851635181n sample is not reused as this run's measurement. XFS speculative allocation is included, not subtracted. The 10 GB measurement must independently qualify the 100 GB launch.

Sampling is approximately one second and gives observed lower bounds, not continuous upper bounds. The central sample checks finite suffix order and BWT predecessors; it does not prove a global permutation or independently validate the entire 100 GB suffix order. Timing values are single concurrent-run observations, without a speedup claim.
