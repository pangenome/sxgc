# In-place cached finite order: real sort gain, combined gate missed

`proc_c6ea` completed all **100,000,504** independently verified byte rows with the pinned narrow SHA and final rank. Same raw+inverse, four producer cores / 512 MiB shared / 320 MiB arrays, same 192,001 total sortable descriptors, three 2,048-row buffers/lane and separate one-core/140 MiB concurrent raw checker as the [tiled control](../finite-tiles64-100m/). No swap/OOM. The post-close independent comparison matched all 165,974,592 posting bytes and SHA `6285d881c12db13a56cee958091a7bb7f42fc8c197e2bd876fee1e3c46d2997c` (0.254 s separate). Same 2,486 bands, 622 waves, 63,177 tiles, 63,655 packets, 95,434,516 decoded descriptors, 85,107,405 finite groups and largest tie 6,545.

| Measurement | Tiled comparison sort | Cached sort |
|---|---:|---:|
| Prepared → verified EOF, s | 76.964 | **64.511** |
| Order wall, s | 57.469 | **36.239** |
| Gather / sort-only lane-s | 28.746 / 151.568 | 48.183 / **68.246** |
| Publish wall / wait / send, s | 14.572 / 9.651 / 4.450 | 22.811 / 17.140 / 5.019 |
| Unblocked expansion lanes | 3.005 | 2.988 |
| Producer CPU-s | 210.75 | 161.76 |
| Producer attributed R/W, B | 125,370,368 / 165,986,304 | identical |

Sort-only and ordering wall met ≤0.60× and ≤0.65× (0.4503× and 0.6306×). **Miss:** 193,478,499 post-gather sort span loads / U = **2.0273**, not ≤1.50; wall **0.8382×**, not ≤0.80. For a 34-bit payload / 30-bit projection: 87,401,125 descriptors used 6,006,651 small caches; 50,537,472 refill probes. 219,453 descriptors proved uniform early. Only 2,149,858 descriptors hit exact fallback, but its 27,769,951 comparisons required **55,539,902** span decodes (25.83 per fallback descriptor). These are logical accesses, not physical reads. No instrumented count for the old comparator. Gather and tile production took more elapsed lane-seconds in this observation despite the identical data/geometry; the receipts alone don't identify the cause. Unchanged row geometry doesn't imply identical cache/scheduling time. A useful candidate, not a promoted default; no retry and no threshold change.

Same 195,533,144 B peak counted bound incl inverse + margin (2N=200,001,006 B); counted sample 191,638,614 B and 391,901,184 B whole allocated incl two additional raw copies. Array upper 247,981,272 B, producer sampled native RSS 225,521,664 B, cgroup peak 427,745,280 B; device counters unavailable. Raw checker 9.20 CPU-s / 64.130 overlapping service seconds. Copy 0.522 s and private cold 0.014 s separate. Not a raw-input or product clock. Retry avoidance matters: only 4,467,862 B of counted headroom remains for other live files, less than the 12.5 MB finish bitmap alone.

Pin `5e772d966b59354a4ede2746398189806b9344cb`, `sandbox/finite-cache64-100m-tATbGc`. Offline `python3 -BE gates/overlap_finite_cache64_study.py score gates/receipts/finite-cache64-100m`. Score SHA `64e56a1b989dee8a174cd42e3be0dc2583189f1d5a9608a85d116893b5556072`.
