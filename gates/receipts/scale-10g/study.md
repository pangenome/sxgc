# 10 GB matched scaling decision

Selected: `packed-10g-l2-b1-t4-r1` ; <2×/byte target at both endpoints: True

## Isolated native dictionary sort

Requested/selected threads | sort s | speed vs 1 | process CPU s | mean CPUs during sort | peak RSS GiB | physical R/W bytes | post-clock digest s
---: | ---: | ---: | ---: | ---: | ---: | ---: | ---:
1 | 1079.267 | 1.000× | 1079.143 | 1.00 | 103.351 | 0/0 | 69.95
4 | 558.868 | 1.931× | 1347.793 | 2.41 | 103.352 | 0/0 | 88.07
2 | 923.223 | 1.169× | 1444.941 | 1.57 | 103.353 | 0/0 | 85.53

All 11,239,544,612 LE i64 entries hashed after each clock; one matching SHA `71a1bdfde1185e6f4a7d6cacc3210b4677c35e902af06956068a55c4324f3109`. Chose 4 (fewest within 5% of fastest with no extra sort IO vs one). Average CPUs are measured process CPU/wall, not an observed OpenMP team count.

## Full product

All four verified 10,000,000,001 raw rows, including 5,705,032,705 positions above u32, and all nine members of the 9,560,930,192-byte v8 container, SHA `26cfb2de0cccab8a8712a332956d58693dcbbb34373a9031a7bbaaa08cc504c9`. New worker source `8513cccb7d0ade1f4917752e3e8be33c3f7e49e3`, harness `076980318c30e576dd2a2729be07506731253121`. Resource grant: 200 GiB shared, 140 GiB arrays, 4 build CPUs, 2 finish threads, separate 14,000 MiB / 1 CPU verifier, no swap; counted scratch ≤20 GB.

10 GB private-cold v8, same raw/rows/reference, 4 build CPUs + 1 oracle. Worker timings are concurrent.

Profile | raw s | prepared s | dict s | emit s | join: sink / audit write s | counted GB | shared GiB | sink syscw
--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---:
packed-10g-bitmap | 10198.88 | 9824.95 | 783.60 | 8581.05 | 2790.91 / 1051.66 | 15.091 | 124.962 | 424,823,569
packed-10g-l4-b1-t4-r1 | 3592.91 | 3218.33 | 504.72 | 2340.99 | 523.73 / 1146.00 | 15.091 | 129.201 | 1,004,844
packed-10g-l2-b1-t4-r1 | 3652.56 | 3326.44 | 549.95 | 2419.12 | 230.56 / 1427.83 | 15.091 | 128.620 | 1,004,844
packed-10g-l1-b1-t4-r1 | 6829.41 | 6497.87 | 499.92 | 5637.53 | 212.06 / 1112.64 | 15.091 | 125.772 | 1,004,844

Per-arm accounting (decimal GB, CPU-seconds):

- `packed-10g-bitmap`: raw attributed worker R/W 34.044/48.637 GB; prepared 23.854/17.763 GB. Raw shared cgroup 22061.6 CPU-s (2.16 average CPUs); front + resident/sink/finish 14815.4 CPU-s; difference 7246.3, not necessarily all monitor.
  Separate raw oracle: 10205.11 s including upstream wait, 1330.11 CPU-s, 10.000/0.000 GB attributed R/W. After both services: post-service certificate interval not instrumented. Oracle wall overlaps construction; the later certification envelope is outside raw build wall.
  Cold raw front 369.85 s + cold handoff 4.08 s; prepared counted observed/reserved 13,215,620,732/13,224,009,340 B; whole raw directory sampled 41,003,700,224 B incl. input copies/output. Front and prepared phases have distinct scratch peaks.
  Frozen finish sweep 8568.83 s, including 6868.21 s piece waits; m5 EF 77.36 s, publish 39.45 s. Finish-attributed writes 17.748 GB, cancelled_write_bytes separately 0.211 GB. 830 m1 sync records covering 3,254,868,712 logical bytes; not physical bytes or seeks.
  Serial emission outside the two measured write calls: 4738.47 s (not an isolated compute benchmark).
- `packed-10g-l4-b1-t4-r1`: raw attributed worker R/W 33.216/51.124 GB; prepared 23.216/20.249 GB. Raw shared cgroup 11215.1 CPU-s (3.12 average CPUs); front + resident/sink/finish 11117.6 CPU-s; difference 97.5, not necessarily all monitor. Measured harness CPU 97.4 s.
  Separate raw oracle: 3597.72 s including upstream wait, 1188.73 CPU-s, 10.000/0.000 GB attributed R/W. After both services: post-service certificate envelope 6.702 s (mtime cross-check 6.702 s). Oracle wall overlaps construction; the later certification envelope is outside raw build wall.
  Cold raw front 369.68 s + cold handoff 4.90 s; prepared counted observed/reserved 13,221,912,188/13,230,300,796 B; whole raw directory sampled 42,582,151,168 B incl. input copies/output. Front and prepared phases have distinct scratch peaks.
  Frozen finish sweep 2317.52 s, including 771.04 s piece waits; m5 EF 65.07 s, publish 35.39 s. Finish-attributed writes 20.240 GB, cancelled_write_bytes separately 0.786 GB. 830 m1 sync records covering 3,254,868,712 logical bytes; not physical bytes or seeks.
  619,387 ordered ranges, 620,434 blocks, 193,413 mean payload B/block (one separate END), zero join row comparisons. Plan 46.71 s, packet waits 620.32 s (62.03 ns/owned row), downstream sink + verifier write calls 1669.72 s (71.3% of emit). These timers are inside emit, concurrent with lane computation. Wait includes successful receives; do not infer worker compute by subtraction or call this a cat baseline.
- `packed-10g-l2-b1-t4-r1`: raw attributed worker R/W 33.216/48.398 GB; prepared 23.216/17.523 GB. Raw shared cgroup 9573.5 CPU-s (2.62 average CPUs); front + resident/sink/finish 9484.3 CPU-s; difference 89.2, not necessarily all monitor. Measured harness CPU 89.2 s.
  Separate raw oracle: 3657.91 s including upstream wait, 1626.80 CPU-s, 10.000/0.000 GB attributed R/W. After both services: post-service certificate envelope 7.196 s (mtime cross-check 7.196 s). Oracle wall overlaps construction; the later certification envelope is outside raw build wall.
  Cold raw front 322.68 s + cold handoff 3.43 s; prepared counted observed/reserved 13,218,766,460/13,227,155,068 B; whole raw directory sampled 41,667,792,896 B incl. input copies/output. Front and prepared phases have distinct scratch peaks.
  Frozen finish sweep 2402.49 s, including 1101.49 s piece waits; m5 EF 65.58 s, publish 36.00 s. Finish-attributed writes 17.514 GB, cancelled_write_bytes separately 0.218 GB. 830 m1 sync records covering 3,254,868,712 logical bytes; not physical bytes or seeks.
  619,387 ordered ranges, 620,434 blocks, 193,413 mean payload B/block (one separate END), zero join row comparisons. Plan 62.40 s, packet waits 695.13 s (69.51 ns/owned row), downstream sink + verifier write calls 1658.39 s (68.6% of emit). These timers are inside emit, concurrent with lane computation. Wait includes successful receives; do not infer worker compute by subtraction or call this a cat baseline.
- `packed-10g-l1-b1-t4-r1`: raw attributed worker R/W 33.668/49.075 GB; prepared 23.668/18.204 GB. Raw shared cgroup 10852.8 CPU-s (1.59 average CPUs); front + resident/sink/finish 10699.3 CPU-s; difference 153.6, not necessarily all monitor. Measured harness CPU 153.5 s.
  Separate raw oracle: 6834.31 s including upstream wait, 1414.20 CPU-s, 10.000/0.000 GB attributed R/W. After both services: post-service certificate envelope 8.092 s (mtime cross-check 8.091 s). Oracle wall overlaps construction; the later certification envelope is outside raw build wall.
  Cold raw front 328.09 s + cold handoff 3.44 s; prepared counted observed/reserved 13,215,620,732/13,224,009,340 B; whole raw directory sampled 41,019,117,568 B incl. input copies/output. Front and prepared phases have distinct scratch peaks.
  Frozen finish sweep 5620.28 s, including 3856.06 s piece waits; m5 EF 66.92 s, publish 37.10 s. Finish-attributed writes 18.192 GB, cancelled_write_bytes separately 0.419 GB. 830 m1 sync records covering 3,254,868,712 logical bytes; not physical bytes or seeks.
  Serial emission outside the two measured write calls: 4312.83 s (not an isolated compute benchmark).

## Performance gates

Matched 1 GB reference: `packed-radix-4-batch`, 304.062 s raw, 269.634 s prepared. New-arm per-byte ratios use the same endpoint/verification.

Lanes | raw vs old serial | same-binary raw speed vs 1 lane | build CPU vs 1 lane | raw/prepared per-byte vs matched 1 GB | raw/prepared per-byte vs 3 GB | prepared R/W vs old serial | eligible | target
---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---:
4 | 0.352× | 1.901× | 1.033× | 1.182× / 1.194× | 0.646× / 0.613× | 0.9733× / 1.1400× | False | True
2 | 0.358× | 1.870× | 0.882× | 1.201× / 1.234× | 0.657× / 0.634× | 0.9733× / 0.9865× | True | True
1 | 0.670× | 1.000× | 1.000× | 2.246× / 2.410× | 1.228× / 1.239× | 0.9922× / 1.0249× | True | False

The historical comparison combines code, sink and sort changes and the previously documented frozen background difference. Only the three new arms isolate lane count. This selection applies to the tested resident v8 grant, not an arbitrary machine or a v10 100 GB build.
