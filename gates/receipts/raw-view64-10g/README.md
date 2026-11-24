# 10 GB raw + certified inverse → verified aligned occurrence view

**3.47× faster to a ready view with four workers**, 3.55× for service including complete ordered-field attestation. Both builds used *only* the 10,000,000,000-byte raw text and its already certified 2,545,315,904-byte text-order inverse. No dictionary, reference keys, original parse IDs, per-byte suffix array or row spool. The native key witness was copied into the sandbox **after both arms** for the separate oracle. This endpoint starts after P1 ranking; it is neither raw-to-product nor a full suffix stream.

| Phase, s | 1 worker | 4 workers |
|---|---:|---:|
| Find triggers | 51.735 | 15.571 |
| Scatter positions/successors with unique rank claims | 18.765 | 6.903 |
| Validate word order, head ranges, canonical key addresses | 102.948 | 27.879 |
| Carry + extend byte PLCP | 84.963 | 24.152 |
| Build range minima | 3.172 | 0.880 |
| **Ready view** | **261.583** | **75.386** |
| Ordered-field proof | 110.270 | 28.456 |
| **Metered service** | **373.042** | **105.066** |
| Process CPU-s | 371.89 | 401.97 |

Same pin, 4 then 1, same four cores / four-CPU quota. Prepared read bytes **12,545,323,008** and writes **8,192 B** on both. Wall ratios ready/service **0.2882 / 0.2816**, CPU **1.0809×**. Bin load isn't evidence of speedup; this matched run *is*. The four PLCP stripes took 23.979–24.152 s. Of **68,354,682,151** common aligned prefix bytes, carry accounted for **63,931,736,304 (93.53%)**, **4,422,945,847** newly extended at both team sizes; max adjacent aligned LCP 699,513. These are logical prefix lengths, not physical IO. Independent direct comparisons did not reuse the carry routine.

All **318,164,488** aligned positions, adjacent LCPs, BWT predecessors, successor ranks and canonical lexical addresses matched the independent 64-shard roots (and fixed nonadjacent/END/self LCE checks). 234,472,723 distinct words, 9,129,290,112 owned distinct offsets, conceptual tape 12,880,853,672 B, 181,581,998 high text starts, 211,844,700 occurrences with native keys above u32. The oracle hashed raw-order fields from scratch, then dropped raw before verifying the original key/inverse relation; 278.591 s on one CPU / 14,000 MiB, 17,660,719,104 B read. Do not add that external certification time to producer-ready time.

20 GiB shared / 14 GiB arrays, zero swap/OOM. Four-worker array bound 14,586,197,448 B, sampled native RSS 14,581,932,032 B, cgroup peak 17,186,742,272 B incl cache; one-worker peak 16,733,487,104 B. Input/witness authenticated copy/cold setup 20.194 / 9.640 s. Counted simultaneous file bound **5,094,826,112 / 20,000,000,000 B**, high sample 5,090,725,994 B, producer only ~2.55 GB; immutable private raw copy additional. Device IO not delegated (`null`). At the measured 100 GB m the view models **145,744,266,192 B (135.73 GiB)** including raw and no vocabulary assumption. Still need finite-tail order, concurrent consumers, raw row verifier and the exact v10 finish.

`proc_2928`, pin `01b9bebfbcac0355fdb4ca8dc94497a7c30928bc`, `sandbox/raw-view64-10g-rsmbAB`. Replay `python3 -BE gates/overlap_raw_view64_study.py score gates/receipts/raw-view64-10g`; score SHA `27d05a8912317bfb29f6b8892886e44717b843b5005284aebb38bfeb4a39101a`. This does not retire any banked file or dispatch 100 GB.
