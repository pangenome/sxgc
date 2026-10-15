# sol7d: exact continuation established; launch objective remains open

Branch `sol-7d-seam-state`, base c9104a5. This is an opt-in exact-state
reference implementation, not a qualifying parallel production launch.
No impossibility claim is made. The performance mission is NOT complete.

The third work term vanishes with exact seeds: the native producers reproduce
the serial ordered comparison operands and all three work counters exactly.
The missing launch ingredient is efficient acquisition of those seeds. The
existing planner binary-searches ranks; contrary to the task premise, it does
not cross split points with a running heap. Our reference obtains the missing
heap arrays through an explicitly charged serial replay. No work is hidden,
normalized or relabeled as duplicated overlap.

The complete state enumeration, including immutable dependencies, thread-local
cache generations/clocks, comparison-local scratch, buffers, budgets and sink
state, is in AUDIT.md. The minimal logical seed is cursors + ordered heap values
and side IDs + true side ends. There are no retained LCPs or toeholds and no
intrinsically unjournalable state. The planner lacks the heap order, rather
than a comparator context that cannot be serialized.

Implementation: `CROSS_SEED_HEAP=1` enables the reference when S>1. It
journals at loop-head checkpoints, restores without hPush, retains global
side ends, and assigns the replacement/sift after each emitted row to that
row's owner. This last transition must run even at an output boundary.
S=1 follows the original charged trajectory. The option is unset by default.
Native and Cargo runtime sources contain the same experiment; the runtime
source hash is refreshed without copying unrelated existing native changes.
Production SequentialV2 still forces S=1. No P-way band sink was added.

## Counter witness, verbatim

The original sol7c native row witness was extended in tools/sol7d with five
fixtures: original repeated three-byte sides, unequal all-equal sides, NUL/05
periodic sides, unequal long-prefix sides, and deterministic full-byte data.
Each runs cyclic/dollar at P=1/4/8/16. Private range vectors check rows and
flanks against brute order. Test-only traces compare EVERY ordered operand
pair of the full replay against the concatenated producers, without hashes.
Per-producer budgets are printed separately and summed by work_witness.py.
The actual full core Budget is also checked against prepare + planning + producers.

```text
SHARD_ROW_SEAM_WITNESS_PASS cases=40 seams=242 P=1,4,8,16 modes=cyclic,dollar exact_rows=1 exact_flanks=1 gaps=0 duplicated_rows=0 production_bands=unvalidated fixtures=5
SEAM_COUNTER_WITNESS_PASS cases=40 fixtures=5 P=1,4,8,16 modes=cyclic,dollar per_producer_sum=exact ordered_comparison_trace=exact SEAM=0 planning_replay=fully_charged launch_gates=UNVALIDATED
```

All 40 numeric lines are retained VERBATIM in counter-summary.log. The full
per-producer, stage, baseline and residual journal is counter-witness.json.
Tuple order is comparisons, symbols, probes. For the original cyclic P=8:

```text
COUNTER_IDENTITY_PASS fixture=0 P=8 mode=cyclic serial=[93, 1535, 124] PLANNING=[359, 4923, 390] OVERLAP=[0, 0, 0] SEAM=[0, 0, 0] total=[452, 6458, 514]
```

Here rank planning is [266,3388,266], seed acquisition is [93,1535,124],
and producer sum is [93,1535,124]. The old [-21,+193,+20] residual is gone.
PLANNING is the sum of the two acquisition stages, OVERLAP is zero because
no row is duplicated, and SEAM is the measured identity residual.
Serial here denotes HEAP work; prepare/repair is separately identical across
P and cancels in the whole-merge identity. These are small native fixtures,
not 1 GB figures.

## Gates and performance, verbatim disposition

```text
100 MB chi=29716349 / cmp vs /mnt/nvme2n1/erikg/extcols-scratch/xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2: NOT RUN
1 GB chi=283296933 / cmp vs /mnt/nvme2n1/erikg/extcols-scratch/pilev2-g2-1b/frag.sxi2: NOT RUN
P=1 1 GB comparisons=3674629435 symbols=60485597061 probes=1987510452: NOT RUN; supplied baseline only
P>1 1 GB total=serial+PLANNING+OVERLAP with SEAM=0: NOT VALIDATED
1 GB PLANNING/OVERLAP overhead <=30%: NOT MEASURED
96t / 100 GiB 1 GB wall P=1: NOT RUN
96t / 100 GiB 1 GB wall P=4: NOT RUN
96t / 100 GiB 1 GB wall P=8: NOT RUN
96t / 100 GiB 1 GB wall P=16: NOT RUN
<=300 s pass / <=270 s launch: NOT ACHIEVED
Amdahl residual per P: UNMEASURED
```

The large gates were not launched because no P-way production sink or
bounded checkpoint acquisition was supplied. This is an explicit incomplete
result, not an assertion that seeding is impossible. The reference adds one
full serial HEAP budget before rank planning, and retains a serial critical
path. This is exactly 100% heap overhead for nonzero metrics, but the ratio
to WHOLE-MERGE serial counters depends on prepare/repair and is not inferred
at 1 GB. The 30% 1 GB threshold was neither measured nor invoked. For example,
the long all-equal fixture has expensive prepare work; heap overhead and
whole-merge overhead must not be confused. No timing of these small checks
is presented as a performance benchmark. The supplied 665.33 s baseline was
not remeasured. No Amdahl fraction is fabricated.

## Standing verification and residual risks

Exact commands and verbatim oracle lines are in validation.json. Tests are
run sequentially to keep the box quiet. New journal construction is serial
before existing producers start, and producer seed/trace storage has no
cross-thread mutation. No transport, publication or retry protocol changed,
so no new interleaving model was required; the existing >=400-schedule
transport model and standing wait model were rerun. Their canonical-band
abstraction still does not validate native P-way band construction.

Residual risks: seed acquisition remains a serial replay; no <=30% 1 GB
work bound or launch wall is established; production remains serial; native
P-way row-to-band reduction and seam-class publication remain unimplemented;
cache hit/miss counters and timings are not continuation-identical; reference
and production heap algorithms must remain synchronized (the exact trace
witness detects drift on the exercised fixtures); no general lower bound on
checkpoint acquisition is proved. SOL7D_TRACE stores complete comparison
traces only in the native witness and is absent from normal builds.

Bounded-accounting proposal: use the reference as the checkpoint oracle for
a replacement acquisition algorithm; charge all of its work to PLANNING and
compare measured rank+acquisition+actual-overlap work against serial whole-merge
counters before promotion. Details and the alternative serial-producer
interpretation are in AUDIT.md.

Coordination limitation: contact_supervisor was absent from the callable tool
catalog (searched first). No substitute messaging and no wider work-contract
permission was assumed. This report does not claim a supervisor-approved
scope change. Scratch and full logs: /mnt/nvme2n1/erikg/sol7d-seam-state/.

Milestones: b0b2dda (state inventory), 8e0ef98 (seeded counter identity),
fe9a47e (exact ordered comparison trace). Final attestation is the subsequent
sol7d validation commit on this branch.
