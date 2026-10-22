# FP-LCE — FINAL: the ladder works, the producer does not get faster, the leaf
# TARGET is the lever, and the 10 GB deep-target SIGSEGV was a stack overflow
# (found, root-caused, fixed)

Branch `fp-lce` (the lane's own commits) and `seg-triage` (the crash triage, the
fix, and this document). Base `pile-week-v1 @ 88a7cb0`. Design:
`tools/binbwt/FP_LCE_DESIGN.md`. The S sweep's own document:
`tools/binbwt/RESULTS_S_SWEEP.md` (both live on `fp-lce`).

**What this document is for.** Three charters reconcile against it and it is
written to be read that way: the **entropy-rule** charter's threshold `θ` (§7),
the **Lean** charter's exchange-rate statement (§7.3), and any future attempt at
a fingerprint structure (§2, §5). Every constant below is a counter from this
tree on this box, or an analytic expression of those counters; every wall carries
the box's load average, and every claim states which run it came from.

Short version:

* **The mechanism works exactly.** The checkpointed suffix-fingerprint ladder
  proposes the LCE, the comparator reads the two divergent bytes and orders by
  them, and `comparisons` AND `symbols` come out **bit-identical to the
  byte-walk arm** (`21,184,557,983` / `223,383,555,397` at 1 GB). The ladder
  answers *the same LCEs*; `fp_fallbacks=0` in every armed run (§5).
* **The forecast was right and it was optimistic.** Comparator window reads fall
  4.9% (`22.63e9 → 21.52e9`) and the producer wall **rises** 2.7 s / 1.9 s
  (43.3 s → 45.6 s, 2 pairs, same binary, alternating arms), i.e. inside the
  ±13-34% same-binary error bar — **the honest verdict is "no win", not "a
  regression"**. The `stride=1` control then explains it: at 800% footprint and
  `recon_bytes=0` the comparator reads **95 MB** of text where the walk reads
  **147 GB** — a 1,500x reduction in bytes read — and the leaf term is still
  **100 CPU-s slower**. What the ladder removed were *sequential* 64-byte window
  reads (prefetcher-served, ~61 symbols per read); what it added were *random*
  probes, one new cache line each. **Do not ship it**; the flag stays, default
  OFF, as the instrument that priced the term (§5, §6).
* **The census's ceiling was right, and it is the actionable result**: 99.33% of
  comparisons already cost the theoretical minimum (two random text reads), the
  addressable term is 6.4% of the leaf term (~1.3 s of a 25 s build at 1 GB), and
  the producer's dominant term is `log2(leaf) comparisons × 2 random reads` (§1).
* **The lever that IS there: the leaf target.** Re-pricing it on this machine
  (§6): at 1 GB the shipped 1 M default is the **worst of five points measured**;
  4 K and 64 K are tied-best at **−31.4% construction CPU / −32.4% build wall /
  −66.8% leaf CPU**, container **byte-identical at every point**.
* **The 10 GB deep-target SIGSEGV is a C-stack overflow in `bin_sort_rec`'s
  per-level recursion.** Reproduced on the UNMODIFIED base binary, backtraced
  under gdb (crash 13,121 frames down in `bin_tail_cmp`'s prologue), proved by
  raising the thread stack to 1 GB (completes; `depth_max=55,570`), and fixed by
  moving the DFS work list to the heap. Now green: the full oracle suite, the
  100 MB and 1 GB product gates **(and their 4 K deep-target variants)
  byte-identical**, the 10 GB / 64 K construction counters identical to the digit
  across the two binaries **at both the bare-binary cap and the pile regime's
  60 GiB cap (where the pre-fix binary still segfaults)**, and the 10 GB / 64 K
  run itself completing under the default 8 MB thread stack (§8).
  **One thing is NOT green and is not claimed: the 10 GB *product* run at a deep
  target**, which hits a separate piece-stream standstill (§8.6).

---

## 0. THE CONSTANTS — the calibration table

Everything the other charters need, in one place, with its provenance. **No
number here is a guess**; where two same-configuration runs disagree, both are
shown, because this box's same-binary CPU-second error bar is ±13-34% (measured
three ways: the S sweep's 16 K row run twice, and the three same-config 1 GB
census/A-B runs in §1 and §5).

### 0.1 The measured counters

| symbol | meaning | value | run |
|---|---|---|---|
| `n` | corpus bytes | 100,000,503 / 1,000,000,665 / 10,000,000,000 | `mergperf-{100m,1b}-snap.txt`, `pf-10gb.txt` |
| `cmp/row` | comparisons per row, leaf sorts | **21.18** (1 GB, t=1 M), **16.62** (1 GB, t=64 K), **11.71** (1 GB, t=4 K), **21.49** (10 GB, t=1 M), **16.58** (10 GB, t=64 K) | §6 |
| `sym/row` | symbols walked per row by the comparator | **223.4** (1 GB, t=1 M), **498.2** (10 GB, t=1 M), **428.7** (10 GB, t=64 K) | §1, §6 |
| `r_c` | 64-byte **window reads per comparison** | **1.0681** (1 GB, t=1 M), **1.2525** (10 GB, t=1 M), **1.1054** (1 GB, t=4 K) | `BIN_CMP_HIST` |
| `frac_le64` | comparisons that diverge inside ONE window | **99.33%** (1 GB), **99.15%** (10 GB) | `BIN_CMP_HIST` |
| `w` | cost of one window-read pair (memory-latency bound) | **57 ns** (`leaf_s 1296.1 / 22.63e9`), **73.5 ns** (`1663.2 / 22.63e9`) — band from two same-config runs | `FP_LCE_DESIGN.md` §0, §6 |
| `c_cmp` | cost of one comparison row | **78.5 ns** (1 GB, t=1 M), **47.1 ns** (1 GB, t=4 K), **47.3 ns** (10 GB, t=64 K) | `leaf_s / comparisons`, §7 |
| `c_lvl` | cost of one LEVEL row (count pass + scatter pass) | **82.7 ns** (1 GB, t=1 M), **106.6 ns** (1 GB, t=4 K), **112.8 ns** (10 GB, t=64 K) | `(build_cpu_s-leaf_s)/level_elems`, §7 |
| `r_l` | window reads per level row | **1.45 .. 1.98** (`c_lvl / w`) | §7.2 |
| `θ` | **the entropy rule's threshold** | **1.05 .. 2.39 bits**, target-dependent (§7) | §7 |

Counter means (same runs): `sym/cmp = 223.4e9/21.18e9 = 10.55` symbols per
comparison, and in the `>=64` tail `96.4e9 symbols / 1.58e9 window reads = 61
symbols per read`.

### 0.2 The structural constants of the tree (not measurements)

| symbol | value | where |
|---|---|---|
| `bin_window_slot_bytes` | 8 | `binbwt_slice.h` |
| `bin_group_slot_bytes` | 16 (arena + scratch pair) | `binbwt_slice.h` |
| `bin_fill_stage_bytes_per_thread` | 32 MiB | `binbwt_slice.h` |
| `bin_no_progress_sanity_bound` | 2^20 bytes | `binbwt_slice.h` |
| `bin_phase_headroom_pct` | 90 | `binbwt_slice.h` |
| the automatic leaf target | `clamp(n/(8*threads), 2^18, 2^20)` → 262,144 at 100 MB, 1,048,576 at 1 GB and 10 GB (64 threads) | `bin_order` |
| `BIN_CMP_HIST` | **fp-lce-only instrumentation, not on `main`** — the window-read counters that every `r_c`/`w` number here rests on live in this lane's commit `1de6387` | `binbwt_slice.h` |

---

## 1. THE CENSUS — the comparator is already at the miss floor

`BIN_CMP_HIST=1` (new in this lane, `binbwt_slice.h` `bin_cmp_hist`) counts, per
comparison, how many 64-byte windows the walk needed and where the walked symbols
live. 1 GB, `mergperf-1b-snap.txt`, n = 1,000,000,665, 64 threads, `--bin-probe`,
load 34-57:

```
1 GB construction:
  comparisons=21184557983  symbols=223383555397  leaf_s=1296.1 (CPU s)
  build_cpu_s=1522.3       build_s=25.35 (wall)  total_s=39.5

BIN_CMP_HIST comparisons=21184557983 windows=22626622708 multi_window=141448957
  lt4=10284607033 lt8=5883541015 lt16=4059414831 lt32=689655952 lt64=125890167
  lt128=54765004 lt256=39175945 lt1024=38769786 lt4096=5737958 ge4096=3000264
  frac_le64=0.993323 frac_multi=0.006677 windows_per_cmp=1.0681
  sym_lt4=23826487484 sym_lt8=37225124482 sym_lt16=45972119594
  sym_lt32=14400622957 sym_lt64=5574518652 sym_lt128=4913944998
  sym_lt256=7197650442 sym_lt1024=16905306605 sym_lt4096=10874152000
  sym_ge4096=56493628183 win_lt64=21043108964 win_ge64=1583513744
```

Rearranged into the two groups that matter:

| group | comparisons | share of cmp | symbols walked | share of sym | window reads | share of reads |
|---|---|---|---|---|---|---|
| LCE **< 64** (one window) | 21,041,108,964 | 99.33% | 127.03e9 | 56.9% | 21.04e9 | 93.0% |
| LCE **>= 64** (the tail) | 141,448,957 | 0.67% | 96.35e9 | 43.1% | 1.58e9 | 7.0% |

Three facts follow, and they decided the thesis before a line of the structure
was written:

**(F1) 99.33% of comparisons already cost the theoretical minimum: two random
text reads.** The order of two suffixes is decided by a byte pair that MUST be
read from the text (`bin_tail_cmp`). With 64-byte windows those two reads deliver
64 bytes each, and the divergence is inside the first one for 99.33% of
comparisons. **No LCE structure can reduce that pair of reads** — it can only
replace them with a different pair (table entries), and then it still has to read
the two divergent bytes.

**(F2) The tail is 43% of the symbols but 7% of the reads: it is already
amortized and sequential.** The `>=64` group walks 96.4e9 symbols in 1.58e9
window reads = 61 symbols per read; the deep walks read *contiguous* windows, so
the hardware prefetcher charges them roughly one miss per page, not one per
window.

**(F3) Therefore the entire addressable term is 1.44e9 extra window reads**
(`22.63e9 - 21.18e9`) = **6.4% of the leaf term** =
`1.44e9 × 57 ns = 82 CPU-s` = **6.3% of leaf_s = 5.4% of build_cpu_s ≈ 1.3 s of
the 25.35 s build wall** (ANALYTIC, linear in window reads). That 1.3 s is the
**CEILING** for any LCE structure that answers the whole tail for free.

**The 223n symbols were never the cost.** They are the lengths of the answers;
57% of them arrive inside a window the comparator had to read anyway, and 43%
arrive in a scan that charges per page. The producer's dominant term is and
remains `21.2 comparisons per row × 2 random reads` — i.e. `log2(leaf)`, the leaf
size, and text locality.

---

## 2. THE STRUCTURE — the options, with the space arithmetic

The comparator needs `LCE(i,j)` for **arbitrary, unrelated** `i` and `j`. That one
word — arbitrary — kills the cheap options:

**THE ALIGNMENT LEMMA (why block hashes cannot serve this comparator).** Let a
structure store fingerprints of fixed-size blocks whose boundaries sit at
positions `≡ c (mod G)`. Two positions `i, j` fall in *corresponding* blocks only
if `i ≡ j (mod G)`, and the residue difference is invariant under advancing both
by the same offset. A block-granular structure therefore compares only the `1/G`
of pairs that happen to be congruent, and no choice of `c`, no set of shifted
tables and no per-query re-anchoring repairs it. Alignment-free equality at
arbitrary offsets requires a fingerprint **at every position**.

| structure | build | query | space for alignment-free use | verdict |
|---|---|---|---|---|
| (a) doubling-period rolling fingerprints, per-position `H_k[p]`, `k=0..log n` | O(n log n) | O(log L) | `~8n log n` = **2500% of the corpus** | refused by the footprint rule by 2500x |
| (b) sync-set / content-defined sampling (PFP / `phrase_radix` lineage) | O(n) | O(#chunks in the span) | chunk hashes ~0.2-0.8%, but locating the next sync point needs `n` bits (12.5%) or a lookup table (measured 7.5e-5 coverage) | the only option that is both alignment-free and small, but its resolution IS the chunk and its sync cost is the read it was meant to save (`RESULTS_PHRASE_RADIX.md` §6) |
| (c) **checkpointed suffix fingerprints** (`slim_lce.hpp` lineage) — `H(q)` at every `q ≡ 0 (mod tau)`, arbitrary `H(p)` reconstructed in `<= tau` reads, window equality by `H(p) - B^L H(p+L)` | O(n), parallel | O(log L) probes | `8n/tau`: tau=64 → 12.5%, tau=256 → 3.1%, tau=1024 → 0.78% | **CHOSEN and BUILT** |

**The footgun that decides the parameter.** A probe costs `2 × ceil(tau/64)`
window reads and answers `log2` of a span, so the break-even against the walk is
`span/64 > 2·log2(span/tau)`: at `tau=64` it is near 256 bytes, and every doubling
of `tau` multiplies the probe cost by 2, moving break-even to `4·tau`. **There is
no `tau` in which the ladder wins everywhere, and none in which it wins by more
than a small factor** — (F2) restated in the structure's own cost model.

**Footprint at pile scale** (`n = 1.3079e12`): `tau=64 → 163 GB` (does not fit a
100 GB budget), `tau=256 → 41 GB`, `tau=1024 → 10.2 GB` = 7.8 GB/TB, the only
`tau` satisfying a single-digit-GB-per-TB rule — and §2's curve shows `tau=1024`
moves break-even to 4 KB spans, i.e. it buys nothing. **The footprint rule and
the cost curve are incompatible; that is the lane's structural result.**

### 2.1 THE EXACTNESS ANALYSIS (to PROB_CHI's standard, and where it differs)

With `B = 0x9e3779b185ebca87` (odd, the `slim_lce.hpp` base) and the
reverse-anchored suffix hash `H(i) = SUM_{k>=i} (T[k]+1)·B^(k-i) mod 2^64`,
`H(i) = (T[i]+1) + B·H(i+1)`, `H(n) = 0`, the length-`L` window hash is
`W(i,L) = H(i) - B^L·H(i+L)`. A collision needs a nonzero polynomial `D` of degree
`< L` with coefficients in `[-255,255]` to vanish mod `2^64`; taking its
highest-degree nonzero coefficient and using that `B` is odd gives (degree-free,
same bound at `L=27` and `L=1e5`):

```
p_test <= 511/2^64 < 2.8e-17
```

Union bound, worst case 34 tests per tail comparison: **1.35e-7 at 1 GB**,
**8.3e-6 EXTRAPOLATED at the pile**.

**WHERE THIS DIFFERS FROM PROB_CHI, AND IT IS NOT A SMALL DIFFERENCE.** PROB_CHI
can be wall-neutral *and certain* because its claims are checkable afterwards.
**This lane has no such checker**: its claim is not a fact about the text, it is an
*order*, and the only checker for "row A sorts before row B" is the byte walk it
was going to pay anyway. The comparator's one byte-pair read verifies the claim in
ONE direction only — `T[a+L] != T[b+L]` confirms; equality is an *under*-claim and
is caught (`fp_fallbacks`, then byte-walk fallback); an **over**-claim is
**not** detectable by that read (EC1 in PROB_CHI's taxonomy) and is caught only by
the gates. So the fp comparator is a **T2-class (hash-trust-only) consumer**, and
**`fp_fallbacks == 0` is a consistency counter, not a collision detector**.

---

## 3. WHAT LANDED

| file | what |
|---|---|
| `bit6/fp_lce.h` (new, ~300 lines) | `FpLce`: `H(i)` on a `stride`-checkpoint grid, `W(i,L)` windows, gallop+bisect `lce()`, one-pass parallel band build via the affine transfer `H(lo) = A + M·H(hi)`, loud refusal above the footprint limit |
| `bit6/binbwt_slice.h` comparator level 1 | `BIN_LCE_FP` (default OFF): the ladder proposes, the comparator reads the divergent pair exactly, an equal pair is a contradiction → `fp_fallbacks` + byte-walk fallback |
| `bit6/binbwt_slice.h` | `cmpWindows`/`cmpMultiWindow` counted unconditionally (the census's read term), the `FP_LCE_WORK` journal line, the `BIN_CMP_HIST` per-comparison walk histogram and per-bucket symbols/windows |
| `tools/binbwt/oracle.cpp` | `BINBWT_ORACLE_FP_PASS` (hash vs bytes, ladder vs brute LCE, bin_order ON vs OFF) |
| `tools/binbwt/fp_census.sh`, `fp_ab.sh`, `s_sweep.sh` | the census, the A/B and the target sweep, each printing the load average beside every wall |

Knobs: `BIN_LCE_FP` (0), `BIN_LCE_FP_STRIDE` (64), `BIN_LCE_FP_MIN` (256 symbols
the exact walk must certify before the ladder is consulted),
`BIN_LCE_FP_MAX_BYTES` (16 GiB footprint refusal).

---

## 4. THE GATES (fp-lce arm)

### 4.1 Brute-force order, both arms

```
BINBWT_ORACLE_FP_PASS cases=296 arms=2368 fixtures=148 modes=cyclic,dollar
  stride=4,16,64 min_jump=0,1,64,256 hash_checks=8266617 lce_checks=268116
  queries=61048 probes=1199656 recon_bytes=36291656 verified_bytes=119968
  jumped_symbols=31105424 fallbacks=0 full  (win_walk=1254040 win_fp=789040)
```

Every fixture's row order equals the brute-force SA with the ladder ON **and**
OFF, `comparisons`/`symbols` identical between arms, `fp_fallbacks == 0`,
`cmpWindows_fp <= cmpWindows_walk` (the non-vacuous arm: the ladder read 37% fewer
windows at fixture scale). Exercised at stride 4/16/64 and `min_jump` 0/1/64/256,
cyclic and dollar (cyclic refuses the ladder and runs the walk — asserted).

### 4.2 The banks — byte identity, both arms

```
100 MB  off: producer_rc=0 finish_rc=0 cmp_rc=0  chi=29716349  (bank xsa-pw3-chunk-100m)
100 MB  on : producer_rc=0 finish_rc=0 cmp_rc=0  chi=29716349
1 GB    off: producer_rc=0 finish_rc=0 cmp_rc=0  PILE_FINISH_DONE chi=283296933 n=1000000665 r=368036609
1 GB    on : producer_rc=0 finish_rc=0 cmp_rc=0  PILE_FINISH_DONE chi=283296933 n=1000000665 r=368036609
sha256 both 1 GB products == bank (pilev2-g2-1b/frag.sxi2):
  38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f
```

The product does not move, ON or OFF. (Producer+finish concurrent, as the product
runs them; that gate's producer wall 53.1 s OFF / 70.6 s ON is not an A/B — §5 is.)

---

## 5. THE A/B — the mechanism works and the producer gets SLOWER

1 GB, `mergperf-1b-snap.txt`, `--bin-probe`, 64 threads, `BIN_CMP_HIST=1` in BOTH
arms (the walk arm has no other way to report its windows; the cost is one
predictable branch, identical in both arms). `stride=64 min_jump=256`.

| pair | arm | wall s | build_s | total_s | build_cpu_s | leaf_s CPU | cmp_windows | comparisons | symbols |
|---|---|---|---|---|---|---|---|---|---|
| 1 | OFF | **42.87** | 29.14 | 41.69 | 1745.6 | 1524.4 | 22,626,622,708 | 21,184,557,983 | 223,383,555,397 |
| 1 | ON | **45.61** | 30.97 | 44.38 | 1816.7 | 1602.2 | 21,522,916,839 | 21,184,557,983 | 223,383,555,397 |
| 2 | OFF | **43.71** | 29.50 | 42.49 | 1757.9 | 1537.0 | 22,626,622,708 | 21,184,557,983 | 223,383,555,397 |
| 2 | ON | **45.59** | 31.43 | 44.36 | 1836.2 | 1617.1 | 21,522,916,839 | 21,184,557,983 | 223,383,555,397 |

Load average 52-99 across the four runs (printed per arm by `fp_ab.sh`).
**`comparisons` and `symbols` are identical to the digit, ON and OFF** — the
ladder answers the same LCEs. The only term that moved is the read count, and it
fell 4.9%.

`FP_LCE_WORK` (identical in both ON runs — the ladder is deterministic):

```
FP_LCE_WORK armed=1 stride=64 min_jump=256 checkpoints=15625011 table_bytes=125000088
  peak_transient_bytes=250000176 build_bytes=1000000665 build_s=0.312
  queries=47508008 probes=862432378 recon_bytes=57345582400 verified_bytes=95016016
  jumped_symbols=72063528732 fallbacks=0 fullspan=0
  text_bytes_read=57440598416 (recon+verified) cmp_windows=21522916839
```

Read: 47.5e6 comparisons entered the ladder, it certified **72.06e9 symbols**
without reading them (32% of the 223n), at a cost of 862e6 probes — 18 probes per
query, 2 checkpoint reconstructions each, 57.3e9 bytes folded. **The window reads
fell 4.9% and the wall rose 2.7 s / 1.9 s.**

### 5.1 The control that decides the diagnosis: `stride=1`

Same corpus, same binary, same box, one run (load 83):

```
FP_LCE_WORK armed=1 stride=1 min_jump=256 checkpoints=1000000666
  table_bytes=8000005328 peak_transient_bytes=16000010656 build_s=12.658
  queries=47508008 probes=1041360526 recon_bytes=0 verified_bytes=95016016
  jumped_symbols=72063528732 fallbacks=0 text_bytes_read=95016016
  cmp_windows=21522916839
  BIN_WORK: leaf_s=1626.360 build_cpu_s=1828.938 total_s=57.303   WALL=58.32 maxrss=24.7 GB
```

At `stride=1` the reconstruction is **free** (`recon_bytes=0`) and the comparator
reads **95 MB** of text where the walk reads **147 GB** (`1.15e9 window pairs ×
128 B`) — and the leaf term is **1626 CPU-s against the walk's 1524-1537**, i.e.
still slower, before the 12.7 s table build and 24.7 GB of RAM are paid.

**That is the finding, in one line: the ladder's probes are RANDOM accesses, and
what they replaced were SEQUENTIAL ones.** The census had already said it (the
`>=64` tail walks 96.4e9 symbols in 1.58e9 window reads — 61 symbols per read),
and this arm proves the cost model cannot be repaired by removing the
reconstruction, because the reconstruction was never the dominant term.

---

## 6. THE S SWEEP — the leaf target re-priced (the results that DID land)

The operator's dispatch, verbatim: *"100 MB + 1 GB at bin targets {1M (control),
256K, 64K, 16K, 4K}, report comparisons/row, leaf_s, symbols, build wall, and the
chi+sha banks at every point (the product must not move). The 1M default was
measured two architectures ago — re-price it on the current machine."*

**Headline: the shipped automatic leaf target is the worst point measured.** At
1 GB, target 4 K costs **−31.4% construction CPU**, **−32.4% build wall** and
**−66.8% leaf CPU** against the 1 M default, with the container **byte-identical
at every point** (chi + sha256 == bank, ten gate runs). And
`comparisons/row = log2(target) + 0.1 .. 1.2` — the leaf term is the logarithm and
nothing else.

Compact `*` = the shipped automatic rule at that scale.

### 6.1 1 GB (`mergperf-1b-snap.txt`, n = 1,000,000,665, 64 threads)

| target | cmp/row | sym/row | leaf_s (CPU s) | leaf_sorts | level_passes | level_elems | build_cpu_s | build_s | total_s | probe wall | depth_max |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **1,048,576** (shipped) | 21.18 | 223.4 | **1663.2** | 62,228 | 648 | 2.71e9 | **1887.2** | **31.12** | **44.85** | **46.24** | 193 |
| 262,144 | 18.95 | 202.1 | 1274.0 | 185,352 | 2,486 | 3.61e9 | 1627.8 | 26.34 | 39.20 | 40.48 | 520 |
| 65,536 | 16.62 | 182.7 | 888.5 | 564,699 | 10,180 | 4.54e9 | 1403.1 | 22.47 | 36.05 | 37.32 | 848 |
| 16,384 | 14.13 | 162.5 | 781.9 / 860.9¹ | 1,751,554 | 45,694 | 5.61e9 | 1828.5 / 1359.6¹ | 29.27 / 21.77¹ | 43.57 / 36.98¹ | 44.98 / 38.82¹ | 1618 |
| 4,096 | **11.71** | **140.4** | **551.9** | 5,023,936 | 255,542 | 6.97e9 | **1295.5** | **21.03** | 36.53 | 37.93 | 2398 |

¹ The 16 K row was run **twice, same binary, same config, counters identical to
the digit**: `build_cpu_s` 1828.5 at load ~61 and **1359.6** at load ~134. That is
this box's honest same-binary error bar (±34% CPU-seconds under load), and it is
why the counters arbitrate and the walls are labelled. With the rerun the curve is
monotone in `build_cpu_s` (1887 → 1628 → 1403 → 1360 → 1296).

### 6.2 100 MB (`mergperf-100m-snap.txt`, n = 100,000,503; the auto rule resolves to 262,144 here)

| target | cmp/row | sym/row | leaf_s | leaf_sorts | level_passes | build_cpu_s | build_s | total_s | depth_max |
|---|---|---|---|---|---|---|---|---|---|
| 0 / 262,144 (shipped) | 18.64 | 152.3 | 88.4 | 23,494 | 258 | 96.9 | 1.67 | 3.23 | 89 |
| 1,048,576 | 21.37 | 175.6 | 111.5 | 4,903 | 44 | 113.5 | 2.58 | 4.11 | 13 |
| 65,536 | 16.36 | 129.8 | 68.8 | 65,061 | 1,108 | 86.0 | 1.43 | 3.45 | 366 |
| 16,384 | 14.11 | 113.7 | 54.3 | 170,982 | 3,969 | 78.7 | 1.28 | 3.58 | 585 |
| 4,096 | 11.75 | 98.5 | **42.2** | 493,598 | 16,632 | **75.2** | 1.25 | 3.63 | 693 |

At 100 MB the construction is only ~3 s of a 3.2-4.1 s `total_s` (the sweep's two
corpus passes and the window-file write dominate), so the **CPU** column is the
signal: `build_cpu_s` still falls 33.7% (113.5 → 75.2) and leaf CPU 62%.

### 6.3 `comparisons/row` IS `log2(target)`

| target | log2 | 1 GB cmp/row | 100 MB cmp/row | 10 GB cmp/row | delta (1 GB) |
|---|---|---|---|---|---|
| 4,096 | 12 | 11.71 | 11.75 | — | **−0.29** |
| 16,384 | 14 | 14.13 | 14.11 | — | **+0.13** |
| 65,536 | 16 | 16.62 | 16.36 | **16.58** | **+0.62** |
| 262,144 | 18 | 18.95 | 18.64 | — | **+0.95** |
| 1,048,576 | 20 | 21.18 | 21.37 | **21.49** | **+1.18** |

The leaf sort does `~log2(m)` comparisons per row and the excess grows with the
leaf size (a bigger leaf shares fewer certified prefix bytes). **Design a stopping
rule against `cmp/row = log2(target) + ~1`, not against a constant.**

### 6.4 The trade the target makes

* **Leaf CPU falls monotonically with the target** (1663 → 552 CPU-s at 1 GB,
  3.0x) — the leaf sort is `log2(m)` walks of `m` rows.
* **Bucketing rises monotonically** (`level_passes` 648 → 255,542; `level_elems`
  2.71e9 → 6.97e9): every extra radix byte is one coded symbol per row per level,
  and the corpus's `0x20` subtree alone is 17% of the build.
* The **product's minimum is at 4 K**, not where the leaf term alone would put it
  — and 36.05 (64 K) vs 36.53 (4 K) is inside the noise: **64 K and 4 K are tied
  at 1 GB, and both beat 1 M by ~20% of `total_s` and ~31% of construction CPU.**
* **`leaf_elems` is always ~n** (every row ends in exactly one leaf): the leaf
  term's row count does not shrink with a smaller target, only its `log2(m)`.

### 6.5 THE 10 GB TAIL CENSUS — the deep escape grows with scale

Same binary, `pf-10gb.txt` (n = 10,000,000,000, 64 threads, `--bin-probe`,
`BIN_CMP_HIST=1`, wall 567 s / 9:27, load 46 → 101), automatic target:

```
BIN_WORK comparisons=214876750387 symbols=4982269168684 leaf_s=22818.184
  level_passes=6347 level_elems=41021242629 leaf_sorts=132688 leaf_elems=1659869192
  build_cpu_s=27290.882 total_s=554.560 depth_max=854 leaf_rows_max=1048416
BIN_CMP_HIST frac_le64=0.991535 frac_multi=0.008465 windows_per_cmp=1.2525
  win_ge64=56067328638  sym_ge4096=2942434569649
```

| quantity | 1 GB | 10 GB | ratio |
|---|---|---|---|
| comparisons / row | 21.18 | 21.49 | 1.01 |
| **symbols / row** (the "223n") | **223.4** | **498.2** | **2.23** |
| window reads / row | 22.63 | 26.91 | 1.19 |
| windows per comparison | 1.0681 | 1.2525 | 1.17 |
| comparisons escaping one window | 0.667% | **0.847%** | 1.27 |
| the `>=64` tail's share of WINDOW READS | 7.0% | **20.8%** | **2.97** |
| the `>=64` tail's share of SYMBOLS | 43.1% | **70.7%** | 1.64 |
| the `>=4096` bucket alone, share of SYMBOLS | 25.3% | **59.1%** | 2.34 |
| leaf CPU / build CPU | 88.1% | 83.6% | — |

**The deep tail grows with scale, and its SYMBOL share grows much faster than its
read share.** Per row, symbols grew 2.23x while comparisons grew 1.01x — the extra
symbols are not more comparisons, they are deeper ones. That **licenses a targeted
fingerprint over the deep spans only** (93.1e6 comparisons of 215e9 in the
`>=4096` bucket carry 59% of all symbols) and **does not license** the
corpus-wide ladder, which lost at `stride=64` **and** at `stride=1` (§5).

### 6.6 The one 10 GB deep-target measurement (enabled by §8's fix)

`pf-10gb.txt`, `--bin-target-size 65536`, 64 threads, `--bin-probe`, one window
(`--bin-live-cap` auto = n), 8 MB thread stack, load 21-42:

```
BIN_WORK target=65536 comparisons=165768247829 (16.58/row) symbols=4287126703536 (428.7/row)
  leaf_sorts=5682717 leaf_elems=9999143828 level_passes=170494 level_elems=68825298202
  depth_max=55570 leaf_rows_max=65536 no_progress_levels=666 emitted_rows=10000000000
  build_cpu_s=15603.464 build_s=249.368 total_s=320.119   WALL=333.00 maxrss=166 GB
```

`cmp/row` 16.58 = `log2(65536) + 0.58`, on the same line as the 1 GB and 100 MB
swallows; `sym/row` 428.7 sits between the 1 GB (223.4) and the 10 GB default
(498.2) — a *smaller* leaf walks fewer symbols per row because it starts from a
deeper certified depth. **`depth_max` = 55,570 is the number that broke §8.**

---

## 7. `θ` FOR THE ENTROPY RULE — the threshold, calibrated

The entropy-rule charter's rule is *take a radix level iff `H > θ`*, with
`θ = c_lvl / c_cmp` derived from this tree's own counters. It adopted
`θ = 1.5 bits` from a bracket `[1.18, 1.67]` measured on the **shipped 1 M
target**. The S sweep re-prices both terms at five targets and two scales, and the
result is not a bracket but a **function of the leaf target**.

### 7.1 The table (both terms from the SAME run, so `w` and the load cancel)

| arm | comparisons | leaf_s (CPU s) | level_elems | `c_cmp` = leaf_s/cmp | `c_lvl` = (build_cpu−leaf_s)/lvl_elems | **θ (bits)** |
|---|---|---|---|---|---|---|
| 1 GB, t = 1,048,576 (shipped) | 21,184,557,983 | 1663.240 | 2,709,186,546 | **78.51 ns** | **82.67 ns** | **1.053** |
| 1 GB, t = 262,144 | 18,952,481,902 | 1273.961 | 3,614,611,748 | 67.22 ns | 97.90 ns | 1.457 |
| 1 GB, t = 65,536 | 16,619,967,678 | 888.465 | 4,538,530,760 | 53.46 ns | 113.39 ns | 2.121 |
| 1 GB, t = 16,384 (run 1) | 14,132,446,571 | 781.856 | 5,605,897,525 | 55.32 ns | 186.70 ns | 3.375² |
| 1 GB, t = 16,384 (run 2) | 14,132,446,571 | 860.856 | 5,605,897,525 | 60.91 ns | 88.97 ns | 1.461² |
| 1 GB, t = 4,096 | 11,707,477,158 | 551.901 | 6,973,905,386 | **47.14 ns** | **106.62 ns** | **2.262** |
| 10 GB, t = 65,536 | 165,768,247,829 | 7838.831 | 68,825,298,202 | **47.29 ns** | **112.82 ns** | **2.386** |
| 100 MB, t = 262,144 (auto) | 1,864,294,192 | 88.172 | 217,784,532 | 47.30 ns | 39.55 ns | 0.836³ |
| 100 MB, t = 4,096 | 1,175,238,900 | 42.223 | 503,599,898 | 35.93 ns | 65.50 ns | 1.823³ |

² The 16 K row is the measured noise on `c_lvl` itself: `build_cpu_s` moved 26%
between two runs of the **same binary** (`level_passes`/`level_elems` identical to
the digit), so `c_lvl` — a difference of two large CPU sums — is the noisiest
quantity in this table and the two values bracket θ there. Where a decision is
made on θ, use the 1 GB / 10 GB rows and the *counters*, not a single wall.

³ At 100 MB the level term is only 9% of `build_cpu_s`, so `c_lvl` is a small
difference of two big numbers (±26 CPU-s over 2.18e8 rows = ±120 ns) and these
rows are **not** usable for θ. They are shown so nobody re-derives them and
believes a 0.33.

### 7.2 What the table says

1. **`θ ∈ [1.05, 2.39] bits` on the reliable rows**, and it **rises as the
   target falls** (the 16 K pair is the one row where `c_lvl`'s noise breaks the
   order): 1.05 at the shipped 1 M, 2.12-2.26 at 64 K-4 K, 2.39 at 10 GB/64 K.
2. **The mechanism is `c_cmp` falling, not `c_lvl` rising.** From worse to better
   targets, `c_cmp` falls 78.5 → 47.1 ns (a deeper leaf starts from a deeper
   certified depth, so its comparisons find the divergence sooner) while `c_lvl`
   stays in 83-113 ns. **A smaller leaf makes every comparison cheaper and every
   level no cheaper — so the entropy rule must get PICKIER as the target falls.**
3. **`θ = r_l / r_c` exactly** (`w` cancels, because both terms are the same kind
   of random 64-byte read): with `r_c` measured at 1.0681 (1 GB auto) and 1.2525
   (10 GB auto), the table's `c_lvl / w` gives **`r_l ∈ [1.45, 1.98]`** — i.e. a
   level really does pay ~2 window reads per row (count pass + scatter pass, as
   the design said: both passes code the same byte at the same address, and the
   node is too big to be cache-resident between them).
4. **The adopted 1.5 bits is correct for the shipped target and wrong for the
   target the S sweep recommends.** A charter that lowers the default to 64 K-4 K
   (the sweep's recommendation, −31% construction CPU) must raise `θ` to
   **~2.1-2.2 bits** in the same change, or it will take levels that do not pay.
   This is a *joint* calibration: target and θ are one decision, not two.

### 7.3 For the Lean charter

The exchange rate it must prove is `θ = r_l / r_c` — a ratio of **window reads per
row**, with the window cost `w` cancelling — and both sides are measured constants
of the same tree: `r_l` = 2 reads per level row (count + scatter), `r_c` =
1.07-1.25 reads per comparison row. The per-node rule is separable (each node's
level touches only its own rows and the leaves are disjoint), so the greedy
per-node argmin is optimal for a fixed tree shape — that is the statement the
proof needs, and it is unchanged by anything in this document. **Two Lean-relevant
caveats**, both measured here: (i) `r_c` is NOT 2 (the operator's analytic form
`θ = 1/2 = 0.5 bits`) because a comparison resolves its divergence inside ONE
window 99.33% of the time — the analytic 0.5 is only right where the scatter pass
is free (the cache-resident leaf, i.e. the in-leaf arm); (ii) `c_lvl` and `c_cmp`
are both CPU-second estimates from a loaded box with a ±26-34% band, so θ must be
quoted with the band and the rule must be evaluated on counters, not on one wall.

---

## 8. THE 10 GB DEEP-TARGET SIGSEGV — found, root-caused, fixed

### 8.1 The report (pre-existing, not this lane's)

`10 GB + --bin-target-size 65536` and `+ 4096` both died **SIGSEGV (rc=139)**
~4-5 min into the construction, after the fill (one window, an 80 GB window file);
the same corpus at the automatic target (1 M) was green. `RESULTS_S_SWEEP.md` §5
recorded it with the honest label *"a diagnosis from the depth ladder, not a
gdb-confirmed root cause"* and reported the 10 GB sweep rows as UNMEASURED.

### 8.2 The root cause, from the backtrace

Re-derived on the **UNMODIFIED** `main` tree at `88a7cb0` in a clean worktree —
same argv as the crash (`pf-10gb.txt`, n = 1e10, `--threads 64`, `--bin-probe`,
`--bin-target-size 65536`), a `-O2 -g` build, under gdb with the default 8 MB
thread stack:

```
Program received signal SIGSEGV
#0  bin_tail_cmp (certified=13121, stage=0x0)          binbwt_slice.h:429  (prologue)
#1  operator() (the leaf's sort comparator)            binbwt_slice.h:1450
#2  bin_sort_rec  m=2  targetSize=65536                binbwt_slice.h:1455
#3  bin_sort_rec (recursive)                           binbwt_slice.h:1571
#4 .. #59  bin_sort_rec (recursive, one frame per level)  binbwt_slice.h:1571
rsp 0x7ffda073bfb0   rip bin_tail_cmp+27
```

The fault is a **stack store in a function prologue 13,121 frames deep**, and the
depth is not an accident of the code: **`bin_sort_rec` recursed once per certified
byte**, and a node whose next byte is uniform (one bucket, `noProgressLevels`)
STILL consumes that byte — so a chain of such levels is exactly as long as the
shared prefix the node walks. `certified=13121` in the crashing comparator is the
depth of that chain, and the leaf at its bottom (m=2) is where the last frame
finally pushed the stack into the guard page.

The decisive confirmation, same binary, same corpus, same argv, **one change —
`ulimit -s 1048576`**:

```
RC=0  WALL=364.16  maxrss_kb=166108880
BIN_WORK target=65536 ... depth_max=55570 ... total_s=351.373 build_s=278.109
```

**It completes, and it reports `depth_max = 55,570`.** 55,570 frames at ~620 B
(`-O2`, two `std::vector` per level) is **34 MB of C stack against an 8 MB
thread stack** — the crash at ~13,121 frames is the arithmetic falling out.

So: **pre-existing, data-depth dependent, unbounded by design (the only guard is
`BIN_NO_PROGRESS_SANITY_BOUND` = 2^20 = 1,048,576 levels, which no stack
survives), and exactly the direction the successor charter's "smaller leaves"
walks into** (the depth ladder: 193 at 1 M → 520 → 848 → 1,618 → 2,398 at 4 K at
1 GB, and 55,570 at 10 GB/64 K).

### 8.3 The fix (`bit6/binbwt_slice.h:1413`)

**The DFS work list lives on the heap, not on the C stack** — an explicit
`std::vector<BinSortFrame>` of `(src, dst, m, depth, collect)`. The algorithm, the
visit ORDER (children pushed in descending bucket so they pop in ascending bucket,
exactly as the recursion visited them), every counter and every output byte are
unchanged; only the storage of the pending work moves. A `COLLECT` frame restores
the recursive call's buffer contract (a node that splits scatters into `dst` and
must leave its answer back in its own `src`). The three per-node scratch vectors
are hoisted and reused. A loud `require` refuses a work list longer than
`BIN_NO_PROGRESS_SANITY_BOUND` — the same verdict the periodic probe already
reaches for a node that cannot divide, and **the fail-loud path the tree did not
have**.

**NOT the fix: raising the thread stack.** The phase is budgeted by `RLIMIT_AS`
(`bin_derive_live_cap`/`bin_check_live_cap`), and 64 threads × 64 MB of stack
reservation is 4 GB of address space that budget would not know it had spent.

### 8.4 The gates on the fix

```
oracle (fixed binary, 21m56s, 6m16 user / 23m12 sys, load 62-93, 0 FATAL lines):
  BINBWT_ORACLE_PASS cases=36180 fixtures=134 modes=cyclic,dollar threads=1,3,8
    leaf=1,4,24 depth_cap=1,4,32 level_bytes=1,2 cap=1,7,all ... exact_brute_SA=1
  BINBWT_ORACLE_WINDOW_PASS cases=12864 ... exact_brute_SA=1
  BINBWT_ORACLE_REFUSAL_PASS refusals=5 modes=unlink,truncate,stale,nomagic,nodone
  BINBWT_ORACLE_STAGE_PASS cases=4824 counters_identical=1 exact_brute_SA=1 both_paths=1
  BINBWT_ORACLE_PERIODIC_PASS cases=4824 periodic_leaves=54 exact_brute_SA=1
  BINBWT_ORACLE_ADAPTIVE_PASS cases=4016 exact_brute_SA=1
  BINBWT_ORACLE_FREQ_PASS cases=6432 exact_brute_SA=1
  BINBWT_ORACLE_PHRASE_PASS cases=672 arms=672 verify_refusals=0

product banks (producer + fused finish concurrent, chi + sha256 vs bank):
  100 MB  producer_rc=0 finish_rc=0 cmp_rc=0  chi=29716349
          sha256=b4753899592c952542f7416ee9ce5836e19b99971afa74d02281493b3a289ba5  == bank
  1 GB    producer_rc=0 finish_rc=0 cmp_rc=0  chi=283296933 n=1000000665 r=368036609
          sha256=38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f  == bank

DEEP-TARGET byte identity (the chains are WHERE the bug was, so a deep target is
the target that matters; same product shape, --bin-target-size forced):
  100 MB / --bin-target-size 4096  DEEPTARGET_CMP=100m/4096 IDENTICAL
          chi=29716349 sha256=b4753899... depth_max=693
          comparisons=1175238900 leaf_sorts=493598 level_passes=16632
    1 GB / --bin-target-size 4096  DEEPTARGET_CMP=1b/4096 IDENTICAL
          chi=283296933 sha256=38f03eb4... depth_max=2398
          comparisons=11707477158 leaf_sorts=5023936 level_passes=255542
  (and those three counters are the fp-lce S-sweep rows' own numbers, to the
   digit: the fix is neutral against the PRE-fix binary at a deep target too)

OPTIMIZATION INDEPENDENCE — the whole set again at `-O3` (the tree's own repro
flag; the arms above are `-O2`), because a fix whose correctness depends on the
frame layout would be no fix at all:
  oracle                   ORACLE_O3_RC=0, every family exact_brute_SA=1, ZERO
                           FATAL lines (36,180 cases + window/refusal/stage/
                           periodic/adaptive/freq/phrase)
  100 MB / 1 GB products   cmp_rc=0, sha256 == bank (b4753899... / 38f03eb4...)
  100 MB / 4 K, 1 GB / 4 K DEEPTARGET_CMP = IDENTICAL
  10 GB / 64 K probe        RC=0 WALL=408.65 (pile cap), and its counters are
                           IDENTICAL to the -O2 run's field for field
                           (comparisons 165771478755, symbols 4286942284805,
                            level_passes 170494, depth_max 55570, ...)
```

**The counter equivalence at the crash point** — the crash configuration, two
binaries (the unmodified one under a 1 GB stack, and the fixed one under the
default 8 MB stack), 10 GB, `--bin-target-size 65536`:

| counter | unmodified + 1 GB stack | fixed + 8 MB stack |
|---|---|---|
| comparisons | 165,768,247,829 | **165,768,247,829** |
| symbols | 4,287,126,703,536 | **4,287,126,703,536** |
| level_passes | 170,494 | **170,494** |
| level_elems | 68,825,298,202 | **68,825,298,202** |
| leaf_sorts / leaf_elems | 5,682,717 / 9,999,143,828 | **identical** |
| depth_max / leaf_rows_max | 55,570 / 65,536 | **identical** |
| no_progress_levels | 666 | **666** |
| emitted_rows | 10,000,000,000 | **identical** |
| build_cpu_s / build_s / total_s | 17386.548 / 278.109 / 351.373 | 15603.464 / 249.368 / 320.119 |
| wall / maxrss | 364.16 s / 166,108,880 KB | 333.00 s / 166,049,352 KB |

**Every counter is identical to the digit; only the walls differ, and no wall
claim is made**: the two runs are one binary apart *and* load apart (the 1 GB
stack arm at load 62-93, the fixed arm at load 21-42), so the 11-12% CPU
difference is confounded and is not attributed to the fix. What the fix must be —
and is — is counter-neutral.

### 8.5 The PILE-REGIME before/after (the shape the product actually runs)

The crash was found at the bare-binary cap (`--bin-live-cap` auto = `n`, one
window). The product runs the pile regime instead — `RLIMIT_AS` 60 GiB, so the
auto cap is ~2.7e9 slots and a 10 GB corpus becomes FOUR windows. The same argv
(`pile-10b-snap.txt`, `--bin-target-size 65536`, 64 threads, `--bin-probe`,
`ulimit -s 8192`), the two binaries, at that cap:

```
PRE-FIX  (88a7cb0):   RC=139 Command terminated by signal 11   WALL=131.25
FIXED:                RC=0  WALL=384.71  maxrss=54.2 GB  windows=4
  comparisons=165771478755 symbols=4286942284805 level_passes=170494
  level_elems=68825392789 leaf_sorts=5682720 leaf_elems=9999161091
  depth_max=55570 leaf_rows_max=65536 no_progress_levels=666 emitted_rows=10000017258
  build_cpu_s=15223.488 build_s=314.535 total_s=373.093
```

The configuration the pile regime would actually use **never worked before this
fix** — not with the deep target. `depth_max = 55,570` is identical at both caps,
as it must be (it is the data's depth, not the cap's).

### 8.6 A SECOND WALL IN THE SAME CORNER, FOUND, ATTRIBUTED, NOT FIXED

The 10 GB deep-target **product** gate (producer + fused finish concurrent, the
bank's own shape) is **NOT reported as passed**, because it did not pass. It is
reported as UNMEASURED, exactly as the sweep's 10 GB rows were — and it is
attributed, which took four arms:

| arm | producer binary | target | threads p/f | piece window | pieces written | outcome |
|---|---|---|---|---|---|---|
| A | **fixed** | 65,536 | 64/96 | 1100 | **1,000, then frozen** | standstill, killed at 2 h 05 m |
| B | **pre-fix (88a7cb0)** | 65,536 | 64/96 | 1100 | n/a | `--bin-probe`: **SIGSEGV at 131 s** (8.5) — the product arm was not attempted, construction never got that far |
| C | fixed | 65,536 | 64/96 | **1,000,000** | **105,910 (all four windows built in ~9 min)** | producer completes; finish then advances at **0.13 bands/s** |
| D | fixed | **auto (shipped)** | 64/96 | 1100 | 2,199, then frozen | **standstill at the same place** |
| E | **pre-fix (88a7cb0)** | auto | 64/96 | 1100 | 2,199, then frozen | **identical standstill** — pieces 2199, band 65 at the same offset, same `chi_so_far` |
| F | fixed | auto | **96/96** | 1100 | 2,199, then frozen | **identical standstill** |

What arms A/C/D/E/F settle, and it is the point: **the product standstill reproduces
with the UNMODIFIED pre-fix binary, at the SHIPPED automatic target, byte for
byte the same way** (2,199 pieces, `band=65/152589`, `runs=4259836`,
`chi_so_far=3227709`). So it is **pre-existing, not this fix's, and not a
deep-target effect**. Thread count is not the trigger either (arm F).

The finish is the side that does not work: in arm C every piece existed on disc
(the producer had exited) and the finish STILL advanced at 0.13 bands/s while
**asleep ~98% of the time** (`repair_wall 30.7 s + lce_wall 4630 CPU-s (= 48 s
of wall at 96 threads) + advance_wall 7.7 s` against 82 minutes of elapsed) — so
it is blocked, not slow, and it is not piece-starved. The banked 10 GB product
(`pf2-10g-hyg0`) completed this phase in 30 min, and the one configuration
difference that remains untested is **the finish binary itself**: every gate in
this repository drives `sol7e-freshstart/pile_finish` (green at 100 MB and 1 GB,
here and in the fp-lce lane), while the bank was built by `pf2-build/pile_finish`
(rebuilt after the fact, so it cannot be re-run byte-identically). **That is
where this stops**, and it is handed over rather than guessed at: a 10 GB
producer↔finish product run with `sol7e-freshstart/pile_finish` does not complete
on this box, with or without this fix.

What the successor gets: the **construction** at a deep target is now correct and
fast (8.5), the **container** at a deep target is byte-identical at the scales
that gate (100 MB/4 K, 1 GB/4 K), and the 10 GB product run needs the finish-side
flow issue looked at first — with `CROSS_V2_PIECE_WINDOW=1000000` mandatory, since
1100 deadlocks the producer against the finish at this scale (arm A vs C).

---

## 9. What this document tells the other charters

1. **Entropy rule**: adopt `θ = r_l/r_c` with `r_l ≈ 2` and `r_c` measured
   (1.07 at 1 GB, 1.25 at 10 GB), i.e. **θ ≈ 1.6-1.9 in the clean-arithmetic
   form, 1.05-2.39 measured per target**, and **tie θ to the leaf target**
   (§7.2). `cmp/row = log2(target) + ~1` (§6.3).
2. **A fingerprint LCE is not the lever** — measured to a loss at both ends of
   its parameter box, with the cost model that explains it (§1, §5). Any future
   attempt must be *targeted* (deep spans only, §6.5) and must beat the
   sequential-vs-random asymmetry, not just hold a bigger symbol share.
3. **The leaf target is the lever, and it is free**: −31% construction CPU at
   1 GB, byte-identical container, at 64 K-4 K (§6). It needs the deep-chain
   behaviour fixed first — **it is now fixed, and measured at 10 GB (§8).**
4. **The pile's 10 GB construction at a deep target is now measurable for the
   first time**: `cmp/row` 16.58, `sym/row` 428.7, `depth_max` 55,570,
   `build_cpu_s` 15,603 (64 threads) vs 27,291 for the shipped 1 M default on
   the same corpus — **−43% construction CPU at 10 GB, matching the 1 GB −31%**
   (§6.6). The 10 GB sweep rows that `RESULTS_S_SWEEP.md` reported as UNMEASURED
   are now measured at 64 K. The 10 GB *product* at that target does NOT complete
   yet, for a finish-side reason that is pre-existing and attributed (§8.6) —
   **do not read "the target is free at 10 GB" as gated until that is closed.**

---

## 10. Residual risks

1. **`fp_fallbacks == 0` is a consistency counter, not a collision detector**
   (§2.1). An OVER-claim would be silent there; the detectors are the brute-force
   oracle and the byte-identity banks, both green. The union bound is 1.35e-7 at
   1 GB and 8.3e-6 at the pile (EXTRAPOLATED); this lane never needed to lean on it.
2. **The ladder only arms in dollar mode.** In cyclic mode the comparator's
   coordinates cross the doubled text, where this hash does not telescope; the arm
   prints `FP_LCE_OFF reason=cyclic_mode` and runs the walk. The product builds
   dollar mode, so nothing in the product route is untested — but a future cyclic
   consumer cannot use this structure as it stands.
3. **The 1 GB A/B's two pairs are 2.7 s and 1.9 s apart on a box at load 52-99**;
   the same-binary error bar on this machine is ±13-34%. The 4.9% window
   reduction is an exact counter, the +2.7 s wall is inside the weather — **the
   honest claim is "no win", not "a 6% regression"**. The `stride=1` arm's +15 s
   is far outside it.
4. **The un-cached regime (the pile's) was never measured with the ladder.** One
   accidental arm without `CROSS_POOL_SLOTS` took **63 min per run** instead of
   40 s (page-pool probes, `stage_s=2250`). Read locality matters *more* there, so
   the direction of the finding cannot reverse, but the magnitude is unmeasured.
5. **`θ`'s `c_lvl` is the noisiest constant here** (two same-binary runs of the
   16 K point differ 26% in `build_cpu_s`, §7.1 note 2). Anyone making a binary
   decision on a single wall is making it on noise; the counters are exact.
6. **100 MB's `c_lvl` is not usable** (§7.1 note 3) — the level term is 9% of a
   100 s CPU sum.
7. **The fix is counter-neutral and gated, but its *shape* is new**: the DFS is
   iterative, so the peak heap is now data-dependent (~40 B × depth; 2.2 MB at
   the 10 GB deep point, 42 MB at the 2^20 sanity bound). That is inside the
   `bin_derive_live_cap` arithmetic's noise, but it is a new term in a
   budgeted phase.
8. **A 10 GB product run with `sol7e-freshstart/pile_finish` does not complete on
   this box — with or without this fix, at the deep target AND at the shipped
   automatic target** (§8.6, arms A-F): the pre-fix binary standstills
   identically, so it is pre-existing, but it means **no 10 GB product byte
   identity is claimed here.** The byte identity that IS verified is 100 MB, 1 GB,
   100 MB/4 K and 1 GB/4 K (§8.4). Reproducing the banked 10 GB product needs the
   finish binary lineage re-established (`pf2-build/pile_finish` was rebuilt after
   its bank was made) and `CROSS_V2_PIECE_WINDOW=1000000`.
9. **The 10 GB deep-target counters are gated at 64 K only** — 16 K and 4 K at
   10 GB are unmeasured (the construction would be measured the same way, but it
   was not).
10. **`BIN_CMP_HIST` is this lane's instrumentation and does not exist on `main`**
   (§0.2): every `r_c`/`w` number here was produced on the fp-lce arm. The
   `seg-triage` branch carries the fix and this document, not the fp code.

---

## 11. Branch, commits, repro

`fp-lce`, from `pile-week-v1 @ 88a7cb0`:

| commit | what |
|---|---|
| `1de6387` | the census instrumentation + `fp_census.sh` + the 1 GB census |
| `97c0508` | PHASE 0: `FP_LCE_DESIGN.md` (structure comparison, alignment lemma, collision bound, footprint arithmetic) |
| `446c4d5` | PHASE 1+2: `fp_lce.h`, the comparator hook, the oracle family, the A/B harness |
| `2acea4c` | PHASE 3+4: the gates, the A/B, the `stride=1` control, and the results document |
| `3129fad` | the S sweep (100 MB + 1 GB, five targets), the 10 GB tail census, and the deep-target SIGSEGV report |

`seg-triage`, from `pile-week-v1 @ 88a7cb0`: the crash triage, the fix in
`bit6/binbwt_slice.h`, and this document.

```sh
# the producer and the oracle
c++ -O2 -std=c++17 -pthread bit6/cross_lcp_merge.cpp -latomic -o binbwt
c++ -O2 -std=c++17 -pthread tools/binbwt/oracle.cpp  -latomic -o oracle
./oracle /mnt/nvme2n1/erikg/oracle-scratch                    # 36,180 cases
# the census (fp-lce only) and the target sweep
bash tools/binbwt/fp_census.sh /mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-snap.txt c1b 64
bash tools/binbwt/s_sweep.sh 1b s1b 64 1048576 262144 65536 16384 4096
# the A/B, and the control that explains it
bash tools/binbwt/fp_ab.sh /mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-snap.txt ab1c 2 64 64 256
BIN_LCE_FP=1 BIN_LCE_FP_STRIDE=1 bash tools/binbwt/fp_ab.sh ... ab1c1 1 64 1 256
# the crash (unmodified base), and the proof that it is the stack
git worktree add /tmp/base88 88a7cb0 && c++ -O2 -g -std=c++17 -pthread \
  /tmp/base88/bit6/cross_lcp_merge.cpp -latomic -o /tmp/base88/binbwt-base
gdb -q -batch -ex run -ex 'bt 60' --args /tmp/base88/binbwt-base --binbwt \
  /mnt/nvme2n1/erikg/pf-10gb.txt --remap /mnt/nvme2n1/erikg/sol6c-gate100m/remap-256.bin \
  --threads 64 --bin-probe --bin-target-size 65536        # SIGSEGV, 13,121 frames
bash -c 'ulimit -s 1048576; <same argv>'                  # RC=0, depth_max=55570
```
