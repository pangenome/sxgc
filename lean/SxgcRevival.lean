import Sxgc

/-!
# Witness revival under concatenation (chunk-merge premise refutation)

The chunk-merge premise audit (`bit6/sxi_logs/chunk-merge/REPORT.md`) proposed an
incremental χ invariant of the shape

  * "new witnesses appear only in the added chunk", and
  * "old witnesses can only be dominated away".

Both halves are **false** under the repository's finite-text suffixient
definitions (`requirements`, `coversAt`, `covSet`, `ScopeLe`, `IsMax`, `chi` in
`Sxgc.lean`).  This file formalizes the audit's brute-verified counterexample:

  `A = (1,2,2,SEP)`,  `B = (1,SEP)`  (both document-aligned),  `J = A ++ B`.

  * position `2` is **not** a maximal coverage class in `A` but **is** one in `J`
    — witness revival: `(1,2)` becomes a requirement covered only by the old
    position 2, because the appended chunk makes the context `1` right-maximal;
  * position `1` **is** a maximal coverage class in `A` but is **not** one in `J`
    — an old witness is destroyed by the append;
  * consequently `chi A = 3` but `chi J = 5`.

The two refutations are stated as negations of the general invariant shapes,
instantiated at this counterexample, so no monotone pruning rule of that form
can be sound.

NOTE on the requested chi corollary: the audit's own phrasing of the corollary
was "there is no lemma of the form `chi T1 ≤ chi (T1 ++ T2)` in general".  That
statement is **not** supported: `chi A = 3 ≤ 5 = chi J` here, and no
chi-decreasing pair was found (exhaustive over `{1,2}`, `|A|,|B| ≤ 7`; `{1,2,3}`
partially; 40k random pairs up to `|A| = 14`).  What is false, and what the
merge algorithm actually needed, is monotonicity of the **witness set** itself,
which is what the theorems below refute.  We therefore do not assert a
chi-decrease, and record the boundary of the corollary honestly.
-/

namespace SxgcRevival

open Sxgc

/-- Document separator of the corpus contract (`0x1E`). -/
def SEP : Nat := 30

/-- Old chunk (two documents' worth of content, separator-terminated). -/
def A : Text := [1, 2, 2, SEP]

/-- Appended document-aligned chunk. -/
def B : Text := [1, SEP]

/-- The concatenated text. -/
def J : Text := A ++ B

/-! ## A kernel-decidable reformulation of `ScopeLe`, and `IsMax` from it -/

/-- `ScopeLe T x y` as a computable `all`-test over the finite coverage list.
Every concrete coverage fact below is checked by `decide` through this form
(the unbounded `p` in `ScopeLe` is what makes the plain definition
non-decidable). -/
def scopeLeB (T : Text) (x y : Nat) : Bool :=
  (covSet T x).all (fun p => decide (p ∈ covSet T y))

theorem scopeLe_iff_scopeLeB (T : Text) (x y : Nat) :
    ScopeLe T x y ↔ scopeLeB T x y = true := by
  unfold scopeLeB ScopeLe
  rw [List.all_eq_true]
  constructor
  · intro h p hp; exact decide_eq_true (h p hp)
  · intro h p hp; exact of_decide_eq_true (h p hp)

/-- `IsMax T x` from the decidable all-test over the finite position list. -/
theorem isMax_of_all (T : Text) (x : Nat) (hne : covSet T x ≠ [])
    (hall : (positionsT T).all (fun y => !scopeLeB T x y || scopeLeB T y x) = true) :
    IsMax T x := by
  unfold IsMax
  refine ⟨hne, ?_⟩
  intro y hy hle
  have h1 : (!scopeLeB T x y || scopeLeB T y x) = true := List.all_eq_true.mp hall y hy
  have hxy : scopeLeB T x y = true := (scopeLe_iff_scopeLeB T x y).mp hle
  rw [hxy] at h1
  simp at h1
  exact (scopeLe_iff_scopeLeB T y x).mpr h1

/-! ## Maximality in `A` and in `J` -/

/-- Position 1 of `A` is a maximal coverage class. -/
theorem isMax_A_one : IsMax A 1 :=
  isMax_of_all A 1 (by decide) (by decide)

/-- Position 2 of `A` is **not** maximal: position 3 strictly dominates it
(`{(epsilon,2)} ⊂ {(epsilon,2),(2,2)}`). -/
theorem not_isMax_A_two : ¬ IsMax A 2 := by
  unfold IsMax
  rintro ⟨_, hdom⟩
  have h23 : ScopeLe A 2 3 := (scopeLe_iff_scopeLeB A 2 3).mpr (by decide)
  have h32 : ScopeLe A 3 2 := hdom 3 (by decide) h23
  have hbad : scopeLeB A 3 2 = false := by decide
  rw [(scopeLe_iff_scopeLeB A 3 2).mp h32] at hbad
  exact Bool.noConfusion hbad

/-- Position 2 of `J` **is** maximal — the revival — with coverage
`{(epsilon,2),(1,2)}`. -/
theorem isMax_J_two : IsMax J 2 :=
  isMax_of_all J 2 (by decide) (by decide)

/-- Position 1 of `J` is **not** maximal: position 5 (an appended position)
strictly dominates it (`{(epsilon,1)} ⊂ {(epsilon,1),(30,1)}`). -/
theorem not_isMax_J_one : ¬ IsMax J 1 := by
  unfold IsMax
  rintro ⟨_, hdom⟩
  have h15 : ScopeLe J 1 5 := (scopeLe_iff_scopeLeB J 1 5).mpr (by decide)
  have h51 : ScopeLe J 5 1 := hdom 5 (by decide) h15
  have hbad : scopeLeB J 5 1 = false := by decide
  rw [(scopeLe_iff_scopeLeB J 5 1).mp h51] at hbad
  exact Bool.noConfusion hbad

/-! ## The two claimed invariants are false -/

/-- "New witnesses appear only in the added chunk" is FALSE: position 2 lies in
the old chunk `A` and becomes a maximal class only after the append. -/
theorem new_witnesses_not_confined_to_old_chunk :
    ¬ (∀ (T₁ T₂ : Text) (p : Nat),
        p ∈ positionsT T₁ → IsMax (T₁ ++ T₂) p → IsMax T₁ p) := by
  intro h
  exact not_isMax_A_two (h A B 2 (by decide) isMax_J_two)

/-- "Old witnesses can only be dominated away" (i.e. survive) is FALSE:
position 1 is a maximal class of `A` and not one of `J`. -/
theorem old_witnesses_need_not_survive :
    ¬ (∀ (T₁ T₂ : Text) (p : Nat), IsMax T₁ p → IsMax (T₁ ++ T₂) p) := by
  intro h
  exact not_isMax_J_one (h A B 1 isMax_A_one)

/-! ## The canonical representative set also moves -/

/-- The canonical (earliest) representative of position 2's class is absent in
`A`. -/
theorem not_isRep_A_two : ¬ IsRep A 2 := fun h => not_isMax_A_two h.2.1

/-- …and present in `J`: the representative set of the appended text gains a
position that lies inside the old chunk. -/
theorem isRep_J_two : IsRep J 2 := by
  unfold IsRep
  refine ⟨by decide, isMax_J_two, ?_⟩
  intro y hy hlt hmax
  have hy1 : y = 1 := by
    simp only [positionsT, J, A, B, List.mem_map, List.mem_range] at hy
    obtain ⟨i, hi, rfl⟩ := hy
    omega
  exact absurd (hy1 ▸ hmax) not_isMax_J_one

/-! ## χ values (the audit's claimed numbers) -/

theorem chi_A : chi A = 3 := by decide

theorem chi_J : chi J = 5 := by decide

theorem maxClassCount_A : maxClassCount A = 3 := by
  rw [← chi_eq_maxClasses A (by decide)]
  exact chi_A

theorem maxClassCount_J : maxClassCount J = 5 := by
  rw [← chi_eq_maxClasses J (by decide)]
  exact chi_J

/-- χ is not preserved by concatenation: its value changes. -/
theorem chi_changes : chi A ≠ chi J := by rw [chi_A, chi_J]; decide

/-- …and here it strictly grows. -/
theorem chi_strictly_grows : chi A < chi J := by rw [chi_A, chi_J]; decide

/-! ## Executable gates -/

#eval ("revival gate: chi A = " ++ toString (chi A) ++ " ; chi J = " ++ toString (chi J))
#eval ("revived requirement: covSet A 2 = " ++ toString (covSet A 2)
  ++ " ; covSet J 2 = " ++ toString (covSet J 2))
#eval ("destroyed witness: covSet A 1 = " ++ toString (covSet A 1)
  ++ " ; covSet J 1 = " ++ toString (covSet J 1))
#eval ("positive A / positive J: " ++ toString (positive A) ++ " / " ++ toString (positive J))

end SxgcRevival
