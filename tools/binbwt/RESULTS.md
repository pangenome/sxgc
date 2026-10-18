# bin-bwt v1: one bin per byte, parallel per-bin tail order, concat

Base `b9e85ea` (pile-week-v1), branch `bin-bwt-v1`. The operator's shape, exactly:

1. **Sweep once.** One pass over the corpus (windowed `CorpusText`, production
   remap) groups every position into the bin of its **first remapped byte**:
   256 bins, fixed depth 1, no top-level recursion.
2. **Each bin, in parallel** (work-stealing over bins *and* over a bin's own
   coarse nodes; the lowest incomplete bin - the emitter's next need - is always
   worked first). Bin `b`'s rows are its suffixes `p` with `M[p]=b`, ordered by
   their **tail** (the suffix at `p+1`); each row contributes `text[p-1]` as the
   BWT symbol and the sink run-length encodes it as it arrives. Construction is
   an MSD refinement over transient positions: coarse nodes are bucketing
   passes, leaves are raw-LCE comparison sorts through the `CorpusText` cache
   starting at the certified depth, so a prefix shared inside a bin is walked
   once by a bucketing pass and never again by a comparison.
3. **Concat.** Bin segments concatenate in byte order straight into the
   EXISTING band/piece/finish protocol (unchanged `SequentialV2`); the sink
   coalesces the one boundary run between adjacent bins by itself (`flush()`
   keeps `pending` across `row()` calls). The SA is never materialized. No
   k-way heap, no merge, no ordered walk of producers anywhere.

Text is only ever read through `CorpusText`; positions live transiently in a
bounded per-window arena pair (16 B/slot: arena + scratch) and are destroyed
before the next window. No per-suffix position column, no key column, no
fingerprint hash scratch (bin-bwt needs none: LCE is direct). `full_SA=0
persistent_positions=0` is journaled by the producer itself.

## Landed, source locations

* `bit6/binbwt_slice.h:44` `bin_tail_of`/`:48` `bin_pos_of`: tail coordinate
  (dollar tails carry no wrap, tail `n` is the empty suffix; cyclic tails rotate).
* `:56` `bin_tail_cmp`: exact total order on tails, `certified` = provably equal
  prefix, 64-byte vectorised windows over the doubled view. Dollar "ties" are
  the shorter-suffix rule; cyclic ties break by ascending source position
  exactly as the inherited chunk rule.
* `:98` `bin_code`: one symbol of a tail becomes `v+1`, an exhausted suffix
  becomes `0`, so bucket order IS dollar order at that depth. `:119` `BinArena`
  (uninitialised, hugepage-advised window storage).
* `:145` `bin_sort_rec`: leaf comparison sort (insertion ≤16, else `std::sort`),
  bucketing otherwise; bucket 0 (dollar-exhausted tails) needs no comparison.
* `:206` `bin_task_run` / `:255` `bin_build_serial`: a coarse node splits into
  independent child tasks (each carries its canonical arena slice), a subtree
  ≤ split size is refined sequentially by one thread.
* `:276` `bin_order`: count sweep → per-window fill → parallel build →
  ordered emit. `:293` parallel first-byte count, `:345` canonical
  single-threaded fill (`:345-350` notes why: multi-threaded fill measured
  2-5x slower), `:380` bin claim order, `:388` `emitBin`, `:412` bin-keyed task
  queue (`Shared`), `:469` emitter walking bins in row order, `:336-343` bin
  window admission under `liveCap`.
* `:501` `binbwt_main`: producer entry, remap bijection checks, `SequentialV2`
  sink, `BIN_WORK` journal line. CLI `:5434` of `bit6/cross_lcp_merge.cpp`:
  `--binbwt CORPUS --remap MAP --out-prefix PREFIX [--threads T]
  [--bin-leaf 24] [--bin-depth-cap 16] [--bin-level-bytes 1]
  [--bin-live-cap SLOTS] [--bin-split SLOTS] [--bin-freq-sched]
  [--bin-mode cyclic]`.

Untouched: the publisher, flow window, seam capture, band/piece formats, fused
finish, witnesses and chi container. `--radix` and the merge path are unchanged.

## Verification pyramid

Brute oracle first (`tools/binbwt/oracle.cpp` includes the producer source and
calls the SAME `bin_order`), 134 fixtures × both modes × threads {1,3,8} × leaf
{1,4,24} × depth cap {1,4,32} × level bytes {1,2} × live cap {1,7,all}:

```text
BINBWT_ORACLE_PASS cases=36180 fixtures=134 modes=cyclic,dollar threads=1,3,8 leaf=1,4,24 depth_cap=1,4,32 level_bytes=1,2 cap=1,7,all NUL=1 byte05=1 wrap_prefix_1e_0a_1e=1 periodic=1 nested=1 single_byte=1 max_byte=1 nonidentity_remap=1 exact_brute_SA=1 multi_window_runs=22500 window_instances=343530
```

Same suite with `CROSS_NO_POOL=1` (file-window path, no RAM cache):

```text
BINBWT_ORACLE_PASS cases=36180 fixtures=134 modes=cyclic,dollar threads=1,3,8 leaf=1,4,24 depth_cap=1,4,32 level_bytes=1,2 cap=1,7,all NUL=1 byte05=1 wrap_prefix_1e_0a_1e=1 periodic=1 nested=1 single_byte=1 max_byte=1 nonidentity_remap=1 exact_brute_SA=1 multi_window_runs=22500 window_instances=343530
```

Fixtures include 0x00/0x05-only text, exact `[0x1e,0x0a,0x1e]`, single-byte
texts, all-'a', 32x`ab` periodic (one giant bin with deep tail ties), nested
repeats with separators, max/min byte edges and 120 random texts; the remap is
nonidentity and moves bytes the fixtures contain (including 'x'↔'y', which
flips periodic order), so remapped order - not raw byte order - is what brute
checks against. 22,500 of the cases force multi-window processing.

Tiny product gate (real producer + real finish; independent brute padded finite
order → raw RLE bytes, chi and ALL witnesses):

```text
BINBWT_TINY_PRODUCT_PASS cases=48 fixtures=12 real_windows=3 NUL=1 byte05=1 wrap_prefix=1 periodic=1 nested=1 cap=1,7 threads=1,3 raw_runs=brute_exact chi=brute_exact witnesses=all_exact container=validated
```

Fixture gates (producer + fused finish concurrently, `cmp` against the bank):

```text
100m single window:  SXI2_PASS bytes=48768685 lite=1 members=6
100m single window:  PILE_FINISH_DONE chi=29716349 n=100000503 r=38647515
100m single window:  rc producer=0 finish=0 cmp=0   producer 14.9s total 29.4s
100m 8 windows:      SXI2_PASS bytes=48768685 lite=1 members=6
100m 8 windows:      PILE_FINISH_DONE chi=29716349 n=100000503 r=38647515
100m 8 windows:      rc producer=0 finish=0 cmp=0   producer 18.3s total 30.5s
1b  all thread counts and the 5-window cap run: SXI2_PASS bytes=467814822 lite=1 members=6
1b: PILE_FINISH_DONE chi=283296933 n=1000000665 r=368036609
1b: producer=0 finish=0 cmp=0 for t=1,8,32,96 and for --bin-live-cap 250000000 (windows=5)
```

Banks: `extcols-scratch/xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2`,
`extcols-scratch/pilev2-g2-1b/frag.sxi2`. `tools/binbwt/attest.py` checks sweep
conservation (`sweep_bytes=(1+windows)*n`), row coverage (`emitted_rows=n`), the
position bound, absence of any full SA, gate return codes and that work is
actually charged:

```text
ATTEST_PASS binbwt-gate-100m-single / binbwt-gate-100m-mw / binbwt-gate-1b-t1 /
binbwt-gate-1b-t96 / binbwt-gate-1b-mw250m   (6/6 checks each)
```

## Walls at 1GB, threads {1,8,32,96}

`mergperf-1b-snap.txt` (n=1,000,000,665). One window unless stated. The box
carries a live 100GB run throughout: **load average 50.6-59.9 during these
runs, 256 cores, no quiet-box claim.** `build_s` is the slowest worker's busy
time (the build critical path); `build_cpu_s` is the sum over threads and
includes memory stalls, so it inflates with thread count and is not a work unit.

| threads | windows | sweep_s | build_s | emit_s | wait_s | total_s | producer wall | with finish |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 4.67 | 615.44 | 177.78 | 0.00 | 798.91 | **800.1** | 884.6 |
| 8 | 1 | 3.99 | 107.93 | 191.97 | 27.67 | 224.62 | **225.7** | 324.2 |
| 32 | 1 | 4.01 | 35.86 | 205.12 | 17.95 | 228.09 | **229.4** | 318.7 |
| 96 | 1 | 4.23 | 30.43 | 172.60 | 23.82 | 201.76 | **203.0** | 308.7 |
| 96 | 5 (cap 250M) | 15.70 | 33.91 | 189.80 | 27.13 | 234.04 | **235.3** | 322.3 |

Construction alone (identical run with a no-op emitter, `tools/binbwt/rowcost`
with the same 16GB pool budget so the text is RAM-cached): **32.1-33.5 s** at
1GB / 96 threads (build 27.6-28.7, sweep+fill 4.0-4.1, no-op emit 0.5-0.6), and
2.0-2.2 s at 100MB. So of the 203 s producer wall, **~170 s is the unchanged
sink's serial row publication** and ~33 s is this shape's own work.

The same no-op run with the default (small) page pool, so the text is NOT
RAM-cached and `span()` falls into the file-window path, took **2551 s**
(2.55 us/byte, build 2536 s, `sys` 3865 min): the construction is **77x slower
without the text cache**. That is the single most important qualifier on the
pile build term below.

## Work counters, measured (identical for every thread count and window cap)

```text
1b: comparisons=3612105756 symbols=112713502363 level_passes=37097322
    level_elems=10179725481 bin_passes=37097322 leaf_sorts=127119193
    leaf_elems=925152140 cert_decided=68 ties=0
    peak_live_slots=2000001330 peak_live_bytes=16000010640 emitted_rows=1000000665
100m: comparisons=319751186 symbols=7214408208 level_elems=877031559
    leaf_sorts=14061338 leaf_elems=91589870 peak_live_slots=200001006
```

Unit rates, each measured on its own unit (never mixed):

| unit | 1GB count | rate | source |
|---|---:|---|---|
| compared symbols | 112,713,502,363 | **5.46 ns/symbol-thread** (1 thread, uncontended 615.4s) | `build_cpu_s/symbols` at t=1 |
| comparisons | 3,612,105,756 | **170.4 ns/comparison-thread** | `build_cpu_s/comparisons` at t=1 |
| bucketing element-codings | 10,179,725,481 | inside the build rates above | `level_elems` |
| corpus bytes swept | 2,000,001,330 (1 count + 1 fill pass) | **1.36 ns/byte** count, **2.87 ns/byte** fill | t96 windows=1 vs windows=5 marginal |
| emitted rows | 1,000,000,665 | **172.6 ns/row** (emitter thread), 168.3 by difference | `emit_s`, no-op comparison |

At t=96 the same work costs 24.7 ns/symbol-thread instead of 5.46: the box is
memory-bandwidth bound long before 96 threads, so thread-local rates at high
thread counts are stall-inflated, not work-inflated. Counters are bit-identical
across thread counts because the fill is canonical (ascending position inside
every bin), so scheduling never changes the work - a checkable invariant.

Symbols per corpus byte = 112.7. Against the retired whole-merge baseline of
60,485,597,061 symbols at 1GB, this shape still compares **1.864x more
symbols** - the k-way log-tax is gone, but leaf LCE re-walking is not. Against
the sol8a gated radix N=8 slice (9,556,970,969 comparisons / 157,116,619,752
symbols) it is **2.65x fewer comparisons and 1.39x fewer symbols**, from one
sweep instead of 36 and with no key column.

## radix-vs-merge at 1GB

| pipeline | producer work at 1GB | producer wall | with finish |
|---|---:|---:|---:|
| retired comparison merge (reference numbers, not remeasured here) | chunk 27.6 + merge 291 = 318.6 s | 318.6 s | 751 s |
| sol8a radix N=8 (same box family, 16 threads) | 36 sweeps, 9.56G cmp, 157.1G symbols | 1181.8 s | 1264.0 s |
| **bin-bwt v1, 96 threads** | **1 sweep, 3.61G cmp, 112.7G symbols** | **203.0 s** | **308.7 s** |
| bin-bwt v1, 8 threads | same work | 225.7 s | 324.2 s |

bin-bwt is 5.8x faster than the radix slice and 1.57x faster than the merge's
chunk+merge at 1GB; 2.4x on the end-to-end figure. Caveats: the merge and radix
numbers are inherited references measured on a differently loaded box; the
finish phase is identical machinery for all three, and here it overlaps the
producer rather than following it, so the "with finish" column is not a
like-for-like phase sum.

## Pile projection (arithmetic shown)

`tools/binbwt/pile_projection.py`, n = 1,310,000,000,000 bytes, every term =
measured rate × byte count. No k-way log-tax exists in this shape: ordering
work is the per-bin build term; there is no merge walk to pay for.

```text
20G slots = 320GB arena pair      66 windows  count .49  fill 68.93  build 11.07  emit 62.81  serial 132.23  total 143.30
65G slots = 1040GB arena pair     21 windows  count .49  fill 21.93  build 11.07  emit 62.81  serial  85.23  total  96.31
131G slots = 2096GB arena pair    10 windows  count .49  fill 10.44  build 11.07  emit 62.81  serial  73.75  total  84.82
```

(hours; `serial` = count + fill + emit, which run on one thread in this shape;
`total` adds the 96-thread build critical path scaled from 30.43 s.)

The three binding terms at pile, in order:

1. **Serial row publication, 62.8 h.** `emit` at 172.6 ns/row is the unchanged
   sink's per-row cost (seam capture, target map lookup, run/band bookkeeping,
   piece packing). Removing the k-way heap does not remove this: any producer
   that feeds this protocol pays it, one row at a time. It is 74-85% of the
   projected total.
2. **Windowed placing passes, 10.4-68.9 h** for 10-66 windows: each extra window
   rereads the corpus and rewrites 8 B/position (`2.87 ns/byte` measured).
3. **Per-bin construction, 11.1 h** at 96 threads.

v1 as landed needs `cap >= largest bin`; the 1GB space bin is 0.1672 of the
corpus, so at pile that bin alone is 219.0G slots = **3.50 TB** of arena pair. So
every row above already assumes the operator's general rule (count and
materialise SUB-bins below the top level); v1's fixed depth 1 cannot reach pile.
The retired merge pipeline's 751 s of serial phases scales to 273.3 h, so this
shape projects **1.9-3.2x better**, but the honest headline is that the serial
publication floor is now the thing to attack, not the ordering.

## Residual risks

* Serial row publication dominates at pile (62.8 h projected) and is inherited.
  The merge path's own header documents sharded emission at fixed run boundaries
  with disjoint `pwrite`s; the final v2 publisher is single-threaded. Parallelising
  it is the highest-value next step and is NOT authorized or implemented here.
* LCE reads at pile hit a 1.31 TB text that cannot be RAM-resident. Every 1GB
  gate number above is with `CORPUS_RAM_CACHE` on (9.97e9 cache hits at 1GB);
  the identical no-op run without it measured **2551 s vs 33 s, 77x worse**
  (`tools/binbwt/logs/rowcost-1b*.log`). The pile build term (11.07 h) is
  therefore the weakest number here: it assumes text locality that a 1.31 TB
  corpus cannot provide. The file-window path is at least gated for exactness
  (`CROSS_NO_POOL` oracle run).
* Leaf comparison sorts re-walk prefixes between siblings that land in different
  leaves; 112.7 symbols/byte vs the merge baseline's 60.5. No LCP carry beyond
  the certified depth is implemented (a merge-time LCP array would cut this and
  is not attempted).
* `--bin-level-bytes 2` is implemented and correct but measured **10x worse CPU**
  (a 66,049-entry code table per coarse task); it is not a usable knob yet.
* Oversized-bucket pathology: a periodic corpus yields one giant bin whose tails
  keep tying. Depth cap 16 falls back to comparison sorts there, which is exact
  (gated) but slow; there is no bounded-selection fallback like sol8a's.
* Peak live RAM at 1GB single window is 2n slots = 16.0 GB (arena + scratch).
  Windows keep that bounded, but the *largest bin* is the hard floor.
* Box load 50-60 throughout (live 100GB run); no quiet-box claim. Thread-local
  rates at 96 threads include memory stalls. `build_cpu_s` is not a work unit.

## Branch, reproduction, milestones

Branch `bin-bwt-v1`, base `b9e85ea`. Commits: (1) slice + brute oracle, (2)
task-level intra-bin parallelism + EDF order + hugepage arenas, (3) canonical
single-threaded fill + fixture gates, (4) this attestation.

```sh
c++ -O3 -std=c++17 -pthread bit6/cross_lcp_merge.cpp -latomic -o binbwt
c++ -O2 -std=c++17 -pthread tools/binbwt/oracle.cpp -latomic -o oracle   # fresh scratch dirs
c++ -O3 -std=c++17 -pthread tools/binbwt/rowcost.cpp -latomic -o rowcost
python3 tools/binbwt/gate.py 100m single 96 4294967296
python3 tools/binbwt/gate.py 1b t96 96 4294967296
python3 tools/binbwt/gate.py 1b mw250m 96 250000000
python3 tools/binbwt/tiny_product.py
python3 tools/binbwt/attest.py <gate roots>
python3 tools/binbwt/pile_projection.py
```

Gate roots must be fresh; the producer refuses an existing `.rlebwt`/`.pieces`.
Raw logs stay under `/mnt/nvme2n1/erikg/binbwt-*`. The live 100GB run's scratch
(`/mnt/nvme2n1/erikg/rung100gb`) was never opened or modified.
