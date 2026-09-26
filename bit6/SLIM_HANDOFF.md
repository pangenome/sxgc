# Current state: pass 3 gates complete

G1, G0, G2 measurements, and G3 passed. See SLIM_ACCEPTANCE.md
and SLIM_COST.md for current results and 466 conditionality.
Evidence: bit6/gate_logs/slim/pass3/. Artifacts: /tmp/laneQ/pass3/.
The earlier blocked entries below are preserved as history.


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

## Pass 3 — active (2026-09-26)

The supplied correction #6 retires the incompatible parse-resolver mandate.
Correction #6 is absent from this checkout of RESEARCH.md; the task itself
is authoritative. No contact_supervisor tool is available in ALL_TOOLS.

Matched yp2t.lcp_index.lcp_index with yp2t.bwt.{heads,len}: R=100,904,881.
Text audit: n=3,336,986,760, one trailing newline, 9,901 internal ! symbols.
BWT has exactly one newline endmarker; this is ONE string, not 9,901
strings. Derived /tmp/laneQ/pass3/yp2real.sc = s0\t0\t3336986759.
The production sample walk is running independently of either oracle.
Watcher pid 2247122; status /tmp/laneQ/pass3/yeast.samples.status.json;
command and timing /tmp/laneQ/pass3/yeast.samples.log. Watcher monitors
only its own process group, with a 150 GB RSS stop threshold.
G1, G0 revalidation, G2 final, and G3 are pending in the ordered ladder.

Gate automation watcher pid 2252640. /tmp/laneQ/pass3/gates.status.json
tracks the ordered runner, which waits for sample validation, then runs G1
(streamed dictionary, both sweeps), G0 (three texts, slim resident/streamed
and standalone --resolve-ri4), G2 sensitivity, and G3 (eight texts).
New query instrumentation separates chunk resolve/LCE/write wall time and
records mean/max directly verified phrase positions. The legacy resolver
and production sample binary remain unchanged.

Located and read correction #6 in the main worktree, read-only:
/home/erikg/sxgc/RESEARCH.md:1173. Its adjacent binding provenance
clarification says the current production walk is PILOT LEGACY REMEDIATION
ONLY. End-state PFP/RLBWT front-end must emit head/tail SA samples and
anchors during the mandatory build: no TeraLCP, no lcp_index, no separate
O(n) walk. This pass gates the consumer, not that future front-end
integration. Exact text saved in pass3/correction6-source.log.

Pass-3 progress: Production sample artifact validated: n/R match; every sample < n; BWT byte-identical to the paired runs-only ri4.

Pass-3 performance finding: the uncached first 65,536-run yeast chunk
spent 259.065 s resolving versus 0.347 s in LCE. The next chunk is also
slow. A bounded, opt-in exact LF-position cache is being tested before
restarting G1; it memoizes only independently sample-derived answers, uses
full-key checks and synchronized slots, and adds no M/b_bwt/w_wt.
The original uncached attempt is still running while this is verified.

Pass-3 progress: Gate runner stopped on AssertionError(('yeast.dump', 143)); no downstream gates claimed.

Intentional performance restart: stopped only our uncached dump pid
2293735 after the first chunk measured 259.065 s resolve vs 0.347 s LCE.
The ordered runner aborted as designed; its failed-exit entry above is
this deliberate stop, not a correctness mismatch. Evidence retained as
pass3/yeast.dump.uncached-attempt.log and gates.uncached-attempt.log.
The bounded exact cache passed 833,579 explicit-SA checks (collision
eviction, 100,001-step walk, 8-thread access). Restarted G1 with
--resolve-cache, preserving independent yp2real.ri4. New watcher pid 2307517;
status /tmp/laneQ/pass3/gates.cached.status.json. Cache maximum 201,326,592
bytes, checkpoint stack bounded at 64 KiB per active resolver call.
No M/b_bwt/w_wt, no oracle seeds, no front-end scope change.

Pass-3 progress: Production sample artifact validated: n/R match; every sample < n; BWT byte-identical to the paired runs-only ri4.

Pass-3 progress: Gate runner stopped on AssertionError(('yeast.dump', 143)); no downstream gates claimed.

Final cache configuration: 2^26 slots, 1,610,612,736 bytes at yeast;
full keys and locked values, with read-only atomic-tag rejection of misses.
Final implementation again passed 833,579 explicit-SA/concurrency checks.
The 8m-slot pilot reached >5.3m runs, but resolution remained dominant;
its partial evidence is retained under yeast.dump.cache8m-attempt.log.
Stopped only own pid 2307586 and restarted G1 with the larger fixed cap.
Final gate watcher pid 2334223; /tmp/laneQ/pass3/gates.final.status.json.
No correctness failure preceded either intentional performance restart.

Pass-3 progress: Production sample artifact validated: n/R match; every sample < n; BWT byte-identical to the paired runs-only ri4.

Pass-3 progress: G1 PASS. See bit6/gate_logs/slim/pass3/gates.log and yeast phase logs.

Pass-3 progress: G0 PASS on the three requested texts, both LCE paths, plus streamed dictionary. Retired --resolve-parse was not run.

Pass-3 progress: G2 measurements complete; final interpretation and conditional 466 projection awaiting report review.

Pass-3 progress: G3 PASS 8/8. Ordered execution complete; acceptance report and final cost interpretation remain.

## Pass 3 complete

**Yeast acceptance gates passed. 466 feasibility remains conditional.**

- Real samples: independent production walk completed in 3237.03 s,
  10.599 GB peak; n=3,336,986,760, R=100,904,881;
  every sample below n. The chain has one newline-delimited string.
- G1: new slim aggregate byte-identical to the reference; streamed and
  resident sweeps both chi=85,404,240; witnesses byte-identical and NumPy
  sorted-set equal to the supplied oracle. Full dump 1209.93 s,
  4.925 GB peak.
- G0: all three requested battery texts passed both LCE paths with
  `--resolve-ri4`; streamed dictionary also passed. No forbidden resolver.
- G2: full per-phase resolve/LCE/write wall and cumulative peak RSS,
  verification mean 15.329365891 phrases/seed and maximum
  22262, tau sensitivity, and corrected 466 model
  recorded in SLIM_COST.md. O(tau) and legacy parse-resolver mandates retired.
- Runtime change: a bounded 1.611 GB exact LF-position cache avoids repeated
  sample walks; verified against explicit suffix arrays and uncached resolution.
  The original uncached partial attempt is retained, not counted as a full gate.
- G3: 8/8 tau1=2 clean runs matched; 8/8 induced wrong candidates failed
  loudly. Lowering tau alone does not induce errors.

V5 head samples are the adopted O(1)/run-head fix for the measured k10
2,156-step tail-walk mean. Adding them to the current LF memory model gives
205.990 GB; an unimplemented no-LF variant is conditionally 162.150 GB.
No 466 or v5 implementation acceptance is claimed. This walk is pilot-only
legacy remediation. The binding end-state requires head/tail samples and
anchors from the mandatory front-end build, with no TeraLCP, lcp_index, or
separate O(n) sample walk; that integration remains ungated here.
All observed peaks were below 150 GB.

Evidence: `bit6/gate_logs/slim/pass3/gates.log`, individual phase/gate logs,
and `SLIM_COST.md`. Large outputs are in `/tmp/laneQ/pass3/`. No commits,
no M/b_bwt/w_wt, and no interference with the separate Lean worktree.
This report records gate results; it does not substitute for reviewer approval.
