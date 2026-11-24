# 100 MB compact bounded order vs resident: exact rows; lower peak, slower wall

`proc_ab4a`, source `e2f0c54`, private `bounded-component-100m-6e72c3pt`, exit 0. Reproduce with `python3 -BE gates/overlap_bounded_review.py`. [Measured inputs](inputs.json), [native/clock accounting](result.json), [offline review](review.json), and [independent input provenance](source.md). Tools are identified by SHA-256; no payloads or generated columns in git.

**Correctness passes at this rung. Performance does not.** Both arms independently built identical lexical tape and text-order IDs from a separate cold copy of the fixed 100,000,503-byte raw input. Each streamed **100,000,504** row43 triples including END over a 65,536-byte pipe to its own concurrent raw-suffix oracle. The two oracles checked exact adjacent order/LCP, predecessors, final rank and the banked narrow-LE row SHA `82747f11a9925bb4d89cc812410d98e411ce80b1767ee1ff4dc22e94e2cd874e`. This hash is **not** a container digest. No row spool, finish or product in either arm.

| 1 CPU per role, no swap | Resident control | Compact candidate |
|---|---:|---:|
| Producer cgroup / array grant (MiB) | 2,048 / 1,536 | 512 / 320 |
| Prep (both 192 MiB), s | 3.330 | 3.234 |
| Prepared columns → verified EOF, s | 36.077 | 177.769 |
| Continuous raw → verified EOF, s | 39.866 | 181.410 |
| Producer CPU-s | 30.03 | 168.14 |
| Producer cgroup peak, B | 1,378,205,696 | 516,653,056 |
| Sampled producer VmHWM, B | 1,224,605,696 | 364,142,592 |
| Producer attributed read / write, B | 140,394,496 / 0 | 140,394,496 / 0 |
| Overlapping verifier worker service wall / native CPU-s | 35.932 / 4.82 | 177.608 / 5.69 |

Same raw and prepared endpoints, **unequal producer RAM**. The compact producer used **0.375×** the cgroup peak but **5.599×** the producer CPU, **4.551× raw wall and 4.928× prepared wall**. Both wall ratios miss `<3×`, much less the preferred `<2×`. No attributed-IO saving. Each prep read/wrote 100,003,840 / 334,897,152 B and each separately capped 140 MiB verifier read/wrote 100,003,840 / 0 B. No OOM; device `io.stat` unavailable. The continuous raw clock includes column hash/eviction between stages, unlike the stage sums (39.407 / 181.004 s); external copy setup, compilation and source authentication are excluded. Verifier elapsed time is inside each prepared and raw clock.

**Space.** Preparation inline counted 163,411,115 B; with 4 MiB log/pipe reserve **167,605,419 / 200,001,006 B**. Cold columns 140,390,236 B; prepared reservation **144,584,540 B**. The private immutable raw copies and executables are additional, not counted scratch. No whole-directory allocated-byte high-water measurement here. The compact array plan charges parse / aligned / lists / emit at 192,505,036 / 259,514,468 / 301,977,663 / 318,358,023 B including the applicable 64 MiB arena. A planned array bound is not RSS.

**Why pursue a different admission algorithm, not more memory or a larger run?** `candidate/producer.log` reports a *complete dry order* to bound tie-window work and a second order to emit. **Each** used 2 census + 16 distribution scans of all 95,434,516 descriptors and 11,770,915,334 local radix digit reads. Together: **3,435,642,576** logical classifications and **23,541,830,668** digit reads (not device reads, not a phase clock). The actual largest finite tie was **6,545** in both arms, below the 8,192 cursor cap: *no* rank-window probes needed. The dry order did real work; its standalone seconds were not observed. Even the theoretical minimum oversized group here would charge `8193 × 388 × 46 = 146,228,664` against the **100,000,000** probe-work guard. Thus this certificate effectively had to prove *all* ties fit. At 10 GB the already measured largest finite tie (546,042) would by itself exceed that guard by orders of magnitude with an unchanged cap. Neither this result nor fixing its 100 MB dry-order cost is 1/10/100 GB clearance.
