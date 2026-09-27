# Current state: pass 4 complete — G0, yeast, k10 PASS

- Extracted h10.head_sa: exactly 1,859,825,801 raw LE u64 values,
  14,878,606,408 bytes; full column byte check and independent LF spots passed.
- Implemented slim --head-sa with O(1) known-run head/tail/previous-tail
  lookups. Original LF/cache fallback remains; LF tables are still retained.
- G0 8/8 passed both modes, including duplicates-600k; invalid size/value
  sidecars fail loudly. Repeated after the collection correction below.
- Yeast byte identity, chi=85,404,240 and NumPy set equality passed.
  Resolve 34.839 s versus supplied 272 s; dump 502.60 s, 3.577 GB peak.
- Full k10 byte identity, chi=1,627,063,183 and NumPy set equality passed.
  Resolve 437.509 s (7.292 min), LF steps=0; dump 5255 s. Complete
  dump/cmp/sweep/set pipeline 5809.321 s (1h 36m 49s), peak 73.982 GB.
- Necessary correctness fix: collection LCP stops before newline. Sparse
  ends come from existing parse/dictionary scans (865 k10 ends); no text scan.
  The failed unclamped attempt is preserved; all final gates use the fix.
- Raw 8 B/run heads cost 21.920 GB at 466: corrected conditional model
  213.867 GB with LF, 170.027 GB without LF (no-LF remains unimplemented).
- No commits/staging, no M/b_bwt/w_wt, no other lane's PIDs touched.
  All measured peaks <150 GB. No requested gates remain running.

Evidence: bit6/gate_logs/slim/pass4/; SLIM_ACCEPTANCE.md and SLIM_COST.md.
Outputs: /tmp/laneQ/pass4/ and the supplied k10 directory's h10.head_sa and
h10.slim.head.agg. Front-end sample emission and native v5 container remain
outside this consumer gate; the binding no-extra-walk provenance is unchanged.
No contact_supervisor tool was exposed in this runtime.

# Historical pass-1..4 working notes (retained)

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

## Pass 4 — active

Started at 8330966. No contact_supervisor tool is exposed in this runtime.
The referenced gate_logs/slim directory and pass-3 automation sources are
absent from the checkout; existing /tmp/laneQ/pass3 artifacts and final runner
log were read instead. Pass-4 evidence goes in bit6/gate_logs/slim/pass4.
Extracting k10 saFirst in one bounded-memory sequential CRA1 pass, with exact
file/count validation and deterministic spread checks of mirrored ri4 tails
and singleton heads. No existing input or another lane's PID is modified.

Pass-4 extraction PASS: h10.head_sa has exactly 1,859,825,801 raw LE u64s
(14,878,606,408 bytes); full CRA1 pass 44.965 s. 1,025 spread tails obey
SA = n-1-mirrored_sample; 32 sampled singleton heads equal those tails.
The sidecar-aware dumper compiles. Direct head/tail queries take known run
IDs, avoiding both LF walks and row-to-run binary searches in query chunks.
LF tables remain for arbitrary calibration rows and absent-sidecar fallback.

Pass-4 progress: G0 PASS 8/8: sidecar and fallback outputs byte-identical to every baseline, including duplicates-600k. Invalid sidecar size/value rejected.

Pass-4 additional k10 sample validation PASS: 1,025 spread runs resolved
independently through the existing LF machinery; zero saFirst/saLast
mismatches, zero sentinel failures (4,260,344 LF steps). Full byte comparison
also confirms h10.head_sa equals the entire CRA1 saFirst column.
The final binary differs from the initial G0/active yeast build only in
accounting: head_lf_steps subtracts startup calibration steps. Final binary
passed duplicates-600k byte identity again; k10 will use this final binary.

Pass-4 progress: yeast PASS: aggregate byte-identical; chi=85404240; NumPy sorted-set equality; zero LF steps. {"wall_seconds": 561.8613212879281, "peak_rss_kib": 3498920}

Pass-4 k10 full pilot active: owned time-wrapper PGID 2961626 (see
pass4/active.json), output h10.slim.head.agg. First 3,211,264 runs: resolve
1.214 s, LCE 17.317 s, LF steps 0. Peak ~60 GB. The gate runner will compare
the entire output, stream the sweep, and check NumPy sorted-set equality
before recording PASS. No completion is claimed while that runner is active.

Pass-4 unexpected finding: full-prefix comparison failed at topLCP run 1.
All inspected SA endpoints agree. The legacy multi-string baseline excludes
newline terminators from LCP; SlimLCE currently compares the concatenation
across them. Independent flat reads confirm the failing SA positions point
to newline bytes. Only our PGID 2961626 was stopped; the failed attempt's
logs/output are preserved as *.unclamped-attempt.log and
h10.slim.head.unclamped.agg.partial. No k10 acceptance claimed.
No contact_supervisor tool is exposed. The required byte-identity semantics
are unambiguous: derive sparse string ends from existing parse/dictionary
and clamp LCE at those ends, without scanning text or building forbidden
structures. This necessary collection-boundary correction will be gated
before restarting the full pilot. Head-SA extraction/resolution checks pass.

Pass-4 progress: G0 PASS 8/8: sidecar and fallback outputs byte-identical to every baseline, including duplicates-600k. Invalid sidecar size/value rejected.

Pass-4 collection correction sampled gate PASS: all four fields agree with
h10.walk.agg on the first 10,000 runs plus 100,000 spread runs (110,000
checks total), with all 865 string ends recovered from parse/dictionary.
G0 was repeated after this correction and passes 8/8 in both resolver modes.
Yeast full re-gate is running. A bounded thread-count probe will select a
measured k10 LCE worker count; exact verification remains enabled throughout.

Pass-4 progress: yeast PASS: aggregate byte-identical; chi=85404240; NumPy sorted-set equality; zero LF steps. {"wall_seconds": 534.8318109019892, "peak_rss_kib": 3498920}

Pass-4 final restart: G0 8/8 and full yeast re-gate passed after the
collection-boundary fix. Yeast resolve 34.839 s, dump 502.60 s, peak
3.577 GB; byte identity, chi=85,404,240 and NumPy set equality passed.
A three-repeat 110,000-run exact LCE probe measured 64 workers fastest
(~0.184 s versus ~0.223 s at 32); all 18 configurations matched.
Full k10 restarted with -t 64, with an automatic first-chunk byte guard
before the full cmp/sweep/set ladder. Active PID/PGID: pass4/active.json.

Pass-4 corrected full pilot: first 65,536 runs passed automatic byte identity
in all four CRA1 columns. At 24,182,784 runs, resolve=5.585 s,
LCE=67.867 s, LF steps=0. Full-output gates remain pending.

Pass-4 corrected prefix gate PASS: all four CRA1 columns are byte-identical
for the first 116,457,472 completed k10 runs, beyond the stopped attempt's
prefix. Evidence: pass4/k10.corrected-prefix-cmp.log. Full pilot continues.

Pass-4 full-pilot quarter milestone: 480,313,344 / 1,859,825,801 runs;
resolve 110.296 s, LCE 1307.432 s, LF steps 0, peak ~63.2 GB.
Full comparison/sweep/set verification remain queued after construction.

Pass-4 full-pilot halfway milestone: 945,881,088 / 1,859,825,801 runs;
resolve 218.026 s, LCE 2535.937 s, LF steps 0, peak ~66.6 GB.
Construction elapsed 2968.819 s. Final gates remain pending.

Pass-4 full-pilot three-quarter milestone: 1,403,060,224 / 1,859,825,801
runs; resolve 327.156 s, LCE 3565.536 s, LF steps 0, peak ~70.3 GB.
Construction elapsed 4111.134 s. Full byte/sweep/set ladder remains queued.

Pass-4 full k10 construction and byte gate PASS: all 1,859,825,801 runs,
head_direct=R, LF steps=0. Resolve 437.509169 s; complete dump 5255 s
(1h 27m 35s); peak 72,248,348 KiB = 73.982 GB. Full cmp returned 0.
The streamed sweep and NumPy witness check are now running in order.

Pass-4 k10 streamed sweep PASS: chi=1,627,063,183; wall 213.928 s
(monitored command), peak 6,144 KiB. NumPy full sorted-set equality against
chi_h10.sA is the final running gate. Full aggregate byte identity passed.

Pass-4 progress: k10 PASS: aggregate byte-identical; chi=1627063183; NumPy sorted-set equality; zero LF steps. {"wall_seconds": 5809.32109882799, "peak_rss_kib": 72248348}

## Pass 4 complete

All requested gates passed. The complete k10 witness arrays are NumPy
sorted-set equal, with all 1,627,063,183 values unique. Pipeline wall
5809.321 s, peak 73.982 GB; sidecar extraction 44.965 s separately.
The current report at the top and SLIM_ACCEPTANCE.md supersede the earlier
pending/failed-attempt entries preserved above. No commits; this lane has
no active jobs.
