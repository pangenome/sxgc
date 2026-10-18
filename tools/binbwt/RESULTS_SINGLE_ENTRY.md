# single-entry: the bin shape becomes the v2 pipeline (one command, end to end)

Base `pile-week-v1 @ b6b1531`. Branch `single-entry`. The bin architecture was
gated and fast but ran as TWO manual binary invocations (`binbwt --binbwt` then
`pile_finish`). It is now ONE entry:

```sh
xsa build --v2-pipeline --input <corpus> --scratch <dir> --threads N \
          --memory-gb N --bin-live-cap SLOTS --timeout S --chi-expect N
```

which drives sweep -> bins -> stitch -> decomposed finish -> container, with the
chi line, the phase journal and the work counters exactly as the old pipeline
produced them. There is no chunk phase and no merge walk on this route.

## 1. The routing and the knob surface

`xsa/src/chain.rs`

* `:958` - the v2 route creates **only** `merged/` (no `chunks/`, `mwork/`,
  `finish/`): the bin shape has no chunk set, no merge work directory and no
  finish side-stream directory.
* `:1135` `run_v2_tail` - spawns the two stages and nothing else:
  * `:1215` producer: `cross_lcp_merge --binbwt <corpus> --remap <map>
    --out-prefix <merged/frag> --threads T --bin-live-cap CAP`
    (`binbwt_main`, `bit6/cross_lcp_merge.cpp:5412`, reached through
    `bit6/binbwt_slice.h`), under `timeout -k 120` + `ulimit -v`;
  * `:1228` consumer: `pile_finish --prefix --corpus --remap --output
    --threads --poll-timeout --wit-ram-budget [--chi-expect]`;
  * both spawned before either is joined (the finish polls the piece directory
    from row zero), and a dead producer kills the finish loud.
  The producer env is `CROSS_V2_PIECES=1`, `CROSS_V2_PIECE_WINDOW` (default
  1100) and a sized `CROSS_POOL_SLOTS` corpus-text page pool - the pool is what
  keeps the placing pass off the file-window path (77x measured there).
* `:349` `--bin-live-cap SLOTS` - THE new knob: the position-arena bound, one
  slot per admitted position, 16 B/slot of anonymous RAM, `0` = unbounded (one
  window). The executor runs the **plan's resolved cap** (`run_v2_tail`'s
  `bin_live_cap` argument comes from `build_plan.v2`), never its own guess, so
  the journal's first entry and the run cannot disagree.
* Still live knobs: `--threads` (bin build fan-out + finish pool),`--memory-gb`
  (the RLIMIT_AS on every phase, and the default source of the arena cap),
  `--wit-ram-gb` (the finish's witness budget; refused above `--memory-gb`),
  `--timeout` (unchanged: required for execution, standing policy is a huge
  value so no phase may be killed by it).
* **Inert knobs refuse loud** (`:401`..`:410`): `--chunks`, `--chunk-mb`,
  `--kway`, `--fanin` under `--v2-pipeline`; `CROSS_V2_PRODUCERS>1` and
  `CROSS_V2_SPOOL_BYTES` (no segment spools exist); `CHUNK_POS_MODE` /
  `CROSS_POS_MODE` (there is no position column of any kind). The remaining
  merge-pipeline env (`CROSS_WM_PACK`, `CROSS_DELETE_CONSUMED`, `CROSS_SHARDS`,
  `CROSS_HASH_K`, `CROSS_ASYNC_FILL`) is journaled `... is INERT under the bin
  shape (merge-pipeline knob; ignored)` rather than silently passed through.
  `XSA_BUILD START` prints `chunks=- kway=-` on this route.
* Gates, all fail-loud (`:1255`..): `V2_BIN_CAP_RAISED` (a single bin larger
  than the cap makes the producer raise it - that means the plan priced a
  different arena, so the run is refused, not accepted), sweep conservation
  `sweep_bytes == (1+windows)*n`, `emitted_rows == n`, `full_SA=0
  persistent_positions=0`, `sink=parallel-v2`, plus the inherited chi /
  `SXI2_PASS` / `ENDPOINT_STREAM_PASS` / `PILE_FINISH_RLEBWT_DELETED` / tree
  rules. `:1290` journals `V2_BIN_WINDOW_DELTA` when the run used more windows
  than ceil(n/cap) (bin granularity; the plan's fill term is then a lower
  bound, said loudly), and refuses an under-count.
* Counters through the front door: `:1265` `V2_BIN_WORK BIN_WORK ...` verbatim,
  `:1302` every `SLIM_FP` / `SLIM_SEEDS` / `PILE_FINISH_PFCK` line from the
  finish verbatim.
* Provenance: `:1109` `journal_bundle_provenance` writes `BUNDLE_TOOL
  name=... path=... bytes=... sha256=... verified=1` for `cross_lcp_merge` and
  `pile_finish`, so a journal attests which binaries ran.

## 2. The plan model (plan.rs): the bin shape, no chunk/merge term left

`xsa/src/plan.rs:485-540`, every constant a measured rate on the 1 GB fixture
(1,000,000,665 B), ns per corpus byte:

| term | value | source |
|---|---:|---|
| `BIN_COUNT_NS_PER_BYTE` | 1.36 | banked count pass (v2 measured 0.043 s/GB; the banked figure kept, i.e. conservative) |
| `BIN_FILL_FIRST_NS_PER_BYTE` | 5.084 | single-window placing pass incl. packing + target join (v2, 1 GB) |
| `BIN_FILL_WINDOW_NS_PER_BYTE` | 3.535 | marginal per EXTRA window = v1 banked 2.87 s/GB x 1.232 (v2 packing+join) |
| `BIN_BUILD_POINTS` | 107.93@8t, 35.86@32t, 23.489@96t | build critical path, s per 1 GB, log2-interpolated and clamped (memory-bandwidth bound above 96) |
| `BIN_STITCH_NS_PER_BYTE` | 11.408 | the ONE ordered stitch thread (11.41 ns/row), thread-insensitive by construction |
| `FINISH_DECOMP_NS_PER_BYTE` | 118.9 | decomposed finish own work (141.68 s - 22.74 s pipe wait) |
| `BIN_ARENA_BYTES_PER_SLOT` | 16 | arena + scratch, 8 B each - anonymous RAM, 0 B of disc |

`bin_windows(n, cap) = ceil(n/cap)` (`:516`, unit-tested against the measured
points: 1 GB cap 250M -> 5, 1 GB cap>=n -> 1, pile cap 20G/65G/131G -> 66/21/10).
`producer = count + fill(first + (W-1)*marginal) + build(threads) + stitch`;
`finish = 118.9 x n`; `e2e = max(producer, finish)` because the two stages run
concurrently and the wire is flow-controlled. The plan refuses loud with the
arithmetic alternative when the arena pair exceeds 90% of `--memory-gb`, when
the tree peak does not fit, or under the scratch hygiene policy.

The chunk/merge constants are **deleted** (`V2_CHUNK_S_PER_MB`,
`V2_MERGE_WALK_S_PER_MB`, `V2_EMIT_FINISH_S_PER_MB`, `V2_CONTAINER_S_PER_MB`,
`chunk_set_gb`, `merge_transient_gb`); the merge path stays in the binary behind
`--radix` for A/B and is priced by `model()`/`model_fanin()`, not by the v2 plan.

Rendered (sparse placeholder corpora - the plan is a function of n and the box;
`--plan-only` never reads the corpus):

```text
##### 1gb (auto cap from --memory-gb 200)
XSA_PLAN_V2_BIN live_cap_slots=1000000665 windows=1 arena_pair_gb=14.9 count_s=1 fill_s=5 build_s=23 stitch_s=11 producer_s=41 finish_s=119
XSA_PLAN_V2_BIN_END e2e_s=119 e2e=0:01:58 producer=0:00:41 finish=0:01:58 rule=max(producer,finish) (concurrent, flow-controlled through the piece window)
XSA_PLAN_DISC FEASIBLE: ALL-PHASE peak 3.6 GiB vs 2588.9 GiB free (bin shape: rlebwt + piece window + container; the position arena is RAM)
##### 10gb (auto cap from --memory-gb 200)
XSA_PLAN_V2_BIN live_cap_slots=10000000000 windows=1 arena_pair_gb=149.0 count_s=14 fill_s=51 build_s=235 stitch_s=114 producer_s=413 finish_s=1189
XSA_PLAN_V2_BIN_END e2e_s=1189 e2e=0:19:49 producer=0:06:53 finish=0:19:49
##### 100gb (auto cap from --memory-gb 200)
XSA_PLAN_V2_BIN live_cap_slots=11408506880 windows=9 arena_pair_gb=170.0 count_s=136 fill_s=3336 build_s=2349 stitch_s=1141 producer_s=6962 finish_s=11890
XSA_PLAN_V2_BIN_END e2e_s=11890 e2e=3:18:10 producer=1:56:02 finish=3:18:10
##### pile = 1.31 TB (auto cap from --memory-gb 200 -> 11.41G slots, W=115)
XSA_PLAN_V2_BIN live_cap_slots=11408506880 windows=115 arena_pair_gb=170.0 count_s=1782 fill_s=534577 build_s=30771 stitch_s=14944 producer_s=582074 finish_s=155759
XSA_PLAN_V2_BIN_END e2e_s=582074 e2e=161:41:13 producer=161:41:13 finish=43:15:59
XSA_PLAN_DISC INFEASIBLE: ALL-PHASE peak 3208.2 GiB vs 2588.9 GiB free
XSA_PLAN_ALTERNATIVE free scratch for the all-phase bin-shape peak
```

The pile number is cap-driven and says exactly what the projection script says -
**the windowed placing pass is the term to buy down with the arena**, and the
finish is 43.3 h regardless:

```text
##### pile --bin-live-cap 20000000000 --memory-gb 362   (the projection script's first row)
XSA_PLAN_V2_BIN live_cap_slots=20000000000 windows=66 arena_pair_gb=298.0 count_s=1782 fill_s=307665 build_s=30771 stitch_s=14944 producer_s=355162 finish_s=155759
XSA_PLAN_V2_BIN_END e2e_s=355162 e2e=98:39:21 producer=98:39:21 finish=43:15:59
##### pile --bin-live-cap 65000000000 --memory-gb 1032
XSA_PLAN_V2_BIN live_cap_slots=65000000000 windows=21 arena_pair_gb=968.6 count_s=1782 fill_s=99277 build_s=30771 stitch_s=14944 producer_s=146774 finish_s=155759
XSA_PLAN_V2_BIN_END e2e_s=155759 e2e=43:15:59 producer=40:46:13 finish=43:15:59
XSA_PLAN_RAM INFEASIBLE: position arena pair ~968.6 GiB ... vs cap 1032 GiB per phase
XSA_PLAN_ALTERNATIVE lower --bin-live-cap (the arena pair is 16 B/slot: 62330712882 slots would fit 90% of --memory-gb 1032) or raise --memory-gb
##### pile --bin-live-cap 131000000000 --memory-gb 2016
XSA_PLAN_V2_BIN live_cap_slots=131000000000 windows=10 arena_pair_gb=1952.1 count_s=1782 fill_s=48338 build_s=30771 stitch_s=14944 producer_s=95834 finish_s=155759
XSA_PLAN_V2_BIN_END e2e_s=155759 e2e=43:15:59 producer=26:37:14 finish=43:15:59
```

So: 1 window at pile is impossible (arena 16n = 19.5 TB), 10 windows needs
1.95 TB of RAM, and at the widest arena the **finish's 43.3 h** is the end-to-end
term - the same headline `RESULTS_FINISH_DECOMP.md` lands on, now produced by one
command's own plan. (The 20G-slot row's 98.6 h is the honest cost of the smaller
arena: 66 windows x 3.535 s/GB of placing plus the first-window term, matching
`pile_projection_v2.py`'s v2 fill within 1%.)

Misconfiguration refusals, verbatim:

```text
$ xsa build --v2-pipeline ... --chunks 32
xsa: error: build: --chunks is INERT under --v2-pipeline: the bin shape has no chunk phase (the producer sweeps the corpus itself); drop the flag
$ xsa build --v2-pipeline ... --kway 8
xsa: error: build: --kway is INERT under --v2-pipeline: the bin shape has no k-way walk (bins are ordered by their own construction); drop the flag
$ xsa build --v2-pipeline --plan-only ... --memory-gb 6 --bin-live-cap 4000000000
XSA_PLAN_RAM INFEASIBLE: position arena pair ~14.9 GiB (16 B/slot x 1000000665 slots, W=1, --bin-live-cap 4000000000 exceeds n=1000000665: capped at n (one window)) vs cap 6 GiB per phase; ...
XSA_PLAN_ALTERNATIVE lower --bin-live-cap (the arena pair is 16 B/slot: 362387865 slots would fit 90% of --memory-gb 6) or raise --memory-gb
```

## 3. Runtime mirror and provenance (the front door really runs these binaries)

The packaged mirror was **stale**: `xsa/runtime/bit6/cross_lcp_merge.cpp` was
pre-bin-bwt (55,151 lines vs 55,577), `pile_finish.cpp`/`slim_lce.hpp` were
pre-finish-decomposed, and `bit6/binbwt_slice.h` was **not packaged at all**
(the `.h` was outside `tools/package_xsa_sources.py`'s patterns), so the
packaged producer could not have been built. Fixed: the pattern now includes
`bit6/binbwt_slice.h`, the mirror was rebuilt from THIS tree, and
`xsa/SOURCES.sha256.json` regenerated (58 entries, every digest verified).

Byte-identical rebuild of the bundled stages from this tree's sources:

```text
g++ -O2 -mcx16 -std=c++17 -pthread bit6/cross_lcp_merge.cpp -latomic
  -> ecb7b5b57eda08c62da5164ebf01fcf395d1aa94a2229d387387b2da5c2e81e9  == the bundled cross_lcp_merge
SXI_DEPS=... bash tools/build_slim_dump.sh out bit6/pile_finish.cpp
  -> 896ced7ac80c432e6ae4bb310b801a56461b6f10ef69d18f529587e18a1ac5b1  == the bundled pile_finish
```

and the run journals them itself:

```text
BUNDLE_TOOL name=cross_lcp_merge path=/home/erikg/.cache/xsa/d10938a1.../cross_lcp_merge bytes=818824 sha256=ecb7b5b5... verified=1
BUNDLE_TOOL name=pile_finish path=/home/erikg/.cache/xsa/d10938a1.../pile_finish bytes=11957448 sha256=896ced7a... verified=1
```

## 4. Gates, verbatim, through the ONE entry

100 MB fixture, 96 threads, `--memory-gb 24`:

```text
$ xsa build --v2-pipeline --input .../mergperf-100m-snap.txt --scratch /mnt/nvme2n1/erikg/se-100m \
      --threads 96 --memory-gb 24 --timeout 7200 --chi-expect 29716349
PHASE bin-v2 END rc=0 wall=0:04.13 peak_rss_kb=1750160 disc_scratch_bytes=588178069
PHASE finish-v2 END rc=0 wall=0:15.54 peak_rss_kb=468364 disc_scratch_bytes=48791578
V2_BIN_WORK BIN_WORK dollar=1 threads=96 level_bytes=1 leaf=24 depth_cap=16 freq_sched=0 split=0 bins=256 windows=1 sweep_bytes=200001006 sweep_passes=2 level_passes=3772691 level_elems=835675235 bin_passes=3772691 comparisons=319751186 symbols=7214408208 cert_decided=14 ties=0 leaf_sorts=13600060 leaf_elems=91943055 peak_live_slots=200001006 peak_live_bytes=1600008048 emitted_rows=100000503 ... runs=38647515 bands=590 target_hits=16573 band_jobs=590 writers=8 ... full_SA=0 persistent_positions=0
V2_FINISH_COUNTER SLIM_FP pile-finish-corpus ... queries=49845539 verified_symbols=1176027277 ...
V2_CHI_EXPECT_OK chi=29716349 == 29716349
V2_CONTAINER_PUBLISHED ... bytes=48768685
XSA_BUILD_DONE chi=29716349 pipeline=v2bin container=.../mergperf-100m-snap.sxi2
cmp container /mnt/nvme2n1/erikg/extcols-scratch/xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2  -> rc=0
sha256 b4753899592c952542f7416ee9ce5836e19b99971afa74d02281493b3a289ba5   (== the bank)
```

1 GB fixture, 96 threads, `--memory-gb 40` (the arena pair is 14.9 GiB):

```text
$ xsa build --v2-pipeline --input .../mergperf-1b-snap.txt --scratch /mnt/nvme2n1/erikg/se-1b \
      --threads 96 --memory-gb 40 --timeout 7200 --chi-expect 283296933
PHASE bin-v2 END rc=0 wall=1:57.02 peak_rss_kb=16715036 disc_scratch_bytes=2721974060
PHASE finish-v2 END rc=0 wall=2:41.54 peak_rss_kb=4916828 disc_scratch_bytes=467854128
V2_BIN_WORK BIN_WORK dollar=1 threads=96 level_bytes=1 leaf=24 depth_cap=16 freq_sched=0 split=0 bins=256 windows=1 sweep_bytes=2000001330 sweep_passes=2 level_passes=37097322 level_elems=10179725481 bin_passes=37097322 comparisons=3612105756 symbols=112713502363 cert_decided=68 ties=0 leaf_sorts=127119193 leaf_elems=925152140 peak_live_slots=2000001330 peak_live_bytes=16000010640 emitted_rows=1000000665 ... runs=368036610 bands=5616 target_hits=213516 band_jobs=5616 writers=8 ... full_SA=0 persistent_positions=0
V2_BIN_WINDOWS windows=1 plan_windows=1 peak_live_slots=2000001330 peak_live_bytes=16000010640
V2_FINISH_COUNTER SLIM_FP pile-finish-corpus tau=22 bytes=363636608 queries=473695414 verified_symbols=12820424563 ...
V2_FINISH_COUNTER SLIM_FP pile-finish-seam tau=22 bytes=363636608 queries=4038381 verified_symbols=29271872 ...
V2_CHI_EXPECT_OK chi=283296933 == 283296933
PEAK_DISC_USED_BYTES 2729868633
XSA_BUILD_DONE chi=283296933 pipeline=v2bin container=.../mergperf-1b-snap.sxi2
cmp container /mnt/nvme2n1/erikg/extcols-scratch/pilev2-g2-1b/frag.sxi2  -> rc=0
sha256 38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f   (== the bank)
```

Multi-window through the same entry (the window arithmetic and the arena
discipline, not just W=1) - 100 MB at `--bin-live-cap 30000000`, `--memory-gb 8`:

```text
$ xsa build --v2-pipeline --input .../mergperf-100m-snap.txt --scratch /mnt/nvme2n1/erikg/se-100m-w4 \
      --threads 96 --memory-gb 8 --bin-live-cap 30000000 --timeout 3600 --chi-expect 29716349
V2_BIN_WINDOWS windows=4 plan_windows=4 peak_live_slots=59899654 peak_live_bytes=479197232
  (sweep_bytes=500002515 == (1+4)*100000503, the gate's conservation check)
V2_CHI_EXPECT_OK chi=29716349 == 29716349
comparisons=319751186 symbols=7214408208      (identical to the single-window run)
cmp container <bank> -> rc=0
```

and the misconfiguration the plan cannot see is refused at the front door rather
than accepted (a cap below the largest bin, exit code 1):

```text
$ xsa build --v2-pipeline ... --bin-live-cap 1000 --memory-gb 8
xsa: error: build: V2_BIN_CAP_RAISED: --bin-live-cap 1000 is below the largest bin; the producer raised it
(BIN_CAP_RAISED requested=1000 largest_bin=16803424). The plan priced a different arena - raise --bin-live-cap
(and --memory-gb if needed) and re-run
```

Walls, labelled: the box carried foreign work throughout (`analysis` lanes plus
the neighbouring 100 GB run's tail); `loadavg 72-125` at every gate, **no
quiet-box claim**. 100m: one-entry 16.14 s wall (bin 4.13, finish 15.54, they
overlap); 1 GB: one-entry **162.35 s** wall (bin 117.0 s of which 67.6 s was
`flow_wait` on the finish, finish 161.5 s; own producer work ~48 s).
Direct-path walls for the same work: 16.10 s and 166.63 s. The "with finish"
figure the chartered bin gates recorded was 142.18 s at load ~50; this run is
within 14% of it at double the load, and the whole difference is the finish's
verification contention (unchanged machinery).

## 5. Counter identity: direct path vs front door

`tools/binbwt/single_entry_gate.py` runs the same bundled binaries by hand (the
direct path); `tools/binbwt/single_entry_compare.py` diffs 20 `BIN_WORK` fields
and 9 finish fields x 2 labels (corpus, seam), never a wall:

```text
100m: COUNTER_IDENTITY_PASS fields=20 finish_fields=9*2
  BIN_WORK comparisons=319751186 symbols=7214408208 emitted_rows=100000503 peak_live_slots=200001006 runs=38647515 bands=590
1b:   COUNTER_IDENTITY_PASS fields=20 finish_fields=9*2
  BIN_WORK comparisons=3612105756 symbols=112713502363 emitted_rows=1000000665 peak_live_slots=2000001330 runs=368036610 bands=5616
```

And the 1 GB front door reproduces the **banked direct gate** (the
`finish-decomposed`/`bin-bwt v2` runs, `/mnt/nvme2n1/erikg/binbwt-gate-1b-fdF`)
field for field:

```text
banked fdF : comparisons=3612105756 symbols=112713502363 cert_decided=68 ties=0
             leaf_sorts=127119193 leaf_elems=925152140 peak_live_slots=2000001330
             level_passes=37097322 level_elems=10179725481 bin_passes=37097322
             runs=368036610 bands=5616 target_hits=213516
front door : comparisons=3612105756 symbols=112713502363 cert_decided=68 ties=0
             leaf_sorts=127119193 leaf_elems=925152140 peak_live_slots=2000001330
             level_passes=37097322 level_elems=10179725481 bin_passes=37097322
             runs=368036610 bands=5616 target_hits=213516
```

The 100 MB front door also reproduces the current-vintage banked 100 MB direct
gate (`binbwt-gate-100m-fdF`) field for field - `comparisons=319751186
symbols=7214408208 level_elems=835675235 leaf_sorts=13600060
leaf_elems=91943055 peak_live_slots=200001006`. (The 100 MB row printed in
`RESULTS.md` carries the older v1 attestation's `level_elems`/`leaf_sorts`
bookkeeping: the LCP work - comparisons and symbols - is byte-identical, only
the leaf/level boundary accounting changed with the parallel-sink branch.)

Finish counters likewise unchanged: `queries=473695414
verified_symbols=12820424563` corpus + `29271872` seam, with `hash_probes`,
`verification_work`, `hash_probe_charges`, `suffix_symbol_reads` and
`total_work` all equal, and 100m `queries=49845539
verified_symbols=1176027277` - the exact numbers `RESULTS_FINISH_DECOMP.md`
quotes.

Rust unit tests (14, all green, `cargo test --release` in the real tree): the window arithmetic against every measured
point, the build-curve interpolation/clamping/monotonicity, the four inert-knob
refusals, `--bin-live-cap` scoping, the witness-budget refusal and the
still-required `--timeout`.

## 6. Residual risks

* **The placing pass is the pile's binding producer term and it is
  load-sensitive** (3.8-188 s per 1 GB pass depending on load, per
  `pile_projection_v2.py`). The plan prices it at the quiet-box rate and says
  so; at pile it is 48-308 h depending on the arena, and every one of those
  hours assumes a quiet box that this box is not.
* **The finish is the end-to-end term at any wide arena** (43.3 h projected) and
  it is verification-throughput-bound; the model is a 1 GB, page-cache-resident
  rate applied to a 1.31 TB corpus whose text cannot be RAM-resident - the same
  77x caveat the bin report carries. The plan journals it as the weakest number
  but does not model a device-read penalty.
* **Windows are an arithmetic lower bound.** Reachable only as
  `ceil(n/cap)` when bins pack well; a dominant bin makes the producer use more
  windows (journaled as `V2_BIN_WINDOW_DELTA`, the plan's fill term then a lower
  bound) and a single bin above the cap is refused (`V2_BIN_CAP_RAISED`). The
  plan cannot SEE the first-byte histogram without reading the corpus.
* **The arena is the producer's whole RAM term** and the plan refuses above 90%
  of `--memory-gb`; the page pool, the text window buffers and the sink's band
  buffers are bounded but not summed into that check, so a run at exactly the
  limit could still die on `RLIMIT_AS` (it dies loud, mid-window, not silently).
* `--plan-only` renders against a placeholder corpus of the right SIZE for the
  10 GB/100 GB/pile rows here (the plan is a function of n and the box, and
  never reads the corpus); a real corpus can differ only through the histogram
  point above.
* The finish's `--poll-timeout` is tied to `--timeout`; a small `--timeout`
  would now kill the finish's poll before the producer's bands land. The
  standing policy (huge values) is journaled, not enforced.
* Inherited, unchanged: `--bin-level-bytes 2` is 10x worse; LCE leaf re-walking
  (112.7 symbols/byte); leaf 1 GB = 16 GB arena; the file-window path is 77x
  slower (exactness-gated, `CROSS_NO_POOL=1`).

## 7. Branch, commits, reproduction

Branch `single-entry`, base `pile-week-v1 @ b6b1531`.

```sh
cd xsa && cargo build --release            # rebuilds + verifies the stage bundle
python3 ../tools/binbwt/single_entry_gate.py front  100m /mnt/nvme2n1/erikg/se-100m 96 24
python3 ../tools/binbwt/single_entry_gate.py front  1b   /mnt/nvme2n1/erikg/se-1b   96 40
XSA_BUNDLE=<the cache dir the journal's BUNDLE_TOOL line names> \
python3 ../tools/binbwt/single_entry_gate.py direct 1b   /mnt/nvme2n1/erikg/se-direct-1b 96 40
python3 ../tools/binbwt/single_entry_compare.py /mnt/nvme2n1/erikg/se-1b /mnt/nvme2n1/erikg/se-direct-1b
xsa build --v2-pipeline --plan-only --input <corpus> --scratch <dir> --memory-gb 200 --threads 96
```

Scratch roots are all fresh and named `se-*`; the live 100 GB run
(`/mnt/nvme2n1/erikg/binbwt100gb`), the queued 10 GB run
(`/mnt/nvme2n1/erikg/binbwt10gb`) and the `fd-build`/`binbwt-v2-build`
binaries were never opened or modified.
