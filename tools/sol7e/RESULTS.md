# sol7e: fresh P-way production and measured seam accounting

Fresh-start producers, byte identity and counter decomposition are implemented
and tested. Best observed 1 GB merge: **P=8, 267.791 s**,
meeting the <=300 s pass and <=270 s launch-shape numbers in this run.
**Quiet-box launch acceptance remains OPEN**: sustained unrelated impg and
sample_projection CPU work is recorded in each gate's box-before.txt. P=4 and
P=16 exceed 300 s in this observed sweep. These are shared-box observations,
not a quiet-box qualification or a claim of linear scaling.

## What landed

| Location | Behavior |
|---|---|
| bit6/cross_lcp_merge.cpp:3776 | P independent compressed-segment producers; committed-prefix spool reads and one ordered canonical-band publisher |
| bit6/cross_lcp_merge.cpp:3724 | Coalesces non-target rows; singleton targets retain every class occurrence and both flanks across producer seams |
| bit6/cross_lcp_merge.cpp:3890 | `CROSS_V2_PRODUCERS` selects S, default 1; 1..4096, clamped to row count |
| bit6/cross_lcp_merge.cpp:3896 | S=1 keeps the existing direct sink; S>1 selects ParallelV2, whose shardCount/admission return P |
| bit6/cross_lcp_merge.cpp:3067 | Explicit diagnostic serial oracle; fresh producers remain unseeded |
| bit6/cross_lcp_merge.cpp:3157 | Immutable-input/split-bound reference journals permit measured SEAM without a replay in timed runs |
| bit6/cross_lcp_merge.cpp:224 | Cache-line-owned reader, grid-cell and corpus-borrow telemetry removes producer false sharing |
| xsa/src/chain.rs:1034, :1051 | Pipeline reads, journals and passes the S knob; operator opt-in is `CROSS_V2_PRODUCERS=8` |
| xsa/src/chain.rs:794 | Reserves the additional worst-case 25n compressed-segment scratch before moving bytes |

The native and Cargo runtime C++ sources are byte-identical, including the
previously banked sol7a RAM cache, borrowed spans, SIMD reads and front prefetch.
Final source SHA256: `a2835231afc089b0580e3172b8884593e4724a3a233772442955e8a678503d62`. All source-manifest entries validate.
The runtime mirror previously lacked sol7a; this dependency was explicitly
promoted for the shipped pipeline. Default S=1 logical work is unchanged.

## Witnesses, verbatim

```text
SHARD_ROW_SEAM_WITNESS_PASS cases=40 seams=242 P=1,4,8,16 modes=cyclic,dollar exact_rows=1 exact_flanks=1 gaps=0 duplicated_rows=0 production_bands=unvalidated fixtures=5
FRESH_SEAM_COUNTER_WITNESS_PASS cases=40 fixtures=5 P=1,4,8,16 modes=cyclic,dollar rows=exact seeded_reference=exact per_seam_journal=exact OVERLAP=0 P1_extra=0
SEAM_COUNTER_WITNESS_PASS cases=40 fixtures=5 P=1,4,8,16 modes=cyclic,dollar per_producer_sum=exact ordered_comparison_trace=exact SEAM=0 planning_replay=fully_charged launch_gates=UNVALIDATED
CACHE_P16_ORACLE_PASS readers=16 spans=3200000 remaps=3 segments=3 nonzero_offsets=1 wrap=1 retained_borrows=2 immutable_shared_cache=1 decoded_front_budget_P16_k8_bytes=4194304
```

All **290 per-range PLANNING/OVERLAP/signed SEAM lines** across the 40 fresh
cases are retained verbatim in [counter-summary.log](counter-summary.log).
The full numeric journal is [counter-witness.json](counter-witness.json).
The seeded oracle checks every ordered comparison operand on the same fixtures;
its range budgets independently equal the fresh witness's reference trajectory.
There are 242 exercised interior seams; empty/repeated cuts are included.
OVERLAP is zero because no row is duplicated. SEAM is signed and includes changed
heap exhaustion at a range's end; it is not generally a positive local prefix.

The new committed-prefix/segment model passed 400 schedules, 48,615 rows,
P=1/4/8/16, exact rows and coalesced runs, repeated cuts, singleton targets and
global-only terminal. The standing 472-schedule transport model and 1,450-
schedule wait model are green. Native production run/column/class oracles pass
at P=1/4/8/16; the final padded build also passes 200 P=16/k=8 cases. Standing
sweep, repair, spool-reader and flow oracles remain green. Details are in
[validation.json](validation.json) and [validation-padding.json](validation-padding.json).

P=16,k=8 front windows are bounded by **4,194,304 bytes**. The cache oracle
checks 3.2 million concurrently borrowed spans, retaining two spans per reader,
with remaps, nonzero offsets, segment boundaries and wraps. Corpus RAM is shared
and immutable. This proves borrowing correctness and the front budget; the
observed P=16 wall is slower than P=8 under the recorded external load.

## Gates, verbatim

```text
100 MB:
SXI2_PASS bytes=48768685 lite=1 members=6
PILE_FINISH_DONE chi=29716349 n=100000503 r=38647515 container=/mnt/nvme2n1/erikg/sol7e-gate-100m-p16-padded/frag.sxi2
merge_rc=0 finish_rc=0 cmp_rc=0
1 GB:
SXI2_PASS bytes=467814822 lite=1 members=6
PILE_FINISH_DONE chi=283296933 n=1000000665 r=368036609 container=/mnt/nvme2n1/erikg/sol7e-gate-1b-p16-padded/frag.sxi2
merge_rc=0 finish_rc=0 cmp_rc=0
```

The 100 MB task path spells `mergerp-100m-snap.sxi2`, which does not exist.
Cmp used the existing bank at
`/mnt/nvme2n1/erikg/extcols-scratch/xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2`.
The 1 GB bank is exactly the requested
`/mnt/nvme2n1/erikg/extcols-scratch/pilev2-g2-1b/frag.sxi2`.
Every final 1 GB P=1/4/8/16 run asserts chi=283296933 and has cmp=0.
Final 100 MB P=16 asserts chi=29716349 and has cmp=0.

## Four-counter decomposition at 1 GB

Tuples are (comparisons, symbols, probes), never percentages mixing units.
WALK is producer heap sums plus the unchanged parallel prepare/repair portion.
PLANNING is separately charged rank-search work; OVERLAP is actual duplicate-row
work (zero). SEAM = producer heap sums minus serial range portions, verified
against the seeded reference. Adding SEAM to already charged WALK would count
it twice: total charged work = WALK + PLANNING, equivalently SERIAL_WALK +
PLANNING + OVERLAP + SEAM. The legacy CROSS_KWAY summary retains charged total;
the named journals form the unblended ledger.

| P | WALK | PLANNING | OVERLAP | signed SEAM |
|---:|---|---|---|---|
| 1 | (3674629435, 60485597061, 1987510452) | (0, 0, 0) | (0, 0, 0) | (0, 0, 0) |
| 4 | (3674619757, 60480105257, 1987526302) | (15835, 75965, 800) | (0, 0, 0) | (-9678, -5491804, 15850) |
| 8 | (3674599453, 60478673268, 1987483450) | (36937, 203480, 2216) | (0, 0, 0) | (-29982, -6923793, -27002) |
| 16 | (3674606698, 60477880116, 1987420158) | (78957, 452649, 5180) | (0, 0, 0) | (-22737, -7716945, -90294) |

P=1 WALK is **EXACTLY (3674629435,60485597061,1987510452)**; all extras are zero.
The serial oracle also independently reproduces that trajectory's total.
Each P>1 timed run loads the same input-bound range reference, contains no
oracle replay, and passes independent checks of every producer/stage sum.
See [ledger-summary.log](ledger-summary.log), [seams-1b.log](seams-1b.log) and
[sweep.json](sweep.json). Per-seam PLANNING/OVERLAP/SEAM for all 16 ranges are
preserved verbatim; no normalization or reclassification hides heap work.

(PLANNING+OVERLAP+SEAM)/WALK, per metric:

| P | comparisons / symbols / probes |
|---:|---|
| 1 | +0.000000% / +0.000000% / +0.000000% |
| 4 | +0.000168% / -0.008955% / +0.000838% |
| 8 | +0.000189% / -0.011112% / -0.001247% |
| 16 | +0.001530% / -0.012011% / -0.004283% |

Maximum positive overhead is **0.001530%**, below 30%; no stop threshold
was triggered. Negative signed deltas are observed work savings. Diagnostic
serial replay is explicitly labeled oracle_reference in calibration logs and
is excluded from production ledgers. Its measured 761.807 s internal wall is
not a launch wall, and no launch wall is inferred by subtracting replay time.

## Final wall sweep: 96t, 100 GiB

All four points use the final padded telemetry, the same eight input chunks,
96-thread merge/finish flags and a 100 GiB address-space cap per process.
No internal compiler/test job overlaps these final gates. Unrelated CPU jobs
remain active; box snapshots are preserved. Merge wall is full process elapsed,
including loads, hash, prepare, planning, heap, spooling and band publication.
End-to-end adds consumer completion; chunk construction is excluded from both.

| P | merge seconds | end-to-end seconds | speedup vs P=1 | effective Amdahl residual f |
|---:|---:|---:|---:|---:|
| 1 | 734.041 | 821.983 | 1.000 | 0.0000 |
| 4 | 392.763 | 555.610 | 1.869 | 0.3801 |
| 8 | 267.791 | 404.982 | 2.741 | 0.2741 |
| 16 | 312.516 | 425.479 | 2.349 | 0.3875 |

`f = (T(P)/T(1) - 1/P)/(1 - 1/P)`. These residuals combine reduction,
preparation, imbalance, transport and shared-box contention; they are not a
measurement of a single pure serial fraction. The measured launch-shape choice
is P=8; P=16 does not improve this wall. The <=270 s observed number
has no quiet-box attestation. Full commands, counters and walls are in sweep.json.

## Residual risks and operational limits

* Quiet-box acceptance is unfulfilled. No other user's processes were stopped;
  contact_supervisor is absent from the complete callable tool catalog.
* Producers emit compressed segments to per-range spools; canonical band
  publication stays centralized. Upstream scratch is input-bounded at 25n,
  rather than a constant-band window. The pipeline reserves that bound; this
  can make large-corpus opt-in infeasible. Disk `.done` retry/recovery is not
  implemented. A failed process refuses global terminal.
* Fully measured P>1 SEAM requires `CROSS_SEAM_REFERENCE` for the same immutable
  source identities/splits, or an explicit diagnostic oracle run. Without one,
  SEAM is journaled as unmeasured, never silently zero. A new chunk identity or
  comparator/heap algorithm requires a fresh reference. Calibration replays
  the serial trajectory and is an offline measurement cost, not a speedup.
* The first scalar oracle diagnostic was stopped and replaced with bulk front
  reads; the initial unpadded P=8 sweep was stopped after discovering telemetry
  false sharing. Their logs remain in scratch. Final reported points all use
  the padded implementation. Rust cargo check passed after correcting the
  changed chain.rs manifest entry; the initial rejection is retained in logs.

## Branch and commits

Branch: `sol-7e-freshstart-pway`, base `20d355e`.
Implementation/milestone commits before the final attestation commit:

```text
cd5fbd3 sol7e: eliminate false sharing from all hot per-thread reader telemetry
4b45dd3 sol7e: keep witness whole-walk and range-heap ledger names consistent
4d96bf2 sol7e: attest P1 exact 1 GB baseline and zero extra counters
3752edb sol7e: attest P16 1 GB chi and byte identity with four-counter decomposition
6d79432 sol7e: ship banked RAM-cache path and validate replay-free seam references
19e9c2c sol7e: attest P16 100 MB chi and container byte identity
0102805 sol7e: bulk-decode oracle fronts without changing seeded comparison traces
6a3406f sol7e: enable fresh P-way compressed spools with ordered band reduction and reusable seam references
ccca5c2 sol7e: attest 40 fresh-start row cases with signed per-seam oracle journals
be70c72 sol7e: record fresh-start accounting and ordered sink ownership
```

The final attestation commit contains this report, the independent sweep
checker and final gate evidence. Scratch and full logs live under
`/mnt/nvme2n1/erikg/sol7e-*`; [AUDIT.md](AUDIT.md) and [PROTOCOL.md](PROTOCOL.md)
state the source/state and transport contracts.
