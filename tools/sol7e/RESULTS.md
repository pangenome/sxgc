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
