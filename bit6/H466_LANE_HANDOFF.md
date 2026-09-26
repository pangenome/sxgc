# 466 r-space confirmation lane — STATE (STOPPED at step 1: text-compat MISMATCH)

Date: 2026-09-26. Worktree /tmp/sxgc-laneK at 68acf72. No commits.

## Step 1 verdict: the "466 parse" does not exist — h10ss_pfp is a
## DIFFERENT corpus (30 GB / 865 strings vs 1.4 TB / 38,790 strings)

Hard evidence (sizes are exact file-byte identities, not estimates):

| artifact | n | k (strings) | R (runs) | text file |
|---|---|---|---|---|
| h466.ri4                        | 1,403,221,068,481 | 38,790 | 2,739,735,806 | h466_rl.txt = 1,403,221,068,481 B |
| h10new2.ri4 (k10 chain)         | 30,151,407,545    | 865    | 1,859,825,801 | h10ss.txt   = 30,151,407,545 B |

- h466.ri4 was produced by `teralcp_chi --samples` at 466 (see chi.log:
  "wrote h466.ri4 (R=2739735806, sa_w=41)").
- The only PFP parse on disk (outside yeast/scaling) is
  `k10/h10ss_pfp.parse` = 1,334,126,904 B / 4 = **333,531,726 phrases**.
  Over the 1.4 TB h466 text that would be an average phrase of ~4,200 B
  (implausible for w=10); over the 30 GB h10ss.txt it is ~90 B/phrase —
  matching the k10/yeast regime. ⇒ h10ss_pfp parses **h10ss.txt**, not 466.
- `find` over /mnt/nvme3n1,/home/erikg for `*.dict`>100MB and `*466*parse*`
  returns **no 466 parse**. The 466 PFP build is the *queued iteration-2 job*
  ("PFP-BWT 466 rebuild once"), not an existing artifact.

⇒ The committed dumper cannot run at 466 at all: `--parse` is mandatory,
and h10ss_pfp's row space (n=3.0e10) is unrelated to h466.ri4's (n=1.4e12).
Per the task's own instruction, STOPPED; fallback is a supervisor decision.

## Cost of the two real 466-route facts

Production 466 machine already ran, once (k466/chi.log):
`teralcp_chi h466rt.lcp_index.lcp_index --rlbwt h466rt --sidecar h466_rl.txt.names.tsv --samples h466.ri4 -o chi_h466.sA -t 96`
  → chi = 2,249,968,075; **wall 135,741.70 s (37.7 h); maxRSS 292,826,944 KB (293 GB)**.
TeraLCP index build: 20,063 s (5.6 h), 91.9 GB RSS.

That walk is what computes the four `.agg` arrays (per-string LF walks with
φ-interval tracking; saLast == the .ri4 v4 per-run SA sample; saFirst/topLCP
at run heads; interiorMin as the running min over the run interior).
**No O(r)-time shortcut exists for saFirst**: SA[run-head] = SA[LF^k(head)]+k
needs the walk (the .ri4 stores only one SA sample per run, at the run tail).

## Feasible routes (need a decision)

(A) **Re-run the production walk with a binary `.agg` writer** (~38 h, 293 GB
    RSS, fits 1 TB): add `--agg-out F` to bit6/teralcp_chi.cpp (write CRA1:
    magic+R+topLCP+saFirst+saLast+interiorMin), run it at 466, then
    `xsa chi-rspace --ri4 h466.ri4 --agg h466.agg` → gate χ==2,249,968,075 +
    witness-set equality vs chi_h466.sA (18 GB, exists). O(n) time, O(r) space.
    This exercises the **Rust r-space sweep at 466**; the aggregate production
    stays the production O(n) walk.

(B) **Build a 466 PFP parse** (pfp++ over 1.4 TB; the syng 466 build has run
    40+ h already) and then run the dump in PFP/`--pfp-index` mode. This would
    make aggregate production O(R polylog) *if* the PFP structures fit:
    b_bwt is O(n)=180 GB and |M| is the open question (≈0.095n at yeast →
    ~137 G entries × 24 B ≈ 3.3 TB ⇒ infeasible; ≈3.1·r ⇒ ~206 GB ⇒ feasible).
    A supervisor probe (m-scaling-probe, /tmp/pfp_index_build_main on
    h10ss_pfp, 700 GB cap) is measuring |M|'s scaling right now.

(C) **Intermediate full-pipeline validation at k10 (30 GB)** — all artifacts
    exist (h10ss_pfp.parse/.dict, h10new2.ri4, h10rt.lcp_index 51 GB, oracle
    chi_h10.sA = 1,627,063,183 witnesses). dump (PFP mode) + `xsa chi-rspace`
    vs the oracle. Feasible today (~100 GB RSS), validates the r-space
    machinery at 30 GB / R=1.86e9, but is **not** 466.

Recommendation: (C) now (cheap, real gate, no decision needed) + decision on
(A) vs (B). The 466 *goal* (χ=2,249,968,075 + witness-set equality) is
reachable only via (A) today.

## Nothing else was started

No `--lcp-index` resurrection was needed yet (would not have helped: the
committed dumper still requires PFP D/PP/PF for resolve_row/interiorMin, and
loader aside, `--parse` is mandatory). Probe file
/mnt/nvme3n1/erikg/h10_index_probe.pfpidx untouched by this lane (still being
written by the supervisor's probe process).

## UPDATE — supervisor decision: (C) now, then (A). Findings since:

1. **Pairing trap found at k10 too.** h10new2.ri4 run-char histogram =
   {0x0A: 859, A/C/G/T, N:247} — the newline-joined 865-contig chain
   (h10_rl.txt). h10ss_pfp parses h10ss.txt = the `'!'`-joined SINGLE string
   (pfpbwt_pilot_k10.sh lines 14-26). So the supervisor's (C) pairing
   (h10new2.ri4 + h10ss_pfp) would have been cross-chain again. Verified by
   histogram (zero 0x21 runs), not just by size (both texts are 30,151,407,545 B).
2. **Oracle self-verified**: chi_h10.sA == chi_h10_opt.sA byte-identical =
   13,016,505,464 B = **1,627,063,183** witnesses (matches BIT_LADDER).
3. **`--agg-out` implemented** in bit6/teralcp_chi.cpp (writes CRA1 directly:
   magic+R+topLCP+saFirst+saLast+interiorMin) to avoid --triples' O(R) text.
   G1-FORMAT gate: byte-identical to the committed chi_rspace_dump .agg on
   **8/8 battery texts** (all regimes incl. duplicates-600k).
4. **(C) run chose the WALK route** (paired with the oracle), not PFP mode:
   `teralcp_chi_agg h10rt.lcp_index.lcp_index --rlbwt h10rt --sidecar
   h10_rl.txt.names.tsv --agg-out h10.walk.agg -t 96` — this is also the exact
   dry run of route (A). PFP-mode at k10 would need a parse of h10_rl.txt,
   which does not exist (and PFP-mode is already yeast-gated anyway).

## (C) K10 FULL-PIPELINE GATE — GREEN (R=1,859,825,801, 18.4x yeast R)

Pipeline (paired: h10rt rlbwt + h10rt.lcp_index 51 GB + h10_rl.txt.names.tsv
sidecar; oracle chi_h10.sA, self-verified 13,016,505,464 B = 1,627,063,183):

| phase | tool | wall | maxRSS |
|---|---|---|---|
| aggregates (O(n) walk) | teralcp_chi_agg --agg-out | **1:01:56** | **178.6 GB** |
| r-space sweep          | xsa chi-rspace | **43:18.96** | **111.7 GB** |

- walk: n=30,151,407,545 runs=1,859,825,801 strings=865; h10.walk.agg
  59,514,425,644 B = 12 + 4*8*R exactly.
- sweep: **chi = 1,627,063,183** (N=30,151,407,546); h10.rs.out
  13,016,505,464 B.
- **GATE: witness count EQUAL and sorted set equality TRUE vs chi_h10.sA.**
- `--agg-out` format gate: byte-identical to committed chi_rspace_dump .agg
  on 8/8 battery texts (validated before the k10 run).

## (A) 466 WALK — LAUNCHED (approved)

`nohup /tmp/laneK/h466_walk.sh &` (pid 286620), started 2026-09-26T05:25:45Z,
cwd k466, `teralcp_chi_agg h466rt.lcp_index.lcp_index --rlbwt h466rt
--sidecar h466_rl.txt.names.tsv --agg-out h466.walk.agg -t 96`.
Log /tmp/laneK/h466_walk.log; expected ~37.7 h walk + ~10 min .agg write
(87.7 GB = 12 + 4*8*2,739,735,806), ~293 GB peak RSS. Kernel: /tmp/laneK.
Next: `xsa chi-rspace --ri4 h466.ri4 --agg h466.walk.agg -o h466.rs.out`
(~1.5-2 h; k10 1.47x less R took 43 min; expect ~160 GB RSS), then gate
chi==2,249,968,075 + sorted-set equality vs chi_h466.sA (18 GB).
Disk: nvme3n1 1.8 TB free (agg 87.7 GB + out 18 GB fine).
Kill by pid only. Machine: do NOT run another 293 GB job concurrently.

## Pre-(A) confirmations (self-verified, not prose)
- chi_h466.sA = 17,999,744,600 B = **2,249,968,075** witnesses (file-size).
- h466.ri4 run-char array == h466rt.bwt.heads on prefix (5M) and mid (2M)
  slices -> walk rlbwt and sweep .ri4 share the run structure (the pairing
  that gated GREEN at k10).
- 466 walk internal checks PASS: "loaded: n=1403221068481
  runs=2739735806 strings=38790" + "sidecar: 38790 rows loaded"
  (idx.F split-run cross-check and R.n==totalLen both passed).
- RSS trajectory: 47 -> 151 -> 234 GB (expect ~293 GB peak).

## EXACT final-step commands (when the walk finishes)
```
cd /mnt/nvme3n1/erikg/sxgc-pilot/k466
/usr/bin/time -v /home/erikg/sxgc/xsa/target/release/xsa chi-rspace \
  --ri4 h466.ri4 --agg h466.walk.agg -o h466.rs.out     # expect chi=2249968075
python3 - <<'PY'
import numpy as np
K='/mnt/nvme3n1/erikg/sxgc-pilot/k466/'
a=np.fromfile(K+'h466.rs.out',dtype=np.uint64); b=np.fromfile(K+'chi_h466.sA',dtype=np.uint64)
print(len(a),len(b),bool(np.array_equal(np.sort(a),np.sort(b))))
PY
```
Expected: 2,249,968,075 == 2,249,968,075, sorted set equality True.

## Files touched by this lane
- bit6/teralcp_chi.cpp: added `--agg-out FILE` (CRA1 writer). NOT committed.
- /tmp/laneK/teralcp_chi_agg (built from above), k10_walk.sh, k10_sweep.sh,
  k10_gate.py, h466_walk.sh.
- k10 artifacts: h10.walk.agg (59.5 GB), h10.rs.out (13 GB).
- 466: h466.walk.agg being written.

## Observation bearing on route (B) / the |M| question
The supervisor's probe (pid 4168670, pfp_index_build on the 30 GB h10ss_pfp)
is at **210 GB RSS after 2h19m and still climbing** (was 54 GB at 8 min),
against a 30 GB text. For the 1.4 TB 466 text that extrapolates to
~10+ TB — i.e. the PFP/pf_parsing route (B) looks **infeasible at 466 on
this machine** (consistent with the b_bwt=O(n) + M≈0.1-0.3n picture;
Θ(r) would have been ~70-200 GB). The probe will confirm/finish or hit its
700 GB cap. (B) already stays queued behind that verdict + iteration-2.
