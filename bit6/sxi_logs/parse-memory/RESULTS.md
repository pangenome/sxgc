# Measurements and scaling

All GB/TB values below are decimal. D includes dictionary separators and EOF.

| Input MB | w/p | distinct phrases | D bytes | D/n | parse entries | parse bytes | distinct length mean / p50 / p90 / p99 / max |
|---:|:---:|---:|---:|---:|---:|---:|:---|
| 100 | 10/100 | 927,820 | 108,616,070 | 1.086161 | 982,041 | 3,928,164 | 116.1 / 83.0 / 249.0 / 511.0 / 15783.0 |
| 100 | 5/100 | 1,014,798 | 103,865,307 | 1.038653 | 1,131,995 | 4,527,980 | 101.4 / 71.0 / 217.0 / 468.0 / 8434.0 |
| 100 | 20/100 | 960,879 | 118,734,778 | 1.187348 | 989,309 | 3,957,236 | 122.6 / 91.0 / 254.0 / 498.0 / 5829.0 |
| 100 | 3/5 | 4,694,258 | 66,160,319 | 0.661603 | 19,061,672 | 76,246,688 | 13.1 / 12.0 / 20.0 / 34.0 / 1254.0 |
| 500 | 10/100 | 4,576,455 | 540,625,242 | 1.081251 | 4,918,240 | 19,672,960 | 117.1 / 84.0 / 250.0 / 511.0 / 27896.0 |
| 500 | 5/100 | 4,980,178 | 516,437,828 | 1.032876 | 5,684,644 | 22,738,576 | 102.7 / 72.0 / 219.0 / 468.0 / 28190.0 |
| 500 | 20/100 | 4,784,783 | 591,990,295 | 1.183981 | 4,956,640 | 19,826,560 | 122.7 / 91.0 / 254.0 / 497.0 / 11108.0 |
| 500 | 3/5 | 18,436,516 | 275,754,953 | 0.551510 | 95,092,760 | 380,371,040 | 14.0 / 13.0 / 22.0 / 36.0 / 7957.0 |
| 1000 | 10/100 | 9,094,817 | 1,079,181,467 | 1.079182 | 9,839,683 | 39,358,732 | 117.7 / 85.0 / 251.0 / 512.0 / 27896.0 |
| 1000 | 5/100 | 9,874,523 | 1,030,454,026 | 1.030454 | 11,387,437 | 45,549,748 | 103.4 / 73.0 / 219.0 / 469.0 / 28190.0 |
| 1000 | 20/100 | 9,545,656 | 1,182,390,554 | 1.182391 | 9,912,682 | 39,650,728 | 122.9 / 91.0 / 254.0 / 497.0 / 19568.0 |
| 1000 | 3/5 | 33,212,825 | 509,355,289 | 0.509355 | 190,150,322 | 760,601,288 | 14.3 / 13.0 / 22.0 / 36.0 / 7957.0 |

Occurrence-weighted length distributions are recorded separately in `phrase-statistics.json`; each occurrence-count sum is checked against the parse length. Distinct-phrase quantiles above intentionally give each dictionary phrase one vote.


## Explicit constant-ratio projection to 1.31 TB

This is an extrapolation from a single prefix, **not** a fitted full-corpus deduplication estimate. Phrase diversity can change with source mixture and scale. It is a capacity warning, not a rigorous lower bound on unseen data.

| Calibration | projected D TB | hypothetical 24D TB | selected SA width | changed-path SA-packing live set TB (D + tmp + packed SA only) | projected phrase IDs (billions) |
|:---|---:|---:|---:|---:|---:|
| w=10, p=100 | 1.4137 | 33.9295 | 6 B | 21.2059 | 11.914 |
| w=5, p=100 | 1.3499 | 32.3975 | 6 B | 20.2484 | 12.936 |
| w=20, p=100 | 1.5489 | 37.1744 | 6 B | 23.2340 | 12.505 |
| w=3, p=5 | 0.6673 | 16.0141 | 5 B | 9.3416 | 43.509 |

The latter column excludes boundary indexes, L2 structures, and merge workspace. These scenarios all exceed 900 GB. For default w=10/p=100, the projected dictionary itself exceeds RAM and the 40-bit address range. Even an actual M5 implementation could not represent that projected D. Projected phrase counts also exceed existing uint32_t phrase IDs; lowering p is not a complete solution.

## Frontend byte gates and R

| Case | n including padding | R | R/n | baseline peak GB | memory peak GB |
|:---|---:|---:|---:|---:|---:|
| parse-100000000-w10-p100 | 100,000,001 | 38,647,349 | 0.386473 | 1.7467 | 1.2234 |
| parse-100000000-w5-p100 | 99,999,996 | 38,647,349 | 0.386474 | 1.7080 | 1.1715 |
| parse-100000000-w20-p100 | 100,000,011 | 38,647,349 | 0.386473 | 1.9030 | 1.3378 |

At the measured 100 MB R/n=0.38647, a constant-ratio projection gives ~506.3 billion runs for 1.31 TB: the RLE plus two 8-byte endpoint arrays alone would be about **10.13 TB** (20 bytes/run, ignoring headers and unusually long-run continuation records). This is another disk-capacity scenario to validate at larger scale, not a proven full-corpus R estimate. R is a property of the text/BWT, not a tunable parse compression factor. Window changes affect artificial padding and can perturb a few boundary runs; an intrinsic R reduction should not be inferred from changing w/p. `gates.jsonl` lists only completed comparisons as GATE_PASS.

## 466 redo projection

With D=18,611,972,670 symbols, removing L1 DA saves 4D=74.448 GB and releasing L1 ISA saves 5D=93.060 GB during all subsequent L2/merge phases. Applying those **resident-array** savings to the supplied 505.9 GB peak yields a conditional **338.4 GB** late-phase estimate. The changed L1 SA packing stage is about (1+8+5+0.16)D=263.5 GB; The retained completed run logs four-byte L1 LCP, so ISA/LCP/colex construction is about (1+5+5+4+0.16)D=282.2 GB plus phrase-ID storage. Its GNU time file actually reports 509,998,396 KiB = 522.238 GB, versus the task calibration of 505.9 GB. Using that measured RSS instead yields **354.7 GB** for the late phase. Budget **380 GB** for a redo experiment; neither 338.4 nor 354.7 is measured for the changed binary. This is not a whole-process upper bound: baseline peak phase, L2/merge allocations and allocator retention still require a full redo or richer telemetry. The read-only reference extracts are retained466.time and retained466-widths.log. Dictionary capacity savings are deliberately excluded from RSS savings.

## Semi-external SA probe

The standalone probe uses the first 9,999,977 dictionary bytes ending at a phrase boundary, M32 gSACA-K, and a 39,999,908-byte MAP_SHARED SA on nvme2n1. The in-memory and disk variants produced hash fa607383921036d5. With systemd MemoryMax=32M and MemorySwapMax=0, the disk probe completed at a recorded cgroup peak of 32 MiB.

- Memory: 2.399 s, maxRSS 49,152 KiB, no block I/O.

- Disk: 122.942 s (51.2x), 2,170 major faults, 491,704,320 input bytes and 2,046,705,664 output bytes (Linux block counters ×512); maxRSS 34,824 KiB, cgroup peak 32 MiB. Timing includes final synchronous writeback.

This demonstrates correctness-preserving OS paging for an SA workspace, **not an integrated, scalable external constructor**. Random-I/O amplification is substantial; do not linearly promise its runtime at TB scale. Default projected 64-bit temporary SA alone is ~11.3 TB, exceeding nvme2n1 space before input/final SA/ISA/LCP. A mapped temporary SA alone cannot close the pile gate. It needs a fundamentally different external construction/storage plan; no disk fallback is silently enabled in the frontend.

