# sol7c: promotion blocked by the bounded work contract

Base `1c19904`; branch `sol-7c-sharded-walk`. The P-way production walk is
**not implemented** and the launch performance objective is **not achieved**.
No production source or protocol files changed. This is a native preflight
and counterexample, not a qualifying pipeline or a 1 GB overhead refusal.

## Promotion versus rebuild

Nothing was promoted. The existing planner at
`bit6/cross_lcp_merge.cpp:2975` supplies exact disjoint row ranges. The heap
at `:3104` restarts independently, so it does not supply the authorized work
identity. `SequentialV2` (`:3383`, forced S=1 at `:3422`) would additionally
need independent production sinks and correct global row/run/seam reduction.
Historical sol5d `7a42011` overlaps counting and replay, not a bounded halo
that supplies serial heap checkpoints. Details and source references are in
AUDIT.md. No wider rebaseline was assumed.

## Native seam and work witnesses

`seam_witness.cpp` directly invokes the native planner and heap with P
independent test sinks. Each range owns a private vector, so even an erroneous
overlap does not introduce a data race in the witness. After joining, it
checks exact full row order against brute force, contiguous exclusive ranges,
and both sides of every interior seam, including repeated split fronts.

```text
SHARD_ROW_SEAM_WITNESS_PASS cases=8 seams=42 P=1,4,8,16 modes=cyclic,dollar exact_rows=1 exact_flanks=1 gaps=0 duplicated_rows=0 production_bands=unvalidated
```

This proves the tested row handoffs, not production band ownership, bounded
overlap reconstruction, or a complete seam protocol. No new production
interleaving was introduced. The existing 472-schedule threaded model and
1,450-schedule wait model were rerun; zero deadlocks, lost or duplicate rows
in the threaded model, peak downstream backlog 1,100. They retain sol7b's
canonical-band abstraction and do not validate a P-way native pipeline.

`work_witness.py` extends sol7b's fixed eight-side, 24-byte native fixture.
Every invocation passes native brute cyclic/dollar order and the legacy
sparse seam/run checks. Production v2 remains serial and its three stage
budgets are identical across these environment settings.

Every tuple below is (comparisons, symbols, probes); these are fixture
measurements, **not 1 GB launch numbers**. PLANNING and OVERLAP are analytic
names for the captured stage work, not newly implemented production journals.

| P | mode | PLANNING | WALK heap | OVERLAP | unaccounted heap delta vs P=1 |
|---:|---|---|---|---|---|
| 1 | cyclic | (0,0,0) | (93,1535,124) | (0,0,0) | (0,0,0) |
| 4 | cyclic | (112,1428,112) | (72,1728,144) | (0,0,0) | (-21,193,20) |
| 8 | cyclic | (266,3388,266) | (72,1728,144) | (0,0,0) | (-21,193,20) |
| 16 | cyclic | (560,7140,560) | (72,1728,144) | (0,0,0) | (-21,193,20) |
| 1 | dollar | (0,0,0) | (95,738,0) | (0,0,0) | (0,0,0) |
| 4 | dollar | (126,808,0) | (78,686,0) | (0,0,0) | (-17,-52,0) |
| 8 | dollar | (294,1881,0) | (78,686,0) | (0,0,0) | (-17,-52,0) |
| 16 | dollar | (630,4040,0) | (78,686,0) | (0,0,0) | (-17,-52,0) |

The heap changes even though no overlap rows are walked. In particular,
negative comparison deltas cannot be nonnegative duplicated work. Adding
planner work separately does not eliminate that residual. This rejects the
unmodified candidate under the authorized identity; it does not prove no
replacement algorithm can satisfy it. Per-metric planner/serial-heap ratios
are retained in work-witness.json, with undefined zero-denominator ratios
encoded as null. No percentage combines unlike units.

## Large gates and wall sweep

NOT RUN: 100 MB chi=29716349 and cmp against
`/mnt/nvme2n1/erikg/extcols-scratch/xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2`.

NOT RUN: 1 GB chi=283296933 and cmp against
`/mnt/nvme2n1/erikg/extcols-scratch/pilev2-g2-1b/frag.sxi2`.

NOT RUN: P=1 1 GB identity (comparisons 3,674,629,435 / symbols
60,485,597,061 / probes 1,987,510,452). These remain the supplied baseline,
not a fresh attestation. P=1 fixture planner work is measured as zero.

| P | 1 GB 96t / 100 GiB wall | PLANNING/OVERLAP overhead |
|---:|---|---|
| 1 | NOT RUN | NOT MEASURED |
| 4 | NOT RUN | NOT MEASURED |
| 8 | NOT RUN | NOT MEASURED |
| 16 | NOT RUN | NOT MEASURED |

There is no qualifying implementation to benchmark. The <=300 s pass and
<=270 s launch bars remain open. Amdahl's residual fraction is unmeasured;
665.33 s is the supplied sol7a baseline. The 30% 1 GB stop threshold was
neither measured nor invoked. Small correctness checks overlapped with
compiler jobs and other small oracle checks; **no wall measurements** are
reported. P=16 front-decode and shared span scaling remain unmeasured.

## Residual risks and coordination

The production walk remains serial. Row-to-band reduction, independent band
publication and seam-class handoff still need implementation and modeling.
No persistent retry, disk publication, overlap journal or 1 GB performance
claim follows from these tests. A replacement preserving the serial trace,
or authorization for an explicit additional heap-rebaseline category, is
needed before the audited candidate can meet the specified work identity.
`contact_supervisor` was absent from the complete callable tool catalog, so
the scope question could not be delivered through the requested channel.

Scratch, native binaries and full logs: `/mnt/nvme2n1/erikg/sol7c-audit/`.
Standing oracle output and exact commands are retained in validation.json.
Milestone commits are recorded by `git log --oneline 1c19904..HEAD`.
