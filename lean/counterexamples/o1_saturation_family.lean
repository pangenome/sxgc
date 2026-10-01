import Sxgc

/-!
Modified-cap saturation evidence on an actual text stream. The fidelity
theorems identify this model with the original scan when the cap is MAXINT.
The concrete duplicate below uses cap 1, so it is not a refutation of a
statement about the original fixed MAXINT.
-/

namespace Sxgc.SaturationEvidence

def scanAuxCap (cap : Int) (N : Nat) :
    List Triple → Nat → Nat → Int → List Cand → List Nat → List Nat
  | [], _, _, _, R, out => (evalStep (-1) R out).1
  | t :: rest, p, pSa, m, R, out =>
    let m' := min m t.lcp
    if t.c != p then
      let (out', R') := evalStep m' R out
      let R'' := upd R' p t.lcp (N - pSa)
      let R''' := upd R'' t.c t.lcp (N - t.sa)
      scanAuxCap cap N rest t.c t.sa cap R''' out'
    else
      scanAuxCap cap N rest t.c t.sa m' R out

def scanCap (cap : Int) (N : Nat) (ts : List Triple) : List Nat :=
  match ts with
  | [] => []
  | t :: rest => scanAuxCap cap N rest t.c t.sa cap defaultR []

theorem scanAuxCap_MAXINT (N : Nat) (ts : List Triple)
    (p pSa : Nat) (m : Int) (R : List Cand) (out : List Nat) :
    scanAuxCap MAXINT N ts p pSa m R out = scanAux N ts p pSa m R out := by
  induction ts generalizing p pSa m R out with
  | nil => rfl
  | cons t rest ih => simp only [scanAuxCap, scanAux, ih]

theorem scanCap_MAXINT (N : Nat) (ts : List Triple) :
    scanCap MAXINT N ts = scan N ts := by
  cases ts with
  | nil => rfl
  | cons t rest => exact scanAuxCap_MAXINT N rest t.c t.sa MAXINT defaultR []

def exampleText : Text := [1, 1, 1, 2, 1, 1]

theorem example_positive : positive exampleText = true := by decide

set_option maxRecDepth 100000 in
set_option maxHeartbeats 2000000 in
theorem modified_cap_output :
    scanCap 1 (exampleText.length + 1) (triplesOf exampleText) = [3, 4, 4] := by
  decide

set_option maxRecDepth 100000 in
set_option maxHeartbeats 2000000 in
theorem example_chi : chi exampleText = 2 := by decide

theorem modified_cap_not_nodup :
    ¬ (scanCap 1 (exampleText.length + 1) (triplesOf exampleText)).Nodup := by
  rw [modified_cap_output]
  decide

theorem modified_cap_length_exceeds_chi :
    chi exampleText < (scanCap 1 (exampleText.length + 1) (triplesOf exampleText)).length := by
  rw [example_chi, modified_cap_output]
  decide

end Sxgc.SaturationEvidence

namespace Sxgc.SaturationEvidence

/-- Exact local saturation criterion: the reset test succeeds and the update
rearms precisely when BOTH adjacent LCPs exceed the reset cap. For an active
singleton-run candidate, this is the emit-and-rearm condition; the following
theorem checks actual emission too. Neither statement assumes later survival. -/
theorem singleton_emit_rearm_iff (cap : Int) (a b c x : Nat)
    (R : List Cand) (out : List Nat)
    (hc1 : 1 ≤ c) (hc2 : c < SIGMA) (hR : SIGMA ≤ R.length)
    (hlen : (getR R c).len = (a : Int)) :
    (min cap (b : Int) < (getR R c).len ∧
      (getR (upd (evalStep (min cap (b : Int)) R out).2 c b x) c).active = true)
      ↔ cap < (a : Int) ∧ cap < (b : Int) := by
  have hc : c < (evalStep (min cap (b : Int)) R out).2.length := by
    rw [evalStep_len]
    omega
  rw [upd_getR_self _ c b x hc, evalStep_getR _ R out c hc1 hc2 hR, hlen]
  by_cases hd : min cap (b : Int) < (a : Int)
  · rw [if_pos hd]
    simp only [hd, true_and]
    by_cases hu : min cap (b : Int) < (b : Int)
    · rw [if_pos hu]
      constructor
      · intro _
        constructor <;> omega
      · intro _
        rfl
    · rw [if_neg hu]
      simp only [Bool.false_eq_true, false_iff, not_and]
      omega
  · simp only [hd, false_and, false_iff, not_and]
    omega

/-- The positive direction also identifies the emitted and rearmed position. -/
theorem singleton_saturation_emits_and_rearms (cap : Int) (a b c x : Nat)
    (R : List Cand) (out : List Nat)
    (hc1 : 1 ≤ c) (hc2 : c < SIGMA) (hR : SIGMA ≤ R.length)
    (hslot : getR R c = ⟨a, x, true⟩)
    (ha : cap < (a : Int)) (hb : cap < (b : Int)) :
    x ∈ (evalStep (min cap (b : Int)) R out).1 ∧
      getR (upd (evalStep (min cap (b : Int)) R out).2 c b x) c =
        ⟨b, x, true⟩ := by
  have hd : min cap (b : Int) < (getR R c).len := by
    rw [hslot]
    simp only
    omega
  constructor
  · apply (evalStep_mem _ R out x).mpr
    exact Or.inr ⟨c, hc1, hc2, hd, by rw [hslot], by rw [hslot]⟩
  · rw [upd_getR_self _ c b x (by rw [evalStep_len]; omega),
      evalStep_getR _ R out c hc1 hc2 hR, if_pos hd]
    exact if_pos (by simpa only using (show min cap (b : Int) < (b : Int) by omega))

/-- A small fixed-MAXINT stream witness. Its SA values are distinct and
in range, but its LCPs do NOT come from a four-row text. -/
def fixedCapStream : List Triple :=
  [⟨1, 0, 3⟩, ⟨2, MAXINT.toNat + 1, 2⟩,
   ⟨0, MAXINT.toNat + 1, 0⟩, ⟨1, MAXINT.toNat, 1⟩]

set_option maxRecDepth 100000 in
set_option maxHeartbeats 2000000 in
theorem fixedCapStream_output : scan 4 fixedCapStream = [1, 2, 2] := by decide

theorem fixedCapStream_distinct_sa : (fixedCapStream.map Triple.sa).Nodup := by decide

theorem fixedCapStream_sa_in_range : ∀ t ∈ fixedCapStream, t.sa < 4 := by
  simp [fixedCapStream]

theorem fixedCapStream_not_nodup : ¬ (scan 4 fixedCapStream).Nodup := by
  rw [fixedCapStream_output]
  decide

/-- Saturation can also emit DISTINCT positions in the SAME maximal class:
deduplicating positions alone would not repair minimality. This uses cap 0. -/
def equalBlocks : Text := [1, 1, 2, 1, 1]

set_option maxRecDepth 100000 in
set_option maxHeartbeats 5000000 in
theorem equalBlocks_output :
    scanCap 0 (equalBlocks.length + 1) (triplesOf equalBlocks) = [2, 3, 3, 5] := by
  decide

set_option maxRecDepth 100000 in
set_option maxHeartbeats 5000000 in
theorem equalBlocks_same_class : ScopeLe equalBlocks 2 5 ∧ ScopeLe equalBlocks 5 2 := by
  unfold ScopeLe
  decide

set_option maxRecDepth 100000 in
set_option maxHeartbeats 5000000 in
theorem equalBlocks_maximal : IsMax equalBlocks 2 ∧ IsMax equalBlocks 5 := by
  letI (x y : Nat) : Decidable (ScopeLe equalBlocks x y) := by
    unfold ScopeLe
    infer_instance
  unfold IsMax
  decide

set_option maxRecDepth 100000 in
set_option maxHeartbeats 5000000 in
theorem equalBlocks_chi : chi equalBlocks = 2 := by decide

theorem equalBlocks_dedup_still_exceeds_chi :
    chi equalBlocks <
      (dedup (scanCap 0 (equalBlocks.length + 1) (triplesOf equalBlocks))).length := by
  rw [equalBlocks_output, equalBlocks_chi]
  decide

/-- With a longer second block, the first unary witness is nonmaximal. -/
def increasingBlocks : Text := [1, 1, 2, 1, 1, 1]

theorem block_examples_positive :
    positive equalBlocks = true ∧ positive increasingBlocks = true := by decide

set_option maxRecDepth 100000 in
set_option maxHeartbeats 5000000 in
theorem increasingBlocks_output :
    scanCap 0 (increasingBlocks.length + 1) (triplesOf increasingBlocks) = [2, 3, 3, 6] := by
  decide

set_option maxRecDepth 100000 in
set_option maxHeartbeats 5000000 in
theorem increasingBlocks_strict_domination :
    ScopeLe increasingBlocks 2 6 ∧ ¬ ScopeLe increasingBlocks 6 2 := by
  unfold ScopeLe
  decide

theorem increasingBlocks_not_maximal : ¬ IsMax increasingBlocks 2 := by
  intro h
  exact increasingBlocks_strict_domination.2
    (h.2 6 (by decide) increasingBlocks_strict_domination.1)

/-- Arbitrarily large LCPs occur on positive texts. Thus the existing bounded
O2 theorem's hypothesis cannot be obtained from positivity alone. -/
theorem positive_text_has_large_lcp (n : Nat) :
    ∃ T : Text, positive T = true ∧ ∃ t ∈ triplesOf T, n < t.lcp := by
  let T : Text := List.replicate (n + 2) 1
  let w : List Nat := List.replicate (n + 1) 1
  have hT : positive T = true := by simp [T, positive, SIGMA]
  have hw : w ≠ [] := by simp [w]
  have hocc : occurs w T = true := by
    apply (occurs_eq_true w T hw).mpr
    refine ⟨0, by simp [T, w], ?_⟩
    simp [T, w, List.take_replicate, Nat.min_eq_left (show n + 1 ≤ n + 2 by omega)]
  have hext : occurs (w ++ [1]) T = true := by
    have heq : w ++ [1] = T := by
      exact (List.replicate_succ' (n := n + 1) (a := 1)).symm
    rw [heq]
    apply (occurs_eq_true T T (by simp [T])).mpr
    exact ⟨0, by simp, by simp⟩
  have hsub : w ∈ subStrings T := by
    change w ∈ subStrings (1 :: List.replicate (n + 1) 1)
    simp only [subStrings, List.mem_cons, List.mem_append]
    have hp : w ∈ (List.range (1 :: List.replicate (n + 1) 1).length).map
        (fun l => (1 :: List.replicate (n + 1) 1).take (l + 1)) := by
      apply List.mem_map.mpr
      refine ⟨n, by simp only [List.mem_range, List.length_cons, List.length_replicate]; omega, ?_⟩
      change (List.replicate (n + 2) 1).take (n + 1) = w
      simp [w, List.take_replicate, Nat.min_eq_left (show n + 1 ≤ n + 2 by omega)]
    first | exact Or.inl (Or.inr hp) | exact Or.inr (Or.inl hp)
  have hmax : rightMaximal w T = true := by
    change (occurs w T && (rightMaximal.isSuffix w T || decide (2 ≤ (rightExts w T).length))) = true
    rw [hocc]
    simp [rightMaximal.isSuffix, w, T, List.drop_replicate]
  have hreq := (mem_requirements w 1 T).mpr
    ⟨hsub, hmax, (mem_rightExts w 1 T).mpr hext⟩
  obtain ⟨k, hk, hkn, _, hprev, hcur, _⟩ := requirement_boundary T hT w 1 hreq
  have hl := (wRow_lcp_step T w k hk hkn hprev).mp hcur
  refine ⟨T, hT, (triplesOf T).getD k ⟨0,0,0⟩, ?_, ?_⟩
  · simp only [List.getD_eq_getElem?_getD, List.getElem?_eq_getElem hkn,
      Option.getD_some]
    exact List.getElem_mem hkn
  · simp only [w, List.length_replicate] at hl
    omega

theorem positivity_does_not_bound_lcp :
    ¬ (∀ T : Text, positive T = true → ∀ t ∈ triplesOf T, t.lcp ≤ MAXINT.toNat) := by
  intro h
  obtain ⟨T, hT, t, ht, hlarge⟩ := positive_text_has_large_lcp MAXINT.toNat
  have := h T hT t ht
  omega

#print axioms singleton_emit_rearm_iff
#print axioms singleton_saturation_emits_and_rearms
#print axioms fixedCapStream_output
#print axioms equalBlocks_same_class
#print axioms equalBlocks_maximal
#print axioms equalBlocks_chi
#print axioms equalBlocks_dedup_still_exceeds_chi
#print axioms increasingBlocks_output
#print axioms increasingBlocks_not_maximal
#print axioms positive_text_has_large_lcp
#print axioms positivity_does_not_bound_lcp

end Sxgc.SaturationEvidence
