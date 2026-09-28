# FACTS — claim → source artifact map (supervisor-checkable)

Every formal and numeric claim in main.tex, with the machine artifact that
warrants it. Formal claims: Lean files, kernel-checked, axioms
{propext, Classical.choice, Quot.sound} (audited via fresh-import
#print axioms on fam_floor_chi, runEdgeHit_true, chi_fam_bounds).

## Formal claims

| Claim in paper | Lean location | Notes |
|---|---|---|
| Definitions (occurs, coversAt, rightMaximal, requirements, suffixient, chi brute) | lean/Sxgc.lean:24-96 | verbatim semantics |
| Theorem 1 chi_eq_maxClasses | lean/Sxgc.lean:1863 | exhaustive pre-proof warrant in docstring (<=8/2-letter, <=7/3-letter, 3000 random) |
| Theorem chi_le_of_suffixient | lean/Sxgc.lean:889 | |
| scan_emits_run_edges | lean/Sxgc.lean:5848 | |
| Theorem 3 scanAux_nodup (O2 discharged) | lean/SxgcNodup.lean (0 sorries) | statement-locked SxgcBounds.O2_bounded proven byte-for-byte; boundedness used once (run-length-1 re-arm block) |
| Theorem 5 capstone parseChi_eq | lean/SxgcBuild.lean:1242 | conditional on hnd/hmax/hdisj/hcover exactly as stated |
| parseTriplesOf_eq, parseScan_eq | lean/SxgcBuild.lean:1224,1236 | |
| VerifiedLCE_determinism (textLCEvia = lcpOf) | lean/SxgcBuild.lean:1257 | |
| Bounds: textLCESteps <= 8(1+tau+ell) | lean/SxgcBounds.lean:195-203 | workConstant=8 (line 136) |
| totalWork <= 16(count+dict+|qs|tau+verifSum) | lean/SxgcBounds.lean:229-243 | explicit no-O(tau)-claim in docstring |
| parseTriplesLCEWork <= 16(... n tau ...) | lean/SxgcBounds.lean:254 | |
| Theorem headFromTail | lean/SxgcPhi.lean:68 | phiInv_phi at :91 |
| Seam: rot_agree, identity_outside_classes, agreement_with_nonseam | lean/SxgcSeam.lean:117,144,157 | seamPos def :134 |
| Bridge emitted_suffixient, chi_le_of_oracle | lean/LowerBound.lean:145,158 | |
| family_counting (pigeonhole) | lean/LowerBound.lean:313 | F.length <= 2^(s+1)-1 |
| chi_fam_bounds 2k <= chi <= 3k-1 | lean/LowerBound.lean:1921 | Sxgc.LowerBound.Fam.chi_fam_bounds; axioms clean incl. fam_run_occ_le, fam_cover |
| fam_floor_half_grid (L/2+1)^k <= 2^(s+1)-1 | lean/LowerBound.lean:2457 | |
| fam_floor (exact floor statement) | lean/LowerBound.lean:2511 | |
| Theorem fam_floor_chi | lean/LowerBound.lean:2525 | statement quoted verbatim in paper §6 |
| fam_oracle_witness refutable as locked (k=1,L=2,s=1) | lean/LowerBound.lean:~2530 docstring | counterexample documented; fam_floor_chi independent of it |
| O1 factorization: O1_maxHit iff RunEdgeHit and RunEdgeDominate | lean/Sxgc.lean:~6060 | maxHit_of_runEdge, runEdgeHit_of_maxHit |
| RunEdgeHit proven | lean/SxgcRunEdge.lean:600 (runEdgeHit_true) | axioms {propext, Classical.choice, Quot.sound} |
| covering_given_stream_of_maxHit | lean/Sxgc.lean (O1 lane additions) | makes capstone (iv) conditional only on maxHit |
| RunEdgeDominate OPEN; differentials 0 counterexamples/729 | lean/Sxgc.lean executable differentials + lean/O1_OBSTRUCTION.md | statement-locked |
| sorry ledger: Sxgc.lean 4 (O1-content, minimality, 2 retired-false records) | lean/Sxgc.lean; unchanged per lane reports | |

## Measurements

| Claim | Source | Value |
|---|---|---|
| yeast n, R | yeast.sxi header (stats --sxi); RESEARCH.md:1503 | n=3,336,986,760; R=100,904,881 |
| yeast chi | same; gate logs | 85,404,240; chi/r=0.846 |
| yeast container bytes | bit6/sxi_logs tap-lane acceptance | 1,804,132,304 B = 17.9 B/run; members: 504,526,453 / 403,619,537 / 807,239,048 / 12 / 88,746,976 |
| yeast per-stage (26 min total) | RESEARCH.md:1503 + SXI acceptance | parse 79.3s, l2 4.1s, rpfbwt 624.2s/8.8GB, endpoints 24.6s, slim 832.8s/3.7GB, sweep 14.9s/6MB, write 23.2s, validate 7.8s |
| yeast235 chi/k/R | yeast235_agc.sxi stats + RESEARCH.md:1525ff | 85,404,336; k=9901; R=100,905,045 |
| boundary delta +96 | RESEARCH.md (seam repair banked entry) | 85,404,336 vs 85,404,240, k=9901 |
| yeast235 3 publications | RESEARCH.md entries (seam-repair, native-reader, distribution lanes) | all five core members byte-equal each time |
| k10 n/R/chi | k10.sxi stats; RESEARCH.md:1762-1774 | n=30,151,407,545; R=1,859,825,862; chi=1,627,067,257; container 33.96 GB, delta member 1.65 GB |
| k10 delta +4,074 vs BCR 1,627,063,183 | RESEARCH.md:1774 | unproved same-frame attribution, flagged |
| k10 per-stage A/B | bit6/sxi_logs/k10-endpoints-debug/from-zero-stages.json | table in paper §8 verbatim from this file; peak 124.7GB endpoints stage, 133.9GB lane-reported overall peak |
| k10 A/B byte-identical publications | same acceptance: gate-heads/gate-runs-tails cmp logs | |
| k10 seam 7 classes/1161 rows | RESEARCH.md:1772 | |
| phi^-1 k10: 751s/64.8GB, 165x vs 38h walk | RESEARCH.md:1449 | heads byte-identical to walk baseline |
| LF walk cost 2,156 steps/head avg | RESEARCH.md:1104,1187 | measured justification for v5 head samples |
| LCE verification 15.33 avg / 22,262 max phrases | RESEARCH.md:1226 | 30 Gbp human |
| k10 parse phrases 333,531,723; 27.4 min/10GB | RESEARCH.md:1126 | |
| 466 parse rehearsal 15,517,244,887 phrases; dict 89,908,623/18.52 Gbp; 10h35m/28.7GB | RESEARCH.md:1509 entry | |
| Reader throughput table | bit6/sxi_logs/pfp-agc/THROUGHPUT.md | 470.7/509.0/534.8 yeast235; 509.1/464.3/437.6 HPRC; identical FNV checksums per row |
| 466 canonical size 1,403,221,068,481 B; parse ETA 6.65h (j16); reader 45.9-47.1 min full corpus | THROUGHPUT.md | planning estimate, flagged as such |
| HPRC headline | PLACEHOLDER - run in flight | historical BCR 2,249,968,075 = RESEARCH.md:920,1084; expected ~2.25e9 canonical |
| cost-per-run decomposition | computed from yeast container member sizes / R | rlbwt 5.0, tails 4.0, heads 8.0 (raw), chi delta 0.88; floor ~12.7 (log2 n=31.6b: 2 samples ~7.9 + rlbwt ~2.5 + counts; conservative statement in paper) |
| proven-theorem bytes at yeast: 18.8MB w/ 1/3 const; 56.5MB natural tier; witness member 88.7MB = 1.57x natural | computed: chi=85,404,240, n/chi=39.1, log2=5.29 | matches recon calibration (LOWER_BOUND_PLAN.md) |
| battery: duplicates-600k + 6 fixtures; 7/7 | G0-final.log | incl. HOR-nested; upstream w-DOLLAR corner documented |
| ropebwt3 MEM parity 23 fixtures; 756 SMEM intervals; 44,583 occurrences | bit6/sxi_logs/rope-compat | reference built from source |
| distribution manifest 75 artifacts; drift single-byte rejected | bit6/sxi_logs/distribution | |
| corrections ledger 12 entries | RESEARCH.md Corrections #1-#12 | |

## Things deliberately NOT claimed in the paper
- No O(tau) claim for verification lengths (explicit in SxgcBounds).
- RunEdgeDominate open (only reduced + battery-warranted).
- Seam in-class reordering lemma statement-locked, not proven.
- Boundary-delta +4074 same-frame attribution unproved.
- 466 numbers pending; only historical BCR value + size cited.
- fam_oracle_witness refutable-as-locked recorded visibly.
- The floor is family-specific, fixed-decoder; general rung open.
