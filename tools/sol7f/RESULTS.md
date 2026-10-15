# sol7f: flow-bounded segment spools

Implemented on `sol-7f-spool-window`, base `5d0b201`. Byte/counter gates and
standing oracles pass. The complete 1GB P=8 measurement is **302.657s**
merge, **8.632421n** sampled allocated scratch including retained
input chunks, with **11 producer block episodes**.

**Quiet-box qualification remains OPEN.** Unrelated impg/sample_projection jobs
were active; no contact_supervisor tool was present in the complete callable
catalog. No other user's processes were stopped. The literal 8.600n threshold
is missed by 32.421 MB; 8.632n rounds
to approximately 8.6n, but this report does not silently round a strict pass.
The <=320s wall bar passes in the observed shared-box run.

## Window design and proof

`CROSS_V2_SPOOL_BYTES` selects the physical data-page budget. Default:
max(2n, P*106496); explicit budgets below P*106496 are rejected. Effective
credits round down to whole 106496-byte slots. P=1 uses the unchanged direct
sink, with no segment spools. The planner reserves the full configured value.

* Before every write, producers encode a batch and reserve its page-rounded
  charge under one mutex. Later ranges leave one maximum-batch progress reserve
  exclusively for the current ordered range. Even if later ranges fill all
  other credits, the earliest producer can publish its next prefix.
* The ordered reducer drains committed prefixes without waiting for local EOF,
  publishes through the unchanged W=1100 canonical band/ACK path, punches the
  consumed physical extents, then releases credits. Independent downstream ACKs
  release intentional publisher stalls. Empty ranges and local EOF advance only
  after coverage validation; only the global reducer publishes terminal.
* Packed blocks contain a character, LEB128 length and n-width endpoints;
  singleton tails are omitted. Raw 25-byte-record fallback bounds expansion.
  Eight-byte framing binds record count and payload size. Fixed logical slots
  avoid a growing RAM offset index. These are segment encodings, never text.
* XFS can speculatively allocate beyond extending pwrite. The final code first
  explicitly fallocates exactly the credited pages, then writes. Hole punching
  precedes credit release. Filesystem metadata is additional; unsupported
  allocation/punching fails closed. Apparent sparse file sizes are not physical
  disc occupancy and can still grow with n.

Locations: `bit6/cross_lcp_merge.cpp:3783` framing/codec,
`:3828` knob, `:3855` credit admission, `:3868` exact extent allocation,
`:3965` reclamation, `:3983` per-producer stall telemetry. The runtime mirror
is byte-identical. Source SHA256:
`74edde327600de23916cc23290e26c4b20bb614fcba2ff6d555199956d995111`.
All 57 source-manifest entries validate; the final packaged build's copied
source also matches this hash.

The initial 400-schedule global-credit model preceded its pipeline
implementation. The final stronger model forces W=1100 exhaustion and also
exhausts all 40,320 P=8 EOF delivery orders; 35,280 require committed-prefix
credit release before any EOF. Its final output, verbatim:

```json
{"P": [1, 4, 8, 16], "P8_EOF_permutations": 40320, "P8_completion_orders": 66, "W": 1100, "blocks": 50840, "boundary": "inclusive", "deadlocks": 0, "duplicate_rows": 0, "exact_rows": true, "exact_runs": true, "lost_rows": 0, "peak_bands": 1100, "persistent_recovery": false, "prefix_credit_releases": 35280, "publication": "committed_prefix_only", "publisher_stalls": true, "repeated_cuts": true, "result": "PASS", "rows": 66735, "schedules": 400, "singleton_targets": true, "spool_budget": "global_with_active_batch_reserve", "terminal": "all_ranges_only"}
```

Codec and actual-XFS extent oracles, verbatim:

```text
SOL7F_SEGMENT_CODEC_PASS cases=512 widths=1..8 records=1..4096 u64_lengths=1 singleton_tail_elision=1 exact_roundtrip=1 packed=480 raw_fallback=32
SOL7F_XFS_EXPLICIT_EXTENT_PASS batches=2000 live_batches=11 variable_page_charges=1 fixed_slot_bytes=106496 budget_bytes=1171456 allocated_peak_bytes=438272 metadata_allowance_bytes=65536
```

The 400 threaded schedules cover inclusive budget boundaries, producer blocking,
repeated/empty cuts, publisher stall/release, singleton targets, exact rows/runs,
and global-only terminal. The separate exhaustive P=8 test exercises every local
EOF order and the prefix-progress rule. This is a live-process liveness model
with eventual ACKs, not durable worker recovery.

## Planner and knob changes

* `xsa/src/chain.rs:793`: parse/validate the byte knob and replace fixed 25n
  with its configured budget in merge phases. `:823` explicitly carries the
  extrapolated witness-spill allowance; `:831` refuses against the all-phase
  peak. `:1081` journals the knob. P=1 retains its measured 734.041s/GB merge
  rate; uncalibrated P choices conservatively use that service rate.
* `xsa/src/plan.rs:454`: version-6 rates, seconds per decimal GB:
  chunk 28, P=8 merge 291, serial finish including container 432. Provenance is
  the quiet-box 1GB numbers supplied with this mission; MiB conversion is
  explicit in code. Service times are conservatively summed despite overlap.
* `xsa/src/plan.rs:489`: current sparse path reserves default-k32 hash .5n +
  overlap .02n; no materialized doubled text or WM. Compact inputs 4.4n,
  rlebwt 1.546n and pfck .364n retain measured provenance. The W-limited pieces
  allowance is bounded in bytes rather than scaling a historic 64-band ratio.
  Container overlap and witness spill are carried conservatively. `:419`
  hosting uses the same all-phase peak as the refusal, removing the obsolete
  whole-WM hosting claim.

Front-door `--plan-only` projections, with P=8, 100GiB memory cap, 1GB chunks,
flat k=chunk-count, W=1100, and the byte knob exactly 2n:

| Corpus | Configured spools | All-phase scratch reserve | Free-capacity scenario | Verdict |
|---|---:|---:|---:|---|
| 100,000,000,000 B | 200,000,000,000 B | 0.983921 TB / 916.3 GiB | 2.6 TB | FITS |
| 1,310,000,000,000 B | 2,620,000,000,000 B | 13.056333 TB / 12159.7 GiB | 13.5 TB | FITS |

These are explicit capacity scenarios via a child-only df fixture and sparse
stat inputs, not claims that this busy host currently has 13.5TB free. Full
verbatim front-door renders are [100gb-plan.txt](100gb-plan.txt) and
[pile-plan.txt](pile-plan.txt); commands/scenarios are [plans.json](plans.json).
The 25n-knob negative control refuses at 100GB/2.6TB free, and one byte below the
progress reserve is rejected. See [100gb-25n-refusal.txt](100gb-25n-refusal.txt).

**The reserve excludes the source corpus itself**, as the hosting render says.
If the pile source must also occupy the same initially empty 13.5TB usable
volume, this conservative plan totals **14.366333TB and does not fit**. The
13.056TB scratch plan fits a 14TB scratch disc with approximately 13.5TB free;
co-hosting source requires a different placement or a tighter validated model.
No unconditional co-hosted-one-disc claim is made.

## Gates, verbatim

```text
100m P=8:
SXI2_PASS bytes=48768685 lite=1 members=6
PILE_FINISH_DONE chi=29716349 n=100000503 r=38647515 container=/mnt/nvme2n1/erikg/sol7f-gate-100m-p8-packed-final/frag.sxi2
merge_rc=0 finish_rc=0 cmp_rc=0
1b P=8:
SXI2_PASS bytes=467814822 lite=1 members=6
PILE_FINISH_DONE chi=283296933 n=1000000665 r=368036609 container=/mnt/nvme2n1/erikg/sol7f-gate-1b-p8-measured-final/frag.sxi2
merge_rc=0 finish_rc=0 cmp_rc=0
1b P=1:
SXI2_PASS bytes=467814822 lite=1 members=6
PILE_FINISH_DONE chi=283296933 n=1000000665 r=368036609 container=/mnt/nvme2n1/erikg/sol7f-gate-1b-p1-serial-final/frag.sxi2
merge_rc=0 finish_rc=0 cmp_rc=0
```

The 1GB cmp bank is exactly
`/mnt/nvme2n1/erikg/extcols-scratch/pilev2-g2-1b/frag.sxi2`.
The 100MB bank is the existing `mergperf-100m-snap.sxi2`; the prior task's
`mergerp` spelling does not exist. All final timed P>1 runs reuse the immutable
input-bound reference, with no diagnostic replay in the timed path.

Counter tuples are (comparisons, symbols, probes):

| Run | WALK | PLANNING | OVERLAP | signed SEAM |
|---|---|---|---|---|
| 1GB P=1 | [3674629435, 60485597061, 1987510452] | [0, 0, 0] | [0, 0, 0] | [0, 0, 0] |
| 1GB P=8 | [3674599453, 60478673268, 1987483450] | [36937, 203480, 2216] | [0, 0, 0] | [-29982, -6923793, -27002] |

P=1 is exactly the banked serial tuple. Producer sums, every range reference,
planning sums and signed deltas are independently checked. Charged work is
WALK + PLANNING = SERIAL_WALK + PLANNING + OVERLAP + SEAM; adding SEAM again to
WALK would double-count it. Full range journals: [seams-1b.log](seams-1b.log).

Standing results, verbatim summaries (full outputs in [validation.json](validation.json)
and [counter-summary.log](counter-summary.log)):

```text
SOL7F_SEGMENT_CODEC_PASS cases=512 widths=1..8 records=1..4096 u64_lengths=1 singleton_tail_elision=1 exact_roundtrip=1 packed=480 raw_fallback=32
SOL7F_XFS_EXPLICIT_EXTENT_PASS batches=2000 live_batches=11 variable_page_charges=1 fixed_slot_bytes=106496 budget_bytes=1171456 allocated_peak_bytes=438272 metadata_allowance_bytes=65536
SELFTEST_PASS count=20 seed=1 kway=8
SELFTEST_PASS count=20 seed=1 kway=8
FRESH_SEAM_COUNTER_WITNESS_PASS cases=40 fixtures=5 P=1,4,8,16 modes=cyclic,dollar rows=exact seeded_reference=exact per_seam_journal=exact OVERLAP=0 P1_extra=0
FLOW_ORACLE_PASS initial_minus_one=1 oversized_positive_window=1 unsigned_credit=1 blocked_until_consumed=1
SWEEP_STATE_CARRY_ORACLE_PASS streams=2048 splits=34785 NUL=1 byte05=1 singleton=1 periodic_colors=1 empty=1 ordered_witnesses=1 final_state=1
CHUNKED_REPAIR_ORACLE_PASS streams=128 widths=3392 workers=4 both_coalescers=1 class_flanks=1 padding_straddle=1 dollar=1 legal_00_05=1
SEAM_SPOOL_READER_ORACLE_PASS empty=1 adversarial_orders=2 padded_flanks=1 NUL=1 byte05=1 malformed_refusals=3
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 4m 34s
SOL7F_PLAN_ONLY_PASS cases=2 configured_spool=2n capacity_scenarios=2.6TB,13.5TB sparse_inputs=1
```

The standing threaded transport model passed 472 schedules and the wait model
1450; their complete JSON outputs are preserved separately. Native P=8 and
packaged runtime P=16 production column/class oracles pass, including the exact
minimum configured budget. Cargo's final packaged build passed.

## Complete 1GB physical measurement

`python3 tools/sol7f/gate.py 1b 8 measured-final`, using eight retained chunks,
96t merge/finish flags, P=8, W=1100, 100GiB per-process address-space cap,
`CROSS_V2_SPOOL_BYTES=2000001330`, effective data credits 1999994880 B.
Native benchmark build: `c++ -O3 -std=c++17 -pthread ... -latomic`;
the packaged O2 runtime has matching source and passed its native oracles.

* Full merge process wall: **302.657457s**; delta versus the supplied
  290.97s baseline: **+11.687457s
  (+4.017%)**. This is a cross-run observation,
  not an isolated causal estimate of backpressure cost on a shared box.
* End-to-end consumer completion: **421.871527s**.
* Sampled allocated peak: **8632426496 B =
  8.632420755n**. This includes 4,023,914,496 B of retained input
  chunks; output-only allocated peak is 4608512000 B.
  It is 32.421MB over a literal
  8.600n cutoff, while rounding to ~8.6n. Samples are every 10s; short-lived
  higher peaks remain possible.
* Producer credit blocking: **11 episodes**, **129.479176
  summed blocked thread-seconds**, not wall seconds. Producer 0 never blocked.

| Producer | Credit block episodes | Blocked seconds |
|---:|---:|---:|
| 0 | 0 | 0.000000 |
| 1 | 1 | 18.510841 |
| 2 | 1 | 18.508728 |
| 3 | 2 | 18.512771 |
| 4 | 2 | 18.519576 |
| 5 | 2 | 18.518772 |
| 6 | 2 | 18.505658 |
| 7 | 1 | 18.402830 |

The full trace is [sol7f-du.log](sol7f-du.log): elapsed seconds, output allocated,
input allocated, combined allocated, output apparent bytes. Sampling remains
alive across concurrent unlink/rename warnings. Attestation checks the complete
cadence and final coverage. [box-before-1b.txt](box-before-1b.txt) preserves the
leading CPU snapshot; the full snapshot is retained in scratch.

Earlier trials are not silently substituted for this complete measurement:
ordinary extending pwrite exposed XFS speculative preallocation and was stopped;
the unpacked strict-window trial was stopped after exceeding 320s; the first
packed point passed byte/counter gates at 265.891s merge /
393.868s completion, but its du thread stopped on a concurrent
unlink at 200s. The sampler was fixed and the final complete point above rerun
after all internal compilation/tests and the independent P=1 gate finished.

## Residual risks and acceptance limits

* Quiet-box qualification is unfulfilled. The final point had unrelated CPU
  work, although no internal compiler/test job overlapped its merge. Timing
  variance between the two packed points is explicit. contact_supervisor was
  unavailable; no normal supervisor messages or permission questions were sent.
* A strict 8.600n limit is missed by 0.032421n / 32.421MB in the sampled point.
  The intended approximate 8.6n shape is observed, and the wall is below 320s,
  but no strict disc-bar pass is claimed. The 10s sampler is not an instantaneous
  high-water proof; the spool data-page bound is separately enforced by credits.
* Capacity and time at 100GB/pile are extrapolations. The measured k=8, warm-cache
  rates do not validate k=100/1310 or cold NVMe behavior. Input/run/chi ratios,
  default hash spacing and 0.3x witness-spill compression need scale validation.
  Packed endpoint width also grows with n, so the 1GB compression/overlap ratio
  is not a pile-scale throughput guarantee.
  The pile co-hosted-source reserve does not fit 13.5TB; scratch alone does.
* The extent contract is verified on this XFS scratch volume; unsupported
  filesystems fail closed. There is no durable worker retry/process recovery.
  A permanently stopped consumer eventually blocks producers intentionally.

## Branch and commits

Branch `sol-7f-spool-window`, base `5d0b201`. Model proof `a149e24`, initial
window/planner `e4c4468`, exact XFS extents `a215c49`, packed-block optimization
`d4720b0`, final capacity renders/sampler fix `052490c`, final-source oracles
`948da15`, full measured gates/EOF permutations `47317d8`. The complete milestone
list is [commits.log](commits.log), captured before this final report commit.
