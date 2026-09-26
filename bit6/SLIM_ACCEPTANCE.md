# SLIM pass-3 acceptance report

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
