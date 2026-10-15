# sol7b result: route (c), performance remains open

Base: 21ad74b, branch `sol-7b-parallel-producer`. No production source, native
mirror, source manifest, or protocol file was changed. This is an audit and
model deliverable, not a parallel-producer implementation. The launch remains
blocked on the merge wall.

## Route and evidence

See AUDIT.md for source locations and the minimal upstream transport proposal.
Anchors contain local occurrence intervals and position-overlay repairs, not
heap checkpoints. Existing exact rank planning can cheaply reconstruct row
ranges, but does not recover the serial heap layout, run-band bases or open
seam state. No heap reconstruction timing is claimed; the previously supplied
76.9 s anchor/repair timing does not measure that operation.

Freshly compiled native code provides the following reproducible witness
(`work_witness.py`, `work-witness.json`). Both S settings pass the native brute
order and sparse-v2 oracles on eight copies of the fixed three-byte fixture.
S here is logical shard count, NOT independent physical producer count in v2.
The production v2 sink still forces S=1.

| Mode / stage | S | comparisons | symbols | probes |
|---|---:|---:|---:|---:|
| dollar prepare | 1 | 8 | 15 | 0 |
| dollar prepare | 8 | 8 | 15 | 0 |
| dollar plan | 1 | 0 | 0 | 0 |
| dollar plan | 8 | 294 | 1881 | 0 |
| dollar heap | 1 | 95 | 738 | 0 |
| dollar heap | 8 | 78 | 686 | 0 |
| cyclic plan | 1 | 0 | 0 | 0 |
| cyclic plan | 8 | 266 | 3388 | 266 |
| cyclic heap | 1 | 93 | 1535 | 124 |
| cyclic heap | 8 | 72 | 1728 | 144 |

This rules out promoting the present sharded loop while claiming unchanged
serial work. It does not establish that every conceivable parallel algorithm
must change work. A fixed logical shard plan, executed by P physical workers,
would give deterministic work at P=1/4/8/16 but a new numeric baseline. That
relaxation has not been authorized and no counters were hidden or normalized.

## Model first

`threaded_model.py` imports the sol5i readiness and window rules and extends
its harness with P producer threads, an ordered spool/publisher, and a finish
poller. It passed 472 schedules: 400 randomized successful schedules, all 24
four-producer finish permutations, and 48 failed/gapped-range abort cases.
P=1/4/8/16, W=1/2/3/64/1100, queue capacity four descriptors per producer.
760 logical restarts redelivered one unacknowledged descriptor each; 760
identical duplicates were suppressed. Successful schedules had zero deadlocks,
lost rows or duplicate rows. Peak upstream queue four; peak downstream band
backlog 1100. These are distinct bounds, not a measured four-piece disk peak.

The model exercises partial final bands, empty ranges, producer stalls,
classes needing the next producer's band at each boundary, the maximum W-1
lookahead, flow exhaustion, and global-terminal EOF clamping. A local range
completion never publishes global terminal. Failure wakes all waiters and
refuses terminal; it does not promise successful completion after permanent
failure. Negative-control inequalities identify full-range-before-drain
queue deadlock and invalid local EOF clamping. No model bug/deadlock was
encountered and fixed during this run.

Model scope is explicit: descriptors already represent canonical bands; it
does not reconstruct heaps, compute run counts from row slabs, capture real
seam classes, or implement persistent crash recovery. It does NOT validate a
production multi-producer pipeline. No pipeline code was touched after it.

## Gates, wall sweep, and residual risks

The 100 MB and 1 GB container/chi/cmp gates were NOT rerun. No P=1/P=8 1 GB
counter-identity claim is made. The required totals remain the supplied
baseline (comparisons 3,674,629,435; symbols 60,485,597,061; probes
1,987,510,452), not new measurements from this branch.

| P | 1 GB merge wall at 96t / 100 GiB |
|---:|---|
| 4 | NOT RUN: no qualifying parallel implementation |
| 8 | NOT RUN: no qualifying parallel implementation |
| 16 | NOT RUN: no qualifying parallel implementation |

No new wall measurement or scaling claim is made. 665.33 s is the supplied
sol7a baseline, not a measurement here. During validation, the compiler and
small merge selftests overlapped with standing-oracle checks; these runs are
correctness checks and must not be interpreted as quiet-box benchmarks.

Residual risks: merge remains serial and the <=300 s bar remains unmet.
The proposed ordered reducer may bottleneck; row slabs increase memory traffic.
A compressed transport must preserve interior seam events and neighboring
flanks. Exact-count preservation would instead need a new method of acquiring
true execution checkpoints without serial replay. Whole-process crash/restart,
persistent acknowledgements, disk `.done` handshakes and real row-to-band
seam reconstruction are outside the model's demonstrated coverage.

Coordination limitation: `contact_supervisor` was absent from the callable
catalog (searched by name and description). No substitute external messaging
was used. The work-rebaseline decision was not assumed. The first merge build
failed to link 16-byte atomics; adding `-latomic` fixed it. Fresh source builds
of merge, finish and flow oracle succeeded. Finish emitted existing SDSL
`std::iterator` deprecation warnings.

Validation details and milestone commit IDs follow in validation.json and
`git log --oneline 21ad74b..HEAD`. Scratch/logs/binaries are under
`/mnt/nvme2n1/erikg/sol7b-audit/`.

Standing gates, verbatim (fresh builds from this worktree):

```text
SELFTEST_PASS count=200 seed=1 kway=1
SELFTEST_PASS count=200 seed=1 kway=2
SELFTEST_PASS count=200 seed=1 kway=8
SWEEP_STATE_CARRY_ORACLE_PASS streams=2048 splits=34785 NUL=1 byte05=1 singleton=1 periodic_colors=1 empty=1 ordered_witnesses=1 final_state=1
CHUNKED_REPAIR_ORACLE_PASS streams=128 widths=3392 workers=4 both_coalescers=1 class_flanks=1 padding_straddle=1 dollar=1 legal_00_05=1
BAND_PROGRESS_READER_ORACLE_PASS trivial_bases=1 band_only=1 deferred_class=1 EOF_clamp=1 terminal=1 delayed_full_band_EOF=1 backward_terminal_refused=1 malformed_refusals=12
SEAM_SPOOL_READER_ORACLE_PASS empty=1 adversarial_orders=2 padded_flanks=1 NUL=1 byte05=1 malformed_refusals=3
FLOW_ORACLE_PASS initial_minus_one=1 oversized_positive_window=1 unsigned_credit=1 blocked_until_consumed=1
```

Standing wait model: 1,450 schedules; zero deadlocks; peak backlog 1,100.
