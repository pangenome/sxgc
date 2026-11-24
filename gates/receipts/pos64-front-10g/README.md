# 10 GB position front: spend 17.2% more wall for 20.7% less native RSS

Cold raw→both canonical wide columns, **one** new arm vs the [published four-worker reference](../raw64-p1-10g/). Same raw SHA, cores, CPU/memory quotas, output fsync and independent binary checking every tagged dictionary record and every text-order address against the authenticated legacy columns. Native 12,880,853,672 B dictionary SHA `5d8008a9d84aa9113a62a84ec92abc045409d4ae5f1d71860cbc51d0de112e07` and 2,545,315,904 B keys SHA `fb6e41baeae24a0d0a253a1e647262229d5f5ba14c27bccdd1b2be60e44a2274` identical, including 211,844,700 occurrences keyed above u32. No rank or raw baseline rerun. No prepared-rank, byte stream or product clock in this arm.

| | Reference | Position front |
|---|---:|---:|
| Raw→columns, s | 122.281 | **143.357** |
| Process CPU-s | 453.30 | 507.38 |
| Planned front arrays, B | 17,648,530,632 | 14,003,919,392 |
| Sampled native RSS high water, B | 17,640,947,712 | 13,996,244,992 |
| Shared front cgroup peak, B | 30,612,901,888 | 29,435,957,248 |
| Attributed reads / writes, B | 10,000,003,072 / 15,426,256,896 | 10,000,003,072 / 15,426,285,568 |

The **1.1724× wall, 0.7934× RSS and 1.0000× / 1.000002× IO** pass the predeclared memory-layout trade (≤1.50 / ≤0.85 / ≤1.05); **not** a speed or storage saving. The cgroup includes cache; only 3.84% lower there. Both builders had 32 GiB shared, 20 GiB array ceiling, four CPUs and zero swap/OOM. Private source copy/hash/cold 18.597 s; separate column verifier 53.145 s, 14,000 MiB/one CPU. Device IO unavailable (`null`). Conservative counted live file bound 15,430,363,880/20,000,000,000 B, peak sampled 15,426,380,069 B; private raw copy additional.

The new scan/partition/order/dictionary publication/key publication took 16.490 / 1.237 / 89.202 / 11.621 / **24.466 s**, vs 18.269 / 2.331 / 83.669 / 14.851 / **3.140 s**. Order lanes occupied 94.32% of the bin loop; shrinking the sort row did not make comparison faster. The main penalty was the post-raw scatter using bitmap ranks and reconstructed record lengths. At measured 100 GB m, this implementation's array bound incl raw is **139,923,425,640 B (130.31 GiB)**; evidence for a candidate front, not a joint grant, runtime guarantee or a reason to change the selected whole-product defaults. Focus next on eliminating downstream redundant lifetimes before tuning this pass again.

`proc_2a7d`, pin `fdfb5104a535003e9cdc92bcbfd767d770fdcf04`, private `sandbox/pos64-front-10g-XTNbSv`. Replay `python3 -BE gates/overlap_front64_pos_study.py score gates/receipts/pos64-front-10g`; SHA `c6b8b58ee87c0bbb3ce98c079358ce3f08034d78d59f792b4a625b9459474edd`.
