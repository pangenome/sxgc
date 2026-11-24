# Whole-key tiles reduced the publication wait, not the routing work

For the same four build cores, 512 MiB shared / 320 MiB arrays, 192,001 total slots, 3×2,048 rows of buffering per lane and separate one-core / 140 MiB checker, tiled publication took **14.572 s** vs **34.124 s** for [one ring per full range](../finite-direct64-100m/). Cold private raw+inverse → independently verified EOF **76.964 vs 93.489 s** (0.8232×). Both arms verified every one of the 100,000,504 position / byte LCP / BWT rows, the final rank and the original narrow SHA `82747f11a9925bb4d89cc812410d98e411ce80b1767ee1ff4dc22e94e2cd874e`. A separate byte comparison after close matched both entire 165,974,592-byte posting files (SHA `6285d881c12db13a56cee958091a7bb7f42fc8c197e2bd876fee1e3c46d2997c`, 0.302 s / 0.267 CPU-s). No row spool or rerun of the full-range control.

| Measured | Full ranges | Tiled |
|---|---:|---:|
| Distinct offsets decoded / local scans | 95,434,516 / 0 | same |
| Whole ranges / order waves | 2,486 / 622 | same |
| Publish units / packets | 2,486 / 50,056 | 63,177 / 63,655 |
| Publish wall / cat wait / send, s | 34.124 / 32.952 / 1.052 | 14.572 / 9.651 / 4.450 |
| Aggregate expansion / blocked lane-s | 84.559 / 46.295 | 53.207 / 9.424 |
| Unblocked expansion lane-s / publish wall | 1.121 | **3.005** |
| Order wall / gather lane-s / sort-only lane-s | 54.461 / 28.327 / 143.967 | **57.469** / 28.746 / 151.568 |
| Producer CPU-s | 199.04 | 210.75 |
| Producer attributed read / write, B | 125,374,464 / 165,986,304 | 125,370,368 / 165,986,304 |

Cuts extended across 287,161 additional descriptors to keep ties whole; largest tile 6,734 descriptors / 10,386 rows (column maxima). Four tiles exceeded a lane ring's entire capacity and streamed normally. Planning took 0.207 s. More, smaller packets raised send time; not a pipe throughput improvement. Same 85,107,405 groups and 6,545-descriptor max tie; no class bypass needed here. Passed predeclared publication ≤0.65× / ≥2 unblocked lanes, overall wall ≤0.90×, read/write ≤1.05×. Elapsed lane clocks include scheduling, not counters of executed CPU instructions. One observation, no confidence interval or claim that unchanged order work became inherently slower.

Joint scratch incl inverse + margin 195,533,144 / 200,001,006 B; counted sample 191,539,748 B, whole allocated sample 391,770,112 B incl the two additional raw copies. Array bound 247,981,272 B; producer sampled RSS 225,460,224 B and cgroup peak 427,880,448 B. Raw checker 8.15 CPU-s, 76.652 overlapping service seconds, 115,392,512 B cgroup peak. No swap or OOM. Per-device IO unavailable, not zero. Setup copy 0.495 s, private cold barrier 0.013 s, proof of posting identity separate. Largest remaining phase is in-memory order, 57.469 s of the 76.964 s endpoint. These are not raw→index, v10, larger-input fit or scale timings.

`proc_904e`, pin `59ec7cd0354f7e5cae11227d61d03031c7780a0a`, `sandbox/finite-tiles64-100m-B7hSwU`. Replay `python3 -BE gates/overlap_finite_tiles64_study.py score gates/receipts/finite-tiles64-100m`. Score SHA `b61006c8d8af7eb2b90ae9fe38005e3ddc2185aa16d1414af7948dd301ebe825`.
