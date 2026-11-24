# Direct finite-key ranges: 2.96× faster to independently verified EOF

One new 4-worker candidate, **93.489 s** vs **276.754 s** for the [archived 4-worker coarse/replay arm](../finite-emit64-100m/). Both start with cold private raw + certified inverse, same 192,001 total descriptor slots, four physical build cores, 512 MiB shared / 320 MiB arrays; same independent verifier binary on one other physical core / 140 MiB. No swap/OOM. All 100,000,504 row43 positions/LCPs/predecessors, final rank and narrow SHA `82747f11a9925bb4d89cc812410d98e411ce80b1767ee1ff4dc22e94e2cd874e` again matched. Not a new raw-to-index time or a repeated control.

| Measurement | Direct | Coarse |
|---|---:|---:|
| Post-index postings decoded / distinct offsets | **1.000×** | 14.458× |
| Average active ordering lanes (elapsed lane-s / order wall) | **3.164** | 1.298 |
| Refinement planning, s | 0.004 | 39.329 |
| Ordering wall, s | 54.461 | 194.752 |
| Gather / sort-only lane-s | 28.327 / 143.967 | 107.850 / 144.851 |
| Publication wall, s | 34.124 | 40.332 |
| Process CPU-s | 199.04 | 333.40 |
| Attributed producer read / write, B | 125,374,464 / 165,986,304 | 125,370,368 / 165,978,112 |

All 159,104 sampled keys produced 2,486 nonempty ranges at the fixed desired granularity (2,485 splitters). Largest 56,784 descriptors / 59,369 rows. No region needed replay or exact-class sort bypass. **85,107,405** finite groups, 3,102,452 multi-descriptor ties, 105,214 bitmap groups, largest tie 6,545, max occurrence mass 10,194. Complete publisher had 50,056 packets / 50,851 successful pipe writes including END/partial writes, zero row merge comparisons. Pipe traffic 1,200,006,048 B, not device writes. At 622 waves the publisher waited 32.952 s and spent 1.052 s in send. Workers spent 46.295 overlapping lane-s blocked on full rings out of 84.559 producing; 38.264 lane-s outside those waits. Lane clocks are not CPU or additive critical-path seconds. This points to testing finer disjoint emission tiles, not a faster syscall.

Passed predeclared ≤2U decodes / ≥2.5 active lanes plus wall ≤0.50× / R,W each ≤1.05×. Wall 0.33781×, CPU 0.5970×, IO 1.00003× / 1.00005×. **More logical scratch, not less:** posting file 165,974,592 vs 130,474,848 B, with 46,883,392 B packed subpage fragments; explicit writes 165,974,576 B. Almost the same attributed writes despite 27% more file bytes. Joint stage file upper incl inverse and 4 MiB margin **195,533,144** actual shape / **199,532,024** preflight / **200,001,006 B** hard limit. Sampled counted high 191,516,012 B; two private raw copies additional, whole allocated sample 391,815,168 B. Array reservation 247,981,272 B, producer sampled RSS 225,439,744 B, cgroup peak 427,917,312 B incl cache. Verifier 8.85 CPU-s, overlapping service 93.193 s. Device counters unavailable (`null`). Authenticated setup 0.435 s, cold barrier 0.012 s separately. The older resident serial 36.077 s and bounded shortest 105.232 s start from *other prepared columns* with different grants; this pair alone doesn't promote a product, a scale rate or a 100 GB run.

`proc_2def`, pin `ca74354eb2cd73e85e79dda5f9baa80a3d80cf98`, `sandbox/finite-direct64-100m-kzCqUS`. Offline replay `python3 -BE gates/overlap_finite_direct64_study.py score gates/receipts/finite-direct64-100m`; score SHA `ce28e5e539ecdb47757f37cfb2d1e4afda00e93a7ac11fb51c0269884368debd`.
