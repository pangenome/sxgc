# Ordered asynchronous audit: full 1 GB products, matched hypothesis passes

`proc_d355`, `async-pair-1g-qci1PA`, source `75869a3`, **sync → async** under the independent timing lease. No replay of an earlier sweep arm. Both used the same new resident/sink binaries, frozen finish, actual matched worker controls (except the async opt-in on the second resident), 4 emitter lanes, batched sink, 1 dictionary thread, 2 finish threads, no marker reclaim. Conventional verified 1 GB product, **not** the bounded-component or byte-MSD fixtures.

| Matched observations | Sync | Async |
|---|---:|---:|
| Raw → container, s | 358.167 | **321.870** |
| Prepared → container, s | 328.711 | **292.387** |
| Emit, s | 244.657 | **208.567** |
| Shared build CPU-s | 1,055.24 | **917.24** |
| Producer / sink / finish CPU-s (post-front) | 630.26 / 75.71 / 275.70 | 492.76 / 69.08 / 288.70 |
| Peak build cgroup, GiB | 13.351 | 13.353 |
| Prepared attributed reads / writes, B | 2,496,507,904 / 1,796,820,992 | 2,496,520,192 / 1,796,780,032 |
| Raw attributed reads / writes, B | 3,496,607,744 / 5,057,720,320 | 3,496,521,728 / 5,057,675,264 |
| Overlapping verifier wall / CPU-s | 358.902 / 145.36 | 322.390 / 112.58 |
| Separate post-service certificate process, s | 0.77 | 0.76 |

**Verdict:** raw/prepared wall ratios **0.898661 / 0.889496**, both ≤0.95. Prepared read/write ratios **1.000005 / 0.999977**, shared CPU ratio **0.869223**, all ≤1.05. One same-binary pair, not statistical significance or a physical IO saving. Both checked 31,784,026 inverse ranks, all **1,000,000,666** SA/LCP/BWT rows against separate raw copies, and all nine members and the whole **1,014,098,520 B** container SHA `b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3`. Builder and independent verifier both exited 0 on each arm. This result **does not change** the published 1 GB/10 GB defaults. The original 1 GB batched observation was 304.062 / 269.634 s, actually shorter than this new async observation; it is not the control for this toggle. No 10 GB async timing or extrapolation.

**What the ordered join did.** Same 66,589 whole-key ranges and 66,593 ready data packets per arm (mean 180,199 B, largest range 103,397 rows); zero serial row comparisons. Async sent exactly **12,000,007,992 B to each branch**, including END; 8 shared blocks at peak, no additional row-buffer pool. The original synchronous publisher charged 11.499 s to ordered packet receive, 93.518 s to sink writes and 135.760 s to audit writes. Async: 81.255 s receive, 124.372 s sink, **0.000005 s for synchronous END only**, 0.069 s enqueue and 0.0013 s drain. The audit worker actually spent **131.810 s** writing/polling over a **206.044 s** overlapping lifetime. Do not sum those to publisher time or claim the full sync audit time was saved: observed net emit saving **36.090 s**. No sinkless `cat` baseline.

**Clocks and accounting.** Dedicated 192 MiB/one CPU preparation authenticated and evicted the two exclusive raw copies, remap and inverse file before construction; both zero resident pages by mincore. Copy/hash/cold 3.546 / 3.313 s (outside raw clock). Raw includes cold lexical front 28.732 / 28.741 s and the 0.718 / 0.736 s cold-column barrier. Both had 20 GiB shared / 14 GiB arrays / 4 build CPUs, separate 1,400 MiB/1 CPU verifier, zero swap and no OOM. Verifiers separately read 1,000,001,536 B each. Cgroup device IO unavailable (`null`). Counted raw peak **1,635,681,177 / 2,000,001,330 B**; prepared sampled / reserved **1,375,646,741 / 1,400,704,518 B** on each, incl. 6,291,456 B queue once. Async alone reserved 2,162,688 B for writer stack/control in emit arrays, not row scratch. Whole private dir sampled allocated 4,830,748,672 / 4,837,920,768 B, not counted scratch.

**Replay without worker, corpus, or product reads:** `python3 -BE gates/overlap_async_review.py gates/receipts/async-audit-1g`. `comparison.json` SHA `97a41868eaa5e8e781ae32470d0302faf025997df6eccdf1d5513203c98bf7fb`. `source.json` records the launch and small bank hashes. The two `source.json` files under the arms pin the original full sink/finish logs; the two corresponding banked logs per arm are explicitly selected lines. Originals and products remain in their private directory. Tests: `python3 -BE gates/overlap_async_pair_test.py` and `python3 -BE gates/overlap_async_review_test.py`.
