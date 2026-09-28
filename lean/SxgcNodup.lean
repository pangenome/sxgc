import SxgcBounds
open Sxgc

/-! # General stream Nodup (the engine of O2_bounded)

THEOREM: for any triple stream with pairwise-distinct `sa` values, all `sa < N`,
and every `lcp ≤ MAXINT`, the one-pass scan emits **no duplicate positions**:

  `(ts.map Triple.sa).Nodup → (∀ t ∈ ts, t.sa < N) → (∀ t ∈ ts, (t.lcp : Int) ≤ MAXINT)
    → (scan N ts).Nodup`

This is strictly stronger than the locked `O2_bounded` (which follows from
`saOrder` distinctness).  The proof makes precise the arming discipline that the
saturation counterexample (an *unbounded* stream) violates:

- positions are `N - sa`, one owner triple each (distinct sa's, `sa < N`);
- a position is armed into the candidate table only at run boundaries adjacent
  to its owner triple: as first-of-run (`upd t.c t.lcp (N - t.sa)`) and as
  last-of-run (`upd p t.lcp (N - pSa)`);
- the two armings of a length-1 run happen at consecutive boundaries with no
  evalStep between them; a re-arm of an *emitted* position at the very boundary
  where it was emitted is blocked there by boundedness: the reset length is the
  evalStep threshold `min m t.lcp`, which equals `t.lcp` exactly when
  `m = MAXINT` and `t.lcp ≤ MAXINT`, making the arm condition `t.lcp > t.lcp`
  fail;
- hence each position is live-armed at most once at any emission point, and
  emissions of distinct chars are distinct positions; the output is Nodup.

Empirical warrant: 1,474,560 bounded distinct-sa streams (len ≤ 4, chars
{1,2}, lcp ≤ 3): zero violations.  The unbounded saturation witness duplicates,
as recorded. -/

namespace SxgcNodup

/-! ## Small list helpers -/

theorem filter_nodup {α : Type} (p : α → Bool) (l : List α) (h : l.Nodup) :
    (l.filter p).Nodup := by
  induction l with
  | nil => simp
  | cons a as ih =>
    rw [List.nodup_cons] at h
    obtain ⟨ha, has⟩ := h
    simp only [List.filter_cons]
    split
    · rw [List.nodup_cons]
      exact ⟨fun hc => ha (List.mem_filter.mp hc).1, ih has⟩
    · exact ih has

theorem map_congr_mem {α β : Type} (f g : α → β) (l : List α) (h : ∀ x ∈ l, f x = g x) :
    l.map f = l.map g := by
  induction l with
  | nil => rfl
  | cons a as ih =>
    simp only [List.map_cons]
    rw [h a (by simp), ih (fun x hx => h x (by simp [hx]))]

/-! ## evalStep specification -/

/-- What one fold step does to a candidate it visits. -/
def resetAt (l : Int) (cd : Cand) : Cand := if l < cd.len then ⟨l, 0, false⟩ else cd

/-- Fold-level spec: over an index list with distinct entries (all visiting
in-range chars), the evalStepGo fold appends exactly the pre-state
active-and-exceeding positions (index order) and resets exactly the visited
chars.  All references are to the STARTING `R` of that fold. -/
theorem fold_evalStepGo_spec (l : Int) :
    ∀ (is : List Nat) (out : List Nat) (R : List Cand), is.Nodup →
    (∀ i ∈ is, i + 1 < R.length) →
    ∃ E R₁, List.foldl (evalStepGo l) (out, R) is = (out ++ E, R₁) ∧
      E = (is.filter (fun i =>
             decide (l < (getR R (i+1)).len ∧ (getR R (i+1)).active))).map
           (fun i => (getR R (i+1)).pos) ∧
      (∀ c, (∃ i ∈ is, i + 1 = c) → getR R₁ c = resetAt l (getR R c)) ∧
      (∀ c, (∀ i ∈ is, i + 1 ≠ c) → getR R₁ c = getR R c) := by
  intro is
  induction is with
  | nil =>
    intro out R _ _
    refine ⟨[], R, by simp, rfl, ?_, ?_⟩
    · intro c hc; obtain ⟨i, hi, _⟩ := hc; exact absurd hi (by simp)
    · intro c _; rfl
  | cons i is' ih =>
    intro out R hnd hrange
    rw [List.nodup_cons] at hnd
    obtain ⟨hi, his'⟩ := hnd
    have hisa : ∀ j ∈ is', j ≠ i := by
      intro j hj heq; exact hi (heq ▸ hj)
    have hgetR : ∀ j ∈ is', getR (R.set (i+1) ⟨l, 0, false⟩) (j+1) = getR R (j+1) := by
      intro j hj
      exact getR_set_ne R (i+1) (j+1) ⟨l, 0, false⟩ (fun heq => hisa j hj (by omega))
    have hlenR : (R.set (i+1) ⟨l, 0, false⟩).length = R.length := List.length_set
    have hin : i + 1 < R.length := hrange i (by simp)
    have hset : getR (R.set (i+1) ⟨l, 0, false⟩) (i+1) = ⟨l, 0, false⟩ := by
      rw [getR_set_eq, if_pos hin]
    have hrange' : ∀ j ∈ is', j + 1 < (R.set (i+1) ⟨l, 0, false⟩).length := by
      intro j hj; rw [hlenR]; exact hrange j (by simp [hj])
    by_cases hlen : l < (getR R (i+1)).len
    · -- visited and reset
      have hresetAt : resetAt l (getR R (i+1)) = ⟨l, 0, false⟩ := by
        unfold resetAt; rw [if_pos hlen]
      have hstep : evalStepGo l (out, R) i
          = ((if (getR R (i+1)).active then out ++ [(getR R (i+1)).pos] else out),
             R.set (i+1) ⟨l, 0, false⟩) := by
        rw [evalStepGo, if_pos hlen]
      have hclauses : ∀ (R₁ : List Cand),
          (∀ c, (∃ j ∈ is', j + 1 = c) → getR R₁ c = resetAt l (getR (R.set (i+1) ⟨l,0,false⟩) c)) →
          (∀ c, (∀ j ∈ is', j + 1 ≠ c) → getR R₁ c = getR (R.set (i+1) ⟨l,0,false⟩) c) →
          (∀ c, (∃ j ∈ i :: is', j + 1 = c) → getR R₁ c = resetAt l (getR R c)) ∧
          (∀ c, (∀ j ∈ i :: is', j + 1 ≠ c) → getR R₁ c = getR R c) := by
        intro R₁ hA hB
        refine ⟨?_, ?_⟩
        · intro c hc
          obtain ⟨j, hj, hjc⟩ := hc
          rcases List.mem_cons.mp hj with hji | hj'
          · have hc1 : i + 1 = c := by rw [← hji]; exact hjc
            have hBc : getR R₁ (i+1) = getR (R.set (i+1) ⟨l, 0, false⟩) (i+1) :=
              hB (i+1) (fun j' hj' _ => hisa j' hj' (by omega))
            rw [hset] at hBc
            rw [← hc1, hBc, hresetAt]
          · have hAc := hA c ⟨j, hj', hjc⟩
            have hcne : i + 1 ≠ c := by
              intro heq; exact hisa j hj' (by omega)
            rw [getR_set_ne R (i+1) c ⟨l, 0, false⟩ hcne] at hAc
            exact hAc
        · intro c hc
          have hci : i + 1 ≠ c := fun heq => hc i (List.mem_cons_self ..) heq
          have hBc : getR R₁ c = getR (R.set (i+1) ⟨l, 0, false⟩) c :=
            hB c (fun j' hj' hj'c => hc j' (List.mem_cons_of_mem _ hj') hj'c)
          rw [hBc, getR_set_ne R (i+1) c ⟨l, 0, false⟩ hci]
      have hcomp : ((is'.filter (fun j =>
            decide (l < (getR (R.set (i+1) ⟨l,0,false⟩) (j+1)).len ∧
                    (getR (R.set (i+1) ⟨l,0,false⟩) (j+1)).active)))).map
            (fun j => (getR (R.set (i+1) ⟨l,0,false⟩) (j+1)).pos)
            = ((is'.filter (fun j =>
            decide (l < (getR R (j+1)).len ∧ (getR R (j+1)).active)))).map
            (fun j => (getR R (j+1)).pos) := by
        rw [List.filter_congr (fun x hx => by rw [hgetR x (by simp [hx])])]
        exact map_congr_mem _ _ _ (fun x hx => by rw [hgetR x (List.mem_filter.mp hx).1])
      by_cases hact : (getR R (i+1)).active = true
      · -- emission
        rw [if_pos hact] at hstep
        obtain ⟨E, R₁, hfold, hE, hA, hB⟩ :=
          ih (out ++ [(getR R (i+1)).pos]) (R.set (i+1) ⟨l, 0, false⟩) his' hrange'
        obtain ⟨hA', hB'⟩ := hclauses R₁ hA hB
        refine ⟨(getR R (i+1)).pos :: E, R₁, ?_, ?_, hA', hB'⟩
        · rw [List.foldl_cons, hstep]
          have happ : out ++ ((getR R (i+1)).pos :: E)
              = (out ++ [(getR R (i+1)).pos]) ++ E := by
            rw [List.append_assoc]; rfl
          rw [happ]
          exact hfold
        · rw [hE, hcomp]
          simp [hlen, hact]
      · -- reset, no emission
        rw [if_neg hact] at hstep
        obtain ⟨E, R₁, hfold, hE, hA, hB⟩ :=
          ih out (R.set (i+1) ⟨l, 0, false⟩) his' hrange'
        obtain ⟨hA', hB'⟩ := hclauses R₁ hA hB
        refine ⟨E, R₁, ?_, ?_, hA', hB'⟩
        · rw [List.foldl_cons, hstep]; exact hfold
        · rw [hE, hcomp]
          simp [hlen, hact]
    · -- not visited: state unchanged
      have hstep : evalStepGo l (out, R) i = (out, R) := by
        rw [evalStepGo, if_neg hlen]
      obtain ⟨E, R₁, hfold, hE, hA, hB⟩ := ih out R his' (fun j hj => hrange j (by simp [hj]))
      have hresetAt' : resetAt l (getR R (i+1)) = getR R (i+1) := by
        unfold resetAt; rw [if_neg hlen]
      have hA' : ∀ c, (∃ j ∈ i :: is', j + 1 = c) → getR R₁ c = resetAt l (getR R c) := by
        intro c hc
        obtain ⟨j, hj, hjc⟩ := hc
        rcases List.mem_cons.mp hj with hji | hj'
        · have hc1 : i + 1 = c := by rw [← hji]; exact hjc
          have hBc : getR R₁ (i+1) = getR R (i+1) :=
            hB (i+1) (fun j' hj' _ => hisa j' hj' (by omega))
          rw [← hc1, hBc]
          exact hresetAt'.symm
        · exact hA c ⟨j, hj', hjc⟩
      have hB' : ∀ c, (∀ j ∈ i :: is', j + 1 ≠ c) → getR R₁ c = getR R c := by
        intro c hc
        exact hB c (fun j' hj' hj'c => hc j' (List.mem_cons_of_mem _ hj') hj'c)
      refine ⟨E, R₁, ?_, ?_, hA', hB'⟩
      · rw [List.foldl_cons, hstep]; exact hfold
      · rw [hE]
        simp [hlen]

theorem evalStep_spec (l : Int) (R : List Cand) (out : List Nat) (hR : SIGMA ≤ R.length) :
    ∃ E R₁, evalStep l R out = (out ++ E, R₁) ∧
      E = ((List.range (SIGMA-1)).filter (fun i =>
             decide (l < (getR R (i+1)).len ∧ (getR R (i+1)).active))).map
           (fun i => (getR R (i+1)).pos) ∧
      (∀ c, getR R₁ c = if 1 ≤ c ∧ c < SIGMA then resetAt l (getR R c) else getR R c) := by
  rw [evalStep_eq]
  obtain ⟨E, R₁, hfold, hE, hA, hB⟩ :=
    fold_evalStepGo_spec l (List.range (SIGMA-1)) out R
      List.nodup_range
      (fun i hi => by
        rw [List.mem_range] at hi
        omega)
  refine ⟨E, R₁, hfold, hE, ?_⟩
  intro c
  by_cases hcs : 1 ≤ c ∧ c < SIGMA
  · rw [if_pos hcs]
    refine hA c ⟨c - 1, ?_, by omega⟩
    rw [List.mem_range]
    omega
  · rw [if_neg hcs]
    refine hB c ?_
    intro i hi hic
    rw [List.mem_range] at hi
    omega

theorem evalStep_length (l : Int) :
    ∀ (is : List Nat) (out : List Nat) (R : List Cand),
      (List.foldl (evalStepGo l) (out, R) is).2.length = R.length := by
  intro is
  induction is with
  | nil => intro out R; rfl
  | cons i is' ih =>
      intro out R
      rw [List.foldl_cons]
      have hstep : (evalStepGo l (out, R) i).2.length = R.length := by
        rw [evalStepGo]
        split
        · rw [List.length_set]
        · rfl
      have hih := ih (evalStepGo l (out, R) i).1 (evalStepGo l (out, R) i).2
      rw [hstep] at hih
      exact hih

theorem evalStep_len' (l : Int) (R : List Cand) (out : List Nat) :
    ((evalStep l R out).2).length = R.length := by
  rw [evalStep_eq]; exact evalStep_length l _ _ _

/-- Emissions are pre-state armed positions at in-range chars. -/
theorem E_mem' {l : Int} {R : List Cand} {x : Nat}
    (hx : x ∈ ((List.range (SIGMA-1)).filter (fun i =>
             decide (l < (getR R (i+1)).len ∧ (getR R (i+1)).active))).map
           (fun i => (getR R (i+1)).pos)) :
    ∃ c, 1 ≤ c ∧ c < SIGMA ∧ (getR R c).active = true ∧ l < (getR R c).len ∧
      x = (getR R c).pos := by
  rw [List.mem_map] at hx
  obtain ⟨i, hi, hxi⟩ := hx
  rw [List.mem_filter] at hi
  obtain ⟨hir, hcond⟩ := hi
  have hcond' : l < (getR R (i+1)).len ∧ (getR R (i+1)).active = true := by
    simpa using hcond
  rw [List.mem_range] at hir
  exact ⟨i+1, by omega, by omega, hcond'.2, hcond'.1, hxi.symm⟩

/-- E-Nodup from armed pairwise distinctness. -/
theorem E_nodup' {l : Int} {R : List Cand}
    (h3 : ∀ c c', (getR R c).active = true → (getR R c').active = true →
          (getR R c).pos = (getR R c').pos → c = c') :
    (((List.range (SIGMA-1)).filter (fun i =>
             decide (l < (getR R (i+1)).len ∧ (getR R (i+1)).active))).map
           (fun i => (getR R (i+1)).pos)).Nodup := by
  refine nodup_map_of_inj (fun i => (getR R (i+1)).pos) _ (filter_nodup _ _ List.nodup_range) ?_
  intro a ha b hb hab
  rw [List.mem_filter] at ha hb
  have hconda : l < (getR R (a+1)).len ∧ (getR R (a+1)).active = true := by simpa using ha.2
  have hcondb : l < (getR R (b+1)).len ∧ (getR R (b+1)).active = true := by simpa using hb.2
  have hcc := h3 (a+1) (b+1) hconda.2 hcondb.2 hab
  omega

/-- Survivors keep their candidates, formula form. -/
theorem survivors_of {l : Int} {R : List Cand} (c : Nat) (R₁ : List Cand)
    (hR₁ : ∀ c, getR R₁ c = if 1 ≤ c ∧ c < SIGMA then resetAt l (getR R c) else getR R c)
    (hact : (getR R₁ c).active = true) :
    getR R₁ c = getR R c := by
  have hc := hR₁ c
  by_cases hcs : 1 ≤ c ∧ c < SIGMA
  · rw [if_pos hcs] at hc
    unfold resetAt at hc
    by_cases hlt : l < (getR R c).len
    · rw [if_pos hlt] at hc
      rw [hc] at hact; simp at hact
    · rw [if_neg hlt] at hc; exact hc
  · rw [if_neg hcs] at hc; exact hc

/-! ## upd lemmas -/

theorem upd_other (R : List Cand) (c : Nat) (l pos : Nat) {d : Nat} (h : d ≠ c) :
    getR (upd R c l pos) d = getR R d := by
  unfold upd
  split
  · exact getR_set_ne R c d ⟨l, pos, true⟩ (Ne.symm h)
  · rfl

theorem upd_arm (R : List Cand) (c : Nat) (l pos : Nat)
    (h : ((l : Int) > (getR R c).len ∧ c < R.length)) :
    getR (upd R c l pos) c = ⟨l, pos, true⟩ := by
  unfold upd
  rw [if_pos h.1, getR_set_eq, if_pos h.2]

theorem upd_dich (R : List Cand) (c : Nat) (l pos : Nat) :
    getR (upd R c l pos) c = ⟨l, pos, true⟩ ∨ getR (upd R c l pos) c = getR R c := by
  unfold upd
  by_cases hcond : (l : Int) > (getR R c).len
  · rw [if_pos hcond, getR_set_eq]
    by_cases hc : c < R.length
    · rw [if_pos hc]; exact Or.inl rfl
    · rw [if_neg hc]; exact Or.inr rfl
  · rw [if_neg hcond]; exact Or.inr rfl

theorem upd_eq (R : List Cand) (c : Nat) (l pos : Nat)
    (h : ¬ ((l : Int) > (getR R c).len ∧ c < R.length)) :
    getR (upd R c l pos) c = getR R c := by
  unfold upd
  by_cases hcond : (l : Int) > (getR R c).len
  · rw [if_pos hcond, getR_set_eq]
    by_cases hc : c < R.length
    · rw [if_pos hc]
      exact absurd ⟨hcond, hc⟩ h
    · rw [if_neg hc]
  · rw [if_neg hcond]

theorem upd_length (R : List Cand) (c : Nat) (l pos : Nat) :
    (upd R c l pos).length = R.length := by
  unfold upd
  split
  · rw [List.length_set]
  · rfl

/-! ## The main induction -/

theorem scanAux_nodup (N : Nat) :
    ∀ (ts : List Triple) (p pSa : Nat) (m : Int) (R : List Cand) (out : List Nat),
      R.length = SIGMA →
      (ts.map Triple.sa).Nodup →
      pSa ∉ ts.map Triple.sa →
      pSa < N →
      (∀ t ∈ ts, t.sa < N) →
      (∀ t ∈ ts, (t.lcp : Int) ≤ MAXINT) →
      out.Nodup →
      (∀ c, (getR R c).active = true → (getR R c).pos ∉ out) →
      (∀ c c', (getR R c).active = true → (getR R c').active = true →
          (getR R c).pos = (getR R c').pos → c = c') →
      (∀ c, (getR R c).active = true → (getR R c).pos ∉ (ts.map (fun t => N - t.sa))) →
      (∀ x ∈ out, x ∉ (ts.map (fun t => N - t.sa))) →
      (N - pSa) ∉ out →
      (∀ c, c ≠ p → (getR R c).active = true → (getR R c).pos ≠ N - pSa) →
      ((getR R p).active = true → (getR R p).pos = N - pSa → m = MAXINT) →
      (scanAux N ts p pSa m R out).Nodup := by
  intro ts
  induction ts with
  | nil =>
    intro p pSa m R out hRlen _ _ _ _ _ h1 h2 h3 h4 h5 h6 h7 h8
    simp only [scanAux]
    obtain ⟨E, R₁, hEq, hE, _⟩ := evalStep_spec (-1) R out (by omega)
    have hEnodup : E.Nodup := by rw [hE]; exact E_nodup' h3
    have hEdisj : ∀ a ∈ out, a ∉ E := by
      intro a ha hae
      rw [hE] at hae
      obtain ⟨c, _, _, hact, _, hxa⟩ := E_mem' hae
      exact h2 c hact (hxa ▸ ha)
    rw [hEq]
    exact (List.nodup_append.mpr ⟨h1, hEnodup,
      fun a ha b hb heq => hEdisj a ha (heq ▸ hb)⟩)
  | cons t rest ih =>
    intro p pSa m R out hRlen hnd hprev hppos htpos hbnd h1 h2 h3 h4 h5 h6 h7 h8
    have hrest_nd : (rest.map Triple.sa).Nodup := by
      rw [List.map_cons] at hnd; rw [List.nodup_cons] at hnd; exact hnd.2
    have htsa : t.sa < N := htpos t (by simp)
    have htpos' : ∀ t' ∈ rest, t'.sa < N := fun t' ht' => htpos t' (by simp [ht'])
    have hbnd' : ∀ t' ∈ rest, (t'.lcp : Int) ≤ MAXINT := fun t' ht' => hbnd t' (by simp [ht'])
    have hbndt : (t.lcp : Int) ≤ MAXINT := hbnd t (by simp)
    have himg_t : (N - t.sa) ∈ ((t :: rest).map (fun t' => N - t'.sa)) :=
      List.mem_map.mpr ⟨t, by simp, rfl⟩
    have h4r : ∀ c, (getR R c).active = true →
        (getR R c).pos ∉ (rest.map (fun t' => N - t'.sa)) := by
      intro c hc hx
      exact h4 c hc (by
        simp only [List.map_cons, List.mem_cons]
        exact Or.inr hx)
    have h5r : ∀ x ∈ out, x ∉ (rest.map (fun t' => N - t'.sa)) := by
      intro x hx xh
      exact h5 x hx (by
        simp only [List.map_cons, List.mem_cons]
        exact Or.inr xh)
    have htsa_rest : t.sa ∉ (rest.map Triple.sa) := by
      rw [List.map_cons] at hnd
      rw [List.nodup_cons] at hnd
      exact hnd.1
    have hp_img : (N - pSa) ∉ (rest.map (fun t' => N - t'.sa)) := by
      intro hc
      obtain ⟨t', ht', heq⟩ := List.mem_map.mp hc
      exact hprev (List.mem_map.mpr ⟨t', List.mem_cons_of_mem _ ht', by omega⟩)
    simp only [scanAux]
    split
    · -- boundary: t.c != p
      rename_i hne0
      have hne : t.c ≠ p := by simpa using hne0
      obtain ⟨E, R', hEq, hE, hR'c⟩ :=
        evalStep_spec (min m (t.lcp : Int)) R out (by omega)
      simp only [hEq]
      have hR'len : R'.length = SIGMA := by
        have hl := evalStep_len' (min m (t.lcp : Int)) R out
        rw [hEq] at hl; exact hl.trans hRlen
      generalize hR''d : upd R' p t.lcp (N - pSa) = R''
      generalize hR'''d : upd R'' t.c t.lcp (N - t.sa) = R'''
      have hR'''len : R'''.length = SIGMA := by
        rw [← hR'''d, ← hR''d]; rw [upd_length, upd_length]; exact hR'len
      -- emitted-set facts
      have hEmem : ∀ x ∈ E, ∃ c, 1 ≤ c ∧ c < SIGMA ∧ (getR R c).active = true ∧
          min m (t.lcp : Int) < (getR R c).len ∧ x = (getR R c).pos := by
        intro x hx; rw [hE] at hx; exact E_mem' hx
      have hEnodup : E.Nodup := by rw [hE]; exact E_nodup' h3
      have hEout : ∀ x ∈ E, x ∉ out := by
        intro x hx hxo
        obtain ⟨c, _, _, hcact, _, hxp⟩ := hEmem x hx
        exact h2 c hcact (hxp ▸ hxo)
      have hout₁nd : (out ++ E).Nodup :=
        List.nodup_append.mpr ⟨h1, hEnodup,
          fun a ha b hb heq => hEout b hb (heq ▸ ha)⟩
      -- R''' equations (via the generalize hypotheses)
      have hR'''p : getR R''' p = getR R'' p := by
        rw [← hR'''d]; exact upd_other _ t.c t.lcp (N - t.sa) (Ne.symm hne)
      have hR''tc : getR R'' t.c = getR R' t.c := by
        rw [← hR''d]; exact upd_other _ p t.lcp (N - pSa) hne
      have hR''p_arm : getR R'' p = ⟨t.lcp, N - pSa, true⟩ ∨ getR R'' p = getR R' p := by
        rw [← hR''d]; exact upd_dich R' p t.lcp (N - pSa)
      -- arm value facts
      have hp_arm : getR R''' p = ⟨t.lcp, N - pSa, true⟩ ∨ getR R''' p = getR R' p := by
        rw [hR'''p]; exact hR''p_arm
      have htc_arm : getR R''' t.c = ⟨t.lcp, N - t.sa, true⟩ ∨ getR R''' t.c = getR R' t.c := by
        rw [← hR'''d]
        rcases upd_dich R'' t.c t.lcp (N - t.sa) with h | h
        · exact Or.inl h
        · exact Or.inr (h.trans hR''tc)
      have hother : ∀ c, c ≠ p → c ≠ t.c → getR R''' c = getR R' c := by
        intro c hcp hct
        rw [← hR'''d, ← hR''d]
        rw [upd_other _ t.c t.lcp (N - t.sa) hct]
        exact upd_other _ p t.lcp (N - pSa) hcp
      have hsurv : ∀ c, (getR R' c).active = true → getR R' c = getR R c :=
        fun c => survivors_of c R' hR'c
      -- key trichotomy (value level)
      have hkey : ∀ c, (getR R''' c).active = true →
          getR R''' c = getR R' c ∨
          (c = p ∧ (getR R''' c).pos = N - pSa) ∨
          (c = t.c ∧ (getR R''' c).pos = N - t.sa) := by
        intro c hcact
        by_cases hcp : c = p
        · rw [hcp] at hcact ⊢
          rcases hp_arm with h | h
          · exact Or.inr (Or.inl ⟨rfl, congrArg (fun cd => cd.pos) h⟩)
          · exact Or.inl h
        · by_cases hct : c = t.c
          · rw [hct] at hcact ⊢
            rcases htc_arm with h | h
            · exact Or.inr (Or.inr ⟨rfl, congrArg (fun cd => cd.pos) h⟩)
            · exact Or.inl h
          · exact Or.inl (hother c hcp hct)
      -- R-activity of survivors
      have hRact : ∀ c, (getR R''' c).active = true → getR R''' c = getR R' c →
          (getR R c).active = true := by
        intro c hcact hs
        have hR'act : (getR R' c).active = true := by rw [← hs]; exact hcact
        rw [← hsurv c hR'act]; exact hR'act
      -- position trichotomy
      have hpostri : ∀ c, (getR R''' c).active = true →
          ((getR R''' c).pos = (getR R c).pos ∧ (getR R c).active = true) ∨
          (c = p ∧ (getR R''' c).pos = N - pSa) ∨
          (c = t.c ∧ (getR R''' c).pos = N - t.sa) := by
        intro c hcact
        rcases hkey c hcact with hs | hd | hd
        · refine Or.inl ?_
          have hR'act : (getR R' c).active = true := by rw [← hs]; exact hcact
          have hRc : (getR R c).active = true := by rw [← hsurv c hR'act]; exact hR'act
          exact ⟨by rw [hs, hsurv c hR'act], hRc⟩
        · exact Or.inr (Or.inl hd)
        · exact Or.inr (Or.inr hd)
      -- THE BLOCKING: previous position emitted at this boundary ⟹ p-arm fails
      have hblock : (N - pSa) ∈ out ++ E →
          getR R''' p = ⟨min m (t.lcp : Int), 0, false⟩ := by
        intro hmem
        have hEin : (N - pSa) ∈ E := by
          rcases (List.mem_append.mp hmem) with h | h
          · exact absurd h h6
          · exact h
        obtain ⟨c₀, hc₀le, hc₀lt, hc₀act, hc₀len, hc₀pos⟩ := hEmem _ hEin
        have hc₀ : c₀ = p := by
          by_cases hne0 : c₀ = p
          · exact hne0
          · exact absurd hc₀pos.symm (h7 c₀ hne0 hc₀act)
        rw [hc₀] at hc₀act hc₀len hc₀pos hc₀le hc₀lt
        have hmMAX : m = MAXINT := h8 hc₀act hc₀pos.symm
        have hmin : min m (t.lcp : Int) = (t.lcp : Int) := by
          rw [hmMAX]; omega
        have hR'p : getR R' p = ⟨min m (t.lcp : Int), 0, false⟩ := by
          have hp' := hR'c p
          rw [if_pos ⟨hc₀le, hc₀lt⟩] at hp'
          unfold resetAt at hp'
          rw [if_pos hc₀len] at hp'
          exact hp'
        have hfail : ¬ ((t.lcp : Int) > (getR R' p).len ∧ p < R'.length) := by
          intro hc
          have hgt : (t.lcp : Int) > min m (t.lcp : Int) := by
            rw [hR'p] at hc; exact hc.1
          rw [hmin] at hgt
          have := hbndt
          omega
        rw [hR'''p, ← hR''d]
        rw [upd_eq R' p t.lcp (N - pSa) hfail]
        exact hR'p
      -- position inequalty helpers
      have hpne : pSa ≠ t.sa := fun heq =>
        hprev (heq ▸ (List.mem_map.mpr ⟨t, by simp, rfl⟩))
      have hpos_ne : N - pSa ≠ N - t.sa := by omega
      -- H2': armed R''' positions are not in out ++ E
      have h2' : ∀ c, (getR R''' c).active = true → (getR R''' c).pos ∉ out ++ E := by
        intro c hcact hmem
        rw [List.mem_append] at hmem
        rcases hkey c hcact with hs | ⟨hcp, hpos⟩ | ⟨hct, hpos⟩
        · -- survivor
          have hRcact := hRact c hcact hs
          have hpR : (getR R''' c).pos = (getR R c).pos := by
            have hR'act : (getR R' c).active = true := by rw [← hs]; exact hcact
            rw [hs, hsurv c hR'act]
          rcases hmem with h | h
          · exact h2 c hRcact (hpR ▸ h)
          · obtain ⟨c₁, hc₁le, hc₁lt, hc₁act, hc₁len, hc₁pos⟩ := hEmem _ h
            have hc₁c : c₁ = c := h3 c₁ c hc₁act hRcact (by rw [← hpR, hc₁pos])
            rw [hc₁c] at hc₁act hc₁len hc₁le hc₁lt
            have hR'c₁ : getR R' c = ⟨min m (t.lcp : Int), 0, false⟩ := by
              have hcl := hR'c c
              rw [if_pos ⟨hc₁le, hc₁lt⟩] at hcl
              unfold resetAt at hcl
              rw [if_pos hc₁len] at hcl
              exact hcl
            rw [hs, hR'c₁] at hcact
            simp at hcact
        · -- p-armed: blocked if emitted
          rw [hcp] at hcact
          have hinact := hblock (List.mem_append.mpr (hpos ▸ hmem))
          rw [hinact] at hcact
          simp at hcact
        · -- t.c-armed: pos = N - t.sa
          rcases hmem with h | h
          · exact h5 (N - t.sa) (hpos ▸ h) himg_t
          · obtain ⟨c₁, _, _, hc₁act, _, hc₁pos⟩ := hEmem _ h
            have himg : (getR R c₁).pos ∈ ((t :: rest).map (fun t' => N - t'.sa)) := by
              have heq : (getR R c₁).pos = N - t.sa := hc₁pos.symm.trans hpos
              rw [heq]
              exact himg_t
            exact h4 c₁ hc₁act himg
      -- H3': armed positions pairwise distinct across chars
      have h3' : ∀ c c', (getR R''' c).active = true → (getR R''' c').active = true →
          (getR R''' c).pos = (getR R''' c').pos → c = c' := by
        intro c c' hact hact' hpeq
        rcases hpostri c hact with ⟨hR, hRca⟩ | hd | hd
        · rcases hpostri c' hact' with ⟨hR', hRca'⟩ | hd' | hd'
          · exact h3 c c' hRca hRca' (hR.symm.trans (hpeq.trans hR'))
          · by_cases hcpp : c = p
            · exact hcpp.trans hd'.1.symm
            · exact absurd (hR.symm.trans (hpeq.trans hd'.2)) (h7 c hcpp hRca)
          · exact absurd (hR.symm.trans (hpeq.trans hd'.2))
              (fun hmem => h4 c hRca (hmem ▸ himg_t))
        · rcases hpostri c' hact' with ⟨hR', hRca'⟩ | hd' | hd'
          · by_cases hcpp : c' = p
            · exact hd.1.trans hcpp.symm
            · exact absurd (hR'.symm.trans (hpeq.symm.trans hd.2)) (h7 c' hcpp hRca')
          · exact hd.1.trans hd'.1.symm
          · exact absurd (hd.2.symm.trans (hpeq.trans hd'.2)) hpos_ne
        · rcases hpostri c' hact' with ⟨hR', hRca'⟩ | hd' | hd'
          · exact absurd (hR'.symm.trans (hpeq.symm.trans hd.2))
              (fun hmem => h4 c' hRca' (hmem ▸ himg_t))
          · exact absurd (hd'.2.symm.trans (hpeq.symm.trans hd.2)) hpos_ne
          · exact hd.1.trans hd'.1.symm
      -- H4': armed positions not in the remaining stream's positions
      have h4' : ∀ c, (getR R''' c).active = true →
          (getR R''' c).pos ∉ (rest.map (fun t' => N - t'.sa)) := by
        intro c hcact hx
        rcases hpostri c hcact with ⟨hR, hRca⟩ | hd | hd
        · exact h4r c hRca (by rw [← hR]; exact hx)
        · rw [hd.2] at hx
          exact hp_img hx
        · rw [hd.2] at hx
          obtain ⟨t', ht', heq⟩ := List.mem_map.mp hx
          exact htsa_rest (List.mem_map.mpr ⟨t', ht',
            by have h1 := htpos' t' ht'; have h2 := htsa; omega⟩)
      -- H5': out ++ E elements are not in the remaining positions
      have h5' : ∀ x ∈ out ++ E, x ∉ (rest.map (fun t' => N - t'.sa)) := by
        intro x hx xh
        rcases (List.mem_append.mp hx) with h | h
        · exact h5r x h xh
        · obtain ⟨c₁, _, _, hc₁act, _, hc₁pos⟩ := hEmem x h
          exact h4r c₁ hc₁act (by rw [← hc₁pos]; exact xh)
      -- H6': the new head position is not emitted yet
      have h6' : (N - t.sa) ∉ out ++ E := by
        intro hmem
        rcases (List.mem_append.mp hmem) with h | h
        · exact h5 (N - t.sa) h himg_t
        · obtain ⟨c₁, _, _, hc₁act, _, hc₁pos⟩ := hEmem _ h
          exact h4 c₁ hc₁act (by rw [← hc₁pos]; exact himg_t)
      -- H7': only t.c may hold the new head position
      have h7' : ∀ c, c ≠ t.c → (getR R''' c).active = true →
          (getR R''' c).pos ≠ N - t.sa := by
        intro c hct hcact hpos
        rcases hpostri c hcact with ⟨hR, hRca⟩ | hd | hd
        · exact h4 c hRca (by rw [← hR, hpos]; exact himg_t)
        · exact absurd (hd.2.symm.trans hpos) hpos_ne
        · exact absurd hd.1 hct
      -- apply the induction hypothesis
      exact ih t.c t.sa MAXINT R''' (out ++ E) hR'''len hrest_nd htsa_rest htsa htpos'
        hbnd' hout₁nd h2' h3' h4' h5' h6' h7' (fun _ _ => rfl)
    · -- same run: t.c = p
      rename_i hne0
      refine ih t.c t.sa (min m (t.lcp : Int)) R out hRlen hrest_nd ?_ htsa htpos' hbnd'
        h1 h2 h3 h4r h5r ?_ ?_ ?_
      · exact htsa_rest
      · intro hmem
        exact h5 (N - t.sa) hmem himg_t
      · intro c hc hcact hpos
        exact h4 c hcact (hpos ▸ himg_t)
      · intro hcact hpos
        exact absurd hpos (fun heq => h4 t.c hcact (heq ▸ himg_t))

/-! ## The stream theorem and O2_bounded -/

theorem scan_nodup (N : Nat) (ts : List Triple)
    (hnd : (ts.map Triple.sa).Nodup) (htpos : ∀ t ∈ ts, t.sa < N)
    (hbnd : ∀ t ∈ ts, (t.lcp : Int) ≤ MAXINT) : (scan N ts).Nodup := by
  cases ts with
  | nil => exact List.nodup_nil
  | cons t rest =>
    have hrest_nd : (rest.map Triple.sa).Nodup := by
      rw [List.map_cons] at hnd; rw [List.nodup_cons] at hnd; exact hnd.2
    have htsa_rest : t.sa ∉ (rest.map Triple.sa) := by
      rw [List.map_cons] at hnd; rw [List.nodup_cons] at hnd; exact hnd.1
    have htsa : t.sa < N := htpos t (by simp)
    have htpos' : ∀ t' ∈ rest, t'.sa < N := fun t' ht' => htpos t' (by simp [ht'])
    have hbnd' : ∀ t' ∈ rest, (t'.lcp : Int) ≤ MAXINT := fun t' ht' => hbnd t' (by simp [ht'])
    show (scanAux N rest t.c t.sa MAXINT defaultR []).Nodup
    have hdef : ∀ c, ¬ ((getR defaultR c).active = true) := by
      intro c hc; rw [getR_defaultR] at hc; simp at hc
    refine scanAux_nodup N rest t.c t.sa MAXINT defaultR [] ?_ hrest_nd htsa_rest htsa htpos'
      hbnd' List.nodup_nil ?_ ?_ ?_ ?_ ?_ ?_ ?_
    · unfold defaultR; simp
    · intro c hc; exact absurd hc (hdef c)
    · intro c c' hc hc' _; exact absurd hc (hdef c)
    · intro c hc; exact absurd hc (hdef c)
    · intro x hx; exact absurd hx (by simp)
    · simp
    · intro c _ hc; exact absurd hc (hdef c)
    · intro hc _; exact absurd hc (hdef t.c)

/-- The sa values of `triplesOf T` are pairwise distinct (they are exactly the
entries of `saOrder`, which is Nodup). -/
theorem triplesOf_sa_nodup (T : Text) : ((triplesOf T).map Triple.sa).Nodup := by
  have hmap : (triplesOf T).map Triple.sa
      = (List.range (saOrder (T.reverse ++ [0])).length).map
          (fun i => (saOrder (T.reverse ++ [0]))[i]!) := by
    unfold triplesOf
    simp only [List.map_map]
    rw [saOrder_length]
    congr 1
  rw [hmap]
  refine nodup_map_of_inj _ _ List.nodup_range ?_
  intro a ha b hb hab
  rw [List.mem_range] at ha hb
  have hlen : (saOrder (T.reverse ++ [0])).length = (T.reverse ++ [0]).length :=
    saOrder_length _
  have hae : (saOrder (T.reverse ++ [0]))[a]! = (saOrder (T.reverse ++ [0]))[a] :=
    getElem!_pos _ a (by omega)
  have hbe : (saOrder (T.reverse ++ [0]))[b]! = (saOrder (T.reverse ++ [0]))[b] :=
    getElem!_pos _ b (by omega)
  rw [hae, hbe] at hab
  exact (saOrder_nodup (T.reverse ++ [0])).getElem_inj.mp hab

/-- **O2_bounded, proven outright**: the statement-locked bounded successor of
the original text-only O2, now a theorem.  The `positive T` hypothesis of the
locked statement is not needed by the underlying argument (a strictly stronger
statement is proven); it is retained here so that the LOCKED statement is what
is proven, byte for byte. -/
theorem O2_bounded_true : SxgcBounds.O2_bounded := by
  unfold SxgcBounds.O2_bounded
  intro T hT hbnd
  have hsg : StreamGood (T.length + 1) (triplesOf T) := triplesOf_streamGood T hT
  have htpos : ∀ t ∈ triplesOf T, t.sa < T.length + 1 := by
    intro t ht
    have h1 := (hsg t ht).1
    omega
  have hbnd' : ∀ t ∈ triplesOf T, (t.lcp : Int) ≤ MAXINT := by
    intro t ht
    have h1 := hbnd t ht
    have h2 : (t.lcp : Int) ≤ (MAXINT.toNat : Int) := by exact_mod_cast h1
    have h3 : (MAXINT.toNat : Int) = MAXINT := by decide
    rwa [h3] at h2
  exact scan_nodup (T.length + 1) (triplesOf T) (triplesOf_sa_nodup T) htpos hbnd'

end SxgcNodup
