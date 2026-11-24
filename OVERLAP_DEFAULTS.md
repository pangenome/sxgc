# Rehearsal-host defaults for verified overlap construction

Use `python3 -B gates/overlap_defaults.py plan 1g` to replay the decision from the bank without opening a corpus. `run 1g` applies that profile to the **fixed rung**, from isolated measured source, private cold copies and a clean live-published `main`. It builds the tools; copied SHA lists describe the measured artifacts, not a compiler-independence claim. Do not run alongside another timing. No automatic large dispatch. Generic producers still default to serial; an arbitrary input has no measured resource admission or reference SHA.

## 1 GB: four ordered emission lanes, batched SXLS escapes

Measured profile **`packed-radix-4-batch`**. [Decision JSON](gates/receipts/radix-full-1g/default.json) SHA-256 `e89d7097cceef60c574340669db8211f063bca70d66b2f935b6233a9fd93b274`. [Complete bank, hash lists and score inputs](gates/receipts/radix-full-1g/). Source `d72ac25db741f98e4295bfdcf9af53c2682ed0a4`, cold raw input of 1,000,000,665 bytes, same wide-interface sort (internally narrow at this size), private tmpfs handoffs and bitmap, 20 GiB shared / 14 GiB arrays / four build CPUs / two finish threads, concurrent 1,400 MiB / one CPU row verifier. No swap. Raw and prepared end at the same finished v8 product, not at the row pipe. Verification compares all 1,000,000,666 rows and all 1,014,098,520 container bytes (SHA `b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3`).

| lanes/writer | raw s | prepared s | emit s | sink write calls | sink CPU s | build cgroup CPU s | peak shared GiB |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1 / scalar | 645.252 | 610.347 | 516.406 | 26,967,434 | 100.37 | 1,107.88 | 13.335 |
| 4 / scalar | 308.244 | 273.547 | 178.763 | 26,967,434 | 82.08 | 810.06 | 13.347 |
| **4 / batch** | **304.062** | **269.634** | **176.734** | **102,423** | **60.15** | **808.01** | **13.348** |

* **Emitter:** four lanes shortened raw wall to **0.478×** and emit to **0.346×** with post-front read/write bytes at **1.0000× / 0.9995×** of scalar. Disjoint ranges encode rows in final order; serial publication requires no per-row comparisons. The subsequent finish remains stateful. The bounded row buffers add 6,291,456 bytes to the live scratch reservation.
* **Writer:** at four lanes, batching cut sink calls to **0.00380×**, saving **26,865,011** calls, and sink CPU by 21.93 s. Raw wall was **0.986×**, *not* the predeclared ≤0.95× isolated product speed win. Adopt for the syscall/CPU saving with no meaningful time or IO regression, not for the 11× local microbenchmark. This is the best of the measured full 1 GB profiles on this grant, not an optimum across all possible lane or CPU counts.
* **Scratch and traffic:** all raw peaks 1,635,681,177 B / 2,000,001,330 B. At four lanes/batch, prepared sampled/reserved 1,375,646,741 / 1,400,704,518 B; raw worker read/write **3.497 / 5.058 GB**, post-front **2.497 / 1.797 GB**. Separate verifier 84.96 CPU-s, 1 GB read. IO attribution is not seek count. Charged build CPU includes concurrent components; do not add their wall clocks.
* **Remaining blocking:** the ordered writer spent 91.915 s in sink writes and 78.704 s in verifier writes *inside* its 176.734 s emit. This motivates testing the opt-in [shared-block audit tee](xsa/pfp/ASYNC_AUDIT.md); it is not in the measured build and has no product-speed verdict.

## 10 GB: two ordered emission lanes, four dictionary threads, batch + marker reclamation

Measured profile **`packed-10g-l2-b1-t4-r1`**. [Decision JSON](gates/receipts/scale-10g/default.json) SHA-256 `b503bf711dfeb87dddec6c26174d62e57b2f0bf4c52bbdf01de885441ea1468b`. [Full grid, product clocks, CPU, IO and scores](gates/receipts/scale-10g/study.md) · [experiment and recovery history](OVERLAP_10G_STUDY.md). `python3 -BE gates/overlap_defaults.py plan 10g` replays the selection without corpus access. For a separately scheduled **new** fixed-rung build on this grant only: `python3 -BE gates/overlap_defaults.py run 10g --deadline DEADLINE_UNIX`. Do not rerun an arm to reproduce the decision.

| Emission lanes | Raw s | Prepared s | Emit s | Build CPU-s | Prepared reads / writes vs historical serial | Eligible |
|---:|---:|---:|---:|---:|---|---|
| 4 | 3,592.912 | 3,218.329 | 2,340.988 | 11,215.1 | 0.9733× / **1.1400×** | No: writes >1.05× |
| **2** | **3,652.559** | **3,326.437** | **2,419.117** | **9,573.5** | **0.9733× / 0.9865×** | **Yes; fastest eligible** |
| 1 | 6,829.413 | 6,497.870 | 5,637.529 | 10,852.8 | 0.9922× / 1.0249× | Yes; slower |

* **Correctness:** each arm independently verified 10,000,000,001 raw SA/LCP/BWT rows and matched the nine-member 9,560,930,192-byte v8 container, SHA `26cfb2de0cccab8a8712a332956d58693dcbbb34373a9031a7bbaaa08cc504c9`. No full row or byte-level SA/LCP file. Pinned worker `8513ccc`, timed harness `0769803`; actual environment and original post-certificate scorer exits are documented per arm.
* **Speed:** the isolated native-wide sort grid chose four threads (558.868 s vs 1,079.267 s at one, same full SA digest, zero sort-attributed disk bytes). In the full products two emission lanes gave **1.870× raw speed vs one lane** with 11.8% less build CPU. Against the combined historical control the two-lane raw wall was 0.3581×; this comparison changes more than lane count. Versus the matched 1 GB default the selected raw/prepared walls per byte are **1.201× / 1.234×**, meeting the preferred <2× at both boundaries. Decimal raw throughput: 2.738 MB/s at 10 GB vs 3.289 MB/s at 1 GB. This does not forecast Pile throughput.
* **Resources:** 200 GiB shared / 140 GiB arrays / four build CPUs / two finish threads, independent 14,000 MiB / one CPU verifier, zero swap. Selected build peaked at **128.620 GiB**. Peak counted scratch **15,090,631,800 B / 20 GB**; prepared sampled/reserved 13,218,766,460 / 13,227,155,068 B incl. 3,145,728 B row buffers. Whole directory sampled 41,667,792,896 B incl. private inputs and product. Worker-attributed raw R/W 33.216/48.398 GB, prepared 23.216/17.523 GB. Device counters unavailable; no seek claim. Separate oracle 1,626.80 CPU-s / 10 GB read, 3,657.91 s **overlapping** construction. Post-service certification envelope 7.196 s, not a second row pass.

[100 GB](OVERLAP_100G_CONTRACT.md) changes dimensions and product contract. These decisions satisfy only the smaller-rung evidence prerequisite for the **scan-only** admission. Neither the 100 GB product nor the Pile is dispatched by this entry.
