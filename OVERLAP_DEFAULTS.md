# Rehearsal-host defaults for verified overlap construction

Use `python3 -B gates/overlap_defaults.py plan 1g` to replay the decision from the bank without opening a corpus. `run 1g` applies that profile to the **fixed rung**, from isolated measured source, private cold copies and a clean live-published `main`. It builds the tools; copied SHA lists describe the measured artifacts, not a compiler-independence claim. Do not run alongside another timing. No automatic large dispatch. Generic producers still default to serial; an arbitrary input has no measured resource admission or reference SHA.

## 1 GB: four ordered emission lanes, batched SXLS escapes

[Complete bank, hash lists and score inputs](gates/receipts/radix-full-1g/). Source `d72ac25db741f98e4295bfdcf9af53c2682ed0a4`, cold raw input of 1,000,000,665 bytes, same wide-interface sort (internally narrow at this size), private tmpfs handoffs and bitmap, 20 GiB shared / 14 GiB arrays / four build CPUs / two finish threads, concurrent 1,400 MiB / one CPU row verifier. No swap. Raw and prepared end at the same finished v8 product, not at the row pipe. Verification compares all 1,000,000,666 rows and all 1,014,098,520 container bytes (SHA `b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3`).

| lanes/writer | raw s | prepared s | emit s | sink write calls | sink CPU s | build cgroup CPU s | peak shared GiB |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1 / scalar | 645.252 | 610.347 | 516.406 | 26,967,434 | 100.37 | 1,107.88 | 13.335 |
| 4 / scalar | 308.244 | 273.547 | 178.763 | 26,967,434 | 82.08 | 810.06 | 13.347 |
| **4 / batch** | **304.062** | **269.634** | **176.734** | **102,423** | **60.15** | **808.01** | **13.348** |

* **Emitter:** four lanes shortened raw wall to **0.478×** and emit to **0.346×** with post-front read/write bytes at **1.0000× / 0.9995×** of scalar. Disjoint ranges encode rows in final order; serial publication requires no per-row comparisons. The subsequent finish remains stateful. The bounded row buffers add 6,291,456 bytes to the live scratch reservation.
* **Writer:** at four lanes, batching cut sink calls to **0.00380×**, saving **26,865,011** calls, and sink CPU by 21.93 s. Raw wall was **0.986×**, *not* the predeclared ≤0.95× isolated product speed win. Adopt for the syscall/CPU saving with no meaningful time or IO regression, not for the 11× local microbenchmark. This is the best of the measured full 1 GB profiles on this grant, not an optimum across all possible lane or CPU counts.
* **Scratch and traffic:** all raw peaks 1,635,681,177 B / 2,000,001,330 B. At four lanes/batch, prepared sampled/reserved 1,375,646,741 / 1,400,704,518 B; raw worker read/write **3.497 / 5.058 GB**, post-front **2.497 / 1.797 GB**. Separate verifier 84.96 CPU-s, 1 GB read. IO attribution is not seek count. Charged build CPU includes concurrent components; do not add their wall clocks.
* **Remaining blocking:** the ordered writer spent 91.915 s in sink writes and 78.704 s in verifier writes *inside* its 176.734 s emit. This motivates testing the opt-in [shared-block audit tee](xsa/pfp/ASYNC_AUDIT.md); it is not in the measured build and has no product-speed verdict.

## 10 GB and beyond

`plan 10g` requires the native-wide [dictionary grid and all three matched full lane arms](OVERLAP_10G_STUDY.md), plus a banked replayable selection; it refuses when these are missing. `run 10g --deadline UNIX_TIME` uses the verified 200 GiB / 140 GiB arrays, four build CPUs and separate 14,000 MiB / one CPU verifier grant, *not* these 1 GB time ratios. The [historical 10 GB control](gates/receipts/packed-serial-10g/) is a correct product but does not select lanes or threads. [100 GB](OVERLAP_100G_CONTRACT.md) changes product profile and dimensions: no 100 GB product dispatch or Pile permission through this entry point.
