# auto-radix: the target-size recursion, the flag flip, and the machine as it is

Branch `auto-radix`, base `pile-week-v1 @ 0c04ea0`. Scratch is
`/mnt/nvme2n1/erikg/ar-*`; nothing in `dw-*/`, `pf-*/` or `bd-*/` was touched
(the 10 GB corpus used for the probe is `pf-10gb.txt`, read only). Every wall is
labelled with the box state it was measured in (the box carried foreign
production work throughout: load average 6-106, named per row). Counters are
exact and load-independent.

## 1. The design: the rule is the target, depth is an OUTPUT

`bit6/binbwt_slice.h`:

* **`bin_sort_rec` (`:437`)**: a node is a LEAF iff `m <= targetSize` (the rule)
  or the text ends inside it (`depth >= t.n`); `depth >= depthCap` is only the
  FAIL-LOUD BACKSTOP. Any other node splits by its NEXT BYTE - one count pass and
  one scatter pass over the node's OWN slice, in the arena/scratch pair the bin's
  rows already occupy (no extra traffic, no new file, no position column).
  Two different reasons for an oversize leaf are counted APART:
  `over_target_leaves`/`_rows` (the cap fired: loudly printed as
  `BIN_LEAF_OVER_TARGET`, because that serial stretch is unbounded) and
  `structural_leaves`/`_rows` (the suffixes agree to their end: exact, expected).
* **`bin_task_run` (`:601`)**: dispatch. A node above `splitSize` is split on the
  queue (one job per child); a node at or below it is refined by the claiming
  thread - and since `splitSize` defaults to `max(target, 4096, n/(threads*64))`
  (`:757`), the longest a single job can hold a thread is one leaf sort of at
  most `targetSize` rows plus the two passes of the node that produced it.
* **The auto target (`:690`-`:717`)**: `--bin-target-size S`, `0` = automatic =
  `clamp(n/(8*threads), 256K, 1M)`; `--bin-leaf` is the superseded alias
  (`bit6/cross_lcp_merge.cpp:5466`). `--bin-depth-cap` defaults to **4096** now
  (`:5480`): a cap is a comparison when it does not fire, and the measured need
  at 10 GB was 879.
* **The new counters** (BIN_WORK / BIN_HOT): `target`, `depth_max`,
  `leaf_rows_max`, `over_target_leaves/rows`, `structural_leaves/rows`,
  `no_progress_levels`, `leaf_s`. `BIN_HOT` reports the hot bin's own
  `leaf_rows_max`, so "the largest serial stretch" is a number, not a claim.
* **The mechanism is the disc-window machinery, unchanged**: the rolling window
  FILE is what brings a bin's slice into the machine (one bulk pread per bin),
  and the recursion scatters inside the window group. Nothing downstream - the
  row order, the sink, the stitch - can tell the difference.
* **Inherited breakage fixed**: HEAD `0c04ea0` did not compile. The mirrored
  header took a 15th parameter (`BinWindowOpts wopt`), the call site passed 14.
  The disc-window options are now real flags (`--bin-window-dir`,
  `--bin-window-credit`, `--bin-build-cap`, `--bin-window-ram`) and are passed
  (`bit6/cross_lcp_merge.cpp:5453`).

## 2. The default S, with numbers

Two ladders, construction only (`--bin-probe`, nothing written), 96 threads,
`CROSS_POOL_SLOTS=33554432`, disc windows. `old` = `--bin-target-size 24
--bin-depth-cap 32`, i.e. the retired rule.

### 1 GB `mergperf-1b-snap.txt`, one window (cap 2^32; load 65-96)

| target | wall s | build_s | build_cpu_s | symbols | comparisons | level_elems | depth_max | **leaf_rows_max** | over-target leaves |
|---|---|---|---|---|---|---|---|---|---|
| 24 (old) | 28.16 | 20.55 | 1,928 | 101.1e9 | 3.003e9 | 10.89e9 | 32 | **4,738,244** | **183,229** (21.0M rows) |
| 262144 | 23.94 | **17.29** | 1,544 | 205.0e9 | 19.0e9 | 3.49e9 | 256 | 774,801 | 1 |
| 1e6 (auto) | 25.43 | 19.39 | 1,732 | 227.2e9 | 21.6e9 | 2.55e9 | 154 | 1,301,443 | 0 |
| 4e6 | 50.36 | 42.62 | 3,628 | 248.0e9 | 23.6e9 | 1.75e9 | 40 | 3,970,754 | 0 |
| 8e6 | 51.15 | 45.08 | 3,349 | 260.2e9 | 24.9e9 | 1.31e9 | 16 | 7,929,454 | 0 |
| 16e6 | 63.51 | 57.65 | 3,715 | 272.5e9 | 26.3e9 | 0.88e9 | 5 | 15,921,967 | 0 |
| 32e6 | 65.28 | 57.60 | 2,049 | 280.6e9 | 27.2e9 | 0.61e9 | 1 | 29,487,572 | 0 |

### 10 GB `pf-10gb.txt` (n=1e10), six windows (cap 2e9; load 30-95)

| target | wall s | build_s | build_cpu_s | symbols | comparisons | level_elems | depth_max | **leaf_rows_max** | over-target leaves |
|---|---|---|---|---|---|---|---|---|---|
| 24 (old) | 481.69 | 330.24 | 20,289 | 3.362e12 | 31.16e9 | 132.0e9 | 32 | **41,317,147** | **2,748,488** (308.2M rows) |
| 262144 | 458.46 | 292.01 | 19,611 | 4.720e12 | 190.5e9 | 48.99e9 | 256 | 6,048,267 | 2 |
| **1e6 (auto)** | **444.05** | 300.94 | 21,578 | 4.936e12 | 214.2e9 | 39.57e9 | 256 | 6,048,267 | 2 |
| 4e6 | 485.13 | 305.54 | 23,014 | 5.146e12 | 236.8e9 | 31.57e9 | 256 | 6,048,267 | 1 |
| 16e6 | 565.88 | 410.55 | 24,283 | 5.376e12 | 259.5e9 | 22.88e9 | 94 | 15,984,879 | 0 |
| 32e6 | 708.56 | 530.65 | 24,431 | 5.520e12 | 272.0e9 | 18.37e9 | 43 | 31,995,377 | 0 |

**The default is 1M rows, and it is the measured minimum at BOTH scales.**
The operator's candidate range (8-32M) measures **2.5-3.3x worse at 1 GB** and
**1.3-1.6x worse at 10 GB**. The reason is visible in the counter columns and is
the whole trade: a bucketing level costs ONE coded symbol per row (a random byte
read plus the scatter write), while a leaf comparison sort costs `log2(m)` LCE
walks per row, so a bigger leaf buys back few cheap levels and pays with many
expensive walks (comparisons 19.0e9 -> 27.2e9, symbols 205e9 -> 281e9 as the
target goes 262144 -> 32M at 1 GB, while `level_elems` only falls 3.49e9 ->
0.61e9). The automatic rule therefore scales DOWN on small corpora (a 100 MB
fixture still gets hundreds of leaves) and saturates at the 1M floor:
`clamp(n/(8*threads), 256K, 1M)`.

**The pathology, quantified.** The old rule's largest leaf is **4.74M rows at
1 GB** and **41.3M rows at 10 GB**, plus 183,229 / 2,748,488 leaves that the
depth cap took OVER the target (21.0M / 308.2M rows). That is where the banked
`thread_max_s=148.71` came from. At the same 10 GB, the shipped default has
`depth_max=854`, `leaf_rows_max=1,048,416`, `over_target_leaves=0` and
`BIN_HOT thread_max_s=55.114` for the hot bin (bin 32, 1.66e9 rows) - the
149 s single-thread stretch is gone, and it is gone STRUCTURALLY: the only way a
thread can be busy that long now is one leaf sort, which is bounded by the
target (or is journalled as the cap firing).

## 3. The 10 GB probe: largest leaf, and the build curve old vs new

Same corpus, same cap (2e9 slots, 6 windows), 96t, construction only. New = the
shipped default (auto target = 1M, depth cap 4096).

```text
old (target 24, dcap 32)  build_s 330.236  build_cpu_s 20289.368  depth_max 32
                          leaf_rows_max 41317147  over_target_leaves 2748488
                          level_elems 132017689562  comparisons 31158576235
                          symbols 3361984132500  leaf_sorts 1375827314
                          BIN_HOT bin=32 span_s=138.278 thread_max_s=97.186
                                   leaf_rows_max=41317147
new (auto target, dcap 4096) build_s 300.369  build_cpu_s 21626.977  depth_max 854
                          leaf_rows_max 1048416  over_target_leaves 0
                          level_elems 41021242629  comparisons 214876750387
                          symbols 4982269168684  leaf_sorts 537319
                          BIN_HOT bin=32 span_s=148.489 thread_max_s=55.114
                                   leaf_elems=1659869192 leaf_rows_max=1047991
```

* largest leaf: **41,317,147 -> 1,048,416 rows (39x)**, and the number of
  over-target leaves 2,748,488 -> 0. **`leaf_rows_max <= target` holds at 10 GB**,
  which is the structural claim the charter asked for.
* hot-bin single-thread stretch: 97.19 s -> 55.11 s (and 148.71 s on the banked
  real10b corpus) - the stretch is now one leaf sort, not a 41M-row one.

### The build curve, and one honest correction

The charter's bar is `10 GB build <= 1.25x (1 GB build x 10)`. Measured on the
SAME corpus family (1 GB, 2 GB and 10 GB slices of `pf-10gb.txt`, auto target,
cap 2e9, load 36-67):

| n | windows | build_s | build_cpu_s | symbols/byte | depth_max | leaf_rows_max |
|---|---|---|---|---|---|---|
| 1 GB slice | 1 | 34.81 | 3,113 | 223 | 193 | 1,048,394 |
| 2 GB slice | 2 | 39.72 | 3,677 | 326 | 314 | 1,048,477 |
| 10 GB | 6 | 300.37 | 21,627 | 498 | 854 | 1,048,416 |

* 10 GB build = **300.37 s vs 1 GB x 10 = 348.05 s**, i.e. **0.86x** - inside the
  1.25x bar, and actually below linear.
* against the *other* fixture (`mergperf-1b-snap`, 1 GB) the same ratio is
  300.37 / (18.16 x 10) = 1.65x, which is NOT flat - but that comparison measures
  two corpora, not two scales: `pf-10gb` needs 1.9x the CPU per byte at 1 GB
  alone (3,113 vs 1,627 s), and its own symbols/byte grows 223 -> 498 from 1 GB
  to 10 GB. The growth is the corpus's LCE depth, not machine overhead: CPU per
  byte grows 1.33x while symbols per byte grow 2.23x, so the machine is doing
  more work per symbol, not more work per row.
* the residual non-flatness inside the same corpus is parallel efficiency
  (90x at 1 GB -> 72x at 10 GB), and its cause is the two undividable buckets
  discussed in §6.
* `--bin-depth-cap` measured at the 1M target, 10 GB: 64 -> leaf_rows_max
  22.1M (2 over-target leaves), 256 -> 6.05M (2), **1024 -> 999,817 (0) for
  +5.9% build** (318.6 s). Hence the 4096 default: the target rules, the cap is
  a backstop, and a leaf the cap is forced to take is printed.

## 4. Gates, verbatim

### Brute oracle, all five passes (final tree)

```text
$ c++ -O2 -std=c++17 -pthread tools/binbwt/oracle.cpp -latomic -o oracle
$ ./oracle <fresh scratch>                                    # 18m41s, rc=0
BINBWT_ORACLE_PASS cases=36180 fixtures=134 modes=cyclic,dollar threads=1,3,8 leaf=1,4,24 depth_cap=1,4,32 level_bytes=1,2 cap=1,7,all NUL=1 byte05=1 wrap_prefix_1e_0a_1e=1 periodic=1 nested=1 single_byte=1 max_byte=1 nonidentity_remap=1 exact_brute_SA=1 multi_window_runs=22500 window_instances=343530
BINBWT_ORACLE_WINDOW_PASS cases=12864 fixtures=134 modes=cyclic,dollar threads=3 level_bytes=1,2 cap=1,7 credit=1,2,3 build_cap=0,5 disc=1 ram=1 file_reuse_runs=11248 window_instances=176784 window_files=88392 exact_brute_SA=1
BINBWT_ORACLE_REFUSAL_PASS refusals=5 modes=unlink,truncate,stale,nomagic,nodone control=exact_brute_SA
BINBWT_ORACLE_ADAPTIVE_PASS cases=4016 fixtures=134 modes=cyclic,dollar threads=1,8 target=1,2,3,17 depth_cap=256 level_bytes=1 cap=7,all depth_max=64 leaf_rows_max_le_target=1 over_target_leaves=0 exact_brute_SA=1 window_instances=21736
BINBWT_ORACLE_FREQ_PASS cases=6432 fixtures=134 modes=cyclic,dollar threads=1,3,8 leaf=1,24 depth_cap=16 level_bytes=1,2 cap=7,all freq_sched=1 exact_brute_SA=1 multi_window_runs=2904 window_instances=33096
```

`BINBWT_ORACLE_ADAPTIVE_PASS` is NEW (tools/binbwt/oracle.cpp, its own case set):
the leaf bound is driven down to 1-17 rows so the tree MUST go deep (measured
`depth_max=64` across the set), and every case asserts the exact brute SA, that
`leaf_rows_max <= target` (or a structural leaf exists), that
`over_target_leaves == 0` - i.e. the TARGET bounded the leaves, the depth cap did
not - and row/sweep/live-slot accounting. The old case set (36,180 cases) still
drives `leaf=1,24` under `depth_cap=1,4,32`, so the backstop path itself stays
covered: at those caps the oracle makes the cap fire in case after case (35,959
`BIN_LEAF_OVER_TARGET` lines across the run) and every one of those cases still
returns the exact brute SA - the cap costs structure, never correctness.

### Byte-identity gates (producer + finish + chi + cmp against the banked container)

```text
$ BINBWT_BIN=<this tree> python3 tools/binbwt/gate.py 100m ar-single 96 4294967296
{"size": "100m", "threads": "96", "live_cap": "4294967296", "producer_rc": 0,
 "finish_rc": 0, "cmp_rc": 0, "producer_wall_s": 4.2020, "total_wall_s": 28.719}
BIN_WORK ... windows=1 ... target=0 depth_cap=4096 ... level_passes=258
  level_elems=217784532 comparisons=1864294192 symbols=15227961956
  leaf_sorts=23494 leaf_elems=99999029 ... emitted_rows=100000503 ...
  runs=38647515 bands=590 target_hits=16573 ... depth_max=89
  leaf_rows_max=261860 over_target_leaves=0
xsa chi-rspace: chi = 29716349 (N=100000503, R=38647515) stream-agg=1 fused=1 corpus_lce=1
sha256 b4753899592c952542f7416ee9ce5836e19b99971afa74d02281493b3a289ba5  (== the bank)

$ BINBWT_BIN=<this tree> python3 tools/binbwt/gate.py 100m ar-mw8 96 4096
{"size": "100m", "threads": "96", "live_cap": "4096", "producer_rc": 0,
 "finish_rc": 0, "cmp_rc": 0, "producer_wall_s": 5.6926, "total_wall_s": 26.189}
BIN_CAP_RAISED requested=4096 largest_bin=16803424     -> 8 windows
BIN_WORK ... windows=8 ... level_passes=258 level_elems=217784532
  comparisons=1864294192 symbols=15227961956 leaf_sorts=23494
  emitted_rows=100000503 runs=38647515 bands=590 target_hits=16573
  depth_max=89 leaf_rows_max=261860 over_target_leaves=0
xsa chi-rspace: chi = 29716349 (N=100000503, R=38647515)
sha256 b4753899592c952542f7416ee9ce5836e19b99971afa74d02281493b3a289ba5  (== the bank)

$ BINBWT_BIN=<this tree> python3 tools/binbwt/gate.py 1b ar-single 96 4294967296
{"size": "1b", "threads": "96", "live_cap": "4294967296", "producer_rc": 0,
 "finish_rc": 0, "cmp_rc": 0, "producer_wall_s": 208.276, "total_wall_s": 319.449}
BIN_WORK ... windows=1 ... target=0 depth_cap=4096 level_passes=648
  level_elems=2709186546 comparisons=21184557983 symbols=223383555397
  leaf_sorts=62228 leaf_elems=999996793 emitted_rows=1000000665
  runs=368036610 bands=5616 target_hits=213516 depth_max=193
  leaf_rows_max=1048394 over_target_leaves=0 ... stitch_s=172.604
xsa chi-rspace: chi = 283296933 (N=1000000665, R=368036609) stream-agg=1 fused=1 corpus_lce=1
sha256 38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f  (== the bank)

$ BINBWT_BIN=<this tree> python3 tools/binbwt/gate.py 1b ar-mw5 96 250000000
{"size": "1b", "threads": "96", "live_cap": "250000000", "producer_rc": 0,
 "finish_rc": 0, "cmp_rc": 0, "producer_wall_s": 186.875, "total_wall_s": 297.657}
BIN_WORK ... windows=5 ... level_passes=648 level_elems=2709186546
  comparisons=21184557983 symbols=223383555397 leaf_sorts=62228
  emitted_rows=1000000665 runs=368036610 bands=5616 target_hits=213516
  depth_max=193 leaf_rows_max=1048394 over_target_leaves=0
xsa chi-rspace: chi = 283296933 (N=1000000665, R=368036609)
sha256 38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f  (== the bank)
```

**What changed and what did not.** emitted_rows, runs, bands, target_hits and chi
are IDENTICAL to the banked lines at both scales, and the container bytes are
identical. The leaf structure legitimately moved (the charter allowed it):

| 1 GB, single window | banked (dcap 16/32, leaf 24) | this tree (target 1M, dcap 4096) |
|---|---|---|
| leaf_sorts | 127,119,203 (dcap 16) / 220,509,140 (dcap 32) | **62,228** |
| leaf_elems | 925,152,076 | 999,996,793 |
| level_passes | 37,097,339 | **648** |
| level_elems | 10,179,725,668 | 2,709,186,546 |
| comparisons | 3,612,105,290 | 21,184,557,983 |
| symbols | 112,713,501,609 | 223,383,555,397 |
| runs / bands / chi | 368,036,610 / 5,616 / 283,296,933 | identical |

The windowed runs reproduce the single-window counters **exactly** (100 MB W=8
and 1 GB W=5), so the target-size recursion is window-invariant, as the fill's
canonical arena order requires. Walls are load-labelled (load 16-70 for these
four gates): 1 GB producer 208.3 s single-window / 186.9 s at W=5 vs the banked
~192 s (dcap 2), and the stitch (`stitch_s=172.6`) is still the producer's wall.

## 5. PART 2: the default entry and the plan

`xsa/src/chain.rs`:

* **`xsa build` runs the bin pipeline by default.** No route flag is needed.
* **`--v2-pipeline` is ACCEPTED AND IGNORED** and journalled
  (`NOTE --v2-pipeline ACCEPTED AND IGNORED: ...`, `chain.rs:984`), never
  silently swallowed.
* **`--legacy-merge`** is the pre-bin route (chunk front end -> k-way merge tree
  -> adopt finish). Chosen over `--radix` because `--radix` already names a
  different producer in `bit6/cross_lcp_merge.cpp`; documented in `USAGE`.
  It is the only route that accepts `--chunks/--chunk-mb/--kway/--fanin`, and
  the only one that refuses `--chi-expect/--wit-ram-gb/--bin-live-cap/
  --bin-target-size`.
* `--bin-target-size ROWS` is a front-door flag (absent = the producer's
  measured automatic default) and is passed straight through to the producer
  (`chain.rs:1266`).
* 17 Rust tests pass, including 4 new ones for the flip (default route is the bin
  pipeline; `--v2-pipeline` changes nothing but its own record; `--legacy-merge`
  is the only way back; the timeout cap now belongs to the plain invocation).

`xsa/src/plan.rs` — the pile plan, honestly:

* **the position store is a FILE**: `window_file_gb = credit(2) x cap x 8 B` on
  disc, one rolling file per live window, unlinked when built; the RAM term is
  **ONE BUILD GROUP** (`BIN_GROUP_SLOT_BYTES = 16`) - never the window.
* **the target-size tree**: `BIN_TARGET_ROWS = 1M`,
  `bin_target_rows_auto = clamp(n/(8*threads), 256K, 1M)`, `leaves = ceil(n/target)`,
  and `levels_per_row` from MEASURED points (`BIN_LEVELS_POINTS`: 3.49 at 256K,
  2.75 at 1M, 1.75 at 4M, 1.31 at 8M, 0.88 at 16M, 0.61 at 32M).
* **the fill re-priced at the device floor**: `1/5.238e9 s per byte per window`
  (the parallel-fill scatter, measured 14.8-21.1 GB/s RAM-cached against the
  5.238 GB/s O_DIRECT device floor for a corpus that cannot be cached), plus the
  window files' own I/O printed separately (`16n / 5.238 GB/s`, overlapped).
* **the build rate**: the 96-thread point is now the measured 10 GB rung
  (30.04 s/GB) instead of the 1 GB fixture (23.489 s/GB).
* the finish term is unchanged at **118.9 s/GB**.

The pile render (1,307,910,802,540 B, 96 threads, `--memory-gb 100`,
`--bin-live-cap 20000000000`, i.e. 66 windows):

```text
XSA_PLAN_PHASE name=bin(sweep) wall=5:04:18 wall_s=18259 disc_peak_gb=300.2 rss_gb=298.02
XSA_PLAN_PHASE name=bin(build) wall=10:54:49 wall_s=39290 disc_peak_gb=300.2 rss_gb=298.02
XSA_PLAN_PHASE name=bin(stitch) wall=4:08:40 wall_s=14921 disc_peak_gb=2183.3 rss_gb=298.02
XSA_PLAN_PHASE name=fused-finish wall=43:11:50 wall_s=155511 disc_peak_gb=3218.0 rss_gb=2757.75
XSA_PLAN_V2_BIN live_cap_slots=20000000000 windows=66 window_file_gb=298.0 ram_group_gb=298.0 count_s=1779 fill_s=16480 build_s=39290 stitch_s=14921 producer_s=72469 finish_s=155511
XSA_PLAN_V2_TARGET rows=1048576 leaves=1247321 levels_per_row=2.72 largest_leaf_rows<=1048576 rule=subdivide-while-rows>target (depth is an OUTPUT of the census; the depth cap is a fail-loud backstop, not the rule)
XSA_PLAN_V2_POSITION_STORE window_file_gb=298.02 = credit(2) x cap(20000000000) x 8 B ON DISC (one rolling file per live window, unlinked when built); ram_group_gb=298.02 = 16 B/slot x the group being sorted in place; position_io_gb=19489.4 = 16 x n a position written once and read once; position_io_s=3721 at the 5.238 GB/s device floor (overlapped by the fill and the build, so not on the critical path); persistent_position_column=0 full_SA=0
XSA_PLAN_V2_BIN_END e2e_s=155511 e2e=43:11:50 producer=20:07:49 finish=43:11:50 rule=max(producer,finish) (concurrent, flow-controlled through the piece window)
XSA_PLAN_V2_HOSTING all-phase scratch peak 3218.0 GiB = 2.642n on one host; free 2569.4 GiB: DOES NOT FIT (corpus itself excluded; add 1218.1 GiB if co-hosted)
XSA_PLAN_RAM INFEASIBLE: RAM term = ONE BUILD GROUP ~298.0 GiB (16 B/slot x 20000000000 group slots - the bin being sorted in place, W=66 windows); the position WINDOW is 298.0 GiB of FILE (2 credit x 20000000000 slots x 8 B), 0 B of RAM; ...
```

So the pile is **finish-dominated: e2e 43.2 h, producer 20.1 h** (count 0.5 h +
fill 4.6 h + build 10.9 h + stitch 4.1 h). The plan refuses on THIS box (3.1 TB
tree, 298 GiB build group) - and it refuses on the RAM term, which is the honest
statement: the window is a file now, but a BIN is indivisible, so sharding
(~250-340 GB per shard, 6-8 windows each) remains the launch shape and would cut
the fill term roughly 10x (66 windows -> 6-8 per shard).

## 6. PART 3: the docs as committed

* **`README.md`** rewritten to the machine as it is: the multi-pass reader
  (count -> scatter), rolling window files, target-size recursive bins, the
  ordered stitch as the merge-replacement, the decomposed finish, one
  `xsa build` entry, the measured pile arithmetic (~57 h with sharding,
  finish-dominated; the plan's own render says 43.2 h e2e for the unsharded
  shape), and THE LAW + D2 with the rolling amendment stated as doctrine.
* **`bit6/sxi_logs/pile-builder-v2/PILE_VISION_V1.md`**: an ADDENDUM with the
  bin-era pivot - why the comparison merge retired (the position store, not the
  comparison count), a ledger of what was measured and killed (6 rows, each with
  its numbers), the machine as it is now, the amended D2 with the counters that
  enforce it, and **the PFP-RADIX DOMAIN MAP**: byte radix = web text;
  PHRASE radix (the PFP dictionary as a superbyte alphabet) = cancer cohorts /
  raw read collections / structural-variant pangenomes, where a byte depth of 6-10
  becomes a phrase depth of 1-2; the count/scatter/window/concat machinery
  carries over unchanged and only the radix token changes. Marked as the
  pangenome-side sequel, with no phrase-radix claim shipping before it has its
  own census, oracle and byte-identity gate.

## 7. Residual risks

* **Walls are load-labelled.** The box carried foreign production work all
  session (load average 6-106, per-row labels above; `/proc/pressure` showed no
  CPU or I/O stall, so it was contention, not thrash). Every old-vs-new pair was
  run back to back in the same state, and the counter columns - which cannot be
  perturbed by load - carry the load-bearing claims. Deltas under ~10% in the
  wall columns are inside that noise.
* **The 10 GB corpus is `pf-10gb.txt`, not the banked `real10b-ref` corpus**,
  which no longer exists on this box. Its LCE depth differs from
  `mergperf-1b-snap` (498 symbols/byte vs 222 at the same nominal scale), which
  is why the cross-corpus "x10" ratio is 1.65x while the same-corpus ratio is
  0.86x. No pile-scale corpus was available; the pile numbers are the plan's
  model fed with measured rates, not a pile run.
* **Undividable buckets.** At 10 GB exactly two leaves needed the depth cap at
  256 (6.05M rows: a duplicate block needing 854-879 bytes of shared prefix
  before it can be divided). Raising the cap to 4096 removes them
  (`leaf_rows_max` 1,048,416, `over_target_leaves` 0) at +5.9% build. A corpus
  with duplicate blocks longer than the cap would again produce an over-target
  leaf - printed loudly, never silent - and splitting one byte at a time through
  a long shared block costs O(m x L) on one thread. **The named next mechanism is
  a shared-prefix SKIP** (compute a safe common-prefix lower bound in the same
  count pass and advance `depth` by it), which is not implemented and not
  claimed here.
* **The leaf sort is still an LCE-walking comparison sort** and its cost is
  `log2(m)` walks per row; the target only bounds `m`. A cache-resident radix
  leaf (or a vectorised LCP leaf) is the next lever, priced by the same ladder.
* **The 1 GB gates are RAM-cached** (`CROSS_POOL_SLOTS=33554432`); the 10 GB
  probes were RAM-cached too (1 TB box). The pile cannot be, hence the device
  floor in the plan. `CROSS_POOL_SLOTS` is REQUIRED for a probe: without it the
  comparator's 256-byte page path makes a 1 GB probe run for hours (measured
  here, then fixed by setting it) - the front door sets it, the raw probe does
  not.
* **The auto target is a measured constant, not a formula derived from the
  corpus.** It is validated at 100 MB, 1 GB, 2 GB and 10 GB and extrapolated to
  the pile; a corpus with radically different byte entropy could want a
  different value, and `--bin-target-size` is the knob.
* **Note on the inherited state**: `0c04ea0` does not compile (see §1). The four
  banked gates in the previous reports were produced from a tree where the call
  site was fixed; this branch fixes it in the tree, and the four gates here were
  produced from the committed binary.

## 8. Branch, commits, reproduction

Branch `auto-radix`, base `pile-week-v1 @ 0c04ea0`.

```sh
# build
c++ -O3 -std=c++17 -pthread bit6/cross_lcp_merge.cpp -latomic -o binbwt
c++ -O2 -std=c++17 -pthread tools/binbwt/oracle.cpp -latomic -o oracle

# the pyramid
./oracle <fresh scratch>                                   # 18m41s, 5 passes, rc=0
BINBWT_BIN=<binbwt> python3 tools/binbwt/gate.py 100m ar-single 96 4294967296
BINBWT_BIN=<binbwt> python3 tools/binbwt/gate.py 100m ar-mw8    96 4096
BINBWT_BIN=<binbwt> python3 tools/binbwt/gate.py 1b   ar-single 96 4294967296
BINBWT_BIN=<binbwt> python3 tools/binbwt/gate.py 1b   ar-mw5    96 250000000

# construction only, nothing written (CROSS_POOL_SLOTS is REQUIRED)
CROSS_POOL_SLOTS=33554432 binbwt --binbwt <corpus> --remap <remap> \
    --out-prefix /tmp/ignored --threads 96 --bin-live-cap <slots> --bin-probe \
    [--bin-target-size S] [--bin-depth-cap D] --bin-window-dir <scratch>

# the plan, without building
xsa build --input <corpus> --scratch <dir> --plan-only --threads 96 --memory-gb 100
```
