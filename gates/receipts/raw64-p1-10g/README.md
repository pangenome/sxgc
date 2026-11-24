# 10 GB raw → wide lexical columns → independently certified P1 inverse

**138.361 s raw**, including **122.281 s raw → columns**, a 3.195 s private cold barrier and **12.880 s cold prepared keys → fsynced `inverse.u64`**. Separate 14.412 s private copy/hash/fsync/cold setup. This path used **no legacy-ID adapter**. The old columns were read only by the separate checker after the build. No full-byte suffix array, byte LCP/BWT stream or container. Not a raw-to-product speed claim.

All **234,472,723** dictionary records (raw bytes, length, EOF tag) and **318,164,488** text-order occurrence addresses matched the authenticated legacy representation. **211,844,700** new addresses exceed `UINT32_MAX` (8-byte headers; not the old lift's 203,696,244). There are **181,581,998 raw starts** above that limit. The written rank column is 2,545,315,904 B; the wide-key verifier proved all rank slots and strict suffix recurrences. Full rank SHA `b2adba88d6bd0bfee923928ba84cb4c9d7bc53eef17a84472fd6965bec8ec89f` matches the previous prepared pair. Native tape 12,880,853,672 B, SHA `5d8008a9d84aa9113a62a84ec92abc045409d4ae5f1d71860cbc51d0de112e07`; keys SHA `fb6e41baeae24a0d0a253a1e647262229d5f5ba14c27bccdd1b2be60e44a2274`. Column proof / rank proof / separate rank hash: 49.753 / 14.293 / 1.856 s.

| Raw front phase | Elapsed s |
|---|---:|
| Find starts (two scans) | 18.269 |
| Partition | 2.331 |
| Sort and deduplicate | 83.669 |
| Publish dictionary incl. sync/unmap | 14.851 |
| Scatter and publish keys incl. fsync | 3.140 |

**Parallelism decision.** 2,380 nonempty whole-key bins; largest and slowest order bin 518 (22,553,017 occurrences, 28.586 s). Order lane active sums 78.106 / 77.822 / 83.668 / 77.824 s. **317.420 active lane-seconds /(4 × 83.669 wall s) = 94.84%**, including stalls. Sort alone summed to 268.111 lane-s; labeling 49.310. The publisher's bin loop was 96.88% occupied. Do **not** split the largest bin just because it took 34.17% of phase wall. If each observed span cost stayed fixed, perfect scheduling alone could save at most `83.669 - 317.420/4 = 4.31 s` from this phase. That is not an available saving or CPU utilization measurement. To make the front fit 100 GB, prioritize live data and comparator locality; an internal team on top of the same four-CPU quota isn't free.

Build grant 32 GiB, four actual OpenMP workers, 20 GiB arrays front / 16 GiB arrays ranks, zero swap/OOM. Front planned arrays **17,648,530,632 B**; sampled native RSS high water 17,640,947,712 B. Shared build cgroup peak 30,612,901,888 B incl file cache (**the same cumulative peak across both stages**, not additive). Front attributed **10,000,003,072 B read / 15,426,256,896 B written**, 453.30 CPU-s. Rank read/write **2,545,319,936 / 2,545,324,032 B**, 41.35 CPU-s. Separate 14,000 MiB/one-CPU verifier service. Counted file bound **17,975,679,784 / 20,000,000,000 B**; highest sampled logical 17,971,666,569 B, includes columns and small artifacts, excludes only the private raw copy. No delegated device IO counters (`null`).

`proc_0ac1` exited 0 on published pin `f060b101d40b18a99f2dfdff4c8ef3ebf96b23a4`. Private receipt `sandbox/raw64-p1-10g-TyJqjC`. Replay: `python3 -BE gates/overlap_front64_study.py score gates/receipts/raw64-p1-10g`. Score SHA `518e22ba6052a15607e0b3961448881d67a3946cfccb51c7860f8066d9ccf25c`. The overnight [prelaunch refusal](../raw64-p1-prelaunch/) remains a separate real exit. No rerun, 100 GB grant or automatic continuation.
