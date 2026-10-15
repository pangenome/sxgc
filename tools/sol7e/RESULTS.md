# sol7e results (in progress)

Production now supports opt-in fresh-start producers via
`CROSS_V2_PRODUCERS` (default 1). Independent compressed segment spools feed
one ordered canonical-band publisher, preserving the existing consumer.
Per-seam PLANNING/OVERLAP/signed SEAM journals cross-check against the exact
sol7d serial trajectory. See PROTOCOL.md for scratch bounds and limitations.

Fresh row witness: 40 cases, 5 fixtures, cyclic/dollar, P=1/4/8/16;
exact rows/flanks, 242 seams, no gaps/duplicates. Original seeded witness:
40 cases, every ordered comparison operand identical, SEAM=0. Full per-seam
values are retained verbatim in counter-summary.log and counter-witness.json.

100 MB P=16 diagnostic gate: merge_rc=0 finish_rc=0 cmp_rc=0,
chi-expect=29716349 (asserted in-process). The task's literal `mergerp` bank
path does not exist; cmp used the existing `mergperf-100m-snap.sxi2` bank.
The diagnostic includes explicitly labeled serial oracle replay. Its 50.757 s
merge wall is NOT a launch wall; the box has unrelated CPU activity.
WALK=(355645746,4016965734,90123088), PLANNING=(61352,276526,1146),
OVERLAP=(0,0,0), signed SEAM=(1998,-109452,-8836). No work was hidden.

The original slow scalar diagnostic was terminated before reference
acquisition finished, then rerun with bulk-decoded oracle fronts. Its failed
logs remain in scratch. The optimized oracle still passes all 40 seeded trace
and 40 fresh row/counter cases. Standing tests and P=16 borrowed cache-span
validation are green. Cargo check passed after fixing the changed chain.rs
entry in the source manifest; the first manifest rejection remains recorded.

1 GB gate and wall sweep: pending. No quiet-box launch claim yet.

Reusable reference path: P=16 oracle counters were coarsened additively to
P=4 for the same nested split targets, then independently validated against
the actual P=4 planner/source identity. The 100 MB P=4 reference-only gate
passed chi=29716349 and cmp=0 with replay=0. A corrupted reference identity
was refused before terminal publication (`REFERENCE_MISMATCH_REFUSED
no_terminal=1`). See reference-test.json; its wall is a nonquiet diagnostic.

## 1 GB P=16 diagnostic gate

```text
SXI2_PASS bytes=467814822 lite=1 members=6
PILE_FINISH_DONE chi=283296933 n=1000000665 r=368036609 container=/mnt/nvme2n1/erikg/sol7e-gate-1b-p16-oracle/frag.sxi2
merge_rc=0 finish_rc=0 cmp_rc=0
CROSS_STAGE_WORK stage=WALK pass=0 comparisons=3674606698 symbols_compared=60477880116 probes=1987420158
CROSS_STAGE_WORK stage=PLANNING pass=0 comparisons=78957 symbols_compared=452649 probes=5180
CROSS_STAGE_WORK stage=OVERLAP pass=0 comparisons=0 symbols_compared=0 probes=0
CROSS_SEAM_TOTAL comparisons=-22737 symbols_compared=-7716945 probes=-90294 reference=serial_range signed=1 replay=1
```

Serial reference plus prepare is EXACTLY
(3674629435,60485597061,1987510452), matching the supplied baseline.
All 16 per-range journals are retained verbatim in seams-1b.log. Fresh heaps
slightly reduce symbols/probes on this fixture: the measured SEAM is signed,
not a claim of 60.5B additional symbols. PLANNING+OVERLAP+SEAM divided by WALK
is (+0.001530%, -0.012011%, -0.004283%) per metric. No 30% stop was triggered.
The diagnostic's 761.807 s internal merge wall includes the explicit reference
replay and is NOT a launch wall. The standalone replay-free P sweep follows.

P=1 replay-free 1 GB gate is now independently green: chi=283296933, cmp=0,
WALK=(3674629435,60485597061,1987510452) EXACTLY, PLANNING=OVERLAP=SEAM=(0,0,0).
Observed merge wall 611.795 s, end-to-end 681.168 s; unrelated CPU load means
these are not quiet-box attestations. See gate1b-p1.json.
