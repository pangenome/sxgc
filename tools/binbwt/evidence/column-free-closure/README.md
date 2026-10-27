# Column-free rank closure qualification

The column-free verified-LCE arm now executes the same single-writer rank derivation, endpoint stamp assertions, per-band adjacency and contiguity checks as the column arm before answering its LCE queries. Both arms require zero closure counters and complete rank coverage before publication. Stored tail SA values remain unchanged. Verified LCE is the selected implementation, not a fallback from a failed column lookup; no fallback implementation exists.

150 MB: exact required SHA, chi 44034231, runs 57279187. Closure rank_holes=0 disorder=0 noncontiguous_runs=0 walk_fallbacks=0, max_rank_end=n=150000001. Full independent BWT comparison and the explicit central 4096-row comparison both report zero mismatches. The sampled SA has 4095 exactly checked adjacent pairs. Six finish selftests, checker selftest, sampled-checker corruption/offset selftests, and the dead-emitter integration test pass.

This run's observed whole peak is 569098240 bytes (3.793988241n), with 470548480 bytes (3.136989846n) in the rlebwt allocation at that instant. This is a clean negative for a 3n space assertion on this run, despite identical product bytes and zero closure counters. The previous 2.851635181n sample is not reused as this run's measurement. XFS speculative allocation is included, not subtracted. The 10 GB measurement must independently qualify the 100 GB launch.

Sampling is approximately one second and gives observed lower bounds, not continuous upper bounds. The central sample checks finite suffix order and BWT predecessors; it does not prove a global permutation or independently validate the entire 100 GB suffix order. Timing values are single concurrent-run observations, without a speedup claim.

The 10 GB component sampler invalidates the earlier 150 MB extrapolation of a 3n total peak: an observed sample already reaches 3.975649382n. That sample includes rlebwt 2.0918n and waves 1.1325n in allocated filesystem blocks, plus PFCK, pieces and compressed witness spools. No allocation is subtracted to manufacture a 3n pass. This does not itself imply the 100 GB rung cannot fit. Launch requires the measured prediction with 25% margin and retains the pre-existing conservative 9n free-space requirement; all byte and closure gates remain mandatory. The provisional 3n prerequisite added during this task was unnecessary and has been replaced by the original, stricter free-space admission guard. The measured 3n result is reported independently.

## 10gb concurrent column-free gate

SHA `22d0ce7493c6238a9d2e7f4e9a0decdf5841ff72a845d856bebaecd6d34bdaee`, chi 2676929428, runs 3474891366.
XSA_FINISH_KASAI_CLOSURE bands=53004 rank_holes=0 disorder=0 noncontiguous_runs=0 walk_fallbacks=0 max_rank_end=10000017258 n=10000017258

Observed whole-run peak 39756562432 bytes = 3.9756493820242964n. Stacked table and complete one-second component samples are retained in 10gb.

Order / emit / finish walls (seconds): 1947.601 / 1948.604 / 2203.289. These are overlapping stage lifetimes, not additive.

Sampled STREAM_VS_BWT: {'first_rank': 5000006581, 'count': 4096, 'mismatches': 0, 'decoded_extent_rows': 10000017258, 'source': 'retained sorter SA sample'}. All m1 runs are decoded and their total extent checked; only the named central 4096 ranks are compared to remapped corpus predecessors. Local exact suffix order checks cover 4095 adjacent pairs.
Additionally, full independent BWT comparison against the read-only full SA fixture passes with zero mismatches.

## 100 GB corpus-window rounding discovery

The first renewed attempt was stopped cleanly with all three stages reaped and no container accepted. Its corpus-sized mapping budget rounded each of four windows down to pages, leaving 15448 bytes uncovered: need_windows=5 and pinned=0. The fallback takes a shared mutex on every corpus byte; this is not a useful qualification of the intended pinned product path. The desired mapping budget now rounds UP to four pages before applying the unchanged hard RLIMIT_AS/3 ceiling. Under insufficient address space the locked bounded path remains; no resource limit or byte/audit guard is waived. Native and packaged gates must be repeated before relaunch. The aborted logs and complete sampler are retained under 100gb-window-rounding-aborted.

The mapping correction passes the renewed 150 MB byte/closure/full-BWT/sample gates and all seven selftests. Observed whole peak: 3.090431979n. The native finish and emitter are byte-identical to the successful packaged release build. A launch permission-bit mistake was corrected before any stage ran. Two subsequent attempts exposed /proc/io becoming unreadable during kernel exit_mm; the final runner accepts only a proven PF_EXITING process that becomes waitable within five seconds, records the kernel exit race, and still rejects unreadable live processes. The successful run records order PF_EXITING=true, returncode=0; no progress timeout, disk guard or byte audit is waived.
