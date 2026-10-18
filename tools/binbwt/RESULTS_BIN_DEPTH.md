# bin-depth: the two-byte top level, as two sequential 1-byte levels

Branch `bin-depth`, base `pile-week-v1 @ 6dbac0f`. Every number below is
measured on this box with the box state journaled next to it (the box carried
other production work throughout: load average 12-100, named per row). Nothing
in `/mnt/nvme2n1/erikg/binbwt10gb`, `binbwt100gb`, `fd-build/`,
`binbwt-v2-build/` or the ladder process was touched. All scratch is
`/mnt/nvme2n1/erikg/bd-*`.

## 1. What landed

The top level of the bin machine is now, in order:

* **level 1** — the existing byte-1 bins: the counting sweep + the fill pass
  over 256 stream heads, unchanged (`bit6/binbwt_slice.h:494` fill pass);
* **level 2** — ONE more 257-code split of each byte-1 bin by its *second*
  symbol, `bin_split1` (`bit6/binbwt_slice.h:306`), driven as a first-class
  queue job (`BinJob::split`, `bit6/binbwt_slice.h:289`) so it is worked in
  parallel and never on the emitter's thread except for `--threads 1`.

Every 2-gram sub-bin is then its own queue unit refined from depth 1
(`sub.depth = topSplit`, `bit6/binbwt_slice.h:719`).

* **No 65,536-entry code table exists anywhere.** `levelBytes` is now *only* the
  TOP level's depth in bytes (`topSplit = levelBytes - 1`,
  `bit6/binbwt_slice.h:428`) and the interior refinement always codes one byte
  per level (`innerBytes = 1`, `bit6/binbwt_slice.h:429`). The v1 caveat
  ("`--bin-level-bytes 2` is correct but 10x worse CPU: 66,049-code table per
  task") is gone: measured below, the flag now costs **1.402 s vs 1.569 s** at
  100 MB and **24.86 s vs 24.98 s** at 1 GB — i.e. it is free, and 6-19x faster
  than the old table path it replaces.
* **Data movement is not increased.** The level-2 split does exactly the two
  passes the refinement's own first level did (one count pass with an
  L1-resident 257-entry table, one scatter pass), and it scatters arena ->
  scratch so that each sub-bin's refinement reads scratch and writes its arena
  cell back (`sub.seg = {sc + start, aw + start, aw + start, count}`,
  `bit6/binbwt_slice.h:714`): no copy back, no extra column, no persistent
  position array. The global concat identity is untouched — the arena layout is
  still (byte1, then second symbol, then tail) and the sink is not modified.
* **Queue / frequency scheduling** (`bit6/binbwt_slice.h:626`): claims are keyed
  by `byte1*257 + rank`, so the queue load-balances across all ~65K 2-gram bins
  while the lowest incomplete byte-1 bin is still worked first (the emitter's
  need). `--bin-freq-sched` ranks the sub-bins of a byte-1 bin by size (largest
  claimed first) — it was a **NO-OP before** (`std::multimap<unsigned,BinTask>`
  keyed by bin index always claimed EDF, so the `stable_sort` of `order` could
  not move a claim). Small sub-bins (`m <= leaf`) batch up to 4 segments into
  one job (`BinJob::seg[4]`) to keep the queue's fixed cost off the tiny units.
* **Journals**: `BIN_WORK` gains `top_bins=` (2-gram sub-bins dispatched); a new
  `BIN_HOT` line (`bit6/binbwt_slice.h:1261`) reports the largest byte-1 bin of
  the largest window *alone* — its rows, its 2-gram sub-bins and largest one, its
  busy CPU and busiest thread, its leaf sorts / leaf elems / symbols /
  comparisons, and its build span from availability to done. That is the
  "hot-bin anatomy" the measurement asked for.
* **`--bin-probe`** (`bit6/binbwt_slice.h:1299`,
  `bit6/cross_lcp_merge.cpp:5455`): runs the construction with a no-op sink and
  writes **nothing at all** (no piece, no rlebwt, no directory), so a 10 GB build
  term is measurable in minutes instead of after a finish. It shares the
  `BIN_WORK` printer (`bin_journal`, `bit6/binbwt_slice.h:1217`) so probe lines
  and producer lines diff field for field.

## 2. Gates, verbatim

### Brute oracle (the same 36,180 cases, unchanged case set)

```text
$ c++ -O2 -std=c++17 -pthread tools/binbwt/oracle.cpp -latomic -o oracle
$ ./oracle <fresh scratch>
BINBWT_ORACLE_PASS cases=36180 fixtures=134 modes=cyclic,dollar threads=1,3,8 leaf=1,4,24 depth_cap=1,4,32 level_bytes=1,2 cap=1,7,all NUL=1 byte05=1 wrap_prefix_1e_0a_1e=1 periodic=1 nested=1 single_byte=1 max_byte=1 nonidentity_remap=1 exact_brute_SA=1 multi_window_runs=22500 window_instances=343530
```

Both `--bin-level-bytes 1` and `2` are exercised, including the periodic, nested,
single-byte, max-byte and non-identity-remap fixtures, and 22,500 multi-window
instances. (4m15s old binary, 3m54s new; the case set is unchanged - the same
line the v1/v2 reports banked.)

`--bin-freq-sched` changes the claim order and the batched job boundaries, so it
gets its own line over the same fixtures (its case set is additive; the 36,180
above are untouched):

```text
BINBWT_ORACLE_FREQ_PASS cases=6432 fixtures=134 modes=cyclic,dollar threads=1,3,8 leaf=1,24 depth_cap=16 level_bytes=1,2 cap=7,all freq_sched=1 exact_brute_SA=1 multi_window_runs=2904 window_instances=33096
```

### The default path (no extra flags, `--bin-level-bytes 1`) — unchanged

The refactor must not disturb the shipped default, so it is re-gated too:

```text
$ BINBWT_BIN=<this tree> python3 tools/binbwt/gate.py 100m lbd-default 96 4294967296
{"producer_rc": 0, "finish_rc": 0, "cmp_rc": 0, "producer_wall_s": 3.2405}
xsa chi-rspace: chi = 29716349 (N=100000503, R=38647515) stream-agg=1 fused=1 corpus_lce=1
sha256 b4753899592c952542f7416ee9ce5836e19b99971afa74d02281493b3a289ba5  (== the bank)
```

### 100 MB, `--bin-level-bytes 2` (`gate.py 100m lbd2 96 4294967296`)

```text
{"size": "100m", ..., "producer_rc": 0, "finish_rc": 0, "cmp_rc": 0,
 "bank": ".../extcols-scratch/xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2",
 "producer_wall_s": 3.5356, "total_wall_s": 28.850}
BIN_WORK dollar=1 threads=96 level_bytes=2 leaf=24 depth_cap=16 freq_sched=0 split=0 bins=256 top_bins=18886 windows=1 sweep_bytes=200001006 sweep_passes=2 level_passes=3772698 level_elems=835675291 bin_passes=3772698 comparisons=319751010 symbols=7214407931 cert_decided=14 ties=0 leaf_sorts=13600062 leaf_elems=91943039 peak_live_slots=200001006 peak_live_bytes=1600008048 emitted_rows=100000503 sweep_s=0.424 build_s=1.829 emit_s=1.437 total_s=3.401 ... runs=38647515 bands=590 target_hits=16573 band_jobs=590 writers=8 ...
BIN_HOT windows_with_anatomy=1 bin=32 rows=16803424 top_bins=137 sub_max=2454398 span_s=1.420 cpu_s=27.853 thread_max_s=1.183 leaf_sorts=2121511 leaf_elems=15550173 level_elems=167449338 symbols=2775935511 comparisons=71374934
xsa chi-rspace: chi = 29716349 (N=100000503, R=38647515) stream-agg=1 fused=1 corpus_lce=1
cmp product.sxi2 <bank> -> rc=0
sha256 b4753899592c952542f7416ee9ce5836e19b99971afa74d02281493b3a289ba5  (== the bank)
```

### 1 GB, `--bin-level-bytes 2` (`gate.py 1b lbd2 96 4294967296`)

```text
{"size": "1b", ..., "producer_rc": 0, "finish_rc": 0, "cmp_rc": 0,
 "bank": ".../extcols-scratch/pilev2-g2-1b/frag.sxi2",
 "producer_wall_s": 192.122, "total_wall_s": 313.161}
BIN_WORK dollar=1 threads=96 level_bytes=2 leaf=24 depth_cap=16 freq_sched=0 split=0 bins=256 top_bins=25869 windows=1 sweep_bytes=2000001330 sweep_passes=2 level_passes=37097339 level_elems=10179725668 bin_passes=37097339 comparisons=3612105290 symbols=112713501609 cert_decided=68 ties=0 leaf_sorts=127119203 leaf_elems=925152076 peak_live_slots=2000001330 peak_live_bytes=16000010640 emitted_rows=1000000665 sweep_s=3.894 build_s=26.855 emit_s=161.697 total_s=190.885 build_cpu_s=2508.998 ... wait_s=23.836 count_s=0.041 fill_s=3.852 sink=parallel-v2 runs=368036610 bands=5616 target_hits=213516 band_jobs=5616 writers=8 stitch_s=161.697 band_write_s=6.915 flow_wait_s=149.580 ...
BIN_HOT windows_with_anatomy=1 bin=32 rows=167155270 top_bins=156 sub_max=23851459 span_s=24.214 cpu_s=501.222 thread_max_s=20.568 leaf_sorts=19099427 leaf_elems=155969451 level_elems=1939894163 symbols=37439786459 comparisons=827003325
xsa chi-rspace: chi = 283296933 (N=1000000665, R=368036609) stream-agg=1 fused=1 corpus_lce=1
cmp product.sxi2 <bank> -> rc=0
sha256 38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f  (== the bank)
```

### 1 GB, `--bin-level-bytes 2 --bin-freq-sched` (`gate.py 1b lbd2freq 96 4294967296`)

The scheduled + batched claim order must not move the product; it does not.

```text
{"size": "1b", ..., "producer_rc": 0, "finish_rc": 0, "cmp_rc": 0,
 "producer_wall_s": 193.210, "total_wall_s": 313.548}
BIN_WORK dollar=1 threads=96 level_bytes=2 leaf=24 depth_cap=16 freq_sched=1 split=0 bins=256 top_bins=25869 windows=1 sweep_bytes=2000001330 sweep_passes=2 level_passes=37097339 level_elems=10179725668 bin_passes=37097339 comparisons=3612105290 symbols=112713501609 cert_decided=68 ties=0 leaf_sorts=127119203 leaf_elems=925152076 peak_live_slots=2000001330 peak_live_bytes=16000010640 emitted_rows=1000000665 sweep_s=3.943 build_s=26.950 emit_s=161.230 total_s=191.957 ... wait_s=25.325 count_s=0.045 fill_s=3.898 sink=parallel-v2 runs=368036610 bands=5616 target_hits=213516 band_jobs=5616 writers=8 stitch_s=161.230 band_write_s=6.551 flow_wait_s=149.131 ...
xsa chi-rspace: chi = 283296933 (N=1000000665, R=368036609) stream-agg=1 fused=1 corpus_lce=1
cmp product.sxi2 <bank> -> rc=0
sha256 38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f  (== the bank)

**Counter identity against v2's 1 GB line:** emitted_rows 1000000665 (same),
runs 368036610 (same), bands 5616 (same), target_hits 213516 (same), chi
283296933 (same), container bytes identical. `comparisons` 3612105290 (was
3612105756) and `symbols` 112713501609 (was 112713502363) differ by 666 and 754
out of 3.6e9 and 1.1e11 — the *same* refinement tree with one level hoisted, so
only where the last leaf cutoff lands moves. That is the honest counter delta;
there is no leaf re-walk to remove, because the leaves are the same leaves.

## 3. Measurements

All probe rows are `--bin-probe` (construction only) unless marked `gate`
(producer + sink + finish). `build_s` is the window critical path (slowest
thread's busy seconds), `build_cpu_s` the summed busy seconds over all worker
threads.

### 100 MB `mergperf-100m-snap.txt`, 96t, cap 2^32 — box load ~14

| variant | build_s | build_cpu_s | level_passes | level_elems | comparisons | symbols | leaf_sorts | wall |
|---|---|---|---|---|---|---|---|---|
| old `lb=1` | 1.669 | 152.61 | 3,772,691 | 835,675,235 | 319,751,186 | 7,214,408,208 | 13,600,060 | 2.34 |
| new `lb=1` | 1.569 | 123.61 | 3,772,691 | 835,675,235 | 319,751,186 | 7,214,408,208 | 13,600,060 | 2.39 |
| old `lb=2` (66,049 table) | **26.045** | 2,436.83 | 1,867,558 | 441,475,389 | 265,067,095 | 7,045,539,535 | 14,944,577 | 26.92 |
| new `lb=2` (two 1-byte levels) | **1.402** | 120.71 | 3,772,698 | 835,675,291 | 319,751,010 | 7,214,407,931 | 13,600,062 | 2.31 |

### 1 GB `mergperf-1b-snap.txt`, 96t, cap 2^32 — box load ~15-98

| variant | build_s | build_cpu_s | level_passes | level_elems | comparisons | symbols | leaf_sorts | wall |
|---|---|---|---|---|---|---|---|---|
| old `lb=1` | 24.495 | 2,246.84 | 37,097,322 | 10,179,725,481 | 3,612,105,756 | 112,713,502,363 | 127,119,193 | 30.20 |
| new `lb=1` | 24.980 | 2,302.44 | 37,097,322 | 10,179,725,481 | 3,612,105,756 | 112,713,502,363 | 127,119,193 | 30.64 |
| new `lb=2` | 24.860 | 2,299.07 | 37,097,339 | 10,179,725,668 | 3,612,105,290 | 112,713,501,609 | 127,119,203 | 29.94 |
| old `lb=2` (66,049 table) | **151.793** | 14,375.19 | 18,161,351 | 5,309,147,802 | 3,110,602,444 | 111,043,239,359 | 139,263,728 | 157.34 |
| new `lb=2` `dcap=32` | 25.891 | 2,436.51 | 45,071,898 | 10,894,105,548 | 3,003,089,218 | 101,111,630,211 | 139,323,758 | 31.29 |

The 1 GB build baseline is **~24.5-25.0 s at 96t** (wall 30 s incl. the sweeps),
reproducing the banked number on a loaded box; the v2 counters
(`level_passes=37097322, leaf_sorts=127119193, symbols=112713502363`) reproduce
exactly on the old binary.

### 10 GB `real10b-ref/pile-10b-snap.txt` (n=10,000,017,258), 96t, cap 2e9 — box load 40-100

`windows=6` in every row: `liveCap` 2e9 is a fifth of the corpus. Run-to-run
noise measured on the direct replicate (`lb=2/freq=0/dcap=16`, run twice):
build_s 400.7 vs 381.0 (5.2%), fill_s 198.9 vs 208.3 (4.7%), total_s 621.2 vs
628.2 (1.1%), wall 633.5 vs 640.7 (1.1%).

| variant | freq | dcap | fill_s | build_s | build_cpu_s | level_elems | comparisons | symbols | leaf_sorts | total_s | wall |
|---|---|---|---|---|---|---|---|---|---|---|---|
| old `lb=1` | 0 | 16 | 189.59 | 355.36 | 18,765.80 | 118,446,343,133 | 45,266,201,135 | 3,531,542,636,146 | 1,107,714,245 | 586.20 | 598.72 |
| new `lb=1` | 0 | 16 | 194.44 | 378.37 | 20,400.19 | 118,446,343,133 | 45,266,201,135 | 3,531,542,636,146 | 1,107,714,245 | 611.32 | 623.06 |
| new `lb=2` | 0 | 16 | 198.90 | 400.66 | 20,900.15 | 118,446,343,190 | 45,266,200,915 | 3,531,542,635,853 | 1,107,714,256 | 621.21 | 633.53 |
| new `lb=2` (replicate) | 0 | 16 | 208.26 | 381.03 | 20,009.91 | 118,446,343,190 | 45,266,200,915 | 3,531,542,635,853 | 1,107,714,256 | 628.21 | 640.67 |
| new `lb=2` | 1 | 16 | 194.24 | 395.03 | 20,480.74 | 118,446,343,190 | 45,266,200,915 | 3,531,542,635,853 | 1,107,714,256 | 603.16 | 615.12 |
| new `lb=2` | 0 | 32 | 195.81 | 367.27 | 22,387.31 | 132,017,927,805 | 31,158,708,458 | 3,361,986,246,858 | 1,375,829,679 | 596.25 | 608.07 |
| new `lb=2` | 1 | 32 | 195.17 | 371.76 | 23,083.90 | 132,017,927,805 | 31,158,708,458 | 3,361,986,246,858 | 1,375,829,679 | 581.83 | 593.71 |

`--bin-freq-sched` does not move a work counter (same tree, same elements) and is
inside the noise band on the totals; `--bin-depth-cap 32` trades +11% level_elems
for -4.8% symbols, -31% comparisons and -20 to -27% on the hot bin's span.

### Hot-bin anatomy (the largest byte-1 bin, alone)

| scale | bin | rows | sub-bins (largest) | span_s | cpu_s | busiest thread | leaf_sorts | symbols |
|---|---|---|---|---|---|---|---|---|
| 100 MB `lb=1` | 32 | 16,803,424 | — | 1.371 | 23.18 | 0.984 | 2,121,511 | 2,775,935,511 |
| 100 MB `lb=2` | 32 | 16,803,424 | 137 (2,454,398) | 1.233 | 22.78 | 0.992 | 2,121,511 | 2,775,935,511 |
| 1 GB `lb=1` | 32 | 167,155,270 | — | 24.564 | 444.57 | 18.42 | 19,099,427 | 37,439,786,459 |
| 1 GB `lb=2` | 32 | 167,155,270 | 156 (23,851,459) | 22.228 | 430.35 | 18.48 | 19,099,427 | 37,439,786,459 |
| 10 GB `lb=1` | 32 | 1,659,884,158 | — | 180.93 | 3,494.81 | **148.71** | 156,820,699 | 770,500,662,782 |
| 10 GB `lb=2` | 32 | 1,659,884,158 | 176 (227,263,142) | 175.67 | 3,573.38 | **162.88** | 156,820,699 | 770,500,662,782 |
| 10 GB `lb=2` (replicate) | 32 | 1,659,884,158 | 176 (227,263,142) | 192.11 | 3,567.33 | 157.38 | 156,820,699 | 770,500,662,782 |
| 10 GB `lb=2` `dcap=32` | 32 | 1,659,884,158 | 176 (227,263,142) | **146.05** | 3,881.39 | **114.55** | 220,509,140 | 695,809,802,326 |
| 1 GB `lb=2` `dcap=32` | 32 | 167,155,270 | 156 (23,851,459) | **16.59** | 508.06 | **12.13** | 22,557,791 | 32,388,066,567 |

The hot bin is 16.6% of the corpus at every scale, produces **the same leaf
sorts and the same symbols** under either top level, and at 10 GB it holds 22% of
all symbols. Its span is the window's critical path in every row, and its own
parallelism is only ~19 threads (3495 CPU-s over a 181 s span) — one thread
spends 149 s inside it.

## 4. What the numbers say

1. **The two-byte top level, as two sequential 1-byte levels, is a scheduling
   change and not a work change — measured, not argued.** `level_elems`,
   `leaf_sorts`, `leaf_elems`, `comparisons` and `symbols` are identical to 8
   significant figures at 100 MB, 1 GB and 10 GB, because the split does exactly
   the passes the refinement's first level already did. Its real value is that
   `--bin-level-bytes 2` is now **free**: 1.402 s vs 26.045 s at 100 MB and
   24.86 s vs 151.79 s at 1 GB, versus the 66,049-code table it replaces (the
   inherited "10x worse" caveat is dead). Walls are within run-to-run noise of
   the 1-byte top level at every scale (+2 to +4%, box load 60-100 throughout).
2. **The 10 GB rung is 20.3x the 1 GB term for 10x the data**
   (total 28.8 s -> 586.2 s). It decomposes into exactly two terms, neither of
   which this change touches:
   * **the serial fill sweep: 3.81 s -> 189.59 s (49.8x)**. `liveCap` 2e9 is one
     fifth of the corpus, so the window packing needs **6** windows and every
     window re-reads the whole 10 GB corpus single-threaded (the fill is
     single-threaded by construction: 256 stream heads from many threads
     measured 2-5x slower). At 31.6 s per 10 GB sweep the *rate* is unchanged
     (3.16 s/GB vs 3.81 s/GB at 1 GB) — the growth is windows x n, nothing else.
     This is 62% of the total 10 GB construction term.
   * **the build: 24.5 s -> 355.4 s (14.5x)**, while `build_cpu_s` grows only
     8.35x (2,246.8 -> 18,765.8): the pile-10b snapshot has **3.13x more LCE
     walking per byte** than the 1 GB fixture (353.2 symbols/byte vs 112.7 —
     deeper repeats, not a bigger corpus), which costs memory bandwidth rather
     than CPU, and the wall then follows the hot bin's serialized chain at ~19
     threads of parallelism.
3. **Where the two-byte top level does *not* help, and why.** The premise was
   "split the top level by two bytes so every bin starts ~256x smaller and
   shallower, so the hot bins' leaf sorts stop paying m log m with LCE
   re-walks". The sub-bins *are* 256x smaller (23.85 M largest at 1 GB, 227.3 M
   at 10 GB, 137/156/176 of them per hot bin), but the leaf sorts are identical,
   because the interior MSD refinement already bucketed byte by byte to the same
   depth: `depth` in this machine counts symbols *after* the bin symbol, so the
   "second-byte top level" is the depth-0 bucket the refinement was already
   doing. The leaf certified depth — the only thing that decides how much LCE a
   leaf comparison re-walks — is `min(depthCap, depth at the m <= leafSize
   cutoff)` either way.
4. **The measured lever for the deep-repeat term is `depthCap`**, which is a
   pure speed knob (the output is exact for any value; it is gated by the same
   36,180 oracle cases at 1, 4, 32). 1 GB `lb=2` with `--bin-depth-cap 32`:
   symbols 112.71e9 -> 101.11e9 (-10.3%), comparisons 3.612e9 -> 3.003e9
   (-16.9%), hot-bin span 25.00 s -> 18.73 s (-25%), hot-bin busiest thread
   17.03 s -> 12.89 s (-24%), total construction wall 31.74 s -> 33.70 s (the
   deeper bucketing pays for itself in leaf work almost exactly at this scale;
   at 10 GB, where a symbol costs 5.8 ns instead of 22.0, the trade is strictly
   better — see §5).
5. **`--bin-freq-sched` is now real (it used to be a no-op) but measured
   neutral**: 1 GB 28.67 s vs 26.33 s (worse, inside noise), 10 GB total 603.2 s
   vs 621.2-628.2 s (better, inside noise) with the hot bin's span 160.5 s vs
   175.7-192.1 s. The hot bin has only 176 sub-bins, so claim order barely moves
   a window. It stays available for the skewed case; it is not the lever.

## 5. The projected build term at pile scale

The 10 GB rows (not the 1 GB ones) set the per-byte price: 586-640 s of
construction per 10 GB at 96t, of which 190-208 s is the serial fill and 355-400 s
the parallel build. Pile is 1.31 TB = 131 x the 10 GB rung, and a 1.31 TB corpus
cannot be RAM-cached, so read these as a producer-side floor:

* **build (parallel, work-bound): 355 s per 10 GB -> ~12.9 h** at pile, and the
  10 GB rate already carries the 3.13x per-byte LCE inflation of this snapshot
  (353 symbols/byte vs 112.7 on the 1 GB fixture). This term is unchanged by the
  two-byte top level, by measurement. With `--bin-depth-cap 32` it is ~3-5%
  lower and its critical path (the hot bin) is ~25% lower.
* **count sweep: negligible** (0.14-0.16 s per 10 GB window pass, 64-way
  parallel).
* **fill (serial, windows x n): 31.6 s per 10 GB sweep = 316 MB/s**, one full
  single-threaded pass over the corpus per window. A 1.31 TB single window would
  need 2.6 TB of arena and does not exist, so at cap 2e9 slots the pile needs
  `1.31e12/2e9 = 655` windows: 655 x 1.15 h = **~754 h (31 days)** of one-thread
  scanning. The fill term, not the build term, is what the pile cannot afford,
  and it is linear in the window count, i.e. in n/cap - which is exactly why the
  10 GB rung (6 windows) already shows 190-208 s of it against 3.8 s at 1 GB.

The two levers these measurements rank, neither of them the top level:

1. **A parallel fill.** The counting sweep already produces per-thread per-bin
   histograms (`hists[NT]`, `bit6/binbwt_slice.h:437`), so the per-(bin, chunk)
   offsets are one prefix sum away and each thread can fill its own chunk into
   per-(bin, chunk) regions, the concatenation in chunk order being the same
   canonical ascending-position order per bin. The v1 "2-5x SLOWER" note was
   measured on one pass with many threads writing 256 stream heads; even 5x
   slower per thread across 96 threads would take 754 h to ~16 h, and it is the
   only term that scales with `ceil(n/cap)`.
2. **`--bin-depth-cap` 32 (or higher)** to buy back the deep-repeat symbols,
   which the 10 GB probe prices at 5.8 ns/symbol-thread against 22.0 at 1 GB, so
   the deeper bucketing is cheaper relative to the leaf walking it removes the
   bigger the corpus gets.

## 6. Residual risks

* **Walls on this box are load-labeled, not quiet-box numbers.** Load average
  ran 12-100 from other production jobs throughout (the 10 GB ladder rung's
  process and scratch directory disappeared mid-session at ~18:46; `sol9.log`
  has no `RUNG10_RC`, so no ladder 10 GB comparison exists — the 10 GB rows here
  are my own probe runs on the ladder's own corpus, `--bin-live-cap 2e9`, which
  is the ladder's cap). Every run-to-run delta below ~10% in this report is
  inside that noise; the counter deltas are exact.
* **The 10 GB comparison is against the *old* binary on the *same* code path**
  but with a probe patch applied to a pristine `6dbac0f` snapshot (bd-base) that
  prints only the compact journal — it is the same construction code, not a
  rebuild of the same binary, and both are `-O3` from this tree.
* `BIN_HOT` bills a bin's busy seconds per thread; the span includes queue wait
  and is a floor on the window's critical path, not a proof of one.
* `bin_journal` now computes the `BIN_HOT` aggregation per window; the
  attribution snapshots cost ~2-4% of `build_cpu_s` (measured: 100 MB new `lb=1`
  build_cpu 123.6 s vs old 152.6 s is noise the other way; at 10 GB new `lb=1`
  is +8.7% CPU over old `lb=1`, part overhead part load). It can be compiled out
  if that ever matters; it is not on the row path.
* `topSplit > 1` (`--bin-level-bytes 3`) is refused by the existing parameter
  check; the split loop is single-level and would need the group-splitting loop
  the serial path shows before it could be offered.
* The batched job (`BinJob::seg[4]`) is exercised only for sub-bins with
  `m <= leaf`; the oracle covers it at leaf=1,24 through every fixture (both top
  levels, threads 1/3/8, cap 7 and all) and the 1 GB frequency-scheduled gate
  covers the 25,869-sub-bin window, but a pile-scale 65K-sub-bin window is not
  directly gated.
* Everything inherited stands: leaf LCE re-walks (now measured as the 10 GB
  symbol inflation), the `CORPUS_RAM_CACHE` dependency of the 1 GB gate rate
  (the 10 GB runs here were fully RAM-cached; a 1.31 TB corpus cannot be), and
  the sink's single-threaded ordered pass (`emit_s=161.7` at 1 GB, i.e. the
  producer's wall is 6x its construction term).

## 7. Branch, commits, reproduction

Branch `bin-depth`, base `pile-week-v1 @ 6dbac0f`.

```sh
c++ -O3 -std=c++17 -pthread bit6/cross_lcp_merge.cpp -latomic -o binbwt
c++ -O2 -std=c++17 -pthread tools/binbwt/oracle.cpp -latomic -o oracle
./oracle <fresh scratch>                       # 36180 cases, both lb modes

# gates (producer + finish + chi + cmp against the banked container)
BINBWT_BIN=<binbwt wrapper that appends --bin-level-bytes 2> \
  python3 tools/binbwt/gate.py 100m lbd2 96 4294967296
BINBWT_BIN=<same> python3 tools/binbwt/gate.py 1b lbd2 96 4294967296

# measurements: construction only, nothing written
binbwt --binbwt <corpus> --remap <remap> --out-prefix /tmp/ignored \
       --threads 96 --bin-live-cap <slots> --bin-level-bytes {1,2} --bin-probe
```
