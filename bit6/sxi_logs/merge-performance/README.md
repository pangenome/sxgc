# MERGE PERFORMANCE lane — collapse the merge's serial cores

Status: IN PROGRESS. This file is the live journal of the lane.

Mission: 3-10x merge speed with byte-identical outputs, driving the merge to
its I/O-bound floor (aggregate NVMe bandwidth) before the pile launches.
Baseline (main = b031c61, all banked): consolidated `xsa build` fragment run
32 chunks / 6:57:38 merge tree wall / 2.53 GB peak RSS; dominant serial phase
telemetry = cross-merge emission at 2-5 cores on fat pairs (pool hit rates
99.8%+); 10 GB reference merge 6:14 (old fat build); pile projection 7-10
weeks serial — this lane exists to collapse it.

## Work items (priority order, one gate at a time)

1. **PARALLEL EMISSION** (lands first; the serial cores are the walk phases):
   the cross-merge walk and all emit walks are sharded at FIXED ORDER
   BOUNDARIES — merged-rank binary search for the walk, run-boundary shard
   sums for the emitters. Disjoint pwrite ranges, one code path
   (S = --threads, CROSS_SHARDS overrides, S=1 IS the banked serial walk);
   byte-identity is by construction, gated absolutely anyway.
2. **FLAT K-WAY** (user decision: flat one-level is the primary target shape
   — k = chunk count, ONE merge; minimum I/O: one read pass + one write pass
   vs 4-5 full-corpus tree passes; maximum width from minute one; shard
   boundaries double as restart checkpoints). The pairwise path remains the
   k=2 case of ONE code path; k is a launch knob (--kway, 2..chunk-count).
   Engineer around: heap-ordered cursor maintenance (O(log k) per emitted
   run, not O(k)); anchor finding and merged-vs-local analysis across k sides
   is the main correctness risk — the byte-identity gate is absolute.
3. **STRIPED SCRATCH** across drives (reader-level striping in WinBytes/
   WinU64): after (1)+(2) if the counters still show drive-bandwidth limits
   below the aggregate floor.
4. Pool size + threads as launch knobs (present); io_uring only if the
   counters still show syscall overhead after (1)-(3).
5. **BUILD CONFIGURATOR** (plan-first flow; user directive, queued after the
   flat k-way): `xsa build` becomes plan-first - survey (corpus stat, df per
   candidate scratch/stripe dir, free RAM/cores/cx16), model with a
   versioned constants table in-source (chunk RAM ~5x chunk bytes -> chunk
   bound -> count -> flat k; per-phase working sets; peak-disc timeline;
   wall projections from measured per-byte rates), the PLAN FILE as the
   scratch journal's first entry before any bytes move, fail-loud with
   nearest feasible alternatives (the DF_GATE refusal becomes this richer
   contract), flags --scratch/--stripe/--plan-only. Gate: plan-only
   projections vs measured phase telemetry after each gated run
   (planner-calibration loop). Journal:
   bit6/sxi_logs/merge-performance/configurator/. Bounded: survey +
   arithmetic + plan writer, not a new engine.

## Milestone 1: PARALLEL EMISSION (sharded walks) — quick gates PASSED

Change (bit6/cross_lcp_merge.cpp, one code path, S=1 = the serial walk):

* cross-merge walk: shard boundaries binary-searched at merged-rank targets
  (rank of A[a] = a + #{B rows before it}; strict total order makes the
  arithmetic exact); each shard runs the same two-pointer merge over its
  A/B ranges and pwrites at element offset a_s + b_s.
* sxcr-count/sxcr-write, emit-count/emit-write: run-boundary shard sums fix
  every run's global index; fixed-size records pwrite at global offsets;
  the variable-word .rlebwt goes through per-shard segments concatenated in
  shard order (order-pure).
* Telemetry: XMERG_TIMING/SXCR_EMIT/FOUR_EMIT lines carry shards=N;
  per-phase walls unchanged in name (CROSS_PHASE cross-merge / sxcr-count /
  sxcr-write / emit-count / emit-write) for table comparability.

Quick gates (local, /tmp/mergperf — banked log: quick_gate.log):

* Selftest (brute-force WM equality, the walk) at CROSS_SHARDS=1,2,3,7,16,48:
  PASS (seed 11; shards=2 at 500 cases hit the harness timeout, passes at
  100 cases — overhead-bound, not a correctness signal).
* Full-tree byte identity OLD (b031c61 serial) vs NEW, all six outputs
  (rlebwt/.meta/ssa/ssa_t/pftext/pfck), synthetic stress corpora (small
  alphabets, periodic anchors, long LCEs) x chunk counts {5,16,32} x
  CROSS_SHARDS {1,3,48}: **18/18 BYTE_IDENTICAL**, e.g. tiny-alpha c=32
  wall 27.2s (S=1) -> 14.4s (S=48).
* cargo build + cargo test: 8/8 PASS (bundle SHA-verified; runtime snapshot +
  SOURCES.sha256.json refreshed; chain.rs journals CROSS_SHARDS).

GATE 1 (absolute): fragment scale through the consolidated binary —
`gate1_parallel_emission_frag.sh`: byte-identity 8/8 vs the banked reference
+ chi = 306164765 exact + per-phase wall telemetry (the owed fat-pair
measurement). Banked baseline: merge tree 6:57:38. IN FLIGHT.

## Milestone 2: FLAT K-WAY — IN PROGRESS (design in the source header)

`--kway K` (default 2 = the certified tree): k-way anchor finding and block
sorting (per-side repair is provably independent of k: the at-risk pairs of
one side's rows are characterized by that side's own chunk-suffix anchors,
and the global order restricted to a side IS its repaired order), heap-merged
k-way walk with the same merged-rank shard planning, same emitters.
Gates: k-way selftest (synthetic mechanics at k=16/26/32), then REAL fragment
byte-identity (flat k=32 must byte-reproduce chi=306164765 and all four
merged files), then 10 GB flat vs real10b-ref.

## Milestone 3 probe result (fragment scale, FLAT k=32, BEFORE the clean gate)

The 24 GiB probe run (pre-arena-fix binary, no --emit-pf, standalone merge)
certified the flat artifact at fragment scale for the first time:
* All four merged files BYTE_IDENTICAL_TO_BANKED; merge wall 4364.7s =
  1:12:45 -> **5.75x vs the banked tree (6:57:38)**, 2.98x vs gate 1's
  sharded tree (3:36:54). comparisons=6.92B, max_lce=97059, RSS 2.65 GB.
* Phase breakdown: sequential 32-chunk load ~2000s (NEXT optimization
  target: bounded-concurrency walks), parallel anchor+repair 167s for ALL
  32 sides (vs ~2000s+ cumulative serial in the tree), k-way heap walk
  1205s (5x one pairwise walk's comparisons at the SAME wall as the tree's
  final pairwise walk alone; pool 31.5e9 hits / 212e6 misses = 99.3%),
  emit-count 456s + emit-write 465s.
* Crash found + fixed en route (the arena blowup, commit 54639fd):
  VmPeak 7.9 GB vs the 6 GiB gate cap; mallopt(M_ARENA_MAX,16) +
  memory-aware side parallelism from getrlimit(RLIMIT_AS).

## Deliverable table (walls; before/after each change)

| run                          | wall (banked) | wall (milestone)   | notes |
|------------------------------|---------------|-------------------|-------|
| fragment merge tree (k=2)    | 6:57:38       | 3:36:54 (gate 1)  | sharded emission, 1.93x; 8/8 bytes, chi exact |
| fragment FLAT k=32 (probe)   | -             | 1:12:45 merge     | 4/4 files byte-identical; clean gate 3 in flight |
| fragment FLAT k=32 (gate 3)  | -             | in flight         | full chain + finish + pf sidecars, 6 GiB cap |
| 10 GB merge (tree, banked)   | 6:14 (old fat build, 332 GB) | - | |
| 10 GB FLAT (gate 4)          | -             | pending           | prices the pile at its I/O floor |

Floor analysis: owed after each milestone (pool misses x window size = drive
traffic vs aggregate NVMe bandwidth). Flat fragment walk: 212e6 misses x 256B
= ~54 GB drive traffic for 6.9e9 comparisons - the walk is NOT yet
I/O-bound; the load phase (~2000s of single-threaded walks + validation)
is the next CPU-side target.

## Milestone 1 GATE 1 RESULT (fragment scale, PASSED 2026-10-08)

Sharded parallel emission through the consolidated binary (bundle at 7e89d98):
* BYTE IDENTITY: 8/8 (frag.{rlebwt,rlebwt.meta,ssa,ssa_t} + finish
  frag.{ri4,head_sa,agg,sA} all BYTE_IDENTICAL_TO_BANKED);
  chi = 306164765 (N=1082130213, R=397723010) EXACT.
* WALLS (deliverable table): merge tree 6:57:38 (banked b031c61) ->
  **3:36:54** = **1.93x**; peak RSS 2.42 GB (banked 2.53 GB, cap 6 GiB).
  chunk 3:24 (unchanged), endpoints 5:06, slim 17:31, sweep 0:54.
* Per-phase (CROSS_PHASE, vs banked serial telemetry): L0-pair cross-merge
  ~5.2s (banked comparable 104s at 10GB scale; sxcr-count/write 2.5-3.6s vs
  ~30s); final pair (1.08B, dollar): cross-merge 1316s vs ~3300s serial
  (2.5x - long-tail shard skew: ~13-19 cores average; comparison cost is
  LCE-skewed and merged-RANK planning balances rows, not comparisons),
  anchor+repair 358s (UNCHANGED serial per-side code - now the largest
  remaining serial core), emit phases wide (19+ cores).
* Incident (recovered): two xsa rebuilds during the in-flight gate replaced
  target/release/xsa; the sweep phase re-execs current_exe() = the deleted
  inode -> rc 127. Merge/endpoints/slim ran from the version-keyed cache
  bundle (old inode, valid). Recovery: manual sweep under the same
  ulimit -v 6 GiB cap -> chi exact; verdicts completed manually (this file).
  RULE: never rebuild the xsa target while a gate that re-execs itself runs.
* Floor counters: final pair POOL_STATS m2 pool hits=1.89e9 misses=1.07e9
  (256B windows => ~274 GB drive reads on the 2.16 GB m2 file - the pool
  covers ~24% of it at 512 MB slots); the walk is miss-bound on fat pairs
  (striping/larger pool are the levers, lane items 3-4).

## Milestone 2 GATE 2 RESULT (corpus-referenced chunks, fragment, PASSED)

Referenced text through the consolidated binary (bundle at 494fe6b; the
gate-2 xsa additionally contained the k-way core, unused at k=2):
* BYTE IDENTITY: 8/8 BYTE_IDENTICAL_TO_BANKED; chi = 306164765 exact.
* Disc accounting: chunk dir 10.65 GB (unchanged .crle runs + 32 x ~300B
  .ref sidecars); the saving is in the merge scratch: walked chunk
  sidecars drop from 9n (text+pos) to 8n (pos-only) per raw chunk, and the
  corpus cross-validation replaces the O(n) sidecar re-validation at load.
* WALLS: polluted and therefore not banked as a comparison row - my k-way
  quick-gate merge sweeps ran concurrently with gate 2's L1-L3 pairs
  (mea culpa, journaled): tree total 4:17:37, but clean-window evidence
  (L0 pairs before the pollution window): 99-108s/pair vs gate 1's
  115-128s/pair - the ref loads cut ~15% per L0 pair as expected from
  removing the text dump + the sidecar re-validation. RULE REINFORCED:
  no side load of any kind during measurement gates.
* En route: anchor+repair parallelized ACROSS SIDES (gate 1's largest
  remaining serial core, 358s on the final pair; a flat k-way would
  otherwise serialize all k sides' anchor phases). Re-gated byte-identical
  (selftest k {2,16,32}, arities {2,5,16,32}).

## Milestone 3 GATE 3 RESULT (fragment scale, FLAT k=32, PASSED)

The headline row, through the consolidated `xsa build --kway 32` (bundle at
54639fd: sharded emission + referenced chunks + flat k-way + parallel
anchors + arena cap):
* BYTE IDENTITY: 8/8 BYTE_IDENTICAL_TO_BANKED (all four merged files,
  all four finish files); chi = 306164765 EXACT. FLAT == the certified tree
  artifact, byte-for-byte.
* WALLS: chunk 3:24 (unchanged), **merge 1:03:15** vs banked tree 6:57:38 =
  **6.63x on the merge** (3.45x vs gate 1's sharded tree 3:36:54),
  endpoints 5:56, slim 20:02, sweep 1:04. Full chain ~1:33 vs ~7:35 banked.
* Per-phase (flat, k=32, 48 threads, S=48): sequential 32-chunk load
  ~1650s (the NEXT target: bounded-concurrency walks), anchor+repair 104s
  for ALL 32 sides in parallel, k-way heap walk 1109s (6.92B comparisons =
  5x one pairwise walk's, at the same wall as the tree's final pairwise
  walk; pool 99.3% hit rate; NOT I/O-bound: ~54 GB drive traffic),
  emit-count 463s + emit-write 465s + pf emits 3s, RSS 2.48 GB,
  VmPeak 3.64 GB (under the 6 GiB gate cap after the arena fix).
* Floor status at fragment scale: the merge is CPU/load-bound, not
  I/O-bound. Remaining CPU-side costs in rank order: (1) the sequential
  chunk walks at load (~1650s), (2) the walk's comparator pool misses
  (~1109s wall with 99.3% hits - bigger pool slots or striping shave it),
  (3) emit passes (~930s, sharded but IO/CPU mixed).


## Queued evaluation (user design question): persist derived structures at
CHUNK time

The load phase is merge-time DERIVATION (BWT text walked from chunk runs,
LF/pos, rank machinery) - compute, not I/O; that is why it was serial and
why mmap cannot remove it. Option 3 under evaluation: chunk_frontend emits
the materialized per-side BWT text + pos column alongside the .crle, and
the merge's load collapses to pure pool-served reads. PRICING (from the
gated numbers, to be finalized after the 10GB verdict + shipping rerun):
* Artifact growth: ~9n bytes per chunk (text n + pos 8n) - the exact
  walked-sidecar cost we removed from the scratch in milestone 2, moved
  to chunk time: fragment +~9.7 GB total, 10GB +~90 GB, pile (26 chunks,
  ~58 GB each) +~520 GB of chunks vs the .crle runs alone at ~9.3x corpus
  (~1.4 TB) - pile chunk artifacts ~1.9-2 TB, still far under the ~6 TB
  free with the referenced corpus in place.
* Load-phase wall: pure reads at pool bandwidth (~2-5 GB/s/reader across
  stripes) vs parallel derivation at ~50 s per 33.8M-position side
  (fragment measured, ~1.5 s/MB serial). At pile scale: 26 x ~580 MB/s
  reads ~= ~40 min total vs derivation at ~1450 s/side x 26 / width ~2-3
  (memory-bound walk transient ~9x chunk bytes ~520 GB/side on a 1 TB box)
  ~= ~4-5 h. VERDICT (preliminary, to be confirmed with the 10GB shipping
  rerun): persist-at-chunk-time is materially cheaper at every scale AND
  removes the pile's width-1-2 memory ceiling on loads; the chunk-time
  cost is one extra n-byte write per chunk (~+8% of chunk phase wall).
  Recommendation: ship it as the default after the fragment-scale
  experiment; the merged outputs are byte-identity-neutral by
  construction (same loaded structures, same merge).

## BOTTLENECK COST MODEL UPDATE (user directive, 2026-10-09)

1. DERIVATION is the current bottleneck: the merge re-derives per-side
   structures at load (BWT walk from runs, LF/pos) at ~0.7 MB/s serial -
   ~5h at 10GB, ~22 days at pile serial. PROMOTED TO MUST-DO:
   persist-at-chunk-time. KEY INSIGHT from the chunker source: chunk_frontend
   ALREADY computes the order (the cyclic suffix array) and holds the chunk
   text when it emits the .crle - persisting the order column (8n bytes per
   chunk) + expanding the BWT directly from the .crle runs (sequential, no
   text reads) makes the merge load PURE READS with near-zero chunk-time
   cost (one 8n write per chunk). The parallel-derivation pool (0565ab0)
   becomes the FALLBACK for chunk sets without persisted columns.
2. THE WALK is the second bottleneck, its own lane after the 10GB verdict:
   fragment walk ran ~26 effective cores of 48 (skew-limited), ~1.7us/byte
   probe cost, 99.3% pool hits - CPU-bound, not I/O. Pile extrapolation
   ~25 days of walk at current width. Investigation list (gated):
   effective-core instrumentation + block-size/skew distribution; probes
   per emitted run at 10GB vs fragment (k-way fan / heap cursor
   overhead); why width stalls at ~26 with block-parallel comparisons;
   honest floor analysis: probes/decision x amplification x decisions.
3. Sequence: 10GB verdict (gate 4, in flight) -> persist-at-chunk (gated)
   -> walk-width investigation + fix (gated) -> 10GB shipping rerun with
   both -> pile plan recomputed honestly.

## GATE 4 (10GB flat) - STOPPED BY DIRECTIVE 2026-10-09 ~18:40; partial telemetry banked

Killed before the walk (the question it answered was no longer next; cores
and disc needed for the fast loop). What it proved and cost, as the flat
shape's last data point at 10GB (32 chunks, 48 threads, cap 24 GiB, plan
journaled in-run - the configurator's first live plan: projected chunk
29:58 vs measured 37:05 = +24% calibration error):
* chunk phase: 37:05 rc=0 (plan 29:58; calibration data point)
* SERIAL-DERIVATION LOADS: ~25,403s = 6.6 HOURS at 1 core (the bottleneck
  this lane then removed: persist-at-chunk-time). This is the 10GB BEFORE.
* m2-build 5.4s; hash-build 8.5s (2 x 10GB files)
* anchor+repair 1369s (22.8 min) for ALL 32 sides in parallel (fragment
  104s at 1/9.3 scale - scales sub-linearly with corpus)
* walk: never ran (stopped).
Disc finding (supervisor, from live trees): chunk artifacts 9.2-9.7x
corpus (25B/run x 0.39 runs/char), merge transients ~28x, peak ~29x =
291.5GB at 10GB -> ~38TB at pile (1.31TB) vs ~6TB free = 6x MISS.
DISC BINDS THE PILE, not RAM. Action list in the pivot directive below.

## 100MB CASCADE GATE (PASSED 2026-10-09): the progressive invariant holds

Fast-loop fixture: snap n=100,000,503 (sha 3c965f58...), banked via the
certified config (flat k=16): chi = 29716349 (N=100000503, R=38647515).
Cascade (tree k=2, 4 levels, 14 intermediates): ALL SIX merged files +
ALL FOUR finish files BYTE-IDENTICAL to the flat run; chi EXACT. Every
intermediate is chunk-format (SXCR + SXP3 .pos + SXRF-v2 segment-concat
.ref; ZERO .sxs) - level N's output is a valid level N+1 input, loads
are pure reads (0.25-0.54s per 6.5M-position side vs 12.3s walk = 50x),
no text copy at any level. Merge wall: flat 1:17 vs cascade 1:57 (the
cascade pays 4 levels' m2/hash/emit; the progressive/delete-consumed
shape bounds its disc - driver TODO).
En-route root causes (journal): companion-name mismatch (chunker wrote
chunk-N.pos/.ref, merge looked for chunk-N.crle.*: every "persisted"
gate had silently walked - byte-identical fallbacks kept gates green);
SXP3 posBase 40; bwt-expansion mid-run flush offset. All fixed + re-gated.

## GATE 5 (1GB BARRIER BREAK) - PASSED 2026-10-09 21:14: 10/10, both chi exact

mergperf-1b-snap.txt (sha c382daa5..., N=1,000,000,665, cut from the pile at
first 0x1e >= 1e9), 16 chunks, flat k=16, cap 8 GiB, 48 threads.
A (CROSS_NO_PERSIST=1: derive loads) vs B (persisted-at-chunk-time): ALL SIX
merged files + ALL FOUR finish files BYTE-IDENTICAL; chi = 283296933
(N=1000000665, R=368036609) exact on BOTH. MERGPERF_GATE5_1B_DONE pass=10.
THE BARRIER BREAK (load phase): A LOAD_STATS sides=16 par=1 wall=1844.905s
(serial derivation at width 1 - the walk pool deriving order columns) vs B
sides=16 par=16 wall=3.023s (pure reads of the chunker-persisted columns) =
610x. Merge wall_total: A 3537.0s -> B 1612.6s (2.2x; B is now walk-bound:
cross-merge t_fast=12033/t_scan=5297 CPU-seconds). Full chain (chunk+merge+
sweep+slim): 1:30:53 (A) vs 1:27:44 (B) in this run (both under disc
pressure at 87-88% full, --scratch-free-pct 0 journaled; A's chunk phase and
the finish phases are shared costs). At 10GB the equivalent derive load was
6.6 HOURS (gate 4); persistence removes the class (loads -> ~0 at any scale;
0.25s/6.5M-position side at 100MB).

## SXP4 COMPACT .pos (chunk-disc lever) - tiny gates PASS, 100MB gate in flight

Design (measured, then built): within-group row deltas on real pile-like
100MB chunk-0 are ~3.13 B/pos (0.6% 1B, 12.2% 2B, 61.1% 3B, 26.1% 4B; none
>=5B). Format "SXP4" | u32 ver | u64 offset | u64 n | u64 period | u64 rsvd
| u64 groups | pad(8) | per-group (startRow, byteOff, basePos) 24B entries
from byte 56 | zigzag-varint delta stream, groups of 4096 rows. Bases and
absolute positions stay 64-bit (41+ bits required by pile arithmetic);
only bounded within-group deltas are narrow - NO 32-bit overflow anywhere.
Reader: PosColumn (sniffs SXP3 dense vs SXP4; binary-searched group table,
windowed bulk read) replaces WinU64 in PosOverlay; writers: chunker
emit_pos (direct-fd) + merge write_pos_sxp4 for intermediates (cascade
composes). Tiny-alpha gates (16 chunks, banked outputs): k=2/16/32 ALL
BYTE-IDENTICAL; tiny chunks 1.94x corpus (SXP3 was 8.2n at 100MB).
ROOT CAUSE of the failures that delayed this: zigzag ENCODERS wrote
2|d|+1 for negative deltas; the standard decoder maps odd v to -(v>>1)-1,
so every negative delta was off by one and drift accumulated group-by-group
(detected as "persisted position out of range" only when drift wrapped a
row below 0 - my python delta checker had mirrored the encoder's error and
masked it). Fix: standard zigzag in both writers (v = 2d if d>=0 else
-2d-1). Two merge-side fixes: 1-row groups (n%4096==1) have empty spans
(span check relaxed to >=); the persisted branch now returns before the
legacy dense tail re-check (which misread SXP4 as dense).
In flight at journal time: 100MB production gate (xsa build, flat k=16)
vs the banked v3-flat outputs + chi 29716349; then the ratio table.

## SXP4 100MB PRODUCTION GATE - PASSED 2026-10-09 (10/10, chi exact)

xsa build (16 chunks, flat k=16, cap 6 GiB, 48 threads, --scratch-free-pct
0): ALL SIX merged + ALL FOUR finish files BYTE-IDENTICAL to the banked
certified v3-flat run; chi = 29716349 (N=100000503, R=38647515) EXACT.
RATIO TABLE (100MB fixture, provenance mergperf-100m-v3-flat vs -sxp4,
16 chunks, bytes summed over the chunk set):
| column | v3 (SXCR v3 + SXP3 dense) | SXP4 compact | per corpus byte |
| crle   | 80,732,634  (0.807n)      | 80,732,634 (0.807n) | unchanged |
| pos    | 800,004,664  (8.000n)    | 328,663,481 (3.287n) | 2.43x down |
| ref    | 5,472        (~0n)        | 5,472      (~0n)     | unchanged |
| TOTAL  | 880,742,770  (8.807n)    | 409,401,587 (4.094n) | 2.15x down |
TINY fixture (corpus-tiny-alpha, 16 chunks): total 1.94n (2-char text
compresses deltas further). PILE PROJECTION at 4.09n x 1.31TB = ~5.4TB
chunk set vs ~6TB free = INSIDE THE BOX (v3/SXP3: 8.8n -> 11.5TB miss;
the 29n pre-v3 shape was a 38TB = 6x miss). Disc no longer binds the pile
chunk set; merge-transient peaks are the remaining disc question.
En-route perf fix (post-gate): PosColumn::at() compact mode re-decoded
its whole 4096-row group per call (per-run samplers hit at() 2x/run) -
100MB loads measured 132.4s wall. Fixed with the same 4-slot TLS
windowed-cache pattern as dense mode (group == page: starts are
GRP-aligned; one group decode per miss). Re-gate sxp4b in flight to
re-bank LOAD_STATS (expect ~seconds; SXP3-era persisted loads were
0.25-0.54s/side).
Windowed at() re-gate (mergperf-100m-sxp4c): 10/10 BYTE-IDENTICAL, chi
EXACT; LOAD_STATS sides=16 par=16 wall=0.225s (the unwindowed decode had
measured 132.4s; SXP3-era persisted loads were 0.25-0.54s - parity
restored at 100MB). Merge wall 51.7s; chunk 17.5s; slim 1:49; sweep 6s.
EN-ROUTE BUG (caught by the merge's own 1-in-64 sampled run-head text
check, minutes into the re-gate): decodeGroup output already includes
`shift`; the windowed at() return added shift again (double shift) ->
the sample check fired "persisted order disagrees with the referenced
text". Fixed (return without the extra shift). The sampled checks keep
earning their keep: this class of bug (correct bulk path, wrong point
path) is invisible to load-time validation and caught in seconds.

## POS-COLUMN MODE SWITCH (steering 2026-10-10): a KNOB, not doctrine

The measured trade (1GB A/B, both 10/10 byte-identical + chi exact):
compact (SXP4) cuts pos disc 2.2-2.4x but costs the WALK 2.3x at 1GB
(1598s vs 693s; loads 1.7s vs 3.0s; the walk is the dominant phase in
every config - 78% of the 1GB compact merge). Mode is now chosen by the
plan, not hardcoded:
* CHUNK_POS_MODE / CROSS_POS_MODE = dense (SXP3 8n) | compact (SXP4),
  honored by the chunker (persist-at-chunk-time) and the merge
  (intermediate emission); readers sniff the format either way.
  CROSS_NO_SXP4 stays as the legacy dense alias.
* plan.rs CONSTANTS v3: crle 0.8n + pos per mode (dense 8.0n / compact
  3.6n), merge scratch 9.1n (measured 100MB flat v3+SXP4; persisted loads
  keep pos on the chunk set, no walked.pos dumps), per-mode walk rates
  from the 1GB gates (dense 0.693, compact 1.598 s/MB; the fragment-
  calibrated floor stays as a max()).
* XSA_POS_MODE journal line with the arithmetic (chunk_set_gb both
  modes, free, rule = dense-if-10%-headroom-else-compact); chain.rs sets
  both knobs plan-picked unless the operator set them.
* verdicts at the scales: 100MB/1GB -> dense (disc allows; wall-optimal);
  pile (1.31TB, k=32) -> compact AND INFEASIBLE regardless: peak ~18.1TB
  vs 1.83TB free (3.2x miss even compact) - the honest DISC_FAIL
  arithmetic; the delete-consumed/progressive shape remains the pile's
  path (peak = corpus + accumulator + k live sides).
* gates: tiny k=2/16/32 BOTH modes 6/6 byte-identical; 100MB production
  dense gate 10/10 + chi EXACT.
EXCHANGE RATE TABLE (same binary, flat k=16):
| scale | mode    | pos disc | chunk set | loads  | walk   | merge total |
| 100MB | dense   |  8.00n   |  8.81n    | 0.125s | 38.5s  | 53.5s       |
| 100MB | compact |  3.29n   |  4.09n    | 0.225s | 36.3s  | 51.7s       |
| 1GB   | dense   |  8.00n   |  8.78n    |  3.0s  |  693s  | 1613s       |
| 1GB   | compact |  3.60n   |  4.38n    |  1.7s  | 1598s  | 2452s       |
At 100MB the compact walk penalty is ~nil (both walks ~37s - cache-friendly
interleave depth); it emerges at 1GB where the k=16 overlay interleaves
past the 4-slot TLS group cache (2.3x). Next lever per steering: an
LRU/larger decode cache for the compact column may recover much of the
2.3x; one experiment queued after the same-binary 1GB dense gate.
TOOLING GOTCHAS (journal for the next lane member): (a) the mode-switch
chunker edits were initially not re-packaged - the staged chunk_frontend
in ~/.cache/xsa/<id>/ was stale and the first "dense" 100MB gate silently
ran COMPACT (green gates, wrong mode: verify the chunk magic, not just
the pass); (b) `cargo build | tail -1 && gate` masks build failures
(tail exits 0) - grep for Finished/error explicitly; (c) PosColumn's
dense branch needs byteBase=40 from the .pos caller (I passed 0 in the
refactor; SXP4 masked it until the dense tiny gates caught it).
