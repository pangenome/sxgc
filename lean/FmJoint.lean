import Sxgc
open Sxgc

/-! ## Joint scan/FM bisimulation: infrastructure

Goal: prove `∀ x, x ∈ scan N ts ↔ x ∈ fmSpec N ts` for ALL streams: the machines
are coupled per character with a three-case invariant that survives
non-boundary triples. -/

def dT : Triple := ⟨0, 0, 0⟩

def Ls (ts : List Triple) : List Nat := ts.map (fun t => t.lcp)

def isBnd (ts : List Triple) (i : Nat) : Bool :=
  decide (1 ≤ i ∧ i < ts.length ∧ (ts.getD i dT).c ≠ (ts.getD (i-1) dT).c)

/-- last boundary index strictly below i (0 = none) -/
def prevBnd (ts : List Triple) : Nat → Nat
  | 0 => 0
  | i+1 => if isBnd ts i then i else prevBnd ts i

/-- min lcp over the OPEN window (pb, i); MAXINT if the window is empty -/
def winOpen (L : List Nat) (pb i : Nat) : Int :=
  if i ≤ pb then MAXINT
  else ((List.range (i - pb - 1)).map (fun k => (L.getD (pb + 1 + k) 0 : Int))).foldl min MAXINT

/-! ### List helpers -/

theorem getD_append {α : Type} : ∀ (l₁ l₂ : List α) (i : Nat) (d : α),
    (l₁ ++ l₂).getD i d = if i < l₁.length then l₁.getD i d else l₂.getD (i - l₁.length) d := by
  intro l₁
  induction l₁ with
  | nil => intro l₂ i d; simp
  | cons a as ih =>
    intro l₂ i d
    match i with
    | 0 => simp
    | i'+1 =>
      have h1 : (a :: as ++ l₂).getD (i'+1) d = (as ++ l₂).getD i' d := List.getD_cons_succ
      rw [h1, ih l₂ i' d]
      by_cases h : i' < as.length
      · have h2 : i' + 1 < (a :: as).length := by simp; omega
        have h3 : (a :: as).getD (i'+1) d = as.getD i' d := List.getD_cons_succ
        rw [if_pos h2, if_pos h, h3]
      · have h2 : ¬ (i' + 1 < (a :: as).length) := by simp; omega
        rw [if_neg h2, if_neg h]
        have h4 : i' + 1 - (a :: as).length = i' - as.length := by
          have : (a :: as).length = as.length + 1 := rfl
          omega
        rw [h4]

theorem getD_map_range {β : Type} (f : Nat → β) (n i : Nat) (d : β) (h : i < n) :
    ((List.range n).map f).getD i d = f i := by
  induction n with
  | zero => omega
  | succ n ih =>
    have hr : List.range (n+1) = List.range n ++ [n] := List.range_succ
    rw [hr, List.map_append, getD_append]
    simp only [List.length_map, List.length_range, List.length_singleton, Nat.add_zero]
    by_cases hlt : i < n
    · rw [if_pos hlt]
      exact ih hlt
    · rw [if_neg hlt]
      have : i = n := by omega
      subst this
      simp

theorem getD_mem {α : Type} : ∀ (l : List α) (i : Nat) (d : α), i < l.length → l.getD i d ∈ l := by
  intro l
  induction l with
  | nil => intro i d h; simp at h
  | cons a as ih =>
    intro i
    match i with
    | 0 => intro d h; exact List.mem_cons_self
    | i'+1 =>
      intro d h
      simp only [List.length_cons] at h
      exact List.mem_cons_of_mem _ (ih i' d (by omega))

theorem foldl_min_le_all : ∀ (l : List Int) (a : Int),
    (∀ x ∈ l, l.foldl min a ≤ x) ∧ l.foldl min a ≤ a := by
  intro l
  induction l with
  | nil =>
    intro a
    refine ⟨fun x hx => absurd hx (by simp), ?_⟩
    show a ≤ a
    omega
  | cons b bs ih =>
    intro a
    obtain ⟨h1, h2⟩ := ih (min a b)
    refine ⟨?_, ?_⟩
    · intro x hx
      rcases List.mem_cons.mp hx with rfl | hm
      · have : (x :: bs).foldl min a ≤ min a x := h2
        omega
      · exact h1 x hm
    · have : (b :: bs).foldl min a ≤ min a b := h2
      omega

theorem foldl_min_lt : ∀ (l : List Int) (a q : Int),
    l.foldl min a < q → (∃ x ∈ l, x < q) ∨ a < q := by
  intro l
  induction l with
  | nil => intro a q h; exact Or.inr h
  | cons b bs ih =>
    intro a q h
    rcases ih (min a b) q h with ⟨x, hx, hxq⟩ | hq
    · exact Or.inl ⟨x, List.mem_cons_of_mem _ hx, hxq⟩
    · by_cases h1 : b < q
      · exact Or.inl ⟨b, List.mem_cons_self, h1⟩
      · exact Or.inr (by have : min a b < q := hq; omega)

theorem foldl_min_ge0 : ∀ (l : List Int) (a : Int), 0 ≤ a →
    (∀ x ∈ l, 0 ≤ x) → 0 ≤ l.foldl min a := by
  intro l
  induction l with
  | nil => intro a ha _; exact ha
  | cons b bs ih =>
    intro a ha hall
    have hb : 0 ≤ b := hall b List.mem_cons_self
    have hall' : ∀ x ∈ bs, 0 ≤ x := fun x hx => hall x (List.mem_cons_of_mem _ hx)
    exact ih (min a b) (by omega) hall'

theorem foldl_min_mem : ∀ (l : List Int) (a : Int),
    (∃ x ∈ l, l.foldl min a = x) ∨ l.foldl min a = a := by
  intro l
  induction l with
  | nil => intro a; exact Or.inr rfl
  | cons b bs ih =>
    intro a
    rw [List.foldl_cons]
    rcases ih (min a b) with ⟨x, hx, hxe⟩ | h
    · exact Or.inl ⟨x, List.mem_cons_of_mem _ hx, hxe⟩
    · by_cases hab : min a b = a
      · exact Or.inr (by omega)
      · exact Or.inl ⟨b, List.mem_cons_self, by omega⟩

/-! ### winOpen lemmas -/

theorem winOpen_ge {L : List Nat} {pb i q : Nat} (h : (q : Int) ≤ winOpen L pb i) :
    ∀ j, pb < j → j < i → (q : Int) ≤ L.getD j 0 := by
  intro j h1 h2
  unfold winOpen at h
  split at h
  · omega
  · have hk : j - pb - 1 < i - pb - 1 := by omega
    have helem : ((List.range (i - pb - 1)).map (fun k => (L.getD (pb + 1 + k) 0 : Int))).getD (j - pb - 1) (0 : Int)
        = (L.getD j 0 : Int) := by
      rw [getD_map_range _ _ _ _ hk]
      have hidx : pb + 1 + (j - pb - 1) = j := by omega
      rw [hidx]
    have hmem := getD_mem ((List.range (i - pb - 1)).map (fun k => (L.getD (pb + 1 + k) 0 : Int)))
      (j - pb - 1) (0 : Int) (by rw [List.length_map, List.length_range]; omega)
    rw [helem] at hmem
    have hfold := (foldl_min_le_all
      (List.map (fun k => (L.getD (pb + 1 + k) 0 : Int)) (List.range (i - pb - 1))) MAXINT).1 _ hmem
    omega

theorem winOpen_lt {L : List Nat} {pb i : Nat} {q : Int} (hqb : q ≤ MAXINT)
    (h : winOpen L pb i < q) : ∃ j, pb < j ∧ j < i ∧ (L.getD j 0 : Int) < q := by
  unfold winOpen at h
  split at h
  · omega
  · rcases foldl_min_lt _ _ _ h with ⟨x, hx, hxq⟩ | hmax
    · rw [List.mem_map] at hx
      obtain ⟨k, hk, hxk⟩ := hx
      rw [List.mem_range] at hk
      refine ⟨pb + 1 + k, by omega, by omega, ?_⟩
      omega
    · omega

theorem winOpen_ach {L : List Nat} {pb i : Nat} (h : winOpen L pb i < MAXINT) :
    ∃ j, pb < j ∧ j < i ∧ (L.getD j 0 : Int) = winOpen L pb i := by
  unfold winOpen at h ⊢
  split at h
  · omega
  · rename_i hnp
    have hpi : pb < i := by omega
    rcases foldl_min_mem
      ((List.range (i - pb - 1)).map (fun k => (L.getD (pb + 1 + k) 0 : Int))) MAXINT with ⟨x, hx, hxe⟩ | hmax
    · rw [List.mem_map] at hx
      obtain ⟨k, hk, hxk⟩ := hx
      rw [List.mem_range] at hk
      have h1 : pb < pb + 1 + k := by omega
      have h2 : pb + 1 + k < i := by omega
      refine ⟨pb + 1 + k, h1, h2, ?_⟩
      rw [if_neg hnp, hxe, hxk]
    · omega

theorem winOpen_geI {L : List Nat} {pb i : Nat} {q : Int} (h : q ≤ winOpen L pb i) :
    ∀ j, pb < j → j < i → q ≤ L.getD j 0 := by
  intro j h1 h2
  by_cases hq : (0 : Int) ≤ q
  · have hqle : (q.toNat : Int) ≤ winOpen L pb i := by omega
    have := winOpen_ge (L := L) (pb := pb) (i := i) (q := q.toNat) hqle j h1 h2
    have hqc : (q.toNat : Int) = q := by omega
    omega
  · have h0 : (0 : Int) ≤ L.getD j 0 := by omega
    omega

theorem winOpen_extend {L : List Nat} {pb i : Nat} (hpb : pb < i) :
    winOpen L pb (i+1) = min (winOpen L pb i) (L.getD i 0) := by
  have h1 : ¬ (i + 1 ≤ pb) := by omega
  have h2 : ¬ (i ≤ pb) := by omega
  unfold winOpen
  rw [if_neg h1, if_neg h2]
  have hr : List.range (i + 1 - pb - 1) = List.range (i - pb - 1) ++ [i - pb - 1] := by
    have hnn : i + 1 - pb - 1 = (i - pb - 1) + 1 := by omega
    rw [hnn, List.range_succ]
  have hmap : ((List.range (i + 1 - pb - 1)).map (fun k => (L.getD (pb + 1 + k) 0 : Int)))
      = ((List.range (i - pb - 1)).map (fun k => (L.getD (pb + 1 + k) 0 : Int))) ++ [(L.getD (pb + 1 + (i - pb - 1)) 0 : Int)] := by
    rw [hr, List.map_append]
    simp
  rw [hmap, List.foldl_append]
  have hlast : (L.getD (pb + 1 + (i - pb - 1)) 0 : Int) = (L.getD i 0 : Int) := by
    have hidx : pb + 1 + (i - pb - 1) = i := by omega
    rw [hidx]
  rw [hlast]
  simp [List.foldl]

theorem winOpen_fresh (L : List Nat) (i : Nat) : winOpen L i (i+1) = MAXINT := by
  have h : ¬ (i + 1 ≤ i) := by omega
  unfold winOpen
  rw [if_neg h]
  have : i + 1 - i - 1 = 0 := by omega
  rw [this]
  simp

theorem winOpen_nonneg (L : List Nat) (pb i : Nat) : 0 ≤ winOpen L pb i := by
  unfold winOpen
  split
  · have : (0 : Int) ≤ MAXINT := by native_decide
    omega
  · exact foldl_min_ge0 _ _ (by native_decide)
      (fun x hx => by
        rcases List.mem_map.mp hx with ⟨k, _, hk⟩
        rw [← hk]
        omega)

/-! ### prevBnd lemmas -/

theorem prevBnd_bnd (ts : List Triple) (i : Nat) (hb : isBnd ts i = true) :
    prevBnd ts (i+1) = i := by simp [prevBnd, hb]

theorem prevBnd_nonbnd (ts : List Triple) (i : Nat) (hb : isBnd ts i = false) :
    prevBnd ts (i+1) = prevBnd ts i := by simp [prevBnd, hb]

theorem prevBnd_lt (ts : List Triple) : ∀ i, prevBnd ts i < i ∨ i = 0 := by
  intro i
  induction i with
  | zero => exact Or.inr rfl
  | succ i ih =>
    by_cases hb : isBnd ts i = true
    · simp [prevBnd, hb]
    · have hbf : isBnd ts i = false := by
        have : ¬ (isBnd ts i = true) := hb
        simp [Bool.not_eq_true] at this
        exact this
      rcases ih with h | h
      · exact Or.inl (by
          have heq : prevBnd ts (i+1) = prevBnd ts i := prevBnd_nonbnd ts i hbf
          omega)
      · exact Or.inl (by
          have heq : prevBnd ts (i+1) = prevBnd ts i := prevBnd_nonbnd ts i hbf
          rw [h] at heq ⊢
          simp [prevBnd] at heq ⊢)

theorem prevBnd_ge_of_bnd (ts : List Triple) : ∀ i b, isBnd ts b = true → b < i → b ≤ prevBnd ts i := by
  intro i
  induction i with
  | zero => intro b _ hb; omega
  | succ i ih =>
    intro b hbt hbl
    by_cases h : isBnd ts i = true
    · rw [prevBnd_bnd ts i h]
      by_cases h1 : b < i
      · exact Nat.le_of_lt h1
      · have hbeq : b = i := by omega
        omega
    · have hbf : isBnd ts i = false := by
        have : ¬ (isBnd ts i = true) := h
        simp [Bool.not_eq_true] at this
        exact this
      have hbl' : b < i := by
        have hne : b ≠ i := by
          intro hbe
          rw [hbe] at hbt
          rw [hbt] at hbf
          exact absurd hbf (by simp)
        omega
      rw [prevBnd_nonbnd ts i hbf]
      exact ih b hbt hbl'

/-! ### PSV/NSV characterizations -/

def psvFilter (L : List Nat) (i : Nat) : List Nat :=
  (List.range i).filter (fun j => decide (L.getD j 0 < L.getD i 0))

theorem range_pairwise (n : Nat) : (List.range n).Pairwise (fun a b => a < b) := by
  induction n with
  | zero => simp
  | succ n ih =>
    rw [List.range_succ, List.pairwise_append]
    exact ⟨ih, by simp, fun x hx y hy => by
      rw [List.mem_range] at hx
      simp only [List.mem_singleton] at hy
      rcases hy with rfl
      omega⟩

theorem psvFilter_pairwise (L : List Nat) (i : Nat) : (psvFilter L i).Pairwise (fun a b => a < b) :=
  (range_pairwise i).filter _

theorem pairwise_lt_last : ∀ {l : List Nat}, l.Pairwise (fun a b => a < b) → ∀ x : Nat,
    x ∈ l → ∃ y, l.getLast? = some y ∧ x ≤ y := by
  intro l
  induction l with
  | nil => intro h x hx; cases hx
  | cons a as ih =>
    intro h x hx
    cases h with
    | cons hhead hp =>
      rcases List.mem_cons.mp hx with hxa | hxa
      · rw [hxa]
        cases as with
        | nil => exact ⟨a, by rw [List.getLast?_cons]; simp, Nat.le_refl _⟩
        | cons b bs =>
          obtain ⟨y, hy, hby⟩ := ih hp b List.mem_cons_self
          refine ⟨y, ?_, ?_⟩
          · rw [List.getLast?_cons_cons]; exact hy
          · have hab : a < b := hhead b List.mem_cons_self
            omega
      · obtain ⟨y, hy, hby⟩ := ih hp x hxa
        cases as with
        | nil => cases hxa
        | cons b bs =>
          refine ⟨y, ?_, hby⟩
          rw [List.getLast?_cons_cons]; exact hy

theorem prevSmaller_mem (L : List Nat) (i : Nat) (h : prevSmaller L i ≠ -1) :
    ∃ j : Nat, prevSmaller L i = (j : Int) ∧ j < i ∧ L.getD j 0 < L.getD i 0 := by
  unfold prevSmaller at h ⊢
  match hget : ((List.range i).filter (fun j => decide (L.getD j 0 < L.getD i 0))).getLast? with
  | none =>
    simp only [hget] at h
    exact absurd rfl h
  | some j =>
    have hmem : j ∈ (List.range i).filter (fun j => decide (L.getD j 0 < L.getD i 0)) :=
      List.mem_of_getLast? hget
    rw [List.mem_filter, List.mem_range] at hmem
    exact ⟨j, rfl, hmem.1, of_decide_eq_true hmem.2⟩

theorem prevSmaller_ge (L : List Nat) (i j : Nat) (hj : j < i)
    (hl : L.getD j 0 < L.getD i 0) : (j : Int) ≤ prevSmaller L i := by
  have hmem : j ∈ psvFilter L i := by
    rw [psvFilter, List.mem_filter, List.mem_range]
    exact ⟨hj, decide_eq_true hl⟩
  obtain ⟨y, hy, hby⟩ := pairwise_lt_last (psvFilter_pairwise L i) j hmem
  simp only [prevSmaller]
  simp only [psvFilter] at hy
  rw [hy]
  simp only
  omega

theorem prevSmaller_none (L : List Nat) (i : Nat) (h : prevSmaller L i = -1) :
    ∀ j, j < i → L.getD i 0 ≤ L.getD j 0 := by
  intro j hj
  by_cases hle : L.getD i 0 ≤ L.getD j 0
  · exact hle
  · have hl : L.getD j 0 < L.getD i 0 := by omega
    have hmem : j ∈ psvFilter L i := by
      rw [psvFilter, List.mem_filter, List.mem_range]
      exact ⟨hj, decide_eq_true hl⟩
    obtain ⟨y, hy, hby⟩ := pairwise_lt_last (psvFilter_pairwise L i) j hmem
    simp only [psvFilter] at hy
    simp only [prevSmaller] at h
    rw [hy] at h
    simp only at h
    omega

theorem prevSmaller_eq_neg1 (L : List Nat) (i : Nat) (h : ∀ j, j < i → L.getD i 0 ≤ L.getD j 0) :
    prevSmaller L i = -1 := by
  by_cases hcon : prevSmaller L i = -1
  · exact hcon
  · obtain ⟨j, hj1, hj2, hj3⟩ := prevSmaller_mem L i hcon
    have := h j hj2
    omega

def nsvFilter (L : List Nat) (i : Nat) : List Nat :=
  (List.range L.length).filter (fun j => decide (i < j ∧ L.getD j 0 < L.getD i 0))

theorem nsvFilter_pairwise (L : List Nat) (i : Nat) : (nsvFilter L i).Pairwise (fun a b => a < b) :=
  (range_pairwise L.length).filter _

theorem pairwise_lt_head : ∀ {l : List Nat}, l.Pairwise (fun a b => a < b) → ∀ x : Nat,
    x ∈ l → ∃ y, l.head? = some y ∧ y ≤ x := by
  intro l
  induction l with
  | nil => intro h x hx; cases hx
  | cons a as ih =>
    intro h x hx
    cases h with
    | cons hhead hp =>
      rcases List.mem_cons.mp hx with hxa | hxa
      · rw [hxa]
        exact ⟨a, List.head?_cons, Nat.le_refl _⟩
      · obtain ⟨y, hy, hby⟩ := ih hp x hxa
        have hymem : y ∈ as := List.mem_of_head? hy
        have hay : a < y := hhead y hymem
        exact ⟨a, List.head?_cons, by omega⟩

theorem nextSmaller_mem (L : List Nat) (i : Nat) (h : nextSmaller L i ≠ L.length + 1) :
    ∃ k : Nat, nextSmaller L i = k ∧ i < k ∧ L.getD k 0 < L.getD i 0 := by
  unfold nextSmaller at h ⊢
  match hget : ((List.range L.length).filter
      (fun j => decide (i < j ∧ L.getD j 0 < L.getD i 0))).head? with
  | none =>
    simp only [hget] at h
    exact absurd rfl h
  | some k =>
    have hmem : k ∈ (List.range L.length).filter
        (fun j => decide (i < j ∧ L.getD j 0 < L.getD i 0)) :=
      List.mem_of_head? hget
    rw [List.mem_filter, List.mem_range] at hmem
    obtain ⟨hk1, hk2⟩ := of_decide_eq_true hmem.2
    exact ⟨k, rfl, hk1, hk2⟩

theorem nextSmaller_le (L : List Nat) (i k : Nat) (hik : i < k) (hkl : k < L.length)
    (hl : L.getD k 0 < L.getD i 0) : nextSmaller L i ≤ k := by
  have hmem : k ∈ nsvFilter L i := by
    rw [nsvFilter, List.mem_filter, List.mem_range]
    exact ⟨by omega, decide_eq_true ⟨hik, hl⟩⟩
  obtain ⟨y, hy, hby⟩ := pairwise_lt_head (nsvFilter_pairwise L i) k hmem
  simp only [nextSmaller]
  simp only [nsvFilter] at hy
  rw [hy]
  simp only
  omega

theorem nextSmaller_none (L : List Nat) (i : Nat) (h : nextSmaller L i = L.length + 1) :
    ∀ k, i < k → k < L.length → L.getD i 0 ≤ L.getD k 0 := by
  intro k hk hkl
  by_cases hle : L.getD i 0 ≤ L.getD k 0
  · exact hle
  · have hl : L.getD k 0 < L.getD i 0 := by omega
    have hmem : k ∈ nsvFilter L i := by
      rw [nsvFilter, List.mem_filter, List.mem_range]
      exact ⟨by omega, decide_eq_true ⟨hk, hl⟩⟩
    obtain ⟨y, hy, hby⟩ := pairwise_lt_head (nsvFilter_pairwise L i) k hmem
    have hymem : y ∈ nsvFilter L i := List.mem_of_head? hy
    simp only [nsvFilter] at hy
    simp only [nextSmaller] at h
    rw [hy] at h
    simp only at h
    simp only [nsvFilter, List.mem_filter, List.mem_range] at hymem
    omega

/-! ### psvList/nsvList indexing -/

theorem psvList_getD (L : List Nat) (i : Nat) (h : i < L.length) :
    (psvList L).getD i (-1) = prevSmaller L i := by
  have : (psvList L).getD i (-1) = ((List.range L.length).map (fun j => prevSmaller L j)).getD i (-1) := rfl
  rw [this, getD_map_range _ _ _ _ h]

theorem nsvList_getD (L : List Nat) (i : Nat) (h : i < L.length) :
    (nsvList L).getD i (L.length + 1) = nextSmaller L i := by
  have : (nsvList L).getD i (L.length + 1) = ((List.range L.length).map (fun j => nextSmaller L j)).getD i (L.length+1) := rfl
  rw [this, getD_map_range _ _ _ _ h]

/-! ### per-char machine lemmas -/

theorem foldl_evalStepGo_len (l : Int) : ∀ (n : Nat) (out : List Nat) (R : List Cand),
    ((List.range n).foldl (evalStepGo l) (out, R)).2.length = R.length := by
  intro n
  induction n with
  | zero => intro out R; simp only [List.range_zero, List.foldl_nil, Prod.snd]
  | succ n ih =>
    intro out R
    rw [List.range_succ, List.foldl_append]
    simp only [List.foldl_cons, List.foldl_nil]
    rcases hf : ((List.range n).foldl (evalStepGo l) (out, R)) with ⟨out₁, R₁⟩
    simp only [evalStepGo]
    have hlen : R₁.length = R.length := by
      have := ih out R
      rw [hf] at this
      exact this
    by_cases hlt : l < (getR R₁ (n+1)).len
    · rw [if_pos hlt]
      simp only [Prod.snd]
      rw [List.length_set]
      exact hlen
    · rw [if_neg hlt]
      exact hlen

theorem foldl_evalStepGo_getR_gt (l : Int) : ∀ (n : Nat) (out : List Nat) (R : List Cand) (c : Nat),
    n < c → getR ((List.range n).foldl (evalStepGo l) (out, R)).2 c = getR R c := by
  intro n
  induction n with
  | zero =>
    intro out R c hgt
    simp only [List.range_zero, List.foldl_nil]
  | succ n ih =>
    intro out R c hgt
    rw [List.range_succ, List.foldl_append]
    simp only [List.foldl_cons, List.foldl_nil]
    rcases hf : ((List.range n).foldl (evalStepGo l) (out, R)) with ⟨out₁, R₁⟩
    have hne : c ≠ n + 1 := by omega
    simp only [evalStepGo]
    have hfold : getR R₁ c = getR R c := by
      have := ih out R c (by omega)
      rw [hf] at this
      exact this
    by_cases hlt : l < (getR R₁ (n+1)).len
    · rw [if_pos hlt]
      simp only [Prod.snd]
      rw [getR_set_ne R₁ (n+1) c ⟨l, 0, false⟩ (Ne.symm hne)]
      exact hfold
    · rw [if_neg hlt]
      exact hfold

theorem foldl_evalStepGo_getR (l : Int) : ∀ (n : Nat) (out : List Nat) (R : List Cand) (c : Nat),
    1 ≤ c → c ≤ n → n + 1 ≤ R.length →
    getR ((List.range n).foldl (evalStepGo l) (out, R)).2 c =
      (if l < (getR R c).len then ⟨l, 0, false⟩ else getR R c) := by
  intro n
  induction n with
  | zero => intro out R c hc1 hcn _; omega
  | succ n ih =>
    intro out R c hc1 hcn hlen
    rw [List.range_succ, List.foldl_append]
    simp only [List.foldl_cons, List.foldl_nil]
    rcases hf : ((List.range n).foldl (evalStepGo l) (out, R)) with ⟨out₁, R₁⟩
    simp only [evalStepGo]
    have hfoldc : ∀ (hc : c ≤ n), getR R₁ c
        = (if l < (getR R c).len then ⟨l, 0, false⟩ else getR R c) := by
      intro hc
      have := ih out R c hc1 hc (by omega)
      rw [hf] at this
      exact this
    have hgt1 : getR R₁ (n+1) = getR R (n+1) := by
      have := foldl_evalStepGo_getR_gt l n out R (n+1) (Nat.lt_succ_self n)
      rw [hf] at this
      exact this
    have hlen1 : R₁.length = R.length := by
      have := foldl_evalStepGo_len l n out R
      rw [hf] at this
      exact this
    by_cases hcl : c ≤ n
    · have hne : c ≠ n + 1 := by omega
      by_cases hlt : l < (getR R₁ (n+1)).len
      · rw [if_pos hlt]
        simp only [Prod.snd]
        rw [getR_set_ne R₁ (n+1) c ⟨l,0,false⟩ (Ne.symm hne)]
        exact hfoldc hcl
      · rw [if_neg hlt]
        exact hfoldc hcl
    · have hceq : c = n + 1 := by omega
      subst hceq
      rw [hgt1]
      by_cases hlt2 : l < (getR R (n+1)).len
      · rw [if_pos hlt2]
        simp only [Prod.snd]
        rw [getR_set_eq]
        rw [if_pos (by omega)]
        simp only [hlt2, if_true]
      · rw [if_neg hlt2]
        simp only [Prod.snd]
        rw [hgt1]
        rw [if_neg hlt2]

theorem evalStep_getR (l : Int) (R : List Cand) (out : List Nat) (c : Nat)
    (hc1 : 1 ≤ c) (hc2 : c < SIGMA) (hR : SIGMA ≤ R.length) :
    getR (evalStep l R out).2 c = if l < (getR R c).len then ⟨l, 0, false⟩ else getR R c := by
  rw [evalStep_eq]
  exact foldl_evalStepGo_getR l (SIGMA-1) out R c hc1 (by omega) (by omega)

theorem foldl_evalStepGo_mem (l : Int) : ∀ (n : Nat) (out : List Nat) (R : List Cand) (x : Nat),
    x ∈ ((List.range n).foldl (evalStepGo l) (out, R)).1 ↔
      x ∈ out ∨ ∃ c, 1 ≤ c ∧ c ≤ n ∧ l < (getR R c).len ∧ (getR R c).active ∧ x = (getR R c).pos := by
  intro n
  induction n with
  | zero =>
    intro out R x
    simp only [List.range_zero, List.foldl_nil, Prod.fst]
    constructor
    · intro h; exact Or.inl h
    · intro h
      rcases h with h | ⟨c, hc1, hcn, _⟩
      · exact h
      · omega
  | succ n ih =>
    intro out R x
    rw [List.range_succ, List.foldl_append]
    simp only [List.foldl_cons, List.foldl_nil]
    rcases hf : ((List.range n).foldl (evalStepGo l) (out, R)) with ⟨out₁, R₁⟩
    simp only [evalStepGo]
    have ihx := ih out R x
    rw [hf] at ihx
    simp only [Prod.fst] at ihx
    have hgt1 : getR R₁ (n+1) = getR R (n+1) := by
      have := foldl_evalStepGo_getR_gt l n out R (n+1) (Nat.lt_succ_self n)
      rw [hf] at this
      exact this
    by_cases hlt : l < (getR R₁ (n+1)).len
    · rw [if_pos hlt]
      by_cases hact : (getR R₁ (n+1)).active
      · rw [if_pos hact]
        rw [List.mem_append]
        constructor
        · intro h
          rcases h with h | h
          · rcases ihx.mp h with h | ⟨c, hc1, hcn, hcl, hcact, hcx⟩
            · exact Or.inl h
            · exact Or.inr ⟨c, hc1, by omega, hcl, hcact, hcx⟩
          · rw [List.mem_singleton] at h
            rw [hgt1] at h hlt hact
            refine Or.inr ⟨n+1, by omega, by omega, hlt, hact, h⟩
        · intro h
          rcases h with h | ⟨c, hc1, hcn, hcl, hcact, hcx⟩
          · exact Or.inl (ihx.mpr (Or.inl h))
          · by_cases hceq : c = n + 1
            · subst hceq
              refine Or.inr ?_
              rw [List.mem_singleton]
              rw [← hgt1] at hcx
              exact hcx
            · refine Or.inl (ihx.mpr (Or.inr ⟨c, hc1, by omega, hcl, hcact, hcx⟩))
      · rw [if_neg hact]
        show x ∈ out₁ ↔ _
        constructor
        · intro h
          rcases ihx.mp h with h | ⟨c, hc1, hcn, hcl, hcact, hcx⟩
          · exact Or.inl h
          · exact Or.inr ⟨c, hc1, by omega, hcl, hcact, hcx⟩
        · intro h
          rcases h with h | ⟨c, hc1, hcn, hcl, hcact, hcx⟩
          · exact ihx.mpr (Or.inl h)
          · by_cases hceq : c = n + 1
            · subst hceq
              rw [hgt1] at hact
              exact absurd hcact hact
            · exact ihx.mpr (Or.inr ⟨c, hc1, by omega, hcl, hcact, hcx⟩)
    · rw [if_neg hlt]
      show x ∈ out₁ ↔ _
      constructor
      · intro h
        rcases ihx.mp h with h | ⟨c, hc1, hcn, hcl, hcact, hcx⟩
        · exact Or.inl h
        · exact Or.inr ⟨c, hc1, by omega, hcl, hcact, hcx⟩
      · intro h
        rcases h with h | ⟨c, hc1, hcn, hcl, hcact, hcx⟩
        · exact ihx.mpr (Or.inl h)
        · by_cases hceq : c = n + 1
          · subst hceq
            rw [hgt1] at hlt
            exact absurd hcl hlt
          · exact ihx.mpr (Or.inr ⟨c, hc1, by omega, hcl, hcact, hcx⟩)

theorem evalStep_mem (l : Int) (R : List Cand) (out : List Nat) (x : Nat) :
    x ∈ (evalStep l R out).1 ↔
      x ∈ out ∨ ∃ c, 1 ≤ c ∧ c < SIGMA ∧ l < (getR R c).len ∧ (getR R c).active ∧ x = (getR R c).pos := by
  rw [evalStep_eq]
  rw [foldl_evalStepGo_mem l (SIGMA-1) out R x]
  constructor
  · intro h
    rcases h with h | ⟨c, hc1, hcn, hcl, hcact, hcx⟩
    · exact Or.inl h
    · exact Or.inr ⟨c, hc1, by omega, hcl, hcact, hcx⟩
  · intro h
    rcases h with h | ⟨c, hc1, hc2, hcl, hcact, hcx⟩
    · exact Or.inl h
    · exact Or.inr ⟨c, hc1, by omega, hcl, hcact, hcx⟩

/-! ### fmStep per-char lemmas -/

theorem getFM_set_eq (R : List (Option CandFM)) (c : Nat) (a : CandFM) :
    getFM (R.set c a) c = if c < R.length then some a else getFM R c := by
  simp only [getFM, List.getD, List.getElem?_set]
  by_cases h : c < R.length
  · simp [h]
  · have h2 : ¬ c < (R.set c a).length := by rw [List.length_set]; exact h
    simp [h]

theorem getFM_set_ne (R : List (Option CandFM)) (c d : Nat) (a : CandFM) (h : c ≠ d) :
    getFM (R.set c a) d = getFM R d := by
  simp only [getFM, List.getD, List.getElem?_set]
  simp [h]

/-! ### The coupling invariant -/

/-- per-char three-case coupling at index i (before processing triple i).
  (c) never-involved: FM none, scan slot untouched (len = -1).
  (a) coupled-active: both machines hold the same pending position; scan len = L[s];
      no LCP below L[s] in any completed window since the arming boundary s.
  (b) delayed: scan has already emitted the position (it is in the scan output);
      FM still holds it; the arming-time NSV is already strictly below i, so any
      future FM overwrite is guaranteed to emit it. -/
def Coupled (ts : List Triple) (i : Nat) (R : List Cand) (R' : List (Option CandFM)) (c : Nat) : Prop :=
  match getFM R' c with
  | none => getR R c = ⟨-1, 0, false⟩
  | some q =>
      q.active = true ∧
      if (getR R c).active then
        ∃ s : Nat, isBnd ts s = true ∧ q.saPos = (s : Int) ∧ s < i
          ∧ (getR R c).len = (Ls ts).getD s 0
          ∧ (getR R c).pos = q.textPos
          ∧ q.nsv = nextSmaller (Ls ts) s
          ∧ ∀ j, s < j → j ≤ prevBnd ts i → (Ls ts).getD s 0 ≤ (Ls ts).getD j 0
      else
        ∃ s : Nat, isBnd ts s = true ∧ q.saPos = (s : Int) ∧ s < i
          ∧ q.nsv = nextSmaller (Ls ts) s
          ∧ nextSmaller (Ls ts) s < i
          ∧ (getR R c).len < (Ls ts).getD s 0
          ∧ (∃ j, s < j ∧ j < i ∧ ((Ls ts).getD j 0 : Int) ≤ (getR R c).len)
          ∧ (∀ j, s < j → j ≤ prevBnd ts i → (getR R c).len ≤ ((Ls ts).getD j 0 : Int))

/-- x is the pending (delayed) textPos of some (b)-character -/
def Delayed (R : List Cand) (R' : List (Option CandFM)) (x : Nat) : Prop :=
  ∃ c, 1 ≤ c ∧ c < SIGMA ∧ (getR R c).active = false ∧
    ∃ s p n, getFM R' c = some ⟨s, p, n, true⟩ ∧ p = x

/-- the joint invariant at index i -/
def JointInv (ts : List Triple) (i : Nat) (R : List Cand) (R' : List (Option CandFM))
    (out : List Nat) (S : List Nat) : Prop :=
  (∀ c, 1 ≤ c → c < SIGMA → Coupled ts i R R' c)
  ∧ (∀ x, x ∈ out ↔ x ∈ S ∨ Delayed R R' x)

/-! ### not-involved per-char step lemma -/

/-- if the boundary-window min w = min m0 L[i] drops below L[v] (v ≤ prevBnd ts i,
then there is a witness index in (v, i] with LCP at or below w. -/
theorem winOpen_witness (ts : List Triple) (i : Nat) (w : Int)
    (hw : w = min (winOpen (Ls ts) (prevBnd ts i) i) ((Ls ts).getD i 0)) :
    ∀ v : Nat, v ≤ prevBnd ts i → prevBnd ts i < i → (w < ((Ls ts).getD v 0 : Int)) →
      ((Ls ts).getD v 0 : Int) ≤ MAXINT →
      ∃ j, v < j ∧ j ≤ i ∧ ((Ls ts).getD j 0 : Int) ≤ w := by
  intro v hv hpb hlt hvmax
  have hwsplit : w = winOpen (Ls ts) (prevBnd ts i) i ∨ w = ((Ls ts).getD i 0 : Int) := by
    rw [hw]; omega
  rcases hwsplit with hwm | hwi
  · have hach : ∃ j, prevBnd ts i < j ∧ j < i ∧ ((Ls ts).getD j 0 : Int) = winOpen (Ls ts) (prevBnd ts i) i := by
      apply winOpen_ach
      rw [← hwm]
      omega
    obtain ⟨j, hj1, hj2, hj3⟩ := hach
    refine ⟨j, by omega, by omega, by omega⟩
  · refine ⟨i, by omega, by omega, by omega⟩

/-- STRICT window witness: when the boundary window-min w drops strictly below
L[i] (so w = m0 < vi; the vi-path w = vi is excluded), the achiever lies
strictly inside the open window: prevBnd < j < i with L[j] <= w. -/
theorem winOpen_witness_lt (ts : List Triple) (i : Nat) (w : Int)
    (hw : w = min (winOpen (Ls ts) (prevBnd ts i) i) ((Ls ts).getD i 0))
    (hmax : ((Ls ts).getD i 0 : Int) ≤ MAXINT)
    (hlt : w < ((Ls ts).getD i 0 : Int)) :
    ∃ j, prevBnd ts i < j ∧ j < i ∧ ((Ls ts).getD j 0 : Int) ≤ w := by
  have hwsplit : w = winOpen (Ls ts) (prevBnd ts i) i ∨ w = ((Ls ts).getD i 0 : Int) := by
    rw [hw]; omega
  rcases hwsplit with hwm | hwi
  · have hach : ∃ j, prevBnd ts i < j ∧ j < i ∧ ((Ls ts).getD j 0 : Int) = winOpen (Ls ts) (prevBnd ts i) i := by
      apply winOpen_ach
      rw [← hwm]
      have hmax : (0 : Int) ≤ MAXINT := by native_decide
      omega
    obtain ⟨j, hj1, hj2, hj3⟩ := hach
    refine ⟨j, hj1, hj2, ?_⟩
    omega
  · omega

/-- A character not involved in boundary i: only evalStep touches it. -/
theorem coupled_step_notinv (ts : List Triple) (i : Nat) (hi1 : 1 ≤ i) (hi : i < ts.length)
    (hb : isBnd ts i = true)
    (hsat : ∀ j, j < ts.length → (Ls ts).getD j 0 ≤ MAXINT.toNat)
    (R : List Cand) (R' : List (Option CandFM)) (c : Nat)
    (hc1 : 1 ≤ c) (hc2 : c < SIGMA)
    (hC : Coupled ts i R R' c)
    (R₃ : List Cand) (R₂' : List (Option CandFM))
    (w : Int)
    (hw : w = min (winOpen (Ls ts) (prevBnd ts i) i) ((Ls ts).getD i 0))
    (hR₃ : getR R₃ c = (if w < (getR R c).len then ⟨w, 0, false⟩ else getR R c))
    (hR₂' : getFM R₂' c = getFM R' c) :
    Coupled ts (i+1) R₃ R₂' c
    ∧ (w < (getR R c).len → (getR R c).active = true →
        ∃ s p n, getFM R₂' c = some ⟨(s : Int), p, n, true⟩ ∧ p = (getR R c).pos
          ∧ (getR R₃ c).active = false) := by
  have hpb : prevBnd ts (i+1) = i := prevBnd_bnd ts i hb
  have hpbi : prevBnd ts i < i := by
    rcases prevBnd_lt ts i with h | h
    · exact h
    · omega
  have hm : winOpen (Ls ts) (prevBnd ts i) i ≤ MAXINT := by
    unfold winOpen
    split
    · omega
    · exact foldl_min_le_all _ _ |>.2
  have hwle1 : w ≤ winOpen (Ls ts) (prevBnd ts i) i := by rw [hw]; omega
  have hwle2 : w ≤ ((Ls ts).getD i 0 : Int) := by rw [hw]; omega
  have hwE : (0 : Int) ≤ w := by
    have h1 := winOpen_nonneg (Ls ts) (prevBnd ts i) i
    rw [hw]
    omega
  have hwsplit : w = winOpen (Ls ts) (prevBnd ts i) i ∨ w = ((Ls ts).getD i 0 : Int) := by
    rw [hw]; omega
  -- main case analysis
  cases hq : getFM R' c with
  | none =>
    simp only [Coupled, hq] at hC
    have hR : getR R c = ⟨-1, 0, false⟩ := hC
    constructor
    · show Coupled ts (i+1) R₃ R₂' c
      simp only [Coupled, hR₂', hq]
      rw [hR₃, hR]
      have : ¬ (w < (-1 : Int)) := by omega
      simp only [this, if_false]
    · intro hlt hact
      simp only [hR] at hlt
      omega
  | some q =>
    simp only [Coupled, hq] at hC
    obtain ⟨hqact, hbody⟩ := hC
    obtain ⟨qsa, qtx, qns, qac⟩ := q
    have hLlen : (Ls ts).length = ts.length := by rw [Ls, List.length_map]
    by_cases hqa : (getR R c).active = true
    · -- case (a): coupled active
      rw [if_pos hqa] at hbody
      obtain ⟨s, hbnd, hsa, hsi, hlen, hpos, hnsv, hpast⟩ := hbody
      have hsL : s < ts.length := by omega
      have hLs : ((Ls ts).getD s 0 : Int) ≤ MAXINT := by
        have := hsat s hsL
        omega
      have hpbge : s ≤ prevBnd ts i := prevBnd_ge_of_bnd ts i s hbnd (by omega)
      by_cases hwlt : w < ((Ls ts).getD s 0 : Int)
      · -- drop: scan emits pos, post-(b)
        have hemit : getR R₃ c = ⟨w, 0, false⟩ := by
          rw [hR₃, hlen]
          rw [if_pos hwlt]
        have hulen : (getR R₃ c).len = w := by rw [hemit]
        have hact3 : (getR R₃ c).active = false := by rw [hemit]
        obtain ⟨jw, hjw1, hjw2, hjw3⟩ := winOpen_witness ts i w hw s hpbge hpbi (by exact hwlt) hLs
        have hnsvi : nextSmaller (Ls ts) s ≤ i := by
          have hjwL : jw < (Ls ts).length := by omega
          have := nextSmaller_le (Ls ts) s jw (by omega) hjwL (by omega)
          omega
        constructor
        · show Coupled ts (i+1) R₃ R₂' c
          simp only [Coupled, hR₂', hq]
          refine ⟨hqact, ?_⟩
          rw [if_neg (by simp [hact3])]
          trace_state
          refine ⟨s, hbnd, hsa, by omega, hnsv, by omega, ?_, ⟨jw, by omega, by omega, by omega⟩, ?_⟩
          · omega
          · intro j hj1 hj2
            rw [hpb] at hj2
            by_cases hjp : j ≤ prevBnd ts i
            · have := hpast j hj1 hjp
              omega
            · by_cases hji : j = i
              · rw [hji]
                omega
              · have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i) (q := w)
                  (by omega) j (by omega) (by omega)
                omega
        · intro hlt hact
          have hsa' : qsa = (s : Int) := hsa
          have hqact' : qac = true := hqact
          rw [hsa', hqact'] at hq
          refine ⟨s, qtx, qns, ?_, ?_, ?_⟩
          · rw [hR₂']
            exact hq
          · rw [hpos]
          · rw [hemit]
      · -- no drop: stays (a)
        have hnoemit : getR R₃ c = getR R c := by
          rw [hR₃, hlen]
          rw [if_neg (by rw [← hlen]; omega)]
        have hact3 : (getR R₃ c).active = true := by rw [hnoemit]; simp [hqa]
        constructor
        · show Coupled ts (i+1) R₃ R₂' c
          simp only [Coupled, hR₂', hq]
          refine ⟨hqact, ?_⟩
          rw [if_pos hact3]
          refine ⟨s, hbnd, hsa, by omega, ?_, ?_, hnsv, ?_⟩
          · rw [hnoemit, hlen]
          · rw [hnoemit]
            exact hpos
          · intro j hj1 hj2
            rw [hpb] at hj2
            by_cases hjp : j ≤ prevBnd ts i
            · exact hpast j hj1 hjp
            · by_cases hji : j = i
              · rw [hji]
                have hwge : ((Ls ts).getD s 0 : Int) ≤ w := by omega
                have hwle2' : w ≤ ((Ls ts).getD i 0 : Int) := hwle2
                omega
              · have hwge : ((Ls ts).getD s 0 : Int) ≤ w := by omega
                have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i)
                  (q := ((Ls ts).getD s 0 : Int)) (by
                    have := hwle1
                    omega) j (by omega) (by omega)
                omega
        · intro hlt hact
          have : ¬ (w < ((Ls ts).getD s 0 : Int)) := by omega
          rw [hlen] at hlt
          omega
    · -- case (b): delayed
      rw [if_neg hqa] at hbody
      obtain ⟨s, hbnd, hsa, hsi, hnsv, hnsvi, hult, hach, hpast⟩ := hbody
      have hsL : s < ts.length := by omega
      have hLs : ((Ls ts).getD s 0 : Int) ≤ MAXINT := by
        have := hsat s hsL
        omega
      have hpbge : s ≤ prevBnd ts i := prevBnd_ge_of_bnd ts i s hbnd (by omega)
      by_cases hwlt : w < (getR R c).len
      · -- decay: u' := w
        have hemit : getR R₃ c = ⟨w, 0, false⟩ := by
          rw [hR₃]
          rw [if_pos hwlt]
        have hulen : (getR R₃ c).len = w := by rw [hemit]
        have hact3 : (getR R₃ c).active = false := by rw [hemit]
        obtain ⟨jw, hjw1, hjw2, hjw3⟩ := winOpen_witness ts i w hw s hpbge hpbi
          (by omega) hLs
        constructor
        · show Coupled ts (i+1) R₃ R₂' c
          simp only [Coupled, hR₂', hq]
          refine ⟨hqact, ?_⟩
          rw [if_neg (by simp [hact3])]
          trace_state
          refine ⟨s, hbnd, hsa, by omega, hnsv, by omega, ?_, ⟨jw, by omega, by omega, by omega⟩, ?_⟩
          · omega
          · intro j hj1 hj2
            rw [hpb] at hj2
            by_cases hjp : j ≤ prevBnd ts i
            · have := hpast j hj1 hjp
              omega
            · by_cases hji : j = i
              · rw [hji]
                omega
              · have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i) (q := w)
                  (by omega) j (by omega) (by omega)
                omega
        · intro hlt hact
          exact absurd hact hqa
      · -- no decay: unchanged (b), past extends
        have hnoemit : getR R₃ c = getR R c := by
          rw [hR₃]
          rw [if_neg hwlt]
        have hact3 : (getR R₃ c).active = false := by simp [hqa, hnoemit]
        have hulen3 : (getR R₃ c).len = (getR R c).len := by rw [hnoemit]
        constructor
        · show Coupled ts (i+1) R₃ R₂' c
          simp only [Coupled, hR₂', hq]
          refine ⟨hqact, ?_⟩
          rw [if_neg (by simp [hact3])]
          obtain ⟨s', hs1, hs2, hs3⟩ := hach
          refine ⟨s, hbnd, hsa, by omega, hnsv, by omega,
            (by rw [hnoemit]; exact hult), (by rw [hnoemit]; exact ⟨s', hs1, by omega, hs3⟩), ?_⟩
          intro j hj1 hj2
          rw [hpb] at hj2
          by_cases hjp : j ≤ prevBnd ts i
          · have hpp := hpast j hj1 hjp
            omega
          · by_cases hji : j = i
            · rw [hji]
              have hwge : w ≥ (getR R c).len := by omega
              omega
            · have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i)
                (q := (getR R c).len) (by
                  have := hwle1
                  omega) j (by omega) (by omega)
              omega
        · intro hlt hact
          exact absurd hact hqa

/-! ### involved per-char step lemma -/

/-- A character involved in boundary i (as ending or starting side).  The scan
composite is evalStep + upd (arm with L[i] at pos); the FM composite is one
fmStep (set/overwrite-hold, maybe emitting the old textPos). -/
theorem coupled_step_inv (ts : List Triple) (i : Nat) (hi1 : 1 ≤ i) (hi : i < ts.length)
    (hb : isBnd ts i = true)
    (hsat : ∀ j, j < ts.length → (Ls ts).getD j 0 ≤ MAXINT.toNat)
    (R : List Cand) (R' : List (Option CandFM)) (c : Nat)
    (hc1 : 1 ≤ c) (hc2 : c < SIGMA)
    (hC : Coupled ts i R R' c)
    (w : Int)
    (hw : w = min (winOpen (Ls ts) (prevBnd ts i) i) ((Ls ts).getD i 0))
    (pos_c : Nat) (psvI : Int) (nsvI : Nat)
    (hpsv : psvI = prevSmaller (Ls ts) i) (hnsvI : nsvI = nextSmaller (Ls ts) i)
    (R₃ : List Cand) (R₂' : List (Option CandFM))
    (hR₃ : getR R₃ c = (if ((Ls ts).getD i 0 : Int) > (if w < (getR R c).len then w else (getR R c).len)
          then ⟨(Ls ts).getD i 0, pos_c, true⟩
          else (if w < (getR R c).len then ⟨w, 0, false⟩ else getR R c)))
    (hR₂' : getFM R₂' c = (match getFM R' c with
              | none => some ⟨(i : Int), pos_c, nsvI, true⟩
              | some q => if q.saPos ≤ psvI then some ⟨(i : Int), pos_c, nsvI, true⟩ else some q))
    (hem : Option Nat)
    (hHem : hem = (match getFM R' c with
              | none => none
              | some q => if q.saPos ≤ psvI ∧ (q.nsv < i) then some q.textPos else none)) :
    Coupled ts (i+1) R₃ R₂' c
    ∧ (∀ p, hem = some p →
        ((getR R c).active = false ∧ ∃ sa sn, getFM R' c = some ⟨sa, p, sn, true⟩)
        ∨ (w < (getR R c).len ∧ (getR R c).active = true ∧ p = (getR R c).pos))
    ∧ ((getR R₃ c).active = true → w < (getR R c).len → (getR R c).active = true →
        hem = some (getR R c).pos)
    ∧ (∀ x, (getR R₃ c).active = false → (∃ sa sn, getFM R₂' c = some ⟨sa, x, sn, true⟩) →
        ((getR R c).active = false ∧ (∃ sa sn, getFM R' c = some ⟨sa, x, sn, true⟩))
        ∨ (w < (getR R c).len ∧ (getR R c).active = true ∧ x = (getR R c).pos))
    ∧ (∀ x, (getR R c).active = false → (∃ sa sn, getFM R' c = some ⟨sa, x, sn, true⟩) →
        hem = none → (getR R₃ c).active = false ∧ (∃ sa sn, getFM R₂' c = some ⟨sa, x, sn, true⟩)) := by
  have hpb : prevBnd ts (i+1) = i := prevBnd_bnd ts i hb
  have hpbi : prevBnd ts i < i := by
    rcases prevBnd_lt ts i with h | h
    · exact h
    · omega
  have hwle1 : w ≤ winOpen (Ls ts) (prevBnd ts i) i := by rw [hw]; omega
  have hwle2 : w ≤ ((Ls ts).getD i 0 : Int) := by rw [hw]; omega
  have hwE : (0 : Int) ≤ w := by
    have h1 := winOpen_nonneg (Ls ts) (prevBnd ts i) i
    rw [hw]
    omega
  have hM : ((MAXINT.toNat : Int)) = MAXINT := rfl
  have hviMax : ((Ls ts).getD i 0 : Int) ≤ MAXINT := by
    have := hsat i hi
    omega
  have hLlen : (Ls ts).length = ts.length := by rw [Ls, List.length_map]
  -- the m₀ < vi witness: some window element below vi
  have hwt : winOpen (Ls ts) (prevBnd ts i) i < ((Ls ts).getD i 0 : Int) →
      ∃ j, prevBnd ts i < j ∧ j < i ∧ ((Ls ts).getD j 0 : Int) < ((Ls ts).getD i 0 : Int) := by
    intro hlt
    have := winOpen_lt (L := (Ls ts)) (pb := prevBnd ts i) (i := i) (q := ((Ls ts).getD i 0 : Int))
      (by
        have := winOpen_nonneg (Ls ts) (prevBnd ts i) i
        omega) hlt
    obtain ⟨j, hj1, hj2, hj3⟩ := this
    exact ⟨j, hj1, hj2, by omega⟩
  have hwsplit : w = winOpen (Ls ts) (prevBnd ts i) i ∨ w = ((Ls ts).getD i 0 : Int) := by
    rw [hw]; omega
  cases hq : getFM R' c with
  | none =>
    -- case (c): first involvement; both machines arm
    have hR : getR R c = ⟨-1, 0, false⟩ := by
      simp only [Coupled, hq] at hC
      exact hC
    rw [hq] at hR₂' hHem
    simp only at hR₂' hHem
    -- hR₂' : getFM R₂' c = some ⟨i, pos_c, nsvI, true⟩ ; hHem : hem = none
    have hemv : hem = none := hHem
    -- scan: no drop (w >= 0 > -1), then arms
    have hpost : getR R₃ c = ⟨(Ls ts).getD i 0, pos_c, true⟩ := by
      rw [hR₃, hR]
      have h1 : ¬ (w < (-1 : Int)) := by omega
      simp only [h1, if_false]
      have h2 : ((Ls ts).getD i 0 : Int) > (-1 : Int) := by omega
      simp only [h2, if_true]
    constructor
    · show Coupled ts (i+1) R₃ R₂' c
      simp only [Coupled, hR₂']
      refine ⟨trivial, ?_⟩
      rw [if_pos (by rw [hpost])]
      refine ⟨i, hb, rfl, by omega, ?_, ?_, hnsvI, ?_⟩
      · rw [hpost]
      · rw [hpost]
      · intro j hj1 hj2
        rw [hpb] at hj2
        omega
    constructor
    · intro p hp
      rw [hemv] at hp
      exact absurd hp (by simp)
    constructor
    · intro h1 h2 h3
      simp only [hR] at h2
      omega
    constructor
    · intro x h1 h2
      rw [hpost] at h1
      exact absurd h1 (by simp)
    · intro x h1 h2 h3
      exact absurd h2 (by simp)
  | some q =>
    simp only [Coupled, hq] at hC
    obtain ⟨hqact, hbody⟩ := hC
    obtain ⟨qsa, qtx, qns, qac⟩ := q
    rw [hq] at hR₂' hHem
    simp only at hR₂' hHem
    -- hR₂' : getFM R₂' c = if qsa ≤ psvI then some ⟨i, pos_c, nsvI, true⟩ else some ⟨qsa, qtx, qns, qac⟩
    -- hHem : hem = if qsa ≤ psvI ∧ (qns < i) then some qtx else none
    by_cases hqa : (getR R c).active = true
    · -- case (a): coupled active
      rw [if_pos hqa] at hbody
      obtain ⟨s, hbnd, hsa, hsi, hlen, hpos, hnsv, hpast⟩ := hbody
      have hsL : s < ts.length := by omega
      have hLs : ((Ls ts).getD s 0 : Int) ≤ MAXINT := by
        have := hsat s hsL
        omega
      have hpbge : s ≤ prevBnd ts i := prevBnd_ge_of_bnd ts i s hbnd (by omega)
      subst hsa
      subst hnsv
      by_cases hvilt : ((Ls ts).getD i 0 : Int) < ((Ls ts).getD s 0 : Int)
      · -- vi < v: scan drops; arm iff m0 < vi
        have hwlt : w < ((Ls ts).getD s 0 : Int) := by omega
        by_cases hmvi : winOpen (Ls ts) (prevBnd ts i) i < ((Ls ts).getD i 0 : Int)
        · -- m0 < vi: emit + arm + overwrite + FM-emit; post-(a) with s := i
          obtain ⟨jw, hjw1, hjw2, hjw3⟩ := hwt hmvi
          have hpsvge : ((s : Int) ≤ psvI) := by
            rw [hpsv]
            have hjwL : jw < (Ls ts).length := by omega
            have := prevSmaller_ge (Ls ts) i jw hjw2 (by omega)
            have hjws : s < jw := by omega
            omega
          have hnsvlt : nextSmaller (Ls ts) s < i := by
            have hjwL : jw < (Ls ts).length := by omega
            have := nextSmaller_le (Ls ts) s jw (by omega) hjwL (by omega)
            omega
          have hemv : hem = some qtx := by
            rw [hHem]
            simp [hpsvge, hnsvlt]
          have hpost : getR R₃ c = ⟨(Ls ts).getD i 0, pos_c, true⟩ := by
            rw [hR₃, hlen]
            rw [if_pos hwlt]
            rw [if_pos (by omega)]
          have hact3 : (getR R₃ c).active = true := by rw [hpost]
          constructor
          · show Coupled ts (i+1) R₃ R₂' c
            simp only [Coupled, hR₂', hpsvge, if_true]
            refine ⟨trivial, ?_⟩
            rw [if_pos hact3]
            refine ⟨i, hb, rfl, by omega, ?_, ?_, hnsvI, ?_⟩
            · rw [hpost]
            · rw [hpost]
            · intro j hj1 hj2
              rw [hpb] at hj2
              omega
          constructor
          · intro p hp
            refine Or.inr ⟨by rw [hlen]; omega, hqa, ?_⟩
            rw [hemv] at hp
            have hqp : qtx = p := Option.some.inj hp
            rw [← hqp, hpos]
          constructor
          · intro h1 h2 h3
            rw [hpos]
            exact hemv
          constructor
          · intro x h1 h2
            rw [hact3] at h1
            exact absurd h1 (by simp)
          · intro x h1 h2 h3
            rw [hqa] at h1
            exact absurd h1 (by simp)
        · -- m0 >= vi: scan drops, no arm; FM holds; post-(b)
          have hnow : ∀ j, s ≤ j → j < i → ((Ls ts).getD i 0 : Int) ≤ ((Ls ts).getD j 0 : Int) := by
            intro j hj1 hj2
            by_cases hjp : j ≤ prevBnd ts i
            · by_cases hsj : s < j
              · have hpp := hpast j hsj hjp
                omega
              · have hjs : j = s := by omega
                rw [hjs]
                omega
            · have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i)
                (q := ((Ls ts).getD i 0 : Int)) (by
                  have := winOpen_nonneg (Ls ts) (prevBnd ts i) i
                  omega) j (by omega) (by omega)
              omega
          have hpsvno : ¬ ((s : Int) ≤ psvI) := by
            intro hge
            rw [hpsv] at hge
            have hne : prevSmaller (Ls ts) i ≠ -1 := by
              intro h0
              rw [h0] at hge
              omega
            obtain ⟨j, hj1, hj2, hj3⟩ := prevSmaller_mem (Ls ts) i hne
            have := hnow j (by omega) hj2
            omega
          have hemv : hem = none := by
            rw [hHem]
            simp [hpsvno]
          have hpost : getR R₃ c = ⟨w, 0, false⟩ := by
            rw [hR₃, hlen]
            rw [if_neg (by rw [← hlen]; omega)]
            rw [if_pos (by rw [← hlen]; omega)]
          have hact3 : (getR R₃ c).active = false := by rw [hpost]
          have hulen : (getR R₃ c).len = w := by rw [hpost]
          -- nsv[s] <= i via the drop witness (w < v)
          obtain ⟨jw, hjw1, hjw2, hjw3⟩ := winOpen_witness ts i w hw s hpbge hpbi (by
            rw [← hlen]; omega) hLs
          have hnsvi : nextSmaller (Ls ts) s ≤ i := by
            have hjwL : jw < (Ls ts).length := by omega
            have := nextSmaller_le (Ls ts) s jw (by omega) hjwL (by omega)
            omega
          constructor
          · show Coupled ts (i+1) R₃ R₂' c
            simp only [Coupled, hR₂', hpsvno, if_false]
            refine ⟨hqact, ?_⟩
            rw [if_neg (by rw [hact3]; simp)]
            refine ⟨s, hbnd, rfl, by omega, rfl, by omega, ?_, ⟨jw, by omega, by omega, by omega⟩, ?_⟩
            · rw [hpost]
              omega
            · intro j hj1 hj2
              rw [hpb] at hj2
              by_cases hjp : j ≤ prevBnd ts i
              · have hpp := hpast j (by omega) hjp
                omega
              · by_cases hji : j = i
                · rw [hji]
                  omega
                · have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i) (q := w)
                    (by omega) j (by omega) (by omega)
                  omega
          constructor
          · intro p hp
            rw [hemv] at hp
            exact absurd hp (by simp)
          constructor
          · intro h1 h2 h3
            rw [hact3] at h1
            exact absurd h1 (by simp)
          constructor
          · intro x h1 h2
            refine Or.inr ⟨by rw [hlen]; omega, hqa, ?_⟩
            obtain ⟨sa, sn, hq2⟩ := h2
            rw [hR₂', if_neg hpsvno] at hq2
            have hx : qtx = x := by
              have e1 := (Option.some.inj hq2 :
                (({ saPos := (s : Int), textPos := qtx, nsv := nextSmaller (Ls ts) s, active := qac } : CandFM)
                  = ⟨sa, x, sn, true⟩))
              exact congrArg CandFM.textPos e1
            exact hx.symm.trans hpos.symm
          · intro x h1 h2 h3
            rw [hqa] at h1
            exact absurd h1 (by simp)
      · by_cases hvieq : ((Ls ts).getD i 0 : Int) = ((Ls ts).getD s 0 : Int)
        · -- vi = v
          by_cases hmv : winOpen (Ls ts) (prevBnd ts i) i < ((Ls ts).getD s 0 : Int)
          · -- m0 < v: both emit + both arm; post-(a) s := i
            obtain ⟨jw, hjw1, hjw2, hjw3⟩ := winOpen_witness_lt ts i w hw hviMax (by omega)
            have hpsvge : ((s : Int) ≤ psvI) := by
              rw [hpsv]
              have hjwL : jw < (Ls ts).length := by omega
              have := prevSmaller_ge (Ls ts) i jw hjw2 (by omega)
              have hjws : s < jw := by omega
              omega
            have hnsvlt : nextSmaller (Ls ts) s < i := by
              have hjwL : jw < (Ls ts).length := by omega
              have := nextSmaller_le (Ls ts) s jw (by omega) hjwL (by omega)
              omega
            have hemv : hem = some qtx := by
              rw [hHem]
              simp [hpsvge, hnsvlt]
            have hpost : getR R₃ c = ⟨(Ls ts).getD i 0, pos_c, true⟩ := by
              rw [hR₃, hlen]
              have hwv : w < ((Ls ts).getD s 0 : Int) := by omega
              rw [if_pos (by rw [← hlen]; omega)]
            have hact3 : (getR R₃ c).active = true := by rw [hpost]
            constructor
            · show Coupled ts (i+1) R₃ R₂' c
              simp only [Coupled, hR₂', hpsvge, if_true]
              refine ⟨trivial, ?_⟩
              rw [if_pos hact3]
              refine ⟨i, hb, rfl, by omega, ?_, ?_, hnsvI, ?_⟩
              · rw [hpost]
              · rw [hpost]
              · intro j hj1 hj2
                rw [hpb] at hj2
                omega
            constructor
            · intro p hp
              refine Or.inr ⟨by rw [hlen]; omega, hqa, ?_⟩
              rw [hemv] at hp
              have hqp : qtx = p := Option.some.inj hp
              rw [← hqp, hpos]
            constructor
            · intro h1 h2 h3
              rw [hpos]
              exact hemv
            constructor
            · intro x h1 h2
              rw [hact3] at h1
              exact absurd h1 (by simp)
            · intro x h1 h2 h3
              rw [hqa] at h1
              exact absurd h1 (by simp)
          · -- m0 >= v: silent; both hold; past extends
            have hnow : ∀ j, s ≤ j → j < i → ((Ls ts).getD s 0 : Int) ≤ ((Ls ts).getD j 0 : Int) := by
              intro j hj1 hj2
              by_cases hjp : j ≤ prevBnd ts i
              · by_cases hsj : s < j
                · have hpp := hpast j hsj hjp
                  omega
                · have hjs : j = s := by omega
                  rw [hjs]
                  omega
              · have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i)
                  (q := ((Ls ts).getD s 0 : Int)) (by
                    have := winOpen_nonneg (Ls ts) (prevBnd ts i) i
                    omega) j (by omega) (by omega)
                omega
            have hpsvno : ¬ ((s : Int) ≤ psvI) := by
              intro hge
              rw [hpsv] at hge
              have hne : prevSmaller (Ls ts) i ≠ -1 := by
                intro h0
                rw [h0] at hge
                omega
              obtain ⟨j, hj1, hj2, hj3⟩ := prevSmaller_mem (Ls ts) i hne
              have := hnow j (by omega) hj2
              omega
            have hemv : hem = none := by
              rw [hHem]
              simp [hpsvno]
            have hpost : getR R₃ c = getR R c := by
              rw [hR₃, hlen]
              rw [if_neg (by rw [← hlen]; omega)]
              rw [if_neg (by rw [← hlen]; omega)]
            have hact3 : (getR R₃ c).active = true := by rw [hpost]; exact hqa
            constructor
            · show Coupled ts (i+1) R₃ R₂' c
              simp only [Coupled, hR₂', hpsvno, if_false]
              refine ⟨hqact, ?_⟩
              rw [if_pos hact3]
              refine ⟨s, hbnd, rfl, by omega, ?_, ?_, rfl, ?_⟩
              · rw [hpost, hlen]
              · rw [hpost]
                exact hpos
              · intro j hj1 hj2
                rw [hpb] at hj2
                by_cases hjp : j ≤ prevBnd ts i
                · have hpp := hpast j (by omega) hjp
                  omega
                · by_cases hji : j = i
                  · rw [hji]
                    omega
                  · have := hnow j (by omega) (by omega)
                    omega
            constructor
            · intro p hp
              rw [hemv] at hp
              exact absurd hp (by simp)
            constructor
            · intro h1 h2 h3
              rw [hlen] at h2
              omega
            constructor
            · intro x h1 h2
              rw [hact3] at h1
              exact absurd h1 (by simp)
            · intro x h1 h2 h3
              rw [hqa] at h1
              exact absurd h1 (by simp)
        · -- vi > v: arm always; emit iff m0 < v
          by_cases hmv : winOpen (Ls ts) (prevBnd ts i) i < ((Ls ts).getD s 0 : Int)
          · -- m0 < v: scan emits; FM overwrites and emits; both arm; post-(a) s := i
            obtain ⟨jw, hjw1, hjw2, hjw3⟩ := winOpen_witness_lt ts i w hw hviMax (by omega)
            have hpsvge : ((s : Int) ≤ psvI) := by
              rw [hpsv]
              have hjwL : jw < (Ls ts).length := by omega
              have := prevSmaller_ge (Ls ts) i jw hjw2 (by omega)
              have hjws : s < jw := by omega
              omega
            have hnsvlt : nextSmaller (Ls ts) s < i := by
              have hjwL : jw < (Ls ts).length := by omega
              have := nextSmaller_le (Ls ts) s jw (by omega) hjwL (by omega)
              omega
            have hemv : hem = some qtx := by
              rw [hHem]
              simp [hpsvge, hnsvlt]
            have hpost : getR R₃ c = ⟨(Ls ts).getD i 0, pos_c, true⟩ := by
              rw [hR₃, hlen]
              rw [if_pos (by rw [← hlen]; omega)]
            have hact3 : (getR R₃ c).active = true := by rw [hpost]
            constructor
            · show Coupled ts (i+1) R₃ R₂' c
              simp only [Coupled, hR₂', hpsvge, if_true]
              refine ⟨trivial, ?_⟩
              rw [if_pos hact3]
              refine ⟨i, hb, rfl, by omega, ?_, ?_, hnsvI, ?_⟩
              · rw [hpost]
              · rw [hpost]
              · intro j hj1 hj2
                rw [hpb] at hj2
                omega
            constructor
            · intro p hp
              refine Or.inr ⟨by rw [hlen]; omega, hqa, ?_⟩
              rw [hemv] at hp
              have hqp : qtx = p := Option.some.inj hp
              rw [← hqp, hpos]
            constructor
            · intro h1 h2 h3
              rw [hpos]
              exact hemv
            constructor
            · intro x h1 h2
              rw [hact3] at h1
              exact absurd h1 (by simp)
            · intro x h1 h2 h3
              rw [hqa] at h1
              exact absurd h1 (by simp)
          · -- m0 >= v: silent arm; post-(a) s := i
            have hnow : ∀ j, s ≤ j → j < i → ((Ls ts).getD s 0 : Int) ≤ ((Ls ts).getD j 0 : Int) := by
              intro j hj1 hj2
              by_cases hjp : j ≤ prevBnd ts i
              · by_cases hsj : s < j
                · have hpp := hpast j hsj hjp
                  omega
                · have hjs : j = s := by omega
                  rw [hjs]
                  omega
              · have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i)
                  (q := ((Ls ts).getD s 0 : Int)) (by
                    have := winOpen_nonneg (Ls ts) (prevBnd ts i) i
                    omega) j (by omega) (by omega)
                omega
            -- FM ALWAYS overwrites here: j := s witnesses s <= psvI since L[s] = v < vi
            have hpsvge : ((s : Int) ≤ psvI) := by
              rw [hpsv]
              have := prevSmaller_ge (Ls ts) i s hsi (by omega)
              omega
            -- but emits nothing: no element of (s, i) is below v, so nsv[s] >= i
            have hnsvge : ¬ (nextSmaller (Ls ts) s < i) := by
              intro hlt
              obtain ⟨k, hk1, hk2, hk3⟩ := nextSmaller_mem (Ls ts) s (by omega)
              have := hnow k (by omega) (by omega)
              omega
            have hemv : hem = none := by
              rw [hHem]
              simp [hpsvge, hnsvge]
            have hpost : getR R₃ c = ⟨(Ls ts).getD i 0, pos_c, true⟩ := by
              rw [hR₃, hlen]
              rw [if_pos (by rw [← hlen]; omega)]
            have hact3 : (getR R₃ c).active = true := by rw [hpost]
            constructor
            · show Coupled ts (i+1) R₃ R₂' c
              simp only [Coupled, hR₂', hpsvge, if_true]
              refine ⟨trivial, ?_⟩
              rw [if_pos hact3]
              refine ⟨i, hb, rfl, by omega, ?_, ?_, hnsvI, ?_⟩
              · rw [hpost]
              · rw [hpost]
              · intro j hj1 hj2
                rw [hpb] at hj2
                omega
            constructor
            · intro p hp
              rw [hemv] at hp
              exact absurd hp (by simp)
            constructor
            · intro h1 h2 h3
              rw [hlen] at h2
              omega
            constructor
            · intro x h1 h2
              rw [hact3] at h1
              exact absurd h1 (by simp)
            · intro x h1 h2 h3
              rw [hqa] at h1
              exact absurd h1 (by simp)
    · -- case (b): delayed
      rw [if_neg hqa] at hbody
      obtain ⟨s, hbnd, hsa, hsi, hnsv, hnsvi, hult, hach, hpast⟩ := hbody
      have hsL : s < ts.length := by omega
      have hLs : ((Ls ts).getD s 0 : Int) ≤ MAXINT := by
        have := hsat s hsL
        omega
      have hpbge : s ≤ prevBnd ts i := prevBnd_ge_of_bnd ts i s hbnd (by omega)
      subst hsa
      subst hnsv
      -- arm/overwrite equivalence
      by_cases hpsv' : ((s : Int) ≤ psvI)
      · -- FM overwrites and emits (nsv[s] < i); scan arms; post-(a) s := i
        have hemv : hem = some qtx := by
          rw [hHem]
          simp [hpsv', hnsvi]
        -- scan arms too: the FM witness forces vi > post-eval len
        have harm : ((Ls ts).getD i 0 : Int) > (if w < (getR R c).len then w else (getR R c).len) := by
          by_cases hvi : ((Ls ts).getD i 0 : Int) ≤ (getR R c).len
          · -- vi <= len: the FM witness lies in the open window, below vi
            rw [hpsv] at hpsv'
            have hne : prevSmaller (Ls ts) i ≠ -1 := by
              intro h0
              rw [h0] at hpsv'
              omega
            obtain ⟨j, hj1, hj2, hj3⟩ := prevSmaller_mem (Ls ts) i hne
            -- s <= j, j < i, L[j] < vi <= len
            by_cases hjp : j ≤ prevBnd ts i
            · -- past region: (b)-past gives len <= L[j], but L[j] < vi <= len: absurd
              by_cases hjs : j = s
              · rw [hjs] at hj3
                omega
              · have hpp := hpast j (by omega) hjp
                exact absurd hj3 (by omega)
            · -- open window: w <= winOpen <= L[j] < vi <= len
              have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i)
                (q := winOpen (Ls ts) (prevBnd ts i) i) (by omega) j (by omega) (by omega)
              split <;> omega
          · -- vi > len: both branch values are below vi
            split
            · omega
            · omega
        have hpost : getR R₃ c = ⟨(Ls ts).getD i 0, pos_c, true⟩ := by
          rw [hR₃]
          rw [if_pos harm]
        have hact3 : (getR R₃ c).active = true := by rw [hpost]
        have hR₂v : getFM R₂' c = some ⟨(i : Int), pos_c, nsvI, true⟩ := by
          rw [hR₂']
          simp [hpsv']
        constructor
        · show Coupled ts (i+1) R₃ R₂' c
          simp only [Coupled, hR₂v]
          refine ⟨trivial, ?_⟩
          rw [if_pos hact3]
          refine ⟨i, hb, rfl, by omega, ?_, ?_, hnsvI, ?_⟩
          · rw [hpost]
          · rw [hpost]
          · intro j hj1 hj2
            rw [hpb] at hj2
            omega
        constructor
        · intro p hp
          rw [hemv] at hp
          have hqp : qtx = p := Option.some.inj hp
          have hqac : qac = true := hqact
          refine Or.inl ⟨by simpa using hqa, s, nextSmaller (Ls ts) s, ?_⟩
          rw [hqac, hqp]
        constructor
        · intro h1 h2 h3
          exact absurd h3 hqa
        constructor
        · intro x h1 h2
          rw [hact3] at h1
          exact absurd h1 (by simp)
        · intro x h1 h2 h3
          rw [hemv] at h3
          exact absurd h3 (by simp)
      · -- FM holds: no witness below vi in [s, i); scan cannot arm
        have hemv : hem = none := by
          rw [hHem]
          simp [hpsv']
        have hqac : qac = true := hqact
        have hR₂v : getFM R₂' c = some ⟨(s : Int), qtx, nextSmaller (Ls ts) s, true⟩ := by
          rw [hR₂']
          simp [hpsv', hqac]
        -- m0 >= vi (else the window min would be an FM witness)
        have hm0 : ((Ls ts).getD i 0 : Int) ≤ winOpen (Ls ts) (prevBnd ts i) i := by
          by_cases h : ((Ls ts).getD i 0 : Int) ≤ winOpen (Ls ts) (prevBnd ts i) i
          · exact h
          · -- winOpen < vi: a window element below vi is an FM witness: contradiction
            obtain ⟨j, hj1, hj2, hj3⟩ := winOpen_lt (L := (Ls ts)) (pb := prevBnd ts i) (i := i)
              (q := ((Ls ts).getD i 0 : Int)) (by omega) (by omega)
            have hwit := prevSmaller_ge (Ls ts) i j hj2 (by omega)
            have hs' : ((s : Int) ≤ psvI) := by
              rw [hpsv]
              omega
            omega
        -- vi <= len (else the achiever ja would be an FM witness)
        obtain ⟨ja, hja1, hja2, hja3⟩ := hach
        have hvile : ((Ls ts).getD i 0 : Int) ≤ (getR R c).len := by
          by_cases h : ((Ls ts).getD i 0 : Int) ≤ (getR R c).len
          · exact h
          · -- vi > len: the achiever ja (L[ja] <= len < vi) is an FM witness: contradiction
            have hwit := prevSmaller_ge (Ls ts) i ja hja2 (by omega)
            have hs' : ((s : Int) ≤ psvI) := by
              rw [hpsv]
              omega
            omega
        have hwv : w = ((Ls ts).getD i 0 : Int) := by rw [hw]; omega
        have hnarm : ¬ ((Ls ts).getD i 0 : Int) > (if w < (getR R c).len then w else (getR R c).len) := by
          split
          · omega
          · omega
        have hpost : getR R₃ c = (if w < (getR R c).len then ⟨w, 0, false⟩ else getR R c) := by
          rw [hR₃]
          rw [if_neg hnarm]
        have hact3 : (getR R₃ c).active = false := by
          rw [hpost]
          split
          · rfl
          · simp at hqa
            exact hqa
        have hlen3 : (getR R₃ c).len = (if w < (getR R c).len then w else (getR R c).len) := by
          rw [hpost]
          split
          · rfl
          · rfl
        constructor
        · show Coupled ts (i+1) R₃ R₂' c
          simp only [Coupled, hR₂v]
          refine ⟨trivial, ?_⟩
          rw [if_neg (by simp [hact3])]
          refine ⟨s, hbnd, rfl, by omega, rfl, by omega, ?_, ?_, ?_⟩
          · -- len' < u
            by_cases hdrop : w < (getR R c).len
            · have hl3 : (getR R₃ c).len = w := by rw [hlen3]; rw [if_pos hdrop]
              rw [hl3, hwv]
              omega
            · have hl3 : (getR R₃ c).len = (getR R c).len := by rw [hlen3]; rw [if_neg hdrop]
              rw [hl3]
              exact hult
          · -- achiever for the post len'
            by_cases hdrop : w < (getR R c).len
            · -- reset case: w = vi; the boundary row i itself achieves it
              have hl3 : (getR R₃ c).len = w := by rw [hlen3]; rw [if_pos hdrop]
              refine ⟨i, hsi, Nat.lt_succ_self i, ?_⟩
              have hvi : ((Ls ts).getD i 0 : Int) ≤ w := by omega
              rw [hl3]
              exact hvi
            · -- keep case: the old achiever ja still works
              have hl3 : (getR R₃ c).len = (getR R c).len := by rw [hlen3]; rw [if_neg hdrop]
              exact ⟨ja, hja1, Nat.lt_succ_of_lt hja2, by rw [hl3]; exact hja3⟩
          · -- past condition at i+1: range (s, i]
            intro j hj1 hj2
            rw [hpb] at hj2
            by_cases hjp : j ≤ prevBnd ts i
            · have hpp := hpast j (by omega) hjp
              by_cases hdrop : w < (getR R c).len
              · have hl3 : (getR R₃ c).len = w := by rw [hlen3]; rw [if_pos hdrop]
                rw [hl3]
                omega
              · have hl3 : (getR R₃ c).len = (getR R c).len := by rw [hlen3]; rw [if_neg hdrop]
                rw [hl3]
                omega
            · by_cases hji : j = i
              · -- the boundary row itself: L[i] = vi covers both branch lens
                by_cases hdrop : w < (getR R c).len
                · have hl3 : (getR R₃ c).len = w := by rw [hlen3]; rw [if_pos hdrop]
                  rw [hji, hl3]
                  omega
                · have hl3 : (getR R₃ c).len = (getR R c).len := by rw [hlen3]; rw [if_neg hdrop]
                  rw [hji, hl3]
                  omega
              · -- strict open window: elements >= m0 >= vi
                have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i)
                  (q := ((Ls ts).getD i 0 : Int)) (by omega) j (by omega) (by omega)
                by_cases hdrop : w < (getR R c).len
                · have hl3 : (getR R₃ c).len = w := by rw [hlen3]; rw [if_pos hdrop]
                  rw [hl3, hwv]
                  omega
                · have hl3 : (getR R₃ c).len = (getR R c).len := by rw [hlen3]; rw [if_neg hdrop]
                  rw [hl3]
                  omega
        constructor
        · intro p hp
          rw [hemv] at hp
          exact absurd hp (by simp)
        constructor
        · intro h1 h2 h3
          rw [hact3] at h1
          exact absurd h1 (by simp)
        constructor
        · intro x h1 h2
          rw [hR₂v] at h2
          obtain ⟨sa, sn, hq2⟩ := h2
          injection hq2 with e0
          injection e0 with r1 r2 r3 r4
          refine Or.inl ⟨by simp [hqa], s, nextSmaller (Ls ts) s, ?_⟩
          rw [r2, hqac]
        · intro x h1 h2 h3
          obtain ⟨sa, sn, hq2⟩ := h2
          injection hq2 with e0
          injection e0 with r1 r2 r3 r4
          refine ⟨hact3, s, nextSmaller (Ls ts) s, ?_⟩
          rw [hR₂v, r2]

/-! ## HANDOFF (session 2026-09-26, worker lane /tmp/sxgc-laneG)

### Current proof state: `lake build` GREEN, FmJoint.lean ZERO errors, ZERO sorries.

Fully proven in this file:
- All Part 1 infrastructure (list/winOpen/prevBnd/PSV-NSV/indexing).
- All Part 2 per-char machine lemmas (evalStep/fmStep field and mem laws).
- `winOpen_witness_lt` (NEW this session): STRICT window witness —
  when the boundary min w drops strictly below L[i] (so w = m0 < vi),
  the achiever lies strictly inside the open window: prevBnd < j < i, L[j] ≤ w.
  Requires the saturation bound hmax : L[i] ≤ MAXINT (else m0 could saturate).
  This closed the jw ≤ i vs jw < i seam that blocked `prevSmaller_ge`.
- `coupled_step_notinv` (non-involved char boundary step).
- `coupled_step_inv` (involved char boundary step) — COMPLETE, all cases:
  (c) first-involvement; (a) coupled-active × {vi<v, vi=v, vi>v} × {m0<vi, m0≥vi};
  (b) delayed × {FM overwrites, FM holds}.

### Key semantic facts discovered while proving the (a)/(b) cases (keep these):
1. In vi > v (v = len of the armed scan candidate), FM ALWAYS overwrites:
   j := s itself witnesses s ≤ psvI because L[s] = v < vi. The old
   `hpsvno` claim ("FM holds when m0 ≥ v") was WRONG and is now corrected:
   FM overwrites but emits NOTHING (nsv[s] ≥ i since no element of (s,i)
   is below v), and the scan silently re-arms — both machines take the
   new candidate; post-(a) with s := i.
2. In the (b) delayed case, FM-overwrite ⟹ scan arms: the FM witness j
   (∃ j ∈ [s,i), L[j] < vi) is either in the past (contradicts (b)-past
   len ≤ L[j] with L[j] < vi ≤ len) or in the open window (w ≤ m0 ≤ L[j]
   < vi, so the post-eval len is below vi). FM-hold ⟹ vi ≤ len via the
   achiever ja (L[ja] ≤ len < vi would witness an overwrite) and
   m0 ≥ vi via winOpen_lt (a window element below vi would witness).
3. The (b) achiever is re-established at i+1: keep-case reuses ja;
   reset-case uses the boundary row i itself (L[i] = vi = len').
   The (b) past at i+1 ranges over (s, i] — INCLUDING j = i — so the
   window argument needs an explicit `j = i` case (L[i] = vi covers it;
   this was the last compile error: hj2 gives j ≤ i, not j < i).
4. `cases hq : getFM R' c` replaces `getFM R' c` by the literal in the
   GOAL but not in hypotheses; `injection` on `some ⟨…⟩ = some ⟨…⟩`
   peels ONE layer (Option) — inject twice for the 4 CandFM fields.
   Mathlib is NOT imported: `by_contra`/`push_neg`/`le_refl` are
   unavailable — use `by_cases` + `omega` instead.
5. `omega` treats `getD j 0` and `getD s 0` as unrelated atoms: prove
   j = s by an explicit `by_cases hjs : j = s` + `rw [hjs] at …` first.

### What remains for `fm_equivalence_aux` (bounded form, hsat side-condition):
1. Main boundary-step assembly: from `coupled_step_notinv` (chars not
   involved at i) + `coupled_step_inv` (the two involved chars, cS then
   cE — note scan's evalStep applies to ALL chars at each row while
   fmStep applies only at char changes; the assembly must interleave
   the scan row-step (evalStep for all chars, then upd for the involved
   char) with the two fmSteps at a boundary row).
2. Non-boundary row step (t.c = prev.c): scan still runs evalStep with
   m' = min m t.lcp for all chars; FM does NOTHING. The invariant's
   "past-completed-windows" formulation survives this (already checked
   empirically: per-char emitted sequences identical, arming points
   coincide, 0/600 streams).
3. Final flush: scan's last evalStep (-1) emits every ACTIVE candidate;
   fmSpec's finalEmit emits every active CandFM. MEM assembly:
   x ∈ scan-out ↔ x ∈ S ∨ Delayed; then Delayed x → x ∈ finalEmit R' S
   (finalEmit emits exactly the active candidates), giving
   ∀ x, x ∈ scan N ts ↔ x ∈ fmSpec N ts.
4. The main induction over rows carrying JointInv (Coupled ∀c + MEM).
5. Statement policy per house rules: prove the BOUNDED
   `fm_equivalence_aux` (hsat : ∀ j < ts.length, L[j] ≤ MAXINT.toNat,
   derivable for real texts since lcp ≤ |T|); mark the unbounded
   `fm_equivalence` REFUTED (saturation counterexample: decreasing
   huge-lcp stream, scan 10 emissions vs FM 2, divergence strictly
   above MAXINT; at MAXINT exactly they agree).

### Where things are:
- Git HEAD 4fb8a54 (clean); FmJoint.lean UNTRACKED in /tmp/sxgc-laneG/lean/
  per protocol (no commits by agents). 1665 lines. Sxgc.lean untouched.
- Sxgc.lean sorry ledger unchanged: 5 (O1 covering_given_stream,
  minimality O2–O4, fm_equivalence + retired-refuted pair).
- File-level O3 adjudication COMPLETE: 0 violations on 729/1092/1364
  exhaustive batteries + 3-chunk reduced stress (25×4-letter len≤16,
  25×binary len≤16, 25×unary/periodic) — all 0. O3 stands as-is.
-/
