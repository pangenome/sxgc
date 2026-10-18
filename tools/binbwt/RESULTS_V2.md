# bin-bwt v2: the serial row publication is gone (62.8 h projected -> 4.2 h)

Base `51a7f02` (pile-week-v1, bin-bwt v1 merged), branch `binbwt-v2-parallel-sink`.
The operator's shape is untouched: one sweep -> 256 byte-bins -> parallel per-bin
rle-BWT -> concat into the band/piece/finish protocol. What changed is the SINK:
the one ordered serial stream that published a row at a time is now an ordered
stitch over packed arena words plus a pool of band writers, and the transport -
`start`, `p-band-0`, `g-band`, `terminal`, the consumed marker that is the flow
credit - is byte-for-byte the v1 wire.

Headline, measured on this box (which carries a live 100GB run; load labelled):

| | v1 | v2 |
|---|---:|---:|
| 1GB/96t producer wall (producer only) | 213.63 s | **41.68 s** (5.13x) |
| its publication term | 185.333 s (172.6 ns/row banked) | **11.408 s** (11.41 ns/row) |
| -> sink collapse | | **15.1x** on the term |
| construction counters | comparisons 3,612,105,756 / symbols 112,713,502,363 | **identical** |
| pile projection, 10/21/66 windows | 84.8 / 96.3 / 143.3 h | **26.1 / 40.2 / 98.1 h** |

## 1. Design

Nothing about *what* is published changed; three things removed the 172.6 ns/row.

**(a) The arena word carries the row's symbol and its capture-target flag**
(`bit6/binbwt_slice.h:73` `bin_word`, decode at `:78-81`). A bin is built by
sorting positions by their tail, so anything derived per row must travel WITH the
element or it is silently wrong after the sort (the equivalence oracle caught
exactly that in my first attempt: a side symbol column and pre-sort hit slots).
The slot is now one word, `tail<<10 | symbol<<2 | hit`:

* `symbol` is `M[p-1]`, which the placing pass already holds one byte behind
  (`bit6/binbwt_slice.h:440-446`), so the ordered pass never chases the corpus at
  random - one cache miss per row is what the serial sink used to pay;
* `hit` is "this position is a seam capture target", decided by a merge join of
  the sink's ascending target list against the ascending positions in the same
  pass, so no per-row hash lookup survives either.

The comparator (`:104`) and bucket coder (`:120`) decode the tail, so the packed
word travels through every bucketing pass and every leaf sort unchanged. The
construction counters are bit-identical to v1 at both gate sizes, which is the
canonical-fill invariant and is now an attestation check.

**(b) The ordered stitch** (`BinBandSink::publishBin`, `bit6/binbwt_slice.h:706`)
is one thread over sequential memory doing only what a run index requires:
coalesce adjacent equal symbols into runs, number the runs, close each 65536-run
band, build that band's BPG1 record. No corpus read, no hash probe, no per-row
callback. 11.41 ns/row measured.

**(c) Band writers at run boundaries** (`:861` `writerLoop`). Each closed band is
handed to a writer slot; writers run concurrently and out of order, publishing
for band k: the piece `p-k-0` (rename, shared code with the serial path:
`bit6/cross_lcp_merge.cpp:3552` `v2_write_piece`), then the `rlebwt` extent by
`pwrite` at that band's global run-boundary word offset, then `g-k` (rename). The
band payload is bounded (slot credit = 2 per writer, capped by the transport
window) and the existing `v2_flow_wait` credit is unchanged (`:835`).

The other half of the 62.8 h was not rows at all: v1's `publishBand` rescanned
every class ever captured once per band (2e9 tests at 1 GB; 3.6e15 at pile).
`firstRun` is non-decreasing in class id and `lastRun` only moves from MAX to its
final value, so a monotone lower bound plus the head test yields the identical
reference set in O(classes + refs) (`:795` `closeBand`).

`--bin-sink serial` (`bit6/cross_lcp_merge.cpp:5458`) keeps the v1 sink in the
binary, driven through the same `bin_order`, so the two can be diffed byte for
byte on any corpus (the equivalence oracle below).

Construction is otherwise untouched: same sweep, same canonical single-threaded
fill, same intra-bin task tree, same arenas, same counters.

## 2. Model first (the wire ordering did not change, the writer interleavings did)

`tools/binbwt/model_sink2.py`: stitcher + P writers + the unchanged poller, with
adversarial interleavings, queue-slot credit and the unchanged flow credit.

```text
MODEL_SINK2_PASS schedules=400 perms=24 aborts=48 negatives=7 shapes=98
peak_queue=8 peak_band_backlog=40 deadlocks=0 lost_bands=0 duplicate_bands=0
order_violations=0 W=[1, 2, 3, 8, 1100] P=[1, 2, 3, 4] Q=[1, 2, 3, 4, 8]
negatives_caught=g-before-piece,missing-g,flow-credit-dropped,early-terminal,
unbounded-queue,duplicate-band,gapped-stream-deadlocks
```

472 schedules (400 randomized, all 24 four-writer permutations, 48 gapped/aborted
streams). It found the one real constraint it should find: a class whose run
extent reaches W bands deadlocks the consumer's `band+extent` wait - which is
precisely what the sink's `active seam class exceeds W-1 run bands` refusal
prevents. The model asserts piece-before-g, monotone consumption, the flow-credit
bound, the queue bound, no lost/duplicate band, terminal last, and 7 negative
controls are all caught.

## 3. Gates (verbatim)

Sink equivalence - the v1 sink and the v2 sink, same binary, same `bin_order`,
whole published stream compared (`.rlebwt`, `.rlebwt.meta`, every file under
`.pieces/` by name and byte) plus every construction counter:

```text
SINK_EQUIV_PASS cases=114 fixtures=19 shapes=6 threads=1,3,8 windows=1,2,3+
rlebwt=byte_identical meta=byte_identical pieces=byte_identical
construction_counters=identical max_piece_files=70 max_rlebwt_bytes=8398528
max_runs=2099632 max_bands=33 max_target_hits=1364
```

Brute oracle (kernel of the pyramid), pool and `CROSS_NO_POOL=1` (file-window
path), 36,180 cases:

```text
BINBWT_ORACLE_PASS cases=36180 fixtures=134 modes=cyclic,dollar threads=1,3,8
leaf=1,4,24 depth_cap=1,4,32 level_bytes=1,2 cap=1,7,all NUL=1 byte05=1
wrap_prefix_1e_0a_1e=1 periodic=1 nested=1 single_byte=1 max_byte=1
nonidentity_remap=1 exact_brute_SA=1 multi_window_runs=22500
window_instances=343530
```

Tiny product gate (real producer + real fused finish, independent brute padded
order -> raw RLE bytes, chi and every witness):

```text
BINBWT_TINY_PRODUCT_PASS cases=48 fixtures=12 real_windows=3 NUL=1 byte05=1
wrap_prefix=1 periodic=1 nested=1 cap=1,7 threads=1,3 raw_runs=brute_exact
chi=brute_exact witnesses=all_exact container=validated
```

Fixture gates (producer + fused finish concurrently, `cmp` against the banks):

```text
100m single window:  SXI2_PASS bytes=48768685 lite=1 members=6
100m single window:  PILE_FINISH_DONE chi=29716349 n=100000503 r=38647515
100m single window:  producer=0 finish=0 cmp=0   producer 3.31 s  (v1 15.27 s)
100m 8 windows:      chi=29716349 r=38647515 producer=0 finish=0 cmp=0   producer 14.82 s
100m threads=1:      chi=29716349 r=38647515 producer=0 finish=0 cmp=0   producer 75.15 s
1b single window:    SXI2_PASS bytes=467814822 lite=1 members=6
1b single window:    PILE_FINISH_DONE chi=283296933 n=1000000665 r=368036609
1b single window:    producer=0 finish=0 cmp=0
1b windows=5 cap250M: chi=283296933 r=368036609 producer=0 finish=0 cmp=0
1b single window, HEAD binary sha256 5fa842fc... (bit-identical on rebuild),
  run twice: at load 328 producer 267.2 s / flow_wait 174.0 s;
             at load  83 producer 209.4 s.  Both: chi=283296933 r=368036609
             producer=0 finish=0 cmp=0
```

Attestation (sweep conservation, row coverage, position bound, no full SA, gate
rcs, charged work, **construction counters bit-identical to the banked v1
values**, rows actually published by the parallel sink):

```text
ATTEST_PASS binbwt-gate-100m-v2 / binbwt-gate-100m-mw8-v2 / binbwt-gate-100m-t1v2
            binbwt-gate-1b-v2 / binbwt-gate-1b-v2free / binbwt-gate-1b-mw250m-v2
            binbwt-gate-1b-v2h / binbwt-gate-1b-v2final  (both on the exact
            HEAD-built binary; v2final is the final source state)
```

Counters at 1GB, v1 vs v2 (identical, i.e. the publication shape did not move the
construction):

```text
comparisons=3612105756 symbols=112713502363 level_passes=37097322
level_elems=10179725481 bin_passes=37097322 leaf_sorts=127119193
leaf_elems=925152140 cert_decided=68 ties=0 peak_live_slots=2000001330
emitted_rows=1000000665 runs=368036610 bands=5616 target_hits=213516
band_jobs=5616 writers=8
```

## 4. Walls

Producer-only runs (no consumer backpressure: `CROSS_V2_PIECE_WINDOW=100000000`),
1GB, threads 96, the v1 and v2 binaries **back to back in the same window**:

| | v1 (base 51a7f02, sha256 equal to the attested v1 binary) | v2 |
|---|---:|---:|
| sweep (count + fill) | 4.165 s | 0.038 + 5.084 s |
| build critical path | 29.767 s | 23.489 s (same work) |
| **publication term** | **185.333 s** (172.6 ns/row banked) | **11.408 s** (11.41 ns/row) |
| band writers (8 threads, overlapped) | — | 6.427 s summed |
| time waiting for the build | 21.772 s | 22.158 s |
| **producer wall** | **213.63 s** | **41.68 s** |
| peak RSS | 16.70 GB | 16.69 GB |

Load average during that pair: 27.9-44.6 (the live 100GB run was on the box; no
quiet-box claim). The v1 lane's banked 203.0 s was measured at load 50.6-59.9, so
the A/B is the defensible number, not the absolute one.

The same pair repeated later at load 104.6 / 92.8 (logs kept:
`/mnt/nvme2n1/erikg/binbwt-pw/ab-v1.log`, `ab-v2.log`):

| | v1 | v2 |
|---|---:|---:|
| sweep | 11.906 s | 0.039 + 13.852 s |
| build critical path | 36.904 s | 24.338 s |
| publication term | **345.388 s** | **9.483 s** (36x) |
| producer wall | **386.06 s** | **49.31 s** (7.8x) |

So the sink term is the load-sensitive one in v1 too (185.3 -> 345.4 s for the
same work) while v2's sequential stitch barely moves (11.4 -> 9.5 s): the A/B
ratio improves with box load, 5.1x at load 28-45 and 7.8x at load 93-105.

With the transport's default window the picture is different and it is the
important operational finding: at 1GB the v2 producer spends **147.874 s of its
191.7 s inside `v2_flow_wait`** waiting for `pile_finish` to consume bands, i.e.
the producer-side term is no longer what sets the 1GB end-to-end wall (with
finish: 312.1 s v2 vs 308.7 s v1). The finish is now the bottleneck there.

Thread scan (single runs, load labelled, and the placing pass is load-sensitive -
see risks): t8 wall 162 s at load 31.6-64.2, t32 wall 115 s at load 31.0-56.1.

### Revised pile projection (`tools/binbwt/pile_projection_v2.py`)

```text
window cap                                    win   count    fill   build  stitch   total  v1 total
20G slots = 320GB arena pair                   66    0.49   84.91    8.55    4.15   98.10    143.30
65G slots = 1040GB arena pair                  21    0.49   27.02    8.55    4.15   40.21     96.31
131G slots = 2096GB arena pair                 10    0.49   12.87    8.55    4.15   26.06     84.82
```

(hours; n = 1.31e12. `total` = count + windows x fill + 96-thread build critical
path + ordered stitch, summed because at 1GB the build finished 22.2 s before the
stitch did; the sum reproduces the measured 41.68 s wall within 1.7 s. `v1 total`
is the banked v1 table.)

**The binding term moved.** v1's serial publication was 62.81 h = 74-85% of its
total; it is now **4.15 h**. The largest term is the single-threaded **windowed
placing pass** (12.9-84.9 h for 10-66 windows), then the per-bin construction
8.55 h. The launch table for a 10-window shape goes from 84.8 h to **26.1 h**, and
for the 66-window shape from 143.3 h to **98.1 h** - where the fill, not the sink,
is what the launch is now waiting on.

## 5. Residual risks

* **The placing pass is now the top term and it is single-threaded and extremely
  load-sensitive.** Per 1GB pass I measured 3.8 s (load 27) to 188 s (load 229):
  a factor of 50, far wider than any other term. Every fill hour above assumes a
  quiet box. v1's 2.87 s/window marginal, scaled by the +23% that packing+join
  cost, is what the table uses; a fully-loaded box would blow the 66-window row up
  by more than the whole v1 sink improvement.
* **The finish is the end-to-end bottleneck at 1GB** under the default transport
  window (147.9 s of producer flow wait). Attacking the sink further buys nothing
  until `pile_finish`'s band consumption is measured and raised; the producer is
  now faster than the consumer.
* The ordered stitch is one thread: 11.41 ns/row, 4.15 h at pile. It is the sum
  of run detection, 4 vector pushes per run and band assembly. Its per-bin
  extraction could be parallelised behind the ordered band numbering (bands are a
  prefix sum, extraction is not), which would take it to ~1-2 h; not attempted.
* The rlebwt extents are `pwrite`n at computed offsets, so the file is sparse until
  the last band lands. The consumer's completion sentinel is the final size
  (`pile_finish.cpp:2657`), and it only reads that file after the sweep, so a
  mid-stream missing extent cannot be read - but the producer's own gate for that
  is `band_jobs == bands` and the writers' join before `terminal`.
* `require(pending.len == uint32_t(pending.len))`: the piece's length field is
  u32, so a single BWT run longer than 4.29e9 is now refused loudly instead of
  silently truncated as in v1. No gate fixture and no plausible corpus has one.
* Packing caps the tail at 2^53 (require in `bin_word`); pile n = 1.31e12 is 2^41.
* `--bin-level-bytes 2`, oversized-bucket pathology, LCE re-walk of 112.7
  symbols/byte and the file-window (non-RAM-cached) construction cost are all
  unchanged from v1 and still as stated there.
* The t8/t32 walls are single runs taken while the neighbour load swung to 92-265
  mid-flight; treat them as indicative only.

## 6. Branch, reproduction

Branch `binbwt-v2-parallel-sink`, base `51a7f02`.

```sh
c++ -O3 -std=c++17 -pthread bit6/cross_lcp_merge.cpp -latomic -o binbwt
c++ -O2 -std=c++17 -pthread tools/binbwt/oracle.cpp -latomic -o oracle
python3 tools/binbwt/model_sink2.py                     # wire ordering model
python3 tools/binbwt/sink_equiv.py <binbwt>             # v1-vs-v2 sink byte identity
BINBWT_BIN=<binbwt> python3 tools/binbwt/tiny_product.py
BINBWT_BIN=<binbwt> python3 tools/binbwt/gate.py 100m v2 96 4294967296
BINBWT_BIN=<binbwt> python3 tools/binbwt/gate.py 1b v2 96 4294967296
GATE_PIECE_WINDOW=100000000 BINBWT_BIN=<binbwt> python3 tools/binbwt/gate.py 1b v2free 96 4294967296
python3 tools/binbwt/attest.py <gate roots>
python3 tools/binbwt/pile_projection_v2.py
```

Gate roots must be fresh (gate.py now refuses a stale root). Raw logs under
`/mnt/nvme2n1/erikg/binbwt-gate-*`, the v1 binary rebuilt from the base commit in
`/mnt/nvme2n1/erikg/binbwt-v1-build/`. The live 100GB run's scratch
(`/mnt/nvme2n1/erikg/rung100gb`) was never opened or modified.
