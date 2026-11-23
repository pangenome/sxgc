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

For copying entire representative tails into an arena, a nonfinal word with stride L contributes exactly L(L+1)/2 + 8L bytes across its L keys. Final words omit the 8L term. Thus one long trigger-free span can make naive staging quadratic even if the descriptor cap holds. Measure actual cumulative/peak key bytes; don't propose whole-tail staging without a byte budget and a refinement rule for long ties. An alternative is to stage fixed-length prefixes, refine only unresolved groups, and compute adjacent-key LCP at group boundaries. Prefix staging alone doesn't certify exact LCP or stop worst-case long-key rescans. Neither scheme removes the independent emit-side predecessor access.
