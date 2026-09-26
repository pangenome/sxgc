# SLIM lane — in progress

Sources: bit6/slim_lce.hpp (two-level fingerprint LCE, sparse position maps,
optional bounded dictionary cache, chunked CRA1 output); --slim --resolve-ri4
in chi_rspace_dump; xsa chi-rspace --stream-agg consumes four buffered streams
and streams witness output, holding neither the full ri4 nor the witness set.

G0: PASS all 8 available texts, including duplicates-600k, resident and streamed
D; duplicates witness byte-identical old/streamed sweep. /tmp/laneQ/G0.log.

G1: BLOCKED at input validation. /tmp/laneY/yp2new.ri4 is explicitly the
runs-only artifact from ri4_from_rle.cpp, whose SA samples are all INF. It
cannot drive the REQUIRED committed --resolve-ri4 machinery. No paired
real-sample yeast_pfp2 ri4 found. y2new.ri4 is the WRONG yeast chain.
The given .agg and .sA remain read-only oracles; do not manufacture samples
from the .agg and call that an independent end-to-end gate.

Design contradiction: two-sided direct verification of every matched symbol
is O(LCE), not O(tau). The implementation is deterministic/fail-loud and
honestly reports verified-symbol counts. Hash jumps do NOT remove that scan.
Decreasing tau1 to 2 increases sampling density; it cannot itself induce hash
collisions. --inject-fingerprint-error explicitly perturbs a hash-derived
candidate so that the direct verifier must reject it.

No contact_supervisor tool is present in ALL_TOOLS in this runtime. A valid
paired sample artifact and an explicit resolution of the verification-time
requirement are the two unresolved coordination issues. No commits/staging.

## Final state and review contract

**Not accepted / not complete.** The requested deterministic O(tau) query
bound cannot be obtained by checking every matched symbol directly. G1
cannot use the supplied all-INF ri4. The ladder stops there: G2 and G3 are
not declared green. G3's tau1=2 plus explicit candidate fault-injection
battery is implemented in the runner but not executed out of ladder order.

Additional independent diagnostics (not substitutes for G1):
- 216,000 exact capped fingerprint LCE tests, random/uniform/periodic, pass.
- All eight battery texts pass resident and streamed dictionary byte gates.
- Final G0 includes direct flat-text calibration with ri4 row offset 0 and
  the PFP coordinate shift 10. Original ROW_OFF=10 applied to the old PFP
  SA resolver, not to ri4 rows. The committed SampleResolver is untouched.
- On yeast, 100,005 spread runs have exact top/interior LCE using baseline
  SA endpoints, for four tau/cache configurations. Clearly oracle-seeded.
- Baseline-agg streamed yeast sweep: chi=85,404,240; old/streamed witness
  byte identity; NumPy sorted-set equality versus the provided oracle.
- Yeast all-structure build, streamed dictionary: 21.56 s, 3.30555 GB peak.
- Conditional 466 peak: 191.947 GB including 10 GB reserve, streamed D and
  8x-class sampling. Literal 1x-class sampling: 228.921 GB. Resident D:
  447.116 GB. Full cost assumptions and phase measurements: SLIM_COST.md.

## Commands and resumption

```
bash tools/build_slim_dump.sh
(cd xsa && cargo build --release)
python3 tools/slim_gate.py --stage G0
python3 tools/slim_gate.py --stage G1   # correctly fails on supplied ri4
bash tools/build_slim_dump.sh /tmp/laneQ/slim_lce_test tools/slim_lce_test.cpp
/tmp/laneQ/slim_lce_test
bash tools/build_slim_dump.sh /tmp/laneQ/slim_lce_probe tools/slim_lce_probe.cpp
/tmp/laneQ/slim_lce_probe PREFIX BASELINE_AGG N TAU1 TAU2 SAMPLE_COUNT DICT_STREAM_0_OR_1
```

To resume G1, supply an independently valid tail-sampled ri4 for the
**yeast_pfp2 chain**, then use `--yeast-ri4 PATH` on the G1 runner. Producing
samples from the .agg oracle would only give another conditional primitive
gate. Rebuilding samples with a legacy O(n) walk is a separate artifact
preparation cost, not a slim parse-space construction success. No such
oracle repair was done in this lane. After G1 passes, rerun/complete G2
measurements and `python3 tools/slim_gate.py --stage G3`.

Actual outputs: /tmp/laneQ/*.agg, yeast.baseline.{old,stream}.sA. No complete
slim yeast aggregate was produced. G0 witness output is byte-identical.
Reviewable logs are copied under bit6/gate_logs/slim. Original external
inputs were not edited. No staging or commits. Reviewer approval remains
required; this handoff does not declare approval.

## Pass 2 — blocked at resolver dependency audit (2026-09-26)

Task 1 cannot be implemented by reusing the supplied legacy resolver while
also obeying “never build M/b_bwt/w_wt.” The committed C++ legacy mode calls
`pfpds::pfp_sa_support` (`chi_rspace_dump.cpp:619,656`). Its query directly
reads `b_bwt_rank_1`, `b_bwt_select_1`, `M`, `w_wt.range_select`, and `saP`
(`bit6/pfp_ds_vendor/pfp/sa_support.hpp:55-66`). ROW_OFF=10 changes the row
coordinate only; it does not eliminate these dependencies.

The named `resolve` and `resolve_inv` functions are Python prototypes, not
a separate slim-compatible C++ resolver. `tools/parse_resolve_proto.py`
builds M and text-row boundaries (lines 126 onward), parse SA, and colex
BWT occurrence lists; resolve (line 217) queries them. `tools/c_probe_r3.py`
inherits that builder (line 32), constructs parse ISA, and adds a dictionary
suffix-to-M-class map. Porting or renaming those structures would not satisfy
the stated reuse/no-M requirement.

The requested RESEARCH.md “SLIM LANE (astra) first pass” entry and correction
#4 are absent from this checkout at c0e81d3. The supplied pass-2 instruction
is accepted as the adjudication retiring O(tau); that is no longer a blocker.
The remaining blocker is the resolver dependency conflict, not verification
complexity. A compatible resolver source or a revised resolver constraint is
needed to continue task 1. This is a source-availability finding, not a claim
that an alternative algorithm is impossible.

No contact_supervisor/intercom tool is exposed in this runtime's ALL_TOOLS,
so the required decision escalation could not be delivered. No forbidden
structures were built, no samples were fabricated from oracles, no code was
changed in pass 2, and no pass-1 gates were rerun. Ordered downstream tasks
(G0 re-gate, G1 end-to-end, G2 final, G3) remain not run. Pass-1 files and logs
are preserved. Evidence: bit6/gate_logs/slim/pass2-resolver-audit.log.
Reviewer approval remains outstanding. No staging or commits.
