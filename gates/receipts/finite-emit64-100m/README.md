# Correct 100 MB wide routed byte stream; parallel performance gate **missed**

`proc_f7b8`, pinned `1563640503aac0fd2636170567a7b225c916c259`, exit 0. Four then one worker, same 512 MiB / 320 MiB arrays / four physical cores and capped four-CPU quota, independent 140 MiB/one CPU verifier on a fifth core. No swap/OOM, no final product. Both arms streamed **100,000,504** correct row43 positions, adjacent byte LCPs and predecessor symbols including END. Concurrent independent raw checks, final rank and narrow-row SHA `82747f11a9925bb4d89cc812410d98e411ce80b1767ee1ff4dc22e94e2cd874e` agreed. No expanded byte-row file. Input is **raw plus previously certified inverse**, not raw alone or the earlier lexical-column interface.

| 100,000,503 B | Four workers | One worker |
|---|---:|---:|
| Prepared inputs → independently verified EOF, s | **276.754** | **317.396** |
| Build aligned view, s | 0.618 | 1.881 |
| Posting census + layout + encode + sync, s | 1.133 | 2.926 |
| Local refinement planning, s | 39.329 | 39.132 |
| Order waves incl gathers, s | 194.752 | 234.273 |
| Publish waves incl expansion/backpressure, s | 40.332 | 38.467 |
| Native process CPU-s | 333.40 | 317.56 |
| Process read/write bytes | 125,370,368 / 165,978,112 | 125,374,464 / 136,753,152 |
| Cgroup peak / native sampled RSS, B | 323,260,416 / 160,870,400 | 313,421,824 / 146,120,704 |

Wall **0.8720×** (needed ≤0.80), reads 0.99997×, writes **1.2137×** (needed ≤1.05). Same row profile and exact proof is necessary but not sufficient. No scale or product promotion. The earlier serial bounded lexical-column stream did 105.232 s prepared/108.660 s raw, and the archived larger resident arm 36.077 s prepared/39.866 s raw; those inputs, algorithms and grants differ. There is no new raw-only clock here. Don't extrapolate this miss into a v10 ETA.

**Where the time went:** 577 coarse bands → 1,206 complete leaves under exactly 192,001 total descriptor slots. 303 local planning scans. Each arm decoded **1,379,799,143 / 95,434,516 = 14.46×U** postings after the index walks, just under the 16U guard. 85,107,405 finite groups, 3,102,452 multi-descriptor, 105,214 bitmap groups, max tie 6,545 descriptors / maximum group mass 10,194 rows. Four-worker order took 252.701 overlapping lane-s across 194.752 wall s, or **1.30 active lanes on average** (not 1.30 CPUs). 703 waves, 1.72 leaves/wave. Of lane time, gather 107.850 and local sort 144.851 s. One worker 99.353 / 134.914 s respectively. Refinement was serial; no rescue from adding publisher threads. Four-worker publisher waited 39.081 s and spent 0.871 s inside sending, for 1,200,006,048 bytes in 49,428 packets/49,652 successful writes incl END and partial writes. Verifier 8.96 CPU-s each; its overlapping service wall is not extra producer time. Four-worker populated posting payload 130,473,368 B vs 130,761,880 B for one: a *smaller logical payload did not produce fewer attributed writes*. Device IO undelegated (`null`).

**Lifetimes:** setup private raw copies and u32→u64 adapter 0.594 s; separate durable-rank proof/readback 0.136 s. Expanded column SHA `afc938c0a1e96a62a2a6159fa119546a919b3042f8820847c207890bc9cafe67`. The witnessed closed private u32 parse was retired before either build. Four/one counted bounds **160,033,400 / 160,321,232 B** including private inverse and 4 MiB margin; a priori **196,914,175 / 200,001,006 B**. Two private immutable raw copies additional. First posting file retired only after its workers exited and row proof succeeded. Array reservations 175,787,536 / 151,883,512 B. No changes to a preserved original. Replay `python3 -BE gates/overlap_finite_emit64_study.py score gates/receipts/finite-emit64-100m`; score SHA `45ceab98f853e3194280fb24a2fd7a9cdb5df8f2b6d47601251e233018f6c861`.
