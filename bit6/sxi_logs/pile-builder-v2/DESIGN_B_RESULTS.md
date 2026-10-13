Design B measurement: single-threaded RAM-resident blocked treap on EPYC 7713.

| Input | Bytes | Seconds | u (us/byte) | Bytes/run | Rank mean/p99 (ns) |
|---|---:|---:|---:|---:|---:|
| random10 | 10,000,000 | 23.267 | 2.3267 | 70.44 | 287.1/590 |
| random50 | 50,000,000 | 187.193 | 3.7439 | 70.17 | 384.4/850 |
| random100 | 100,000,000 | 396.786 | 3.9679 | 70.32 | 385.3/720 |
| real100 | 100,000,503 | 296.914 | 2.9691 | 71.06 | 330.8/870 |
| periodic10 | 10,000,000 | 0.659 | 0.0659 | 2082.67 | 30.6/40 |

Verdict: real-text u=2.969125902e-6 seconds/byte, 9.00x the 0.33 us goal and 2.02x the 1.47 us construction-only ceiling. Random text is 7.05–12.02x off the 0.33 us goal; periodic text meets it. No evidence supports the desired whole-pile wall for this prototype. Not a lower bound on a better data structure. Real structure allocation 2,746,227,176 bytes (~27.46n), outside literal 1.6n budget. Periodic bytes/run is fixed one-node overhead divided by three runs.

Correctness: CHECK_PASS tests=10314. Online sentinel BWT equals direct suffix-sort oracle on every small correctness input. Maximal terminal-prefix interval conversion equals direct cyclic order, with ascending-position ties; exact position order implies exact predecessor characters and run endpoints. Proof and test categories in NOTES.md. Large timing inputs were not brute-force sorted; production RepairStream was not run. Logical tagged padding is required for bytes 0/5.

Performance limits: only construction timed; no conversion, finish, input loading, or disk index. Real snapshot used raw byte ordering, not production involution remap. Cross-block equal segments remain physical fragments, coalesced logically on emission; blocks do not repack after shrinking. Allocator overhead excluded from bytes/run. Hardware cache counters denied; all five runs had zero major faults and zero swaps. A disk-backed index adds dependent random I/O and quota/cache pressure, likely worsening u unless access design changes. Per-byte q recurrence is serial; no trivial parallel version attempted.

Artifacts: proto.cpp, bench, check, check.log, results.json, NOTES.md, per-input .log/.time, perf.err. Repository git status remained clean.
