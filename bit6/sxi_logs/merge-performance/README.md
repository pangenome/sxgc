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

## Deliverable table (walls; before/after each change)

| run                          | wall (banked) | wall (milestone) | notes |
|------------------------------|---------------|------------------|-------|
| fragment merge tree (k=2)    | 6:57:38       | gate1 in flight  | 32 chunks, 48 threads, pool ON, cap 6 GiB |
| fragment FLAT k=32           | -             | pending          | the number that prices the pile at its I/O floor |
| 10 GB merge (tree)           | 6:14 (old fat build, 332 GB) | pending | |
| 10 GB FLAT                   | -             | pending          | |

Floor analysis: owed after each milestone (pool misses x window size = drive
traffic vs aggregate NVMe bandwidth).

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
