# Locality study: will a small corpus reproduce the cliff?

The failed 256 MiB arm motivates two separate hypotheses: a source-page refault cliff during comparator sort, and a separate SA-order preceding-byte read in emit. This first experiment isolates the former. It does not need the stranded finish or a live pressure run.

For a page reference, let D be the number of distinct other pages referenced since its previous access (infinity if cold). An ideal LRU of C pages misses exactly when D >= C. The transferable quantity is the distribution of D/P, where P is corpus pages, **not** simply corpus size or byte histogram. Repetition, phrase length tails, cell skew, and spatial clustering can break transfer. Under independent uniform references miss probability after warmup is 1-C/P; real comparator accesses arrive in correlated bursts and need not follow that curve.

`gates/overlap_locality_study.cpp` extracts the same w=8 trigger phrases and representatives on contiguous samples. For each cell window it regenerates descriptors in physical order, sorts by the finite tail and EOF flag, and simulates five fully associative LRUs on the operand pages touched by a scalar comparator. This is a comparator-only trace (cache retained between sorts), not a trace of Rust's sort or a prediction of actual OS reads. Batch cap is about n/24; oversized indivisible cells are counted, not hidden. Four spaced offsets at each of 128 kB, 512 kB and 2 MB test size vs distribution; 4 KiB pages are fixed. Compare normalized modeled page bytes (`4096 * misses / n`), both across offsets and between sizes at fixed C/P. The full-cache counter must be <= ceil(n/4096).

## Baseline result

Raw counters: `gates/receipts/overlap-locality-baseline.csv`. The full-cache check passed on all 12 samples; no oversized cell. Median modeled source miss bytes per input byte (four locations per size):

|input|50% cache|90% cache|100% cache|
|---:|---:|---:|---:|
|128 kB|7604|1150|1.02|
|512 kB|5885|854|1.00|
|2 MB|5306|810|1.00|

Small samples reproduce the **shape**, not an exact IO multiplier. At 2 MB and 90% cache the four traces range from 454x to 1161x, versus ~1x when every touched page fits. These are ideal-LRU page fills of 4096 bytes, not storage reads. Whole-tail staging copies ~38–48 input lengths in aggregate; the outlier at offset 91 MB needs 15.67 MB of key bytes in one window on just 2 MB input. Next arm varies a resident 4/8/16-byte prefix per descriptor at the same batch size, charges its incremental memory, counts its physical-order loads and only traces source comparisons on fallback. Holding source cache fixed isolates comparator benefit, not the effect of paying for prefixes under a fixed shared cap.

## Fixed source-cache prefix result

`gates/receipts/overlap-locality-prefix.csv` contains 36 traces. On the four 2 MB locations, median fractions of comparisons that needed a corpus byte after a 4/8/16-byte prefix were 41.15%, 11.48%, and 1.51%. At 90% source cache the median modeled miss bytes/input byte fell from 810 (no prefix) to 339, 173 and 22 respectively, including source reads to stage prefixes. At 50% cache: 5306 to 3063, 1313 and 142. The 16-byte arm consumed ~1.33 MB of additional prefix storage at 2 MB. The 4,194,304-pair product cap would require 64 MiB extra for 16 bytes/pair — most of the ~87 MiB file cache observed during the failed packed run. Thus the fixed-cache comparison alone cannot justify this choice in production.

Follow-up `fixed` mode reserves the full cap of prefix storage and debits rounded-up pages from each baseline file-cache allowance; other descriptor and metadata costs held constant. Combinations leaving no source pages are infeasible (`NA`). No product/finish or real disk reads in these arms.

## Matched total budget and implementation gate

`gates/receipts/overlap-locality-budget.csv`: all 12 zero-prefix controls exactly reproduce all five baseline miss counters. At 2 MB and 90% nominal source cache, median modeled amplification for prefixes 0/4/8/16 is 811/1292/1102/255 (nearest integer). The 16-byte arm gets just 114 of 489 source pages after the debit. At a 75% starting allowance, 0 vs 16 is 2216 vs 344; at 50%, 16 no longer fits. At 100%, no-prefix wins (~1 vs 210). Prefixes are not a global default and even 255x could blow the actual guard. Next experimental Rust gate: store an exact 16-byte padded prefix in each row descriptor in physical regeneration order; resolve equal prefixes using the original finite key including EOF, take group-head LCP locally when prefixes differ, retain continuation-rank merge. Opt-in `XSA_OVERLAP_INLINE_PREFIX=16`; default zero. Validate SA and all LCP rows before any real IO claim. Raw BWT predecessor reads still need separate treatment.

**Inline-16 correctness gate passed:** `ce729b8`, eight banked fixtures (`adv`, `mixed`, `period2`, `eofrun`, `docsep`, `internsep`, `single`, `r0`), 44,060,200 SA/LCP row pairs verified under 384 MiB no-swap (fixture verifier included). Receipt `gates/receipts/overlap-inline16-fixtures.csv`. No finished-product or IO claim. `gates/overlap_probe_pair.sh` isolates the real constructor + raw BWT predecessor lookup without the stuck finish: 0 vs 16 inline bytes, private cold 100 MB copies, identical 224 MiB no-swap producer-only cap, same descriptor cap, first 6M rows folded into a diagnostic digest, 15 GB per-arm attributed-read stop. This does not test total pipeline RAM, finish or full-container identity.

For copying entire representative tails into an arena, a nonfinal word with stride L contributes exactly L(L+1)/2 + 8L bytes across its L keys. Final words omit the 8L term. Thus one long trigger-free span can make naive staging quadratic even if the descriptor cap holds. Measure actual cumulative/peak key bytes; don't propose whole-tail staging without a byte budget and a refinement rule for long ties. An alternative is to stage fixed-length prefixes, refine only unresolved groups, and compute adjacent-key LCP at group boundaries. Prefix staging alone doesn't certify exact LCP or stop worst-case long-key rescans. Neither scheme removes the independent emit-side predecessor access.
