# Four rolling epochs leave 120.44 MB of counted headroom at verified EOF

The [whole-file tiled control](../finite-tiles64-100m/) used a 165,974,592-byte posting. The [rolling producer](../../../xsa/pfp/FINITE_EPOCH64.md) kept one named inode, peaked at **49,896,400 B**, and reached independently verified EOF in **70.306 s** vs **76.964 s**. This is a space result, not evidence that more walks are intrinsically faster. Same cold raw + certified inverse, four build cores / 512 MiB shared / 320 MiB arrays, 192,001 total slots, separate single-core/140 MiB raw checker, zero swap. All **100,000,504** byte positions/LCPs/predecessors, final rank and original narrow SHA `82747f11a9925bb4d89cc812410d98e411ce80b1767ee1ff4dc22e94e2cd874e` matched. No row spool, ranker or finish.

| Epoch | Whole-key ranges | Byte rows (apart from global END) | Extent, bytes |
|---:|---:|---:|---:|
| 0 | [0,748) | 30,240,300 | 49,896,400 |
| 1 | [748,1492) | 29,900,612 | 49,781,624 |
| 2 | [1492,2240) | 29,950,248 | 49,874,856 |
| 3 | [2240,2486) | 9,909,343 | 16,421,712 |

Exactly four encoding walks (381,738,064 distinct-offset visits) and **one post-index decode per offset**. Same 622 order waves, 63,177 output tiles, 63,655 packets, 85,107,405 finite groups and 6,545-descriptor largest tie. Producer hashed every complete post-fsync image before reuse (165,974,592 B read back, 0.157 s included in the timed service). Separate post-close proof read the entire frozen posting in order and reconstructed each epoch digest from its interior and tail extents. All four digests and full control SHA matched. The final 16,421,712 B image also matched byte-for-byte; the other three no longer occupy files. Separate proof 0.347 s / 0.314 CPU-s. No old file deletion.

* **Space:** 79,558,803 B preflight producer bound vs **79,454,952 B actual geometry** incl 25,364,248 B inverse + 4 MiB margin. **0.40635×** prior counted bound of 195,533,144 B. Reserved 120,442,203 B for other stages throughout; stage plus unused reservation 199,897,155 / 200,001,006 B. This is space *available* to a future finish, not measured joint consumption. Sampled counted high 75,499,052 B; separate raw copies and whole allocated high 275,533,824 B. Arrays upper 251,389,144 B; sampled producer RSS 175,443,968 B, cgroup peak 259,289,088 B vs 427,880,448 B for control.
* **Traffic/time:** accumulated extent stayed 165,974,592 B; **not** a 3.3× IO saving. Producer attributed reads 125,370,368 B both; writes 165,990,400 vs 165,986,304 B. Device counters null. Producer CPU 208.67 vs 210.75 s; order wall 52.080 vs 57.469 s and publish wall 13.487 vs 14.572 s. Checker 8.72 CPU-s, 69.978 overlapping service seconds; peak 115,720,192 B. Setup 0.582 s, private cold barrier 0.012 s separately.
* Met the predeclared ≤0.41× counted producer bound and ≥1.20N reserve, wall ≤1.30×, CPU ≤1.25× and R/W each ≤1.05×. Only one matched comparison. No raw→product time, joint v10 fit or scale admission. The cached sort miss was not the control and was not rerun.

`proc_9060`, pin `2357995f2d2ee828a74a7f37f212a0f3308aac90`, `sandbox/finite-epoch64-100m-7Br6Ro`. Offline replay `python3 -BE gates/overlap_finite_epoch64_study.py score gates/receipts/finite-epoch64-100m`. Score SHA `fc87f879608719e016221b6a383b2079e09d158885be494e09f90a2cafbc7cab`.
