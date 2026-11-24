# Real 64-bit P1 ranks: 2.90× prepared speedup at four workers

The 10 GB text has **318,164,488** parse tokens. The compatibility adapter mapped all **234,472,723** lexical IDs to genuine packed word addresses, highest **11,942,962,749**; **203,696,244 occurrences exceeded UINT32_MAX**. No high-bit bias or invented alphabet. Each arm published a **2,545,315,904-byte `inverse.u64`**, same full SHA `b2adba88d6bd0bfee923928ba84cb4c9d7bc53eef17a84472fd6965bec8ec89f`. Separate verifier checked every one-based rank, uniqueness and strict `(original ID,successor rank)` recurrence, END=0. This is a durable P1 intermediate, not the byte suffix stream or v10 product.

| Prepared keys → fsynced ranks | One worker | Four workers |
|---|---:|---:|
| Metered wall, s | 33.759 | **11.652** |
| Native radix/load, s | 12.433 | 4.099 |
| Tied-prefix refinement incl. initial labeling, s | 13.824 | 3.888 |
| In-process certificate, s | 5.054 | 1.388 |
| Final column write + fsync, s | 1.067 | 1.059 |
| Native process CPU-s | 32.94 | 37.16 |
| Build cgroup peak, B | 12,765,814,784 | 12,771,889,152 |
| Attributed reads / writes, B | 2,545,319,936 / 2,545,332,224 | 2,545,319,936 / 2,545,328,128 |
| Separate written-file verifier, s | 10.455 | 10.651 |
| Separate post-verifier SHA, s | 1.610 | 1.606 |

Actual order 4 then 1. **0.3452× wall**, 1.1281× CPU, 1.0000× / 0.999998× prepared R/W; meets the predeclared 0.80/1.05 goal in one pair. No device IO (`null`) or statistical significance claim. Five radix digits, 375,961 biggest equal first key, 99,082,360 initially unresolved; 36,237,586 at depth 2, 3,686,911 at depth 64, 25,310 at 16,384; all resolved at 32,768. Same logical refinement on both teams. Not all wall intervals add to the service clock. The historical dense signed parse sort alone was not this endpoint.

Cold private-column copy/hash 20.855 s (192 MiB/1 CPU); wide key adapter 7.431 s, 11.20 CPU-s; separate copy/hash/cold for the second key inode 3.764 s. No raw text read or raw-to-product clock. The array grant was 16 GiB; 24 GiB shared / four CPUs incl. one-worker arm, supervisor-pinned same four distinct cores, no swap/OOM. Separate 14,000 MiB / one CPU verifiers. Simultaneous conservative file bound **15,765,130,940 / 20,000,000,000 B**, including private copied legacy columns, active key/rank payloads and 4 MiB margin; the generated tape copy retired before the second key copy. Originals, wide keys and both rank files remain private; the bank contains small receipts only.

`proc_63bb`, pin `cca298fe03903d7ab88088ce6d29e582c1bf803b`. Replay the verdict: `python3 -BE gates/overlap_parse64_study.py score gates/receipts/parse64-10g`. Score SHA `3e05977d201c7afda0487314df9a22bf736154759da687a2c40869660776b37d`. The [stage and 100 GB liveness model](../../../xsa/pfp/PARSE64.md) do not authorize a bigger run. No default product changed.
