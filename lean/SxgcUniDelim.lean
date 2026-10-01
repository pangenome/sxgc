import SxgcChiMono

/-!
# Unique trailing delimiter restores χ-count monotonicity

`SxgcChiMono` proves that χ is *not* monotone under append: appending can erase a
terminal-context requirement and demote a maximal class with no compensating creation
(`chi_append_decreases`, `chi_single_symbol_document_decreases`). The oracle hammer
(`lean/FLEET_CHIMONO_ORACLE_REPORT.md`) located the *safe boundary* of that refutation:
appending never decreases χ when the last symbol of the old text is unique in it (its
condition `p1`), hardened over 3.0M randomized pairs and every applicable exhaustive family.

This file turns the observation into a theorem. The mechanism is the exact negation of the
demotion mechanism. Say the last symbol of `A` is `d` and `d` occurs nowhere else in `A`
(`A = pre ++ [d]`, `d ∉ pre`). Then no requirement of `A` can be lost by appending:

* a requirement `(w, c)` with `w = []` is right-maximal in every text and keeps its right
  extension `c`;
* a requirement `(w, c)` with `w ≠ []` whose context `w` is a *suffix* of `A` must end in the
  unique symbol `d`; but `d` sits at the very last position of `A`, so no occurrence of `w`
  has a symbol after it, i.e. `rightExts w A = []`, contradicting `c ∈ rightExts w A`.
  Hence such a `w` is right-maximal in `A` through *two distinct right extensions* — and
  right extensions only accumulate under append (`rightExts_append_subset`).

So `requirements A ⊆ requirements (A ++ B)` for *every* `B`, and therefore
`chi A ≤ chi (A ++ B)` on positive texts.
-/

namespace SxgcUniDelim
open Sxgc SxgcRevival SxgcChiMono

/-! ## The unique-last-symbol obstruction -/

/-- In a text whose last symbol `d` occurs nowhere else, any decomposition `A = q ++ w`
with `w` nonempty places no `d` inside `q`: the only `d` of `A` is its final symbol. -/
theorem not_mem_left_of_unique_last {A pre : Text} {d : Nat}
    (hA : A = pre ++ [d]) (hd : d ∉ pre) {q w : Text}
    (h : A = q ++ w) (hw : w ≠ []) : d ∉ q := by
  subst hA
  intro hdq
  have hlen : q.length + w.length = (pre ++ [d]).length := by rw [h]; simp
  have hwpos : 1 ≤ w.length := by cases w with
    | nil => exact absurd rfl hw
    | cons a t => simp
  have hqle : q.length ≤ pre.length := by
    rw [List.length_append, List.length_singleton] at hlen
    omega
  have hq : q = pre.take q.length := by
    have h1 : q = (pre ++ [d]).take q.length := by rw [h]; simp
    calc q = (pre ++ [d]).take q.length := h1
      _ = pre.take q.length := List.take_append_of_le_length hqle
  exact hd (List.mem_of_mem_take (hq ▸ hdq))

/-- A nonempty suffix of a text with unique last symbol `d` has no right extension at all:
`d` is its final symbol and nothing follows the unique occurrence of `d`. -/
theorem rightExts_eq_nil_of_suffix_unique_last {A pre : Text} {d : Nat}
    (hA : A = pre ++ [d]) (hd : d ∉ pre) {w : Text} (hwne : w ≠ [])
    (hsuf : rightMaximal.isSuffix w A = true) :
    rightExts w A = [] := by
  subst hA
  rw [List.eq_nil_iff_forall_not_mem]
  intro c hc
  rw [mem_rightExts] at hc
  -- the suffix form of `w`, ending in the unique symbol `d`
  unfold rightMaximal.isSuffix at hsuf
  rw [Bool.and_eq_true] at hsuf
  obtain ⟨_, hsbeq⟩ := hsuf
  have hw : w = (pre ++ [d]).drop ((pre ++ [d]).length - w.length) := beq_iff_eq.mp hsbeq
  have hwpos : 1 ≤ w.length := by cases w with
    | nil => exact absurd rfl hwne
    | cons a t => simp
  have hk_le : (pre ++ [d]).length - w.length ≤ pre.length := by
    rw [List.length_append, List.length_singleton]
    omega
  have hsplit : w = pre.drop ((pre ++ [d]).length - w.length) ++ [d] := by
    calc w = (pre ++ [d]).drop ((pre ++ [d]).length - w.length) := hw
      _ = pre.drop ((pre ++ [d]).length - w.length) ++ [d] :=
          List.drop_append_of_le_length hk_le
  have hdmem : d ∈ w := by
    rw [hsplit]
    exact List.mem_append_right _ (List.mem_singleton_self d)
  -- an occurrence of `w ++ [c]` in `A` exposes a `d` strictly before the end
  obtain ⟨i, _, htake⟩ := (occurs_eq_true (w ++ [c]) (pre ++ [d]) (by simp)).mp hc
  have hwclen : (w ++ [c]).length = w.length + 1 := by simp
  rw [hwclen] at htake
  have hdrop : (pre ++ [d]).drop i = (w ++ [c]) ++ (pre ++ [d]).drop (i + (w.length + 1)) := by
    have h1 := List.take_append_drop (w.length + 1) ((pre ++ [d]).drop i)
    rw [htake, List.drop_drop] at h1
    exact h1.symm
  have hAeq : pre ++ [d] =
      (pre ++ [d]).take i ++ ((w ++ [c]) ++ (pre ++ [d]).drop (i + (w.length + 1))) := by
    have h2 := List.take_append_drop i (pre ++ [d])
    rw [hdrop] at h2
    exact h2.symm
  have hdecomp : pre ++ [d] = ((pre ++ [d]).take i ++ w) ++
      ([c] ++ (pre ++ [d]).drop (i + (w.length + 1))) := by
    calc pre ++ [d]
        = (pre ++ [d]).take i ++ ((w ++ [c]) ++ (pre ++ [d]).drop (i + (w.length + 1))) :=
          hAeq
      _ = (pre ++ [d]).take i ++ (w ++ ([c] ++ (pre ++ [d]).drop (i + (w.length + 1)))) := by
          rw [List.append_assoc]
      _ = ((pre ++ [d]).take i ++ w) ++ ([c] ++ (pre ++ [d]).drop (i + (w.length + 1))) := by
          rw [← List.append_assoc]
  exact not_mem_left_of_unique_last rfl hd hdecomp (by simp)
    (List.mem_append_right _ hdmem)

/-! ## Requirements survive append when the trailing symbol is unique -/

/-- Unfolding lemma for a nonempty context. -/
theorem rightMaximal_cons (a : Nat) (t T : Text) :
    rightMaximal (a :: t) T =
      (occurs (a :: t) T &&
        (rightMaximal.isSuffix (a :: t) T || 2 ≤ (rightExts (a :: t) T).length)) := rfl

/-- No requirement of a text with unique last symbol is lost by appending anything. -/
theorem requirements_subset_append_of_unique_last {A B : Text} {pre : Text} {d : Nat}
    (hA : A = pre ++ [d]) (hd : d ∉ pre) :
    ∀ p ∈ requirements A, p ∈ requirements (A ++ B) := by
  intro p hp
  obtain ⟨hwsub, hrm, hwext⟩ := (mem_requirements p.1 p.2 A).mp hp
  refine (mem_requirements p.1 p.2 (A ++ B)).mpr
    ⟨subStrings_append A B p.1 hwsub, ?_, rightExts_append_subset A B p.1 p.2 hwext⟩
  cases hw : p.1 with
  | nil => simp [rightMaximal]
  | cons a t =>
    have hne : (a :: t) ≠ [] := by simp
    have hoccA : occurs (a :: t) A = true := by
      rw [hw, rightMaximal_cons, Bool.and_eq_true] at hrm
      exact hrm.1
    have hocc : occurs (a :: t) (A ++ B) = true := occurs_append A B (a :: t) hoccA
    have htwo : 2 ≤ (rightExts (a :: t) A).length := by
      rw [hw, rightMaximal_cons, Bool.and_eq_true, Bool.or_eq_true] at hrm
      rcases hrm.2 with hsuf | hlen
      · exfalso
        have hempty := rightExts_eq_nil_of_suffix_unique_last hA hd hne hsuf
        rw [hw] at hwext
        rw [hempty] at hwext
        exact List.not_mem_nil hwext
      · exact of_decide_eq_true hlen
    have htwoJ : 2 ≤ (rightExts (a :: t) (A ++ B)).length :=
      Nat.le_trans htwo (rightExts_length_append A B (a :: t))
    rw [rightMaximal_cons, Bool.and_eq_true, Bool.or_eq_true]
    exact ⟨hocc, Or.inr (by simpa using htwoJ)⟩

/-- χ-count monotonicity under append when the trailing symbol of `A` is unique in `A`.

This is the safe-boundary condition of the oracle hammer (`FLEET_CHIMONO_ORACLE_REPORT.md`,
condition `p1`), and the positive counterpart of `chi_append_not_monotone`. No condition on
`B` beyond positivity is needed: the unique trailing symbol of `A` alone blocks the
terminal-context demotion mechanism. -/
theorem chi_append_unique_last (A B : Text) (pre : Text) (d : Nat)
    (hApos : positive A = true) (hBpos : positive B = true)
    (hA : A = pre ++ [d]) (hd : d ∉ pre) :
    chi A ≤ chi (A ++ B) := by
  have hJpos : positive (A ++ B) = true := by
    unfold positive at hApos hBpos ⊢
    rw [List.all_append, hApos, hBpos]
    rfl
  exact chi_le_of_requirements_subset A (A ++ B) hApos hJpos
    (requirements_subset_append_of_unique_last hA hd)

/-- User-facing form of `chi_append_unique_last`: it suffices that the last symbol of the
old text occurs exactly once in it. -/
theorem chi_append_unique_symbol (A B : Text) (d : Nat)
    (hApos : positive A = true) (hBpos : positive B = true)
    (hlast : A.getLast? = some d) (hcount : A.count d = 1) :
    chi A ≤ chi (A ++ B) := by
  obtain ⟨pre, hpre⟩ := List.getLast?_eq_some_iff.mp hlast
  subst hpre
  refine chi_append_unique_last (pre ++ [d]) B pre d hApos hBpos rfl ?_
  rw [← List.count_eq_zero]
  have h := hcount
  rw [List.count_append] at h
  have h1 : List.count d [d] = 1 := by simp
  rw [h1] at h
  omega

/-- Document-contract corollary: appending anything to a *single-document* text
(`pre` free of the separator `0x1e`, terminated by it) cannot decrease χ. The document
contract as a whole is *not* safe (`chi_single_symbol_document_decreases`), because the
separator recurs at every document end; safety returns exactly in the one-document regime,
consistent with the oracle hammer's finding that violations need ≥ 4 documents in `A`. -/
theorem chi_append_single_document (pre B : Text)
    (hApos : positive (pre ++ [SEP]) = true) (hBpos : positive B = true)
    (hsep : SEP ∉ pre) :
    chi (pre ++ [SEP]) ≤ chi (pre ++ [SEP] ++ B) :=
  chi_append_unique_last (pre ++ [SEP]) B pre SEP hApos hBpos rfl hsep

/-! ## Non-vacuity checks (kernel-evaluated) -/

/-- The safe regime is inhabited: appending a fresh document to a single-document text. -/
example : chi ([1, SEP]) ≤ chi ([1, SEP] ++ [2, SEP, 3, SEP]) :=
  chi_append_single_document [1] [2, SEP, 3, SEP] (by decide) (by decide) (by decide)

/-- The general statement, instantiated with a non-separator unique delimiter. -/
example : chi [5, 7] ≤ chi ([5, 7] ++ [7, 7, 5]) :=
  chi_append_unique_last [5, 7] [7, 7, 5] [5] 7 (by decide) (by decide) rfl (by decide)

/-- The refutation's witnesses violate the safe condition: their trailing symbols recur,
so the hypothesis of `chi_append_unique_symbol` is genuinely non-vacuous. -/
example : lossText.count 2 = 2 ∧ docLoss.count SEP = 5 := by decide

end SxgcUniDelim
