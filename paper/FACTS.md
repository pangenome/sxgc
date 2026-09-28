# FACTS — claim → source artifact map (bilingual: paper form beside exact Lean form)

Every formal and numeric claim in main.tex, with the machine artifact that
warrants it. Formal claims: Lean files, kernel-checked, axioms
{propext, Classical.choice, Quot.sound} (audited via fresh-import
#print axioms on fam_floor_chi, runEdgeHit_true, chi_fam_bounds); the
LM.lean continuation-bridge theorems audit cleaner at
{propext, Quot.sound} (fresh-import #print axioms on
chi_le_of_cont_oracle_distinct, chi_le_of_realized_cont).

All formal pointers below are commit-pinned to
101403519196e5129ce113eae57123227219f59e (the version whose line numbers
appear in the paper's clickable pointers and here).
URL form: https://github.com/pangenome/sxgc/blob/<hash>/lean/<file>#L<line>

## Formal claims — bilingual map

### §2 Definitions (requirements, coverage, suffixient sets)
- **Paper form.** Word w occurs at x if T[x-|w|+1..x]=w; w is right-maximal
  if it occurs and either is a suffix of T or has ≥2 distinct one-symbol
  right extensions (empty word right-maximal by convention); a requirement
  is (w,c) with w right-maximal and wc occurring; x covers (w,c) if wc is a
  suffix of T[1..x]; S suffixient if every requirement is covered by some
  x ∈ S; χ = minimum cardinality.
- **Lean form.** `coversAt (wc) (x) (T)` = `wc.length ≤ p.length && wc == p.drop (p.length - wc.length)` with `p = T.take x`;
  `rightMaximal w T = occurs w T && (isSuffix w T || (rightExts w T).length ≥ 2)` (`[] => true`);
  `requirements T = (subStrings T).flatMap (fun w => (rightExts w T).flatMap (fun c => if rightMaximal w T then [(w,c)] else []))`;
  `suffixient S T = (requirements T).all (fun p => S.any (fun x => coversAt (p.1 ++ [p.2]) x T))`;
  `chi T` = the brute-force minimum over suffixient sublists of positions.
- **Location.** lean/Sxgc.lean:24-96 (occurs :24, coversAt :44, rightMaximal :52,
  requirements :67, suffixient :76, chi :96).

### §2 Definition (coverage classes)
- **Paper form.** C_T(x) = set of requirements covered at x; x ⪯ y when
  C_T(x) ⊆ C_T(y) ("x dominated by y"); x maximal when C_T(x) ≠ ∅ and
  x ⪯ y implies y ⪯ x for every position y; maximal classes are the
  equivalence classes of maximal positions under C_T(x)=C_T(y).
- **Lean form.** `def ScopeLe (T) (x y) : Prop := ∀ p, p ∈ covSet T x → p ∈ covSet T y`;
  `def IsMax (T) (x) : Prop := covSet T x ≠ [] ∧ ∀ y, y ∈ positionsT T → ScopeLe T x y → ScopeLe T y x`;
  `maxClassCount T` = the number of earliest representatives of maximal classes
  (`IsRep`), i.e. the number of distinct maximal coverage sets.
- **Location.** lean/Sxgc.lean:777 (ScopeLe), 781 (IsMax), 783 (IsRep), 789 (maxClassCount).

### §2 Theorem (Characterization of χ)
- **Paper form.** For every text T over Σ (no sentinel), χ(T) equals the
  number of maximal coverage classes of T.
- **Lean form.** `theorem chi_eq_maxClasses (T : Text) (hT : positive T = true) : chi T = maxClassCount T`
- **Location.** lean/Sxgc.lean:1863. Warrant history: exhaustive pre-proof
  ({1,2} |T|≤8, {1,2,3} |T|≤7, 3000 random 4-letter texts, 0 mismatches);
  `positive T = true` = every character in [1, SIGMA) (no sentinel byte).

### §2 Theorem (Minimality transfer)
- **Paper form.** If S ⊆ [1..n] is suffixient for T then χ(T) ≤ |S|.
- **Lean form.** `theorem chi_le_of_suffixient (T : Text) (S : List Nat) (hsub : S.Sublist (positionsT T)) (hs : suffixient S T = true) : chi T ≤ S.length`
- **Location.** lean/Sxgc.lean:889. (Sublist-of-positions formalizes
  "distinct text positions"; any set can be so listed.)

### §2 Theorem (The scan emits run edges)
- **Paper form.** Every position emitted by the scan is the position of a
  run-edge row of the triple stream.
- **Lean form.** `theorem scan_emits_run_edges (N : Nat) (ts : List Triple) : ∀ x ∈ scan N ts, EdgeGood ts N x`
- **Location.** lean/Sxgc.lean:5848 (EdgeGood = the run-edge-row predicate).

### §2 Theorem (No duplicates, for all bounded streams) — the discharged no-inflation pillar
- **Paper form.** Any finite triple stream — not necessarily from a text —
  with pairwise-distinct SA values < N and all LCP values ≤ the fixed bound
  M of the scan: the scan emits no position twice.
- **Lean form.** `theorem scan_nodup (N : Nat) (ts : List Triple) (hnd : (ts.map Triple.sa).Nodup) (htpos : ∀ t ∈ ts, t.sa < N) (hbnd : ∀ t ∈ ts, (t.lcp : Int) ≤ MAXINT) : (scan N ts).Nodup`
  (proved via the stronger invariant theorem `scanAux_nodup`, SxgcNodup.lean:328).
  Statement-locked pillar proven byte-for-byte: `O2_bounded_true : SxgcBounds.O2_bounded`
  (SxgcNodup.lean:678; the locked statement is SxgcBounds.lean:654).
- **Location.** lean/SxgcNodup.lean:623 (scan_nodup), 328 (scanAux_nodup), 678 (O2_bounded_true).
  Warrant history: 1,474,560 bounded test streams (length ≤ 4), zero violations.

### §3 Theorem (Construction capstone, conditional)
- **Paper form.** T over Σ, P a well-formed parse of T^R 0, S the scan output
  of the parse-based triple stream. If (i) S duplicate-free, (ii) every
  x ∈ S maximal, (iii) no two distinct x,y ∈ S coverage-equivalent, (iv) S
  suffixient — then |S| = χ(T).
- **Lean form.** `theorem parseChi_eq (T : Text) (P : Parse) (hWF : ParseWF P (T.reverse ++ [0])) (hT : positive T = true) (hnd : (scan (T.length+1) (parseTriplesOf T P)).Nodup) (hmax : ∀ x ∈ scan (T.length+1) (parseTriplesOf T P), IsMax T x) (hdisj : ∀ x ∈ scan (T.length+1) (parseTriplesOf T P), ∀ y ∈ scan (T.length+1) (parseTriplesOf T P), ScopeLe T x y → ScopeLe T y x → x = y) (hcover : suffixient (scan (T.length+1) (parseTriplesOf T P)) T = true) : (scan (T.length+1) (parseTriplesOf T P)).length = chi T`
- **Location.** lean/SxgcBuild.lean:1242. Supporting welds:
  parseTriplesOf_eq :1224 (the parse-based stream IS triplesOf), parseScan_eq :1236
  (congruence to the production scan).

### §3 Hypothesis status (i): discharged
- **Paper form.** The parse-based stream provably equals the text's own
  triple stream, whose SA values are pairwise distinct; the no-duplicates
  theorem applies.
- **Lean form.** `theorem parseTriplesOf_eq ... : parseTriplesOf T P = triplesOf T` (given ParseWF P (T.reverse ++ [0]));
  SA distinctness of `triplesOf T` from saOrder Nodup (via SxgcNodup's `triplesOf_sa_nodup`).
- **Location.** lean/SxgcBuild.lean:1224; SxgcNodup.lean (triplesOf_sa_nodup).

### §3 Hypothesis status (ii)+(iii): reduced — the class-hit statement and its factorization
- **Paper form.** (ii)+(iii) together ⟺ every maximal coverage class
  contains an emitted position (class-hit). Class-hit ⟺ (every maximal
  class contains a run-edge position) ∧ (every run-edge position is
  dominated by an emitted one). Run-edge hit: PROVEN. Run-edge domination:
  OPEN. Given class-hit, (iv) follows unconditionally.
- **Lean form.**
  `def O1_maxHit (T) : Prop := ∀ m, m ∈ positionsT T → IsMax T m → ∃ e, e ∈ scan (T.length+1) (triplesOf T) ∧ ScopeLe T m e` (Sxgc.lean:5962);
  `def RunEdgeHit (T) : Prop := ∀ m, m ∈ positionsT T → IsMax T m → ∃ w, w ∈ runEdgePositions T ∧ ScopeLe T m w` (:6051);
  `def RunEdgeDominate (T) : Prop := ∀ w ∈ runEdgePositions T, ∃ e, e ∈ scan (T.length+1) (triplesOf T) ∧ ScopeLe T w e` (:6056);
  `theorem maxHit_of_runEdge (T) (hhit : RunEdgeHit T) (hdom : RunEdgeDominate T) : O1_maxHit T` (:6062);
  `theorem runEdgeHit_of_maxHit (T) (hT : positive T = true) (hhit : O1_maxHit T) : RunEdgeHit T` (:6071);
  `theorem runEdgeHit_true (T : Text) (hT : positive T = true) : RunEdgeHit T` (SxgcRunEdge.lean:604, axioms {propext, Classical.choice, Quot.sound});
  `theorem covering_given_stream_of_maxHit ... : ... suffixient ... ` (Sxgc.lean:5984).
- **Location.** lean/Sxgc.lean:5962-6072; lean/SxgcRunEdge.lean:604.
  RunEdgeDominate differentials: 0 counterexamples over exhaustive {1,2}≤8,
  {1,2,3}≤7, 3000 random 4-letter texts; the single remaining open capstone
  hypothesis (obstruction analysis: lean/O1_OBSTRUCTION.md).

### §3 Theorem (Determinism of the parse-space LCE)
- **Paper form.** For a well-formed parse and positions i, j, given a step
  budget of at least n−i, the two-level fingerprint LCE with direct
  verification returns the true lcp length of T[i..] and T[j..]; no
  probability anywhere.
- **Lean form.** `theorem VerifiedLCE_determinism (T : Text) (P : Parse) (hWF : ParseWF P T) (fuel i j : Nat) (hi : i ≤ T.length) (hj : j ≤ T.length) (hf : T.length - i ≤ fuel) : textLCEvia P T fuel i j = lcpOf (T.drop i) (T.drop j)`
- **Location.** lean/SxgcBuild.lean:1257.

### §3 Theorems (Explicit per-query bounds; Total work)
- **Paper form.** Each text LCE ≤ 8(1+τ+ℓ) elementary steps (τ = fingerprint
  sampling period, ℓ = verified symbols). Total work ≤ 16(count + dictSize +
  |qs|·τ + Σℓ_q); the construction's own queries ≤ 16(count + dict + n·τ + Σℓ_q).
- **Lean form.** `theorem textLCESteps_bound (P : Parse) (fuel i j τ : Nat) : textLCESteps P fuel i j ≤ workConstant * (1 + τ + verificationLength P fuel i j)` (workConstant = 8, :136);
  `theorem totalWork_bound ...` / querySteps_bound form :229-243;
  `theorem parseTriplesLCEWork_bound ...` :254.
- **Location.** lean/SxgcBounds.lean:195, 229, 254. Explicit NO-O(tau)-claim
  for verification lengths (docstring at :136 area; measured 15.33 avg /
  22,262 max on 30 Gbp human).

### §4 Theorem (Heads by inverse permutation)
- **Paper form.** pos(a) = φ⁻¹(pos(a−1)) for every row 0 < a < |U|; the
  deployment contract is the run-boundary instance (head sample of run a =
  φ⁻¹(tail sample of run a−1)).
- **Lean form.** `theorem headFromTail (R : Text) (a : Nat) (hpos : 0 < a) (ha : a < R.length) : rowPos R a = phiInv R (rowPos R (a - 1))`;
  `theorem runHeadFromTail (R) (a tailSample headSample) (hb : RunBoundary R a) (ht : tailSample = rowPos R (a-1)) (hh : headSample = rowPos R a) : headSample = phiInv R tailSample`;
  `theorem phiInv_phi (R) (p) (hp : p < R.length) : phiInv R (phi R p) = p`.
- **Location.** lean/SxgcPhi.lean:68 (headFromTail), :85 (runHeadFromTail),
  :91 (phiInv_phi). Interval-lookup semantics: 19 theorems, 0 sorries.

### §5 Definition (Seam position) + Theorem (Identity outside classes)
- **Paper form.** Row i is a seam position if its finite suffix is
  prefix-comparable with some other row's suffix. For two non-seam rows,
  finite suffix order and cyclic rotation order agree; a non-seam row's
  order against every row is stable (in-class reordering cannot displace a
  row across a non-seam row).
- **Lean form.** `def seamPos ... ` (:137); `theorem rot_agree ...` (:117);
  `theorem identity_outside_classes ...` (:144);
  `theorem agreement_with_nonseam ...` (:157).
- **Location.** lean/SxgcSeam.lean:117, 137, 144, 157. The in-class
  reordering lemma (sorting a seam class by cyclic LCE equals the cyclic
  order restriction) remains statement-locked with byte-level warrant
  (yeast235: repaired output byte-identical to a brute-force cyclic oracle).

### §6 Index model definitions
- **Paper form.** Oracle = function from words to positions-or-no-answer;
  answers locate-one correctly if every occurring word gets a genuine
  x ∈ [1..n] where it ends; index = bit string; space = length; decoder =
  function from indexes to oracles; D realizes dec(D).
- **Lean form.** `def CorrectLocateOne (f : Answer) (T : Text) : Prop := ∀ w, occurs w T = true → ∃ x, f w = some x ∧ 1 ≤ x ∧ x ≤ T.length ∧ coversAt w x T = true`;
  `def Index := List Bool`; `def space (D : Index) : Nat := D.length`;
  `def Realizes (dec) (D) (f) : Prop := dec D = f`;
  `def emitted (f) (T) : List Nat := (reqWords T).flatMap (fun w => match f w with | some x => [x] | none => [])`.
- **Location.** lean/LowerBound.lean:58 (CorrectLocateOne), :89 (Index),
  :92 (space), :95 (Realizes), :105 (emitted).

### §6 Theorem (Bridge)
- **Paper form.** Any locate-one-correct oracle's positions on T's
  requirement words form a suffixient set; χ(T) ≤ |E(f,T)|.
- **Lean form.** `theorem emitted_suffixient (f : Answer) (T : Text) (h : CorrectLocateOne f T) : suffixient (emitted f T) T = true`;
  `theorem chi_le_of_oracle (f : Answer) (T : Text) (h : CorrectLocateOne f T) : chi T ≤ (emitted f T).length`.
- **Location.** lean/LowerBound.lean:145, 158.

### §6.1 Definition (Continuation oracle) + subsumption + bridge + distinct + index form
- **Paper form.** f is a correct continuation oracle if for every
  requirement (w,c), f returns on wc a genuine x ∈ [1..n] where wc ends;
  nothing demanded elsewhere. Locate-one subsumes continuation. The
  continuation bridge: emitted positions suffixient, χ ≤ |E|. Distinct
  consultation: χ ≤ |dedup(E)|. Fixed-decoder form: dec(D) correct
  continuation ⇒ consults ≥ χ distinct positions.
- **Lean form.** `def CorrectContinuation (f : Answer) (T : Text) : Prop := ∀ w c, (w, c) ∈ requirements T → ∃ x, f (w ++ [c]) = some x ∧ 1 ≤ x ∧ x ≤ T.length ∧ coversAt (w ++ [c]) x T = true` (:59);
  `theorem correctLocateOne_imp_cont {f} {T} (h : CorrectLocateOne f T) : CorrectContinuation f T` (:66);
  `theorem emitted_suffixient_of_cont (f) (T) (h : CorrectContinuation f T) : suffixient (emitted f T) T = true` (:92);
  `theorem chi_le_of_cont_oracle (f) (T) (h : CorrectContinuation f T) : chi T ≤ (emitted f T).length` (:104);
  `theorem suffixient_dedup {S} {T} (h : suffixient S T = true) : suffixient (dedup S) T = true` (:118) + `theorem dedup_length_le (l) : (dedup l).length ≤ l.length` (:141);
  `theorem chi_le_of_cont_oracle_distinct (f) (T) (h : CorrectContinuation f T) : chi T ≤ ((dedup (emitted f T))).length` (:154, axioms {propext, Quot.sound});
  `theorem chi_le_of_realized_cont (dec : Index → Answer) (D : Index) (T : Text) (h : CorrectContinuation (dec D) T) : chi T ≤ ((dedup (emitted (dec D) T))).length` (:166, axioms {propext, Quot.sound}).
- **Location.** lean/LM.lean:59, 66, 92, 104, 118, 141, 154, 166.
  (Line numbers updated to the pinned commit; earlier records of this file
  cited pre-rename lines.)

### §6 Theorem (Pigeonhole)
- **Paper form.** Fixed decoder, s ≥ 0, duplicate-free family F, every
  member served by some ≤s-bit index via a correct locate-one oracle, no
  two members sharing a correct answer function: |F| ≤ 2^(s+1) − 1.
- **Lean form.** `theorem family_counting (dec : Index → Answer) (F : List Text) (s : Nat) (hnd : F.Nodup) (hex : ∀ T ∈ F, ∃ D : Index, space D ≤ s ∧ ∃ f, Realizes dec D f ∧ CorrectLocateOne f T) (hincompat : ∀ T ∈ F, ∀ T' ∈ F, T ≠ T' → ∀ f, CorrectLocateOne f T → ¬ CorrectLocateOne f T') : F.length ≤ 2 ^ (s + 1) - 1`
- **Location.** lean/LowerBound.lean:313.

### §6 The witness-perturbation family — definitions and facts
- **Paper form.** Block i on private letters ρ_i = 2i+2, μ_i = 2i+3;
  B_i(p_i) = ρ_i^{p_i} μ_i ρ_i^{L−p_i} (length L+1); T(p) = concatenation,
  n = k(L+1); half grid = 2p_i ≤ L. On the half grid 2k ≤ χ ≤ 3k−1;
  distinct offsets ⇒ no common correct answer function; half grid injects
  into texts.
- **Lean form.** `def runL (i) := 2*i+2`; `def markL (i) := 2*i+3`;
  `def block (i L p) := (List.replicate p (runL i)) ++ ([markL i] ++ (List.replicate (L - p) (runL i)))`;
  `def famText (k L ps) := (((List.range k).map (fun i => block i L (ps.getD i 0))).flatten)`;
  `theorem chi_fam_bounds {k L ps} (hL : 1 ≤ L) (hhalf : ∀ j, j < k → 2 * ps.getD j 0 ≤ L) (hp : ∀ j, j < k → ps.getD j 0 ≤ L) : 2 * k ≤ chi (famText k L ps) ∧ chi (famText k L ps) ≤ 3 * k - 1`;
  `theorem fam_forced_incompat {k L ps ps'} (hL) (hhalf) (hhalf') (hps : ∃ i, i < k ∧ ps.getD i 0 ≠ ps'.getD i 0) (f) : ¬ (CorrectLocateOne f (famText k L ps) ∧ CorrectLocateOne f (famText k L ps'))`;
  `theorem famText_inj_grid ... : ... base = base'`.
- **Location.** lean/LowerBound.lean:682-688 (runL, markL, block, famText),
  :1921 (chi_fam_bounds), :2016 (fam_forced_incompat), :2347 (famText_inj_grid).
  χ-law empirical warrant: battery-confirmed chi-linearity (2,2,2)→(16,8,8),
  2k ≤ χ ≤ 3k−1 on every emitted grid member.

### §6 Theorem (Family floor)
- **Paper form.** Fix k ≥ 1, L ≥ 1, decoder dec, s ≥ 0; suppose every
  half-grid member is served by some ≤s-bit index with dec(D) locate-one
  correct. Then for every half-grid p⁰: s+1 ≥ ⌊χ(T(p⁰))·⌊log₂(n/χ)⌋/3⌋ with
  n = k(L+1) (both divisions integer).
- **Lean form.** `theorem fam_floor_chi {k L : Nat} (hL : 1 ≤ L) (hk : 1 ≤ k) (ps₀ : List Nat) (hhalf : ∀ i, i < k → 2 * ps₀.getD i 0 ≤ L) (hp : ∀ i, i < k → ps₀.getD i 0 ≤ L) (dec : Index → Answer) (s : Nat) (hex : ∀ ps, (∀ i, i < k → 2 * ps.getD i 0 ≤ L) → ∃ D : Index, space D ≤ s ∧ CorrectLocateOne (dec D) (famText k L ps)) : chi (famText k L ps₀) * (Nat.log2 (k * (L + 1) / chi (famText k L ps₀))) / 3 ≤ s + 1`
  (composed from `fam_floor_half_grid` :2457 → `fam_floor` :2511 → here; the
  hp hypothesis is implied by the half-grid condition).
- **Location.** lean/LowerBound.lean:2525 (fam_floor_chi), :2511 (fam_floor),
  :2457 (fam_floor_half_grid). Axioms {propext, Classical.choice, Quot.sound},
  fresh-import audited.
- **Refuted skeleton, recorded visibly:** `fam_oracle_witness` as originally
  locked is refutable (counterexample k=1, L=2, s=1; documented in its
  docstring, LowerBound.lean:~2530). The intended content is proven as
  fam_floor_half_grid; fam_floor_chi does not depend on the refuted statement.

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
| LF walk cost 2,156 steps/head avg | RESEARCH.md:1104,1187 | measured justification for head samples |
| LCE verification 15.33 avg / 22,262 max phrases | RESEARCH.md:1226 | 30 Gbp human |
| k10 parse phrases 333,531,723; 27.4 min/10GB | RESEARCH.md:1126 | |
| 466 parse rehearsal 15,517,244,887 phrases; dict 89,908,623/18.52 Gbp; 10h35m/28.7GB | RESEARCH.md:1509 entry | |
| Reader throughput table | bit6/sxi_logs/pfp-agc/THROUGHPUT.md | 470.7/509.0/534.8 yeast235; 509.1/464.3/437.6 HPRC; identical FNV checksums per row |
| 466 canonical size 1,403,221,068,481 B; parse ETA 6.65h (j16) | THROUGHPUT.md | planning estimate, flagged as such |
| HPRC headline | PLACEHOLDER - run in flight | historical BCR 2,249,968,075 = RESEARCH.md:920,1084; expected ~2.25e9 canonical |
| cost-per-run decomposition | computed from yeast container member sizes / R | rlbwt 5.0, tails 4.0, heads 8.0 (raw), chi delta 0.88; floor ~12.7 (log2 n=31.6b: 2 samples ~7.9 + rlbwt ~2.5 + counts; conservative statement in paper) |
| proven-theorem bytes at yeast: 18.8MB w/ 1/3 const; 56.5MB natural tier; witness member 88.7MB = 1.57x natural | computed: chi=85,404,240, n/chi=39.1, log2=5.29 | matches recon calibration (LOWER_BOUND_PLAN.md) |
| battery: duplicates-600k + 6 fixtures; 7/7 | G0-final.log | incl. HOR-nested; upstream w-DOLLAR corner documented |
| ropebwt3 MEM parity 23 fixtures; 756 SMEM intervals; 44,583 occurrences | bit6/sxi_logs/rope-compat | reference built from source |
| distribution manifest 75 artifacts; drift single-byte rejected | bit6/sxi_logs/distribution | |
| corrections ledger 12 entries | RESEARCH.md Corrections #1-#12 | |

## Things deliberately NOT claimed in the paper
- The continuation-counting analog of the family floor is a MECHANICAL
  FOLLOW-ON, recorded as such in sec:floor and sec:open; NOT claimed as proven.
- The implicit (weight-space, non-position-consulting) form of the continuation
  bound is NOT claimed; recorded as a statement-lock candidate in sec:lm and
  sec:open, unproven.
- No O(tau) claim for verification lengths (explicit in SxgcBounds).
- Run-edge domination is open (only reduced + battery-warranted).
- Seam in-class reordering lemma statement-locked, not proven.
- Boundary-delta +4074 same-frame attribution unproved.
- 466 numbers pending; only historical BCR value + size cited.
- The refuted skeleton statement is recorded visibly with its counterexample.
- The floor is family-specific, fixed-decoder; the general rung is open.

## Compile
Built with tectonic at the pinned sources: 0 errors. PDF intentionally
untracked in git (build artifact; regenerate with `tectonic main.tex`).
