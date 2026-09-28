import Sxgc

/-!
# SxgcRunEdge — `RunEdgeHit` outright

The semantic half of the O1 factorization (see `lean/O1_OBSTRUCTION.md`,
attack 1).  **Theorem `runEdgeHit_true`: every inclusion-maximal coverage
class contains a run-edge position** — statement-locked in `Sxgc.lean`
(`RunEdgeHit`), proven here without touching the emission rule.

## The mathematical argument

Row/position alignment (Layers 1–2 below):

* row `k` of `triplesOf T` carries `sa = (saOrder R)[k]` where
  `R = T.reverse ++ [0]`; its **position** is `rowPos T k = N - sa`
  (`N = T.length + 1`);
* the BWT char of row `k` is the char **at** position `rowPos T k` of `T`
  (when `sa ≥ 1`; `sa = 0` gives the sentinel char `0`);
* the R-suffix of row `k` is `reverse (T.take (rowPos T k - 1)) ++ [0]`.

Coverage alignment: `p = w ++ [c]` is covered at `x` iff `w` is a suffix of
`T.take (x - 1)` and `c = T[x - 1]`.  Hence the occurrences of `w` are in
bijection with the rows whose R-suffix starts with `reverse w` — the
**w-interval** — and the BWT chars of those rows are exactly the chars
following `w`'s occurrences.

Main proof (by contradiction on "no run-edge row of the w-interval has
char `c`"):

1. **Block argument.**  The w-interval is convex in SA order (rows are
   sorted by their R-suffixes, and "starts with `reverse w`" is
   lex-convex).  If some w-row has char `≠ c`, then along the interval the
   is-`c` status changes at an adjacent pair `(k, k+1)` of w-rows, and the
   `c`-side of that pair is a run edge (its char differs from its
   neighbour's).  So: no char-`c` w-row is an edge ⟹ **every** w-row has
   char `c`.

2. **The sentinel row kills the suffix case.**  If `w` is a suffix of `T`
   (including `w = []`), the `sa = 0` row is a w-row with char `0 ≠ c`
   (positive texts), contradicting (1).

3. **Right-maximality kills the rest.**  Otherwise every occurrence of `w`
   is followed by `c`, so `rightExts w T = [c]` has length 1, and `w` is
   not a suffix of `T` — so `w` is not right-maximal, contradicting
   `(w, c) ∈ requirements`.

So some run-edge row `k` of the w-interval has char `c`; its position `v`
ends an occurrence of `p = w ++ [c]`, and since every requirement covered
at `m` is a suffix of the maxW witness `p` (which ends at `v`), all of
`covSet T m` is covered at `v`: `ScopeLe T m v`, and `v` is a run-edge
position.  -/

namespace Sxgc

/-! ## Layer 0: small list lemmas -/

/-- take beyond the end of the list -/
theorem take_full (l : List Nat) (n : Nat) (h : l.length ≤ n) : l.take n = l := by
  have hd : l.drop n = [] := (List.drop_eq_nil_iff).mpr h
  have hsplit : l = l.take n ++ l.drop n := (List.take_append_drop n l).symm
  rw [hd, List.append_nil] at hsplit
  exact hsplit.symm

/-- `append` with a single-element suffix is injective in both arguments. -/
theorem append_last_inj (a b : List Nat) (x y : Nat) (h : a ++ [x] = b ++ [y]) :
    x = y ∧ a = b := by
  have hrev : [x] ++ a.reverse = [y] ++ b.reverse := by
    have h2 : (a ++ [x]).reverse = (b ++ [y]).reverse := by rw [h]
    simpa [List.reverse_append, List.reverse_singleton] using h2
  simp only [List.singleton_append, List.cons.injEq] at hrev
  obtain ⟨rfl, htail⟩ := hrev
  exact ⟨rfl, List.reverse_inj.mp htail⟩

/-- a boolean walk that changes value somewhere has an adjacent change -/
theorem exists_bool_change (f : Nat → Bool) : ∀ d a, f a ≠ f (a + d) →
    ∃ k, a ≤ k ∧ k < a + d ∧ f k ≠ f (k + 1) := by
  intro d
  induction d with
  | zero => intro a h; rw [Nat.add_zero] at h; exact absurd rfl h
  | succ d' ih =>
    intro a h
    by_cases hmid : f a = f (a + d')
    · refine ⟨a + d', by omega, by omega, ?_⟩
      rw [← hmid]; exact h
    · obtain ⟨k, hk1, hk2, hk3⟩ := ih a hmid
      exact ⟨k, hk1, by omega, hk3⟩

/-- all-skip case of `dedupAux`: everything is in `seen` -/
theorem dedupAux_all_skip (seen : List Nat) : ∀ l : List Nat,
    (∀ x ∈ l, seen.contains x = true) → dedup.dedupAux seen l = [] := by
  intro l
  induction l with
  | nil => intro _; rfl
  | cons a rest ih =>
    intro hall
    simp only [dedup.dedupAux]
    rw [if_pos (hall a (by simp))]
    exact ih (fun x hx => hall x (List.mem_cons_of_mem _ hx))

/-- dedup of an all-equal list has length at most 1 -/
theorem dedup_all_eq_le_one (c : Nat) : ∀ l : List Nat, (∀ x ∈ l, x = c) → (dedup l).length ≤ 1 := by
  intro l hall
  cases l with
  | nil => exact Nat.zero_le 1
  | cons a rest =>
    have ha : a = c := hall a List.mem_cons_self
    rw [ha]
    unfold dedup
    simp only [dedup.dedupAux]
    by_cases h0 : List.contains [] c = true
    · rw [if_pos h0]; exact absurd h0 (by simp)
    · rw [if_neg h0]
      have hskip : dedup.dedupAux [c] rest = [] :=
        dedupAux_all_skip [c] rest (fun x hx => by
          rw [hall x (List.mem_cons_of_mem _ hx)]; simp)
      rw [hskip]; simp

/-! ## Layer 1: the reverse-text alignment -/

/-- The R-suffix at 0-based R-position `j` reverses the prefix of length
`T.length - j` and appends the sentinel. -/
theorem R_drop (T : Text) (j : Nat) (hj : j ≤ T.length) :
    (T.reverse ++ [0]).drop j = (T.take (T.length - j)).reverse ++ [0] := by
  have h3 : T = T.take (T.length - j) ++ T.drop (T.length - j) :=
    (List.take_append_drop _ _).symm
  have h4r : T.reverse = (T.take (T.length - j) ++ T.drop (T.length - j)).reverse :=
    congrArg List.reverse h3
  rw [List.reverse_append] at h4r
  rw [List.drop_append, show j - T.reverse.length = 0 from by rw [List.length_reverse]; omega,
      List.drop_zero, h4r, List.drop_append,
      show j - (T.drop (T.length - j)).reverse.length = 0 from by
        rw [List.length_reverse, List.length_drop]; omega,
      List.drop_zero,
      show (T.drop (T.length - j)).reverse.drop j = [] from
        List.drop_eq_nil_iff.mpr (by rw [List.length_reverse, List.length_drop]; omega)]
  simp

/-! ## Layer 2: rows, positions, and characters -/

instance : Inhabited Triple := ⟨⟨0,0,0⟩⟩

/-- sa field of a stream row -/
def saRow (T : Text) (k : Nat) : Nat := (triplesOf T)[k]!.sa

/-- char field of a stream row -/
def charRow (T : Text) (k : Nat) : Nat := (triplesOf T)[k]!.c

/-- getElem! and getD agree in range -/
theorem bang_getD : ∀ (l : List Triple) (i : Nat), i < l.length →
    l[i]! = l.getD i ⟨0,0,0⟩ := by
  intro l i h
  rw [getElem!_pos l i h]
  simp only [List.getD]
  rw [show l[i]? = some l[i] from (List.getElem?_eq_some_iff).mpr ⟨h, rfl⟩]
  simp

theorem getD_bang (T : Text) (k : Nat) (hk : k < (triplesOf T).length) :
    (triplesOf T).getD k ⟨0,0,0⟩ = (triplesOf T)[k]! :=
  (bang_getD (triplesOf T) k hk).symm

/-- row of a map over range: the index function -/
theorem map_range_bang (f : Nat → Triple) (n i : Nat) (h : i < n) :
    ((List.range n).map f)[i]! = f i := by
  rw [bang_getD _ _ (by rw [List.length_map, List.length_range]; exact h),
    getD_map_range f n i ⟨0,0,0⟩ h]

/-- stream length -/
theorem triplesOf_length (T : Text) : (triplesOf T).length = T.length + 1 := by
  simp only [triplesOf]
  rw [List.length_map, List.length_range]
  simp

/-- the sa field of row k is the k-th saOrder entry of R -/
theorem saRow_eq (T : Text) (k : Nat) (hk : k < (triplesOf T).length) :
    saRow T k = (saOrder (T.reverse ++ [0]))[k]! := by
  have hk' : k < T.length + 1 := by rwa [triplesOf_length] at hk
  simp only [saRow, triplesOf, List.length_append, List.length_reverse,
    List.length_singleton] at ⊢
  rw [map_range_bang _ (T.length + 1) k hk']

/-- sa = 0 rows carry the sentinel char -/
theorem charRow_zero (T : Text) (k : Nat) (hk : k < (triplesOf T).length)
    (h0 : (saOrder (T.reverse ++ [0]))[k]! = 0) :
    charRow T k = 0 := by
  have hk' : k < T.length + 1 := by rwa [triplesOf_length] at hk
  simp only [charRow, triplesOf, List.length_append, List.length_reverse,
    List.length_singleton] at ⊢
  rw [map_range_bang _ (T.length + 1) k hk']
  show (if ((saOrder (T.reverse ++ [0]))[k]! == 0) = true then 0
      else (T.reverse ++ [0])[(saOrder (T.reverse ++ [0]))[k]! - 1]!) = 0
  rw [h0]
  simp

/-- sa ≥ 1 rows carry the R-char before their suffix -/
theorem charRow_pos (T : Text) (k : Nat) (hk : k < (triplesOf T).length)
    (h1 : 1 ≤ (saOrder (T.reverse ++ [0]))[k]!) :
    charRow T k = (T.reverse ++ [0])[(saOrder (T.reverse ++ [0]))[k]! - 1]! := by
  have hk' : k < T.length + 1 := by rwa [triplesOf_length] at hk
  simp only [charRow, triplesOf, List.length_append, List.length_reverse,
    List.length_singleton] at ⊢
  rw [map_range_bang _ (T.length + 1) k hk']
  show (if ((saOrder (T.reverse ++ [0]))[k]! == 0) = true then 0
      else (T.reverse ++ [0])[(saOrder (T.reverse ++ [0]))[k]! - 1]!) =
    (T.reverse ++ [0])[(saOrder (T.reverse ++ [0]))[k]! - 1]!
  have hb : ((saOrder (T.reverse ++ [0]))[k]! == 0) = false := by
    cases hbb : ((saOrder (T.reverse ++ [0]))[k]! == 0) with
    | false => rfl
    | true => rw [beq_iff_eq] at hbb; omega
  rw [hb]
  simp

/-- every row's sa is an R-position -/
theorem saRow_lt (T : Text) (k : Nat) (hk : k < (triplesOf T).length) :
    saRow T k < T.length + 1 := by
  rw [saRow_eq T k hk]
  have hlen : (saOrder (T.reverse ++ [0])).length = T.length + 1 := by
    rw [saOrder_length]; simp
  have hklt : k < (saOrder (T.reverse ++ [0])).length := by
    rw [hlen]; rwa [triplesOf_length] at hk
  have hmem : (saOrder (T.reverse ++ [0]))[k]! ∈ saOrder (T.reverse ++ [0]) := by
    rw [getElem!_pos _ k hklt]; exact List.getElem_mem hklt
  have := saOrder_lt (T.reverse ++ [0]) _ hmem
  simpa using this

/-- sa determines the row -/
theorem saRow_inj (T : Text) {k k' : Nat} (hk : k < (triplesOf T).length)
    (hk' : k' < (triplesOf T).length) (h : saRow T k = saRow T k') : k = k' := by
  classical
  by_cases hne : k = k'
  · exact hne
  have hlen : (saOrder (T.reverse ++ [0])).length = T.length + 1 := by
    rw [saOrder_length]; simp
  have hpair : ∀ i j, i < (saOrder (T.reverse ++ [0])).length →
      j < (saOrder (T.reverse ++ [0])).length → i < j →
      (saOrder (T.reverse ++ [0]))[i]! ≠ (saOrder (T.reverse ++ [0]))[j]! := by
    intro i j hi hj hij
    have h := (List.pairwise_iff_getElem.mp (saOrder_nodup _)) i j hi hj hij
    rw [getElem!_pos _ i hi, getElem!_pos _ j hj]
    exact h
  have hklt : k < (saOrder (T.reverse ++ [0])).length := by
    rw [hlen]; rwa [triplesOf_length] at hk
  have hklt' : k' < (saOrder (T.reverse ++ [0])).length := by
    rw [hlen]; rwa [triplesOf_length] at hk'
  rw [saRow_eq T k hk, saRow_eq T k' hk'] at h
  rcases Nat.lt_or_ge k k' with hlt | hge
  · exact absurd h (hpair k k' hklt hklt' hlt)
  · have hlt' : k' < k := by omega
    exact absurd h.symm (hpair k' k hklt' hklt hlt')

/-- every R-position is some row's sa -/
theorem saRow_surj (T : Text) (p : Nat) (hp : p < T.length + 1) :
    ∃ k, k < (triplesOf T).length ∧ saRow T k = p := by
  have hmem : p ∈ saOrder (T.reverse ++ [0]) := by
    refine saOrder_mem _ p ?_
    simp
    omega
  obtain ⟨k, hk, hget⟩ := List.mem_iff_getElem.mp hmem
  have hlen : (saOrder (T.reverse ++ [0])).length = T.length + 1 := by
    rw [saOrder_length, List.length_append, List.length_reverse, List.length_singleton]
  have hkt : k < (triplesOf T).length := by
    rw [triplesOf_length]
    rw [hlen] at hk
    exact hk
  refine ⟨k, hkt, ?_⟩
  rw [saRow_eq T k hkt]
  rw [getElem!_pos _ k hk]
  exact hget

/-- the R-char at position i is the text char at `T.length - 1 - i` -/
theorem R_get (T : Text) (i : Nat) (hi : i < T.length) :
    (T.reverse ++ [0])[i]! = T[T.length - 1 - i]! := by
  have h1 : i < (T.reverse ++ [0]).length := by simp; omega
  have h2 : i < T.reverse.length := by rw [List.length_reverse]; exact hi
  have h3 : T.length - 1 - i < T.length := by omega
  rw [getElem!_pos (T.reverse ++ [0]) i h1, getElem!_pos T (T.length - 1 - i) h3,
    List.getElem_append_left h2, List.getElem_reverse h2]

/-- text position of row k -/
def rowPos (T : Text) (k : Nat) : Nat := T.length + 1 - saRow T k

/-- the BWT char of a row is the text char AT its position -/
theorem charRow_at_pos (T : Text) (k : Nat) (hk : k < (triplesOf T).length)
    (hsa : 1 ≤ saRow T k) : charRow T k = T[rowPos T k - 1]! := by
  have hlt := saRow_lt T k hk
  have heq := saRow_eq T k hk
  have ho : 1 ≤ (saOrder (T.reverse ++ [0]))[k]! := by rw [← heq]; exact hsa
  have h1 : (saOrder (T.reverse ++ [0]))[k]! - 1 < T.length := by omega
  rw [charRow_pos T k hk ho, R_get T _ h1]
  have hfix : T.length - 1 - ((saOrder (T.reverse ++ [0]))[k]! - 1) = rowPos T k - 1 := by
    rw [← heq]
    unfold rowPos
    omega
  rw [hfix]

/-! ## Layer 3: the w-interval (rows whose R-suffix starts with reverse w) -/

/-- a suffix relation on plain lists -/
def SuffixOf (w A : List Nat) : Prop := ∃ q, A = q ++ w

/-- suffix gives reverse-prefix (with the trailing sentinel) -/
theorem suffix_rev_prefix (A w : List Nat) (h : SuffixOf w A) :
    ∃ rest, A.reverse ++ [0] = w.reverse ++ rest := by
  obtain ⟨q, rfl⟩ := h
  exact ⟨q.reverse ++ [0], by rw [List.reverse_append, List.append_assoc]⟩

/-- reverse-prefix (with the trailing sentinel) gives suffix, for positive w -/
theorem rev_prefix_suffix (A w : List Nat) (h : ∃ rest, A.reverse ++ [0] = w.reverse ++ rest)
    (hw : ∀ x ∈ w, 1 ≤ x) : SuffixOf w A := by
  obtain ⟨rest, hrest⟩ := h
  by_cases hlen : w.length ≤ A.length
  · have hlen' : w.length ≤ A.reverse.length := by rw [List.length_reverse]; exact hlen
    have hsub1 : w.length - A.reverse.length = 0 := by omega
    have hlenw : w.reverse.length = w.length := List.length_reverse
    have htake : (A.reverse ++ [0]).take w.length = (w.reverse ++ rest).take w.length := by
      rw [hrest]
    rw [List.take_append, List.take_append, hsub1, hlenw, Nat.sub_self, List.take_zero,
      List.take_zero, List.append_nil, List.append_nil] at htake
    have hwr : w.reverse.take w.length = w.reverse :=
      take_full w.reverse w.length (by rw [List.length_reverse]; omega)
    rw [hwr] at htake
    have hsplit : A.reverse = w.reverse ++ A.reverse.drop w.length := by
      have h1 : A.reverse.take w.length ++ A.reverse.drop w.length = A.reverse :=
        List.take_append_drop _ _
      rw [htake] at h1
      exact h1.symm
    refine ⟨A.reverse.drop w.length |>.reverse, ?_⟩
    calc A = A.reverse.reverse := (List.reverse_reverse A).symm
      _ = (w.reverse ++ A.reverse.drop w.length).reverse := congrArg List.reverse hsplit
      _ = (A.reverse.drop w.length).reverse ++ w.reverse.reverse := List.reverse_append
      _ = (A.reverse.drop w.length).reverse ++ w := by rw [List.reverse_reverse]
  · -- w.length ≥ A.length + 1
    have hlenA : A.reverse.length = A.length := List.length_reverse
    have hlenr : w.reverse.length = w.length := List.length_reverse
    have hlenEq : A.reverse.length + 1 = w.length + rest.length := by
      have := congrArg List.length hrest
      simp only [List.length_append, List.length_singleton] at this
      omega
    have hrest0 : rest.length = 0 := by omega
    have hw0 : rest = [] := List.length_eq_zero_iff.mp hrest0
    have hwfull : w.reverse = A.reverse ++ [0] := by
      rw [hw0, List.append_nil] at hrest; exact hrest.symm
    have hwA : w = 0 :: A := by
      have hw1 : w = (A.reverse ++ [0]).reverse := by
        rw [← hwfull, List.reverse_reverse]
      rw [hw1, List.reverse_append, List.reverse_singleton, List.reverse_reverse,
        List.singleton_append]
    have hhead : 0 ∈ w := by rw [hwA]; exact List.mem_cons_self
    have := hw 0 hhead
    omega

/-- row k is a w-row: its R-suffix starts with reverse w -/
def wRow (T : Text) (w : List Nat) (k : Nat) : Prop :=
  ∃ rest, (T.reverse ++ [0]).drop (saRow T k) = w.reverse ++ rest

/-- w-rows are exactly the rows whose position ends an occurrence of w -/
theorem wRow_iff_suffix (T : Text) (w : List Nat) (k : Nat)
    (hkw : k < (triplesOf T).length) (hw : ∀ x ∈ w, 1 ≤ x) :
    wRow T w k ↔ SuffixOf w (T.take (rowPos T k - 1)) := by
  have hlt := saRow_lt T k hkw
  constructor
  · intro h
    rcases Nat.eq_zero_or_pos (saRow T k) with h0 | h1
    · -- sa = 0: the full R is the suffix; rowPos - 1 = T.length
      have hrp : rowPos T k - 1 = T.length := by
        unfold rowPos; rw [h0]; omega
      have hR : (T.reverse ++ [0]).drop 0 = (T.reverse ++ [0]) := by
        rw [List.drop_zero]
      obtain ⟨rest, hrest⟩ := h
      rw [h0, hR] at hrest
      have : SuffixOf w T := rev_prefix_suffix T w ⟨rest, hrest⟩ hw
      obtain ⟨q, hq⟩ := this
      refine ⟨q, ?_⟩
      rw [hrp, take_full T T.length (by omega)]
      exact hq
    · -- sa ≥ 1
      have hsa : saRow T k ≤ T.length := by omega
      have hrp : rowPos T k - 1 = T.length - saRow T k := by
        unfold rowPos; omega
      obtain ⟨rest, hrest⟩ := h
      have hdrop : (T.reverse ++ [0]).drop (saRow T k)
          = (T.take (T.length - saRow T k)).reverse ++ [0] := R_drop T _ hsa
      rw [hdrop] at hrest
      have hA : SuffixOf w (T.take (T.length - saRow T k)) :=
        rev_prefix_suffix _ w ⟨rest, hrest⟩ hw
      obtain ⟨q, hq⟩ := hA
      refine ⟨q, ?_⟩
      rw [hrp]
      exact hq
  · intro hsuffix
    rcases Nat.eq_zero_or_pos (saRow T k) with h0 | h1
    · have hrp : rowPos T k - 1 = T.length := by
        unfold rowPos; rw [h0]; omega
      have hR : (T.reverse ++ [0]).drop 0 = (T.reverse ++ [0]) := by
        rw [List.drop_zero]
      have hT : T.take (rowPos T k - 1) = T := by
        rw [hrp]; exact take_full T T.length (by omega)
      obtain ⟨q, hq⟩ := hsuffix
      have hfull : SuffixOf w T := ⟨q, by rw [← hT]; exact hq⟩
      obtain ⟨rest, hrest⟩ := suffix_rev_prefix T w hfull
      refine ⟨rest, ?_⟩
      rw [h0, hR]
      exact hrest
    · have hsa : saRow T k ≤ T.length := by omega
      have hrp : rowPos T k - 1 = T.length - saRow T k := by
        unfold rowPos; omega
      have hdrop : (T.reverse ++ [0]).drop (saRow T k)
          = (T.take (T.length - saRow T k)).reverse ++ [0] := R_drop T _ hsa
      obtain ⟨rest, hrest⟩ := suffix_rev_prefix (T.take (rowPos T k - 1)) w hsuffix
      rw [hrp] at hrest
      refine ⟨rest, ?_⟩
      rw [hdrop]
      exact hrest

/-! ## Layer 3b: lex-convexity of the w-interval -/

/-- two lex-comparable prefix-sharing lists force the middle to share the prefix -/
theorem lexLE_between_prefix : ∀ (u a x b : List Nat),
    (∃ ra, a = u ++ ra) → (∃ rb, b = u ++ rb) →
    lexLE a x = true → lexLE x b = true → ∃ rx, x = u ++ rx := by
  intro u
  induction u with
  | nil => intro a x b _ _ _ _; exact ⟨x, rfl⟩
  | cons un ur ih =>
    intro a x b ha hb h1 h2
    obtain ⟨ra, rfl⟩ := ha
    obtain ⟨rb, rfl⟩ := hb
    cases x with
    | nil => simp [lexLE] at h1
    | cons xn xr =>
      simp only [List.cons_append, lexLE, Bool.or_eq_true, Bool.and_eq_true] at h1 h2
      rcases h1 with hlt | ⟨heq, hrest⟩
      · -- un < xn: h2 impossible
        have hun : un < xn := of_decide_eq_true hlt
        have h2' : (decide (xn < un) = true) ∨ ((xn == un) = true ∧ lexLE xr (ur ++ rb) = true) := h2
        rcases h2' with hlt2 | ⟨heq2, _⟩
        · have : xn < un := of_decide_eq_true hlt2
          omega
        · have : xn = un := beq_iff_eq.mp heq2
          omega
      · -- un = xn
        have hun : un = xn := beq_iff_eq.mp heq
        have hrest2 : lexLE xr (ur ++ rb) = true := by
          rcases h2 with hlt2 | ⟨heq2, hrest2'⟩
          · have : xn < un := of_decide_eq_true hlt2
            omega
          · exact hrest2'
        obtain ⟨rx, hrx⟩ := ih (ur ++ ra) xr (ur ++ rb) ⟨_, rfl⟩ ⟨_, rfl⟩ hrest hrest2
        refine ⟨rx, ?_⟩
        rw [hrx, hun, List.cons_append]

/-! ## Layer 4: convexity of the w-interval and the block argument -/

/-- the w-rows are convex in SA order -/
theorem wRow_convex (T : Text) (w : List Nat) {k k' k'' : Nat}
    (hk : k < (triplesOf T).length) (hk'' : k'' < (triplesOf T).length)
    (hlt : k < k') (hlt' : k' < k'') (hWk : wRow T w k) (hWk'' : wRow T w k'') :
    wRow T w k' := by
  have hlen : (saOrder (T.reverse ++ [0])).length = T.length + 1 := by
    rw [saOrder_length, List.length_append, List.length_reverse, List.length_singleton]
  have hklt : k < (saOrder (T.reverse ++ [0])).length := by
    rw [hlen]; rwa [triplesOf_length] at hk
  have hklt'' : k'' < (saOrder (T.reverse ++ [0])).length := by
    rw [hlen]; rwa [triplesOf_length] at hk''
  have hmid : k' < (saOrder (T.reverse ++ [0])).length := by omega
  have hk'len : k' < (triplesOf T).length := by
    have := triplesOf_length T; omega
  have hs1 : lexLE ((T.reverse ++ [0]).drop (saOrder (T.reverse ++ [0]))[k])
      ((T.reverse ++ [0]).drop (saOrder (T.reverse ++ [0]))[k']) :=
    saOrder_sorted_getElem (T.reverse ++ [0]) k k' hklt hmid hlt
  have hs2 : lexLE ((T.reverse ++ [0]).drop (saOrder (T.reverse ++ [0]))[k'])
      ((T.reverse ++ [0]).drop (saOrder (T.reverse ++ [0]))[k'']) :=
    saOrder_sorted_getElem (T.reverse ++ [0]) k' k'' hmid hklt'' hlt'
  obtain ⟨rest, hrest⟩ := hWk
  obtain ⟨rest'', hrest''⟩ := hWk''
  rw [saRow_eq T k hk, getElem!_pos _ k hklt] at hrest
  rw [saRow_eq T k'' hk'', getElem!_pos _ k'' hklt''] at hrest''
  obtain ⟨rx, hrx⟩ := lexLE_between_prefix w.reverse _ _ _ ⟨rest, hrest⟩ ⟨rest'', hrest''⟩ hs1 hs2
  refine ⟨rx, ?_⟩
  rw [saRow_eq T k' hk'len, getElem!_pos _ k' hmid]
  exact hrx


/-- charRow is the getD-char -/
theorem charRow_getD (T : Text) (k : Nat) (hk : k < (triplesOf T).length) :
    ((triplesOf T).getD k ⟨0,0,0⟩).c = charRow T k := by
  rw [getD_bang T k hk]; rfl

theorem saRow_getD (T : Text) (k : Nat) (hk : k < (triplesOf T).length) :
    ((triplesOf T).getD k ⟨0,0,0⟩).sa = saRow T k := by
  rw [getD_bang T k hk]; rfl

/-- char differs from the next row's: run edge -/
theorem isRunEdge_of_diff_next (ts : List Triple) (k : Nat) (hk : k < ts.length)
    (hdiff : (ts.getD k ⟨0,0,0⟩).c ≠ (ts.getD (k+1) ⟨0,0,0⟩).c) : isRunEdge ts k = true := by
  unfold isRunEdge
  rw [Bool.or_eq_true]
  refine Or.inr ?_
  rw [Bool.and_eq_true]
  refine ⟨decide_eq_true hk, ?_⟩
  rw [Bool.or_eq_true]
  refine Or.inr ?_
  exact bne_iff_ne.mpr hdiff

/-- char differs from the previous row's: run edge -/
theorem isRunEdge_of_diff_prev (ts : List Triple) (k : Nat) (hk : k < ts.length) (hk1 : 1 ≤ k)
    (hdiff : (ts.getD k ⟨0,0,0⟩).c ≠ (ts.getD (k-1) ⟨0,0,0⟩).c) : isRunEdge ts k = true := by
  unfold isRunEdge isBoundary
  rw [if_neg (by omega), if_neg (by omega), Bool.or_eq_true]
  exact Or.inl (bne_iff_ne.mpr hdiff)

/-- take m splits at position m -/
theorem take_succ_last (T : Text) {m : Nat} (hm : 1 ≤ m) (hm2 : m ≤ T.length) :
    T.take m = T.take (m - 1) ++ [T[m - 1]!] := by
  have hm1 : m - 1 < T.length := by omega
  have hopt : T[m - 1]? = some T[m - 1]! :=
    (List.getElem?_eq_some_iff).mpr ⟨hm1, (getElem!_pos T (m - 1) hm1).symm⟩
  rw [show m = (m - 1) + 1 from by omega, List.take_add_one, hopt]
  rfl

/-- if a w-row of char ≠ c exists, a char-c w-row is a run edge -/
theorem exists_edge_wRow (T : Text) (w : List Nat) (c : Nat) {a b : Nat}
    (hla : a < (triplesOf T).length) (hlb : b < (triplesOf T).length)
    (hWa : wRow T w a) (hca : charRow T a = c)
    (hWb : wRow T w b) (hcb : charRow T b ≠ c) :
    ∃ k, k < (triplesOf T).length ∧ wRow T w k ∧ charRow T k = c ∧
      isRunEdge (triplesOf T) k = true := by
  classical
  have key : ∀ x y : Nat, x < y → x < (triplesOf T).length → y < (triplesOf T).length →
      wRow T w x → wRow T w y →
      decide (charRow T x = c) ≠ decide (charRow T y = c) →
      ∃ k, k < (triplesOf T).length ∧ wRow T w k ∧ charRow T k = c ∧
        isRunEdge (triplesOf T) k = true := by
    intro x y hxy hlx hly hWx hWy hne
    obtain ⟨k, hk1, hk2, hk3⟩ :=
      exists_bool_change (fun k => decide (charRow T k = c)) (y - x) x (by
        have hxy' : x + (y - x) = y := by omega
        intro hcon
        apply hne
        rw [hxy'] at hcon
        exact hcon)
    have hkl1 : x ≤ k := hk1
    have hkl2 : k + 1 ≤ y := by omega
    have hkl : k < (triplesOf T).length := by
      have := triplesOf_length T; omega
    have hkl1' : k + 1 < (triplesOf T).length := by
      have := triplesOf_length T; omega
    -- both are w-rows (convexity)
    have hWk : wRow T w k := by
      rcases Nat.eq_or_lt_of_le hkl1 with heq | hlt
      · rw [← heq]; exact hWx
      · exact wRow_convex T w hlx hly hlt (by omega) hWx hWy
    have hWk1 : wRow T w (k + 1) := by
      rcases Nat.eq_or_lt_of_le hkl2 with heq | hlt
      · rw [heq]; exact hWy
      · exact wRow_convex T w hlx hly (by omega) hlt hWx hWy
    -- the status change gives a char difference
    have hcdiff : charRow T k ≠ charRow T (k + 1) := by
      intro heqchar
      have hbeta : decide (charRow T k = c) ≠ decide (charRow T (k + 1) = c) := hk3
      rw [heqchar] at hbeta
      exact absurd hbeta (by simp)
    by_cases hck : charRow T k = c
    · refine ⟨k, hkl, hWk, hck, ?_⟩
      exact isRunEdge_of_diff_next (triplesOf T) k hkl
        (by rw [charRow_getD T k hkl, charRow_getD T (k+1) hkl1']; omega)
    · have hck1 : charRow T (k + 1) = c := by
        have hbeta : decide (charRow T k = c) ≠ decide (charRow T (k + 1) = c) := hk3
        have hf : decide (charRow T k = c) = false := by simp [hck]
        rw [hf] at hbeta
        cases hbb : decide (charRow T (k + 1) = c) with
        | true => exact of_decide_eq_true hbb
        | false => rw [hbb] at hbeta; exact absurd rfl hbeta
      refine ⟨k + 1, hkl1', hWk1, hck1, ?_⟩
      exact isRunEdge_of_diff_prev (triplesOf T) (k + 1) hkl1' (by omega)
        (by rw [show k + 1 - 1 = k from by omega, charRow_getD T (k+1) hkl1',
            charRow_getD T k hkl]; omega)

  rcases Nat.lt_trichotomy a b with hlt | heq | hgt
  · exact key a b hlt hla hlb hWa hWb (by simp [hca, hcb])
  · rw [← heq] at hcb
    exact absurd hca hcb
  · exact key b a hgt hlb hla hWb hWa (by simp [hca, hcb])


/-! ## Layer 5: the assembly -/

/-- `rightMaximal.isSuffix = true` gives a plain suffix -/
theorem suffix_of_isSuffix (w T : List Nat)
    (h : rightMaximal.isSuffix w T = true) : SuffixOf w T := by
  unfold rightMaximal.isSuffix at h
  rw [Bool.and_eq_true] at h
  obtain ⟨_, hbeq⟩ := h
  refine ⟨T.take (T.length - w.length), ?_⟩
  have hdrop : List.drop (T.length - w.length) T = w := by
    simp at hbeq
    exact hbeq.symm
  calc T = T.take (T.length - w.length) ++ List.drop (T.length - w.length) T :=
        (List.take_append_drop _ _).symm
    _ = T.take (T.length - w.length) ++ w := by rw [hdrop]

/-- **RunEdgeHit outright** — every inclusion-maximal coverage class contains
a run-edge position. -/
theorem runEdgeHit_true (T : Text) (hT : positive T = true) : RunEdgeHit T := by
  intro m hmP hmax
  -- position bounds for m
  have hm1 : 1 ≤ m ∧ m ≤ T.length := by
    rcases List.mem_map.mp hmP with ⟨i, hi, rfl⟩
    rw [List.mem_range] at hi
    omega
  obtain ⟨hm1, hmT⟩ := hm1
  -- the maxW witness
  obtain ⟨p, hpCov, hpW⟩ := exists_maxW (T := T) (x := m) (hne := hmax.1)
  obtain ⟨hpReq, hpCover⟩ := (mem_covSet T m p).mp hpCov
  obtain ⟨hwSub, hwMax, hcExt⟩ := (mem_requirements p.1 p.2 T).mp hpReq
  obtain ⟨w, c⟩ := p
  -- the cover decomposition
  obtain ⟨u, hu⟩ := (coversAt_iff_suffix (w ++ [c]) m T).mp hpCover
  rw [pref] at hu
  have hsplit : T.take m = T.take (m - 1) ++ [T[m - 1]!] := take_succ_last T hm1 hmT
  have hlast : c = T[m - 1]! ∧ u ++ w = T.take (m - 1) := by
    have hassoc : (u ++ w) ++ [c] = T.take (m - 1) ++ [T[m - 1]!] := by
      rw [show (u ++ w) ++ [c] = u ++ (w ++ [c]) from List.append_assoc u w [c],
        ← hu, hsplit]
    exact append_last_inj _ _ _ _ hassoc
  -- positivity
  have hwpos : ∀ x ∈ w, 1 ≤ x := by
    intro x hx
    have hxw : x ∈ T.take (m - 1) := by
      rw [← hlast.2]
      exact List.mem_append_right _ hx
    have hxT : x ∈ T := List.mem_of_mem_take hxw
    exact (positive_of_mem T hT hxT).1
  have hcpos : 1 ≤ c := by
    have hcT : c ∈ T := occurs_append_last w c T ((mem_rightExts w c T).mp hcExt)
    exact (positive_of_mem T hT hcT).1
  -- the row of m
  obtain ⟨k₀, hk₀, hsa₀⟩ := saRow_surj T (T.length + 1 - m) (by omega)
  have hrp₀ : rowPos T k₀ = m := by
    unfold rowPos; rw [hsa₀]; omega
  have hchar₀ : charRow T k₀ = c := by
    rw [charRow_at_pos T k₀ hk₀ (by rw [hsa₀]; omega), hrp₀]
    exact hlast.1.symm
  have hW₀ : wRow T w k₀ := by
    refine (wRow_iff_suffix T w k₀ hk₀ hwpos).mpr ⟨u, ?_⟩
    rw [hrp₀]
    exact hlast.2.symm
  -- KEY: a char-c run-edge w-row exists
  have key : ∃ k, k < (triplesOf T).length ∧ wRow T w k ∧ charRow T k = c ∧
      isRunEdge (triplesOf T) k = true := by
    classical
    by_cases hB : ∃ k₁, k₁ < (triplesOf T).length ∧ wRow T w k₁ ∧ charRow T k₁ ≠ c
    · obtain ⟨k₁, hk₁, hW₁, hc₁⟩ := hB
      exact exists_edge_wRow T w c hk₀ hk₁ hW₀ hchar₀ hW₁ hc₁
    · exfalso
      have hall : ∀ k, k < (triplesOf T).length → wRow T w k → charRow T k = c := by
        intro k hk hW
        by_cases hne : charRow T k = c
        · exact hne
        · exact absurd ⟨k, hk, hW, hne⟩ hB
      by_cases hSuf : SuffixOf w T
      · -- the sentinel row is a w-row of char 0 ≠ c
        obtain ⟨k₀', hk₀', hsa₀'⟩ := saRow_surj T 0 (by omega)
        obtain ⟨q₀, hq₀⟩ := hSuf
        have hW₀' : wRow T w k₀' := by
          refine (wRow_iff_suffix T w k₀' hk₀' hwpos).mpr ⟨q₀, ?_⟩
          have hrp : rowPos T k₀' - 1 = T.length := by
            unfold rowPos; rw [hsa₀']; omega
          rw [hrp, take_full T T.length (by omega)]
          exact hq₀
        have hc₀' : charRow T k₀' = 0 := by
          refine charRow_zero T k₀' hk₀' ?_
          rw [← saRow_eq T k₀' hk₀', hsa₀']
        have hcc := hall k₀' hk₀' hW₀'
        omega
      · -- every occurrence of w is followed by c
        have hfoll : ∀ e, e ≤ T.length - 1 → SuffixOf w (T.take e) → T[e]! = c := by
          intro e he hsuf
          obtain ⟨q, hq⟩ := hsuf
          obtain ⟨k, hk, hsak⟩ :=
            saRow_surj T (T.length + 1 - (e + 1)) (by omega)
          have hsa1 : 1 ≤ saRow T k := by rw [hsak]; omega
          have hrp : rowPos T k = e + 1 := by
            unfold rowPos; rw [hsak]; omega
          have hWk : wRow T w k := by
            refine (wRow_iff_suffix T w k hk hwpos).mpr ⟨q, ?_⟩
            rw [hrp, show e + 1 - 1 = e from by omega]
            exact hq
          have hck := hall k hk hWk
          rw [charRow_at_pos T k hk hsa1, hrp,
            show e + 1 - 1 = e from by omega] at hck
          exact hck
        -- every right extension equals c
        have hocc : ∀ x, occurs (w ++ [x]) T = true → x = c := by
          intro x hoccx
          obtain ⟨i, hi, htake⟩ := (occurs_eq_true (w ++ [x]) T (by simp)).mp hoccx
          have hlen : (w ++ [x]).length = w.length + 1 := by simp
          have hf : i + w.length + 1 ≤ T.length := by omega
          have hfull : T.take (i + w.length + 1) = T.take i ++ w ++ [x] := by
            rw [show i + w.length + 1 = i + (w ++ [x]).length from by
                  rw [hlen]; omega,
              List.take_add, htake, List.append_assoc]
          have hsucc : T.take (i + w.length + 1)
              = T.take (i + w.length) ++ [T[i + w.length]!] :=
            take_succ_last T (by omega) (by omega)
          rw [hsucc] at hfull
          obtain ⟨hx, hqw⟩ := append_last_inj _ _ _ _ hfull
          have he : i + w.length ≤ T.length - 1 := by omega
          have hfolli := hfoll (i + w.length) he ⟨T.take i, hqw⟩
          omega
        have hext : ∀ x ∈ rightExts w T, x = c :=
          fun x hx => hocc x ((mem_rightExts w x T).mp hx)
        have hlen1 : (rightExts w T).length = 1 := by
          have hle : (rightExts w T).length ≤ 1 := by
            rw [rightExts]
            refine dedup_all_eq_le_one c _ ?_
            intro x hx
            obtain ⟨_, hxocc⟩ := List.mem_filter.mp hx
            exact hocc x hxocc
          have hge : 1 ≤ (rightExts w T).length := List.length_pos_of_mem hcExt
          omega
        -- w nonempty would contradict ¬SuffixOf
        cases w with
        | nil => exact absurd (show SuffixOf [] T from ⟨T, by simp⟩) hSuf
        | cons a as =>
          simp only [rightMaximal] at hwMax
          rw [Bool.and_eq_true, Bool.or_eq_true] at hwMax
          obtain ⟨_, hisf | hlen2⟩ := hwMax
          · exact absurd (suffix_of_isSuffix _ _ hisf) hSuf
          · exact absurd (of_decide_eq_true hlen2) (by omega)
  obtain ⟨k, hk, hW, hck, hedge⟩ := key
  -- sa of the edge row is positive
  have hsa1 : 1 ≤ saRow T k := by
    by_cases h0 : 1 ≤ saRow T k
    · exact h0
    · exfalso
      have hz : saRow T k = 0 := by omega
      have hcz : charRow T k = 0 := charRow_zero T k hk (by
        rw [← saRow_eq T k hk, hz])
      omega
  have hv1 : 1 ≤ rowPos T k := by
    have hlt := saRow_lt T k hk
    unfold rowPos; omega
  have hv2 : rowPos T k ≤ T.length := by
    have hlt := saRow_lt T k hk
    unfold rowPos; omega
  -- the witness word ends at v := rowPos T k
  obtain ⟨q, hq⟩ := (wRow_iff_suffix T w k hk hwpos).mp hW
  have hvchar : T[rowPos T k - 1]! = c := by
    rw [← charRow_at_pos T k hk hsa1]
    exact hck
  have hpsuf : ∃ qq, T.take (rowPos T k) = qq ++ (w ++ [c]) := by
    refine ⟨q, ?_⟩
    rw [take_succ_last T hv1 hv2, hvchar, hq, List.append_assoc q w [c]]
  -- ScopeLe
  have hscope : ScopeLe T m (rowPos T k) := by
    intro q hq'
    obtain ⟨hqReq, hqCov⟩ := (mem_covSet T m q).mp hq'
    have hle : (q.1 ++ [q.2]).length ≤ (w ++ [c]).length := by
      have h1 : wlen q ≤ maxW T m := wlen_le_maxW T m q hq'
      have h2 : maxW T m ≤ wlen (w, c) := hpW
      have h3 : wlen (w, c) = (w ++ [c]).length := wlen_congr (w, c)
      have h4 : wlen q = (q.1 ++ [q.2]).length := wlen_congr q
      omega
    have hdrop : (w ++ [c]).drop ((w ++ [c]).length - (q.1 ++ [q.2]).length)
        = q.1 ++ [q.2] := coversAt_suffix_of_coversAt hqCov hpCover hle
    obtain ⟨qq, hqq⟩ := hpsuf
    have hpv : coversAt (w ++ [c]) (rowPos T k) T = true :=
      (coversAt_iff_suffix _ _ T).mpr ⟨qq, hqq⟩
    have hqv : coversAt (q.1 ++ [q.2]) (rowPos T k) T = true :=
      coversAt_of_suffix hpv hdrop
    exact (mem_covSet T (rowPos T k) q).mpr ⟨hqReq, hqv⟩
  -- membership in runEdgePositions
  have hmem : rowPos T k ∈ runEdgePositions T := by
    unfold runEdgePositions
    refine List.mem_filter.mpr ⟨?_, decide_eq_true ⟨hv1, hv2⟩⟩
    refine List.mem_map.mpr ⟨k, ?_, ?_⟩
    · refine List.mem_filter.mpr ⟨by rw [List.mem_range]; exact hk, hedge⟩
    · rw [saRow_getD T k hk]
      rfl
  exact ⟨rowPos T k, hmem, hscope⟩

end Sxgc
