# parallel-fill: the prefix-sum scatter (the fill term is no longer a term)

Branch `parallel-fill`, base `pile-week-v1 @ 3399b2b` (the `bin-depth` merge).
The task was §4.1 of `tools/binbwt/RESULTS_BIN_DEPTH.md`: the fill was
**655 windows x 1.15 h = 754 h** at pile scale because each window's fill was a
single-threaded pass over the WHOLE corpus (316 MB/s) to place that window's
bin-range positions. Every number below was measured on this box with the box
state journaled next to it; scratch is `/mnt/nvme2n1/erikg/pf-*`, and nothing in
`binbwt100gb/`, `bd-*`, `real10b-*` or the ladder's scratch was touched.

## 1. The design (file:line)

`bit6/binbwt_slice.h`, the fill pass (`:502`-`:625`), and `NT` (`:431`).

The counting sweep already produced `hists[t][c]` — the count of byte `c` in
corpus chunk `t` (`:463`-`:487`). One prefix sum over (chunk x bin) therefore
gives every (bin, chunk) pair its first arena slot:

```text
slot0(t, c) = off[c] + sum_{t' < t} hists[t'][c]        (bit6/binbwt_slice.h:544)
```

* The (bin, chunk) regions are **disjoint**, so every thread scatters its own
  chunk's positions into its own regions with **no lock, no atomic and no shared
  cursor** (`fillChunk`, `:556`-`:619`). NT threads, one pass each.
* Concatenated in chunk order, a bin's regions hold its positions in **ascending
  position order** — the exact canonical order the serial pass produced, i.e. the
  SAME position set in the SAME arena slots. That is asserted, not assumed:
  * per chunk: `cur[c] == slot0(t,c) + hists[t][c]` (`:610`), i.e. every slot of
    the chunk's regions written exactly once and no other chunk's slot touched;
  * per window: the slot0 prefix sum must land exactly on `off[c] + bins[c]`
    (`:552`).
* The other two jobs the fill does stay in the same pass and stay free (no
  random access): the BWT symbol `M[p-1]` comes from the window buffer one byte
  behind each position (`:583`), and the sink's ascending capture-target list is
  merge-joined per chunk (two bisects per chunk per window, `:558`-`:564`), with
  `hits` summed across chunks and asserted to conserve the target list at the
  last window (`:621`-`:628`). That conservation check is new: it is the only
  thing that proves every target was flagged exactly once *across* windows.
* `NT` is now `min(threads, 256)` (`:431`) instead of `min(threads, 64)`: it was
  capped when only the count sweep used it; the fill wants the whole machine.
  The cap is the histogram table's size (256 x 256 counts = 512 KB).
* **Data movement is unchanged and D2 holds**: nothing is persisted, no per-row
  position column exists, the arena is still window-sized (`live` slots + a
  `live`-slot scratch), `peak_live_slots = 2*live` (`:930`), and a window's arena
  is dead before the next window is allocated. At pile scale the fill's own
  bounded-transient footprint is one 1 MB read buffer per thread.

Nothing downstream of the fill changed: the refinement, the claim order, the row
order, the sink, the counters. The only product-visible change is the default
`--bin-depth-cap` (32, §4).

### The window discipline survived as-is

The task allowed restructuring it. It was not needed: with the scatter, a
window costs ONE parallel pass over the corpus instead of one *serial* pass, so
the cap still bounds peak live slots (requirement (a)) and the counter identity
is untouched (requirement (b)). §5 shows what the window count now costs — and
what it no longer costs.

### A correction I have to record

My first commit claimed a latent multi-window capture-target bug: "the serial
fill advanced its target cursor on every position, so every target whose bin was
not in the first window was silently dropped". That was wrong — the cursor is
declared *inside* the window body, so it restarts at 0 in every window and every
window matches the whole ascending target list, placing each hit only where its
bin is admitted. I "fixed" a bug that did not exist and introduced one: advancing
only on placement deadlocks an in-step merge as soon as a target's bin is not
admitted (the cursor then blocks every later target of that chunk). The 100 MB
W=8 gate caught it in one run (`CROSS_LCP_FATAL bin fill target join coverage`,
15,078 of 16,573 joined). Commit `58cef28` restores the serial semantics per
chunk and adds the two asserts above. The banked W>=4 gates were never passing by
luck; they were passing because the code was right.

## 2. Gates, verbatim

Ten-core box, load average 12-91 from other production work throughout (a 100 GB
bin-bwt producer was running for most of the session; the 1.31 TB pile histogram
in §5 filled the page cache and is the source of the one outlier in §3).

### Brute oracle (unchanged case set, final tree)

```text
$ c++ -O2 -std=c++17 -pthread tools/binbwt/oracle.cpp -latomic -o oracle
$ ./oracle <fresh scratch>                                    # 9m40s
BINBWT_ORACLE_PASS cases=36180 fixtures=134 modes=cyclic,dollar threads=1,3,8 leaf=1,4,24 depth_cap=1,4,32 level_bytes=1,2 cap=1,7,all NUL=1 byte05=1 wrap_prefix_1e_0a_1e=1 periodic=1 nested=1 single_byte=1 max_byte=1 nonidentity_remap=1 exact_brute_SA=1 multi_window_runs=22500 window_instances=343530
BINBWT_ORACLE_FREQ_PASS cases=6432 fixtures=134 modes=cyclic,dollar threads=1,3,8 leaf=1,24 depth_cap=16 level_bytes=1,2 cap=7,all freq_sched=1 exact_brute_SA=1 multi_window_runs=2904 window_instances=33096
```

The oracle drives `bin_order` directly at threads 1/3/8 (so NT = 1/3/8 chunks,
including empty chunks when n < NT) and caps 1/7/all (so 1..n windows), both
top levels, and requires the exact brute SA plus
`sweepBytes == (1 + windows) * n`, `emittedRows == n` and
`peakLiveSlots <= 2*max(cap, n)`. The scatter is therefore exercised with n as
small as 1 through 220 bytes, and the arena-order identity is what it checks.

### 100 MB, single window (`gate.py 100m final-r3 96 4294967296`)

```text
{"producer_rc": 0, "finish_rc": 0, "cmp_rc": 0, "cmp_rc": 0,
 "producer_wall_s": 3.038, "total_wall_s": ...}
BIN_WORK ... windows=1 ... depth_cap=32 ... count_s=0.050 fill_s=0.092 ...
             target_hits=16573 ...
sha256 b4753899592c952542f7416ee9ce5836e19b99971afa74d02281493b3a289ba5  (== the bank)
```

### 100 MB, windowed W=8 (`gate.py 100m final-mw 96 4096`)

`BIN_CAP_RAISED requested=4096 largest_bin=16803424` -> 8 windows.

```text
{"producer_rc": 0, "finish_rc": 0, "cmp_rc": 0, "producer_wall_s": 5.673}
BIN_WORK ... windows=8 ... depth_cap=32 ... count_s=... fill_s=0.485
             target_hits=16573 ...
sha256 b4753899592c952542f7416ee9ce5836e19b99971afa74d02281493b3a289ba5  (== the bank)
```

### 1 GB, single window (`gate.py 1b final-s 96 4294967296`)

```text
{"producer_rc": 0, "finish_rc": 0, "cmp_rc": 0, "producer_wall_s": 267.820,
 "total_wall_s": 382.167}
BIN_WORK ... windows=1 ... depth_cap=32 level_passes=45071881
         comparisons=3003089684 symbols=101111630965 ... fill_s=23.632 ...
         target_hits=213516 ...
xsa chi-rspace: chi = 283296933 (N=1000000665, R=368036609)
sha256 38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f  (== the bank)
```

(`fill_s=23.6` in that row is not the fill: the 1.31 TB pile histogram had just
left 873 GB of page cache, so first-touching the 16 GB arena pair dominated. The
same gate on a clean cache measured `fill_s=0.148`, and 0.092-0.121 at 100 MB.)

### 1 GB, windowed W=5 (`gate.py 1b final-mw 96 250000000`)

```text
{"producer_rc": 0, "finish_rc": 0, "cmp_rc": 0, "producer_wall_s": 208.954,
 "total_wall_s": 325.928}
BIN_WORK ... windows=5 ... depth_cap=32 level_passes=45071881
         comparisons=3003089684 symbols=101111630965 ... fill_s=1.298 ...
         target_hits=213516 ...
xsa chi-rspace: chi = 283296933 ...
sha256 38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f  (== the bank)
```

**Counter identity**: single-window and 5-window runs of the same corpus produce
the *same* work counters (`level_passes=45071881`, `comparisons=3003089684`,
`symbols=101111630965`, `target_hits=213516`) and the same bytes. The window
boundary cannot move a counter, which is what the canonical fill guarantees.

## 3. The fill wall: old vs new at 10 GB, 6 windows

Protocol identical to `RESULTS_BIN_DEPTH.md`: `--bin-probe` (construction only,
nothing written), 96 threads, `--bin-live-cap 2000000000` (6 windows), same
corpus, same box state, the two binaries run **back to back** (load 69-91 for
both):

| binary | fill_s | count_s | sweep_s | build_s | total_s | comparisons | symbols |
|---|---|---|---|---|---|---|---|
| `3399b2b` (serial fill) | **213.006** | 0.132 | 213.138 | 375.373 | 639.473 | 45,338,075,221 | 3,574,308,585,903 |
| `parallel-fill` | **4.045** | 0.142 | 4.187 | 340.407 | 388.173 | 31,158,576,235 | 3,361,984,132,500 |

* **fill: 213.006 s -> 4.045 s = 52.7x** (bar was >= 10x).
* **per-pass rate: 282 MB/s -> 14.8 GB/s** (6 passes x 10 GB / 4.045 s). On a
  quiet box, `--bin-fill-only` measured **21.1 GB/s** (2.839 s, load 12-20), i.e.
  **75x** the serial per-pass rate.
* the fill's share of the construction term: **33.3% -> 1.0%**.
* the construction term: 639.47 s -> 388.17 s (**1.65x**), of which the fill is
  4.0 s and the `--bin-depth-cap 32` default is most of the rest.

**Canonical identity at full scale.** The `dcap=16` replicate of the new binary
is counter-for-counter identical to the old one:
`windows=6 level_passes=330651797 level_elems=118446129067 bin_passes=330651797
comparisons=45338075221 symbols=3574308585903 cert_decided=28 ties=0
leaf_sorts=1107712459 leaf_elems=9360122938 peak_live_slots=3918468478
peak_live_bytes=31347747824 emitted_rows=10000000000`, and `BIN_HOT` identical
(`bin=32 rows=1659881407 span_s=186.958/199.442 cpu_s=3418.290/3431.402`). Only
`fill_s`, `count_s`, `sweep_s` and the wall move. The scatter places the same
positions in the same slots.

## 4. `--bin-depth-cap 32` is now the default

The bin-depth report gated it at fixture scale only. My 10 GB probe (n=1e10, 96t,
cap 2e9, `--bin-probe`) confirms the projection:

| dcap | comparisons | symbols | level_elems | leaf_sorts | build_s | hot-bin comparisons |
|---|---|---|---|---|---|---|
| 16 | 45,338,075,221 | 3,574,308,585,903 | 118,446,129,067 | 1,107,712,459 | 375.373 | 10,435,971,258 |
| 32 | 31,158,576,235 | 3,361,984,132,500 | 132,017,689,562 | 1,375,827,314 | 340.407 | 6,306,996,771 |

comparisons **-31.3%**, symbols -5.9%, level_elems +11.5%, hot-bin comparisons
-39.6%, build **-9.3%**. Adopted as `bit6/cross_lcp_merge.cpp:5453` (with the
measurement in the comment there), and re-gated at fixture scale: 100 MB single,
100 MB W=8, 1 GB single and 1 GB W=5 all `cmp_rc=0` with the banked shas.

## 5. The pile projection (1.3079 TB, measured, not extrapolated)

Anchors measured in this session:

| anchor | value |
|---|---|
| pile corpus `pile.txt` | n = 1,307,910,802,540 |
| **largest byte-1 bin** (space, `pf-binhist`, 64t, 296 s) | **217,236,904,897 = 0.166095 n** |
| device sequential read, O_DIRECT, 96 independent streams (`pf-iobench`) | **5.238 GB/s** (1 stream 4.72, 8 streams 5.12) |
| fill rate, warm page cache, 100 GB snapshot, 8 windows | 16.8 GB/s (51.203 s for 859 GB) |
| fill rate, RAM cached, 10 GB, 6 windows | 14.8-21.1 GB/s |
| serial fill rate | 282 MB/s |

### The fill term

One fill pass at pile = one 1.308 TB corpus read:

* at the device floor 5.238 GB/s: **249.7 s = 4.16 min** per pass;
* at warm-page-cache 16.8 GB/s: 77.9 s per pass (the pile's 1.31 TB cannot be
  cached, so treat this as the optimistic end);
* serial (282 MB/s): 1.29 h per pass — which is the 1.15-1.28 h/pass the
  bin-depth report banked.

| arena discipline | windows | fill OLD (serial) | fill NEW (device floor) | fill NEW (warm cache) |
|---|---|---|---|---|
| `--bin-live-cap 2e9` slots (the report's assumption) | 655 | 754-844 h | **45.4 h** | 14.2 h |
| `--bin-live-cap 65e9` slots ("RAM-limited") | 21 | 24.2-27.1 h | **1.46 h** | 0.45 h |
| as `bin_order` actually packs at pile scale | see below | — | — | — |

### The blocker the projection has to carry (found while measuring)

`bin_order` **raises** `liveCap` to the largest bin (`bit6/binbwt_slice.h:471`),
because a bin's arena slice must be contiguous. At pile the largest bin is
0.166095 n = 2.1724e11 slots, and the arena is a slice+scratch pair at 8 bytes
per slot, so a pile-scale run needs

```text
2 x 217,236,904,897 x 8 B = 3.48 TB of position storage
```

on a box with 1.007 TB of RAM. **The pile at 1.31 TB is not buildable in one
shard by this design**, independently of the fill. Measured evidence at 100 GB:
`BIN_CAP_RAISED requested=2000000000 largest_bin=17836546879`, 8 windows,
`peak_live_slots=35673093758 peak_live_bytes=285384750064` (285 GB, 28% of RAM).

So the launch table's shape is **sharded**, which is the shape the pipeline
already has (fragments -> pieces -> the finish protocol):

| shard size | shards for the pile | largest bin | arena pair | windows/shard | fill/shard (device) | fill total |
|---|---|---|---|---|---|---|
| 339 GB (900 GB arena budget) | 4 | 56.3e9 slots | 900 GB | 6-8 | 6.5-8.6 min | **26-34 min** |
| 263 GB (700 GB arena budget) | 5 | 43.7e9 slots | 700 GB | 6-8 | 5.0-6.7 min | **25-34 min** |

(`windows/shard` is 6-8 by measurement, not by arithmetic: 100 GB gave 8 where
`n/largest_bin` predicts 6, because small bins pack around a big one unevenly.)

### The whole pile, term by term

* **build 12.9 h** (unchanged in kind; the 10 GB rate is 340 s/10 GB with the new
  `dcap 32` default vs 355-375 s at `dcap 16`, so ~12.2-12.9 h);
* **new fill: 25-34 min** with the sharding the design actually requires, or
  1.46 h if a 65e9-slot arena is possible, or 14-45 h if someone pins 2e9 slots
  and accepts 655 windows. **The fill is no longer the term that breaks the
  pile**: at the discipline the code enforces it is ~0.5 h, and even at the
  report's 655-window assumption it is 45 h instead of 754 h;
* **stitch/finish 43.3 h** (untouched by this branch, and now the largest term);
* count sweep: 0.14 s per pass at 10 GB, i.e. minutes at pile.

So the pile construction is **12.9 h + ~0.5 h + 43.3 h ≈ 57 h** in the shape it
can actually be run (5 shards), against ~810 h before this branch — and the
launch table's real limiter is now the **arena capacity per shard** (3.48 TB
unsharded) and the **finish's 43.3 h**, not the fill.

## 6. Instruments added (and why they are safe)

* `--bin-fill-only` (`bit6/binbwt_slice.h:404`, `:644`, flag at
  `bit6/cross_lcp_merge.cpp:5458`): counts and fills every window exactly as the
  product does, then skips the refinement and the emission; writes nothing at all
  (it implies `--bin-probe`'s contract) and journals `sink=probe-fill-only`.
  Rows and the live-slot bound are still accounted, so `require(row == n)` and
  `peak_live_slots` still mean something. It is the only way to time the fill
  alone on a corpus bigger than the RAM budget — i.e. in the pile's regime.
* `CorpusText::ramCached()` (`bit6/cross_lcp_merge.cpp:1027`) and `seqSpan`
  (`bit6/binbwt_slice.h:443`): the two sequential readers (count sweep and fill)
  now borrow the pointer when the corpus is RAM cached (zero copy, unchanged
  speed) and take one bulk `read()` per span otherwise. Before this, a pooled
  (non-cached) view degraded a 1 MB span into one synchronous 256-byte pool-page
  fill per 256 bytes, because `span()`'s fast path is `len <= 128` inside one
  256-byte page — the comparator's path. At pile scale that would have been the
  fill's real cost. Verified on the 100 GB snapshot: `POOL_STATS fills=95
  hits=665` for the whole run.
* `pf-iobench.cpp` and `pf-binhist.cpp` live in `/mnt/nvme2n1/erikg/` (scratch),
  not in the tree: they are one-off measurements, not tools.

## 7. Residual risks

* **Walls are load-labelled.** Load average ran 12-91 throughout (other
  production work; a 100 GB bin-bwt producer; my own 1.31 TB histogram read that
  left 873 GB of page cache). The old/new 10 GB pair in §3 was run back to back
  under the same load, and the honest spread of the new fill term is 2.84-4.05 s
  at 10 GB with one 12.82 s outlier while the page cache was pressured; the
  52.7x claim uses the same-state pair.
* **The device floor is a 96-stream O_DIRECT benchmark**, not the fill itself on
  a cold 1.31 TB corpus. It brackets the pile number honestly (5.24 GB/s device,
  16.8-21.1 GB/s warm), but no run in this session did a cold-cache pile pass.
* **The pile cannot be built unsharded** (§5): 3.48 TB arena pair for a 1.31 TB
  corpus. That is a capacity property of the bin design, not of this change, and
  it decides the launch table more than any speed term. Whether the fragment
  protocol's existing shard boundaries can be set to ~263-339 GB per shard is a
  finish-side question this branch did not test.
* `xsa/runtime/bit6/binbwt_slice.h` is a stale 1,051-line snapshot of this
  header (already 662 diff lines behind before this branch). It is untouched;
  anything building against the runtime mirror does not get the parallel fill.
* The `--bin-probe`/`--bin-fill-only` paths use the per-row no-op emitter, so the
  fill's *target join* is not exercised by them (no sink). The join is covered by
  the 100 MB and 1 GB sink gates at W=8 and W=5 (16573 and 213516 targets, all
  conserved).
* `NT` is now up to 256 chunks: 256 threads x up to 256 write streams. Measured
  fine at 96 threads/6 windows/10 GB and at 96 threads/8 windows/100 GB, but a
  256-thread pile-scale run is not measured, and the offsets table is 512 KB.
* Everything inherited stands: the leaf LCE re-walks, the sink's ordered pass,
  and the finish term (43.3 h) which this branch does not touch.

## 8. Branch, commits, reproduction

Branch `parallel-fill`, base `pile-week-v1 @ 3399b2b`.

```text
c5e9e12 parallel-fill: the prefix-sum scatter (fill 211.1 s -> 4.35 s at 10 GB/6 windows)
58cef28 parallel-fill: CORRECTION - the target join was never broken; restore its per-window semantics
e1e1366 bin-depth-cap 32 is now the DEFAULT (confirmed at 10 GB, not just fixture scale)
bda89ec parallel-fill: bulk sequential reads when the corpus is NOT RAM cached + --bin-fill-only
```

```sh
c++ -O3 -std=c++17 -pthread bit6/cross_lcp_merge.cpp -latomic -o binbwt
c++ -O2 -std=c++17 -pthread tools/binbwt/oracle.cpp -latomic -o oracle
./oracle <fresh scratch>                                    # 36180 + 6432 cases

# gates (producer + finish + chi + cmp against the banked container)
BINBWT_BIN=<this tree> python3 tools/binbwt/gate.py 100m tag 96 4294967296   # 1 window
BINBWT_BIN=<this tree> python3 tools/binbwt/gate.py 100m tag 96 4096         # 8 windows
BINBWT_BIN=<this tree> python3 tools/binbwt/gate.py 1b   tag 96 4294967296   # 1 window
BINBWT_BIN=<this tree> python3 tools/binbwt/gate.py 1b   tag 96 250000000    # 5 windows

# the fill term alone, at any scale (nothing written)
binbwt --binbwt <corpus> --remap <remap> --out-prefix /tmp/ignored \
       --threads 96 --bin-live-cap 2000000000 --bin-fill-only
```
