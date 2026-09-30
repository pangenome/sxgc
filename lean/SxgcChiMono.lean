import SxgcRevival

/-!
# Append and the number of maximal coverage classes

Exact target: `∀ T₁ T₂ : Text, chi T₁ ≤ chi (T₁ ++ T₂)`.
This file uses the finite-text definitions in `Sxgc`; `coversAt` does NOT
wrap. The unrestricted target is false (see `chi_append_not_monotone`).

Proposed dynamics lemma, stated before the attempt: choose a longest covered
requirement for each old maximal class. If these requirements remain requirements
of the extension, each has a covering new maximal class, and distinct old classes
must receive distinct new classes. Their words are suffix-incomparable, so a
single new position cannot cover two of them. This conditional dynamics statement
is proved below. The stronger claim that every lost requirement/class always has
compensation is false: a new requirement may strengthen an existing class without
adding a class. The explicit 4-to-3 example certifies that obstruction.
-/
namespace SxgcChiMono
open Sxgc SxgcRevival

/-- Statement lock for the exact unrestricted conjecture; not an axiom. -/
def ChiAppendMonotone : Prop :=
  ∀ T₁ T₂ : Text, chi T₁ ≤ chi (T₁ ++ T₂)

/-- Derived by following the loss of terminal contexts, not by an oracle search. -/
def lossText : Text := [1, 2, 3, 1, 3, 1, 2]
def lossAppend : Text := [3]

theorem chi_lossText : chi lossText = 4 := by decide
theorem chi_lossJoined : chi (lossText ++ lossAppend) = 3 := by decide

theorem chi_append_decreases : chi (lossText ++ lossAppend) < chi lossText := by
  rw [chi_lossText, chi_lossJoined]; decide

theorem chi_append_not_monotone : ¬ ChiAppendMonotone := by
  intro h
  have := h lossText lossAppend
  rw [chi_lossText, chi_lossJoined] at this
  omega

/-- The counterexample is inside the repository's positive alphabet domain. -/
theorem loss_positive : positive lossText = true ∧
    positive (lossText ++ lossAppend) = true := by decide

theorem loss_class_counts : maxClassCount lossText = 4 ∧
    maxClassCount (lossText ++ lossAppend) = 3 := by
  rw [← chi_eq_maxClasses lossText loss_positive.1,
    ← chi_eq_maxClasses (lossText ++ lossAppend) loss_positive.2]
  exact ⟨chi_lossText, chi_lossJoined⟩

/-- Even a joined cover wholly inside the old text need not cover the old text. -/
theorem local_cover_does_not_restrict :
    suffixient [4, 5, 7] (lossText ++ lossAppend) = true ∧
    (∀ x ∈ [4, 5, 7], x ∈ positionsT lossText) ∧
    suffixient [4, 5, 7] lossText = false := by
  refine ⟨by decide, ?_, by decide⟩
  intro x hx
  simp only [List.mem_cons, List.not_mem_nil, or_false] at hx
  rcases hx with rfl | rfl | rfl <;> decide

/-- No replacement of those three witnesses by three old witnesses can work. -/
theorem no_three_witness_repair (S : List Nat)
    (hm : ∀ x ∈ S, x ∈ positionsT lossText) (hl : S.length ≤ 3) :
    suffixient S lossText ≠ true := by
  intro hs
  have := chi_le_of_suffixient_mem lossText S hm hs
  rw [chi_lossText] at this
  omega

/-- The disappearing private requirement. Its context was an old suffix. -/
theorem lost_requirement :
    ([1, 2], 3) ∈ requirements lossText ∧
    ([1, 2], 3) ∉ requirements (lossText ++ lossAppend) := by decide

/-- Its new suffix extension only strengthens a pre-existing class. -/
theorem strengthening_requirement :
    ([1, 2, 3], 1) ∉ requirements lossText ∧
    ([1, 2, 3], 1) ∈ requirements (lossText ++ lossAppend) := by decide

/-- Position 3 was maximal; after append position 5 strictly dominates it. -/
theorem lost_class : IsMax lossText 3 ∧
    ScopeLe (lossText ++ lossAppend) 3 5 ∧
    ¬ ScopeLe (lossText ++ lossAppend) 5 3 := by
  refine ⟨isMax_of_all _ _ (by decide) (by decide),
    (scopeLe_iff_scopeLeB _ _ _).mpr (by decide), ?_⟩
  intro h
  have hb := (scopeLe_iff_scopeLeB _ _ _).mp h
  have hn : scopeLeB (lossText ++ lossAppend) 5 3 = false := by decide
  rw [hn] at hb
  contradiction

/-- Strengthening an old maximal class is not an increase in class count. -/
theorem strengthened_class : IsMax lossText 4 ∧
    IsMax (lossText ++ lossAppend) 4 :=
  ⟨isMax_of_all _ _ (by decide) (by decide),
   isMax_of_all _ _ (by decide) (by decide)⟩

/-- Coverage of an old position is literally unchanged as a word predicate. -/
theorem coversAt_append_old (A B : Text) (x : Nat) (hx : x ≤ A.length)
    (w : List Nat) : coversAt w x (A ++ B) = coversAt w x A := by
  unfold coversAt pref
  rw [List.take_append_of_le_length hx]

/-- A longest requirement of an old maximal class dominates its entire scope
at every position where that word is covered, even in a different text. -/
theorem scope_of_longest_word {A J : Text} {r x : Nat}
    {p : List Nat × Nat} (hp : p ∈ covSet A r)
    (hl : maxW A r ≤ wlen p)
    (hx : coversAt (p.1 ++ [p.2]) x J = true) :
    ∀ q ∈ covSet A r, coversAt (q.1 ++ [q.2]) x J = true := by
  intro q hq
  have hpc := ((mem_covSet A r p).mp hp).2
  have hqc := ((mem_covSet A r q).mp hq).2
  have hlen : (q.1 ++ [q.2]).length ≤ (p.1 ++ [p.2]).length := by
    simpa only [← wlen_congr] using Nat.le_trans (wlen_le_maxW A r q hq) hl
  exact coversAt_of_suffix hx (coversAt_suffix_of_coversAt hqc hpc hlen)

/-- Two old representatives whose longest words share ANY covering position
are the same representative. No membership of that position in A is needed. -/
theorem longest_words_disjoint {A J : Text} {r s x : Nat}
    (hr : IsRep A r) (hs : IsRep A s)
    {p q : List Nat × Nat} (hp : p ∈ covSet A r) (hq : q ∈ covSet A s)
    (hlp : maxW A r ≤ wlen p) (hlq : maxW A s ≤ wlen q)
    (hpx : coversAt (p.1 ++ [p.2]) x J = true)
    (hqx : coversAt (q.1 ++ [q.2]) x J = true) : r = s := by
  have comparable : ScopeLe A r s ∨ ScopeLe A s r := by
    rcases Nat.le_total (wlen p) (wlen q) with hle | hle
    · have hd := coversAt_suffix_of_coversAt hpx hqx
        (by simpa only [← wlen_congr] using hle)
      have hps := coversAt_of_suffix ((mem_covSet A s q).mp hq).2 hd
      exact Or.inl (fun z hz => (mem_covSet A s z).mpr
        ⟨((mem_covSet A r z).mp hz).1, scope_of_longest_word hp hlp hps z hz⟩)
    · have hd := coversAt_suffix_of_coversAt hqx hpx
        (by simpa only [← wlen_congr] using hle)
      have hqr := coversAt_of_suffix ((mem_covSet A r p).mp hp).2 hd
      exact Or.inr (fun z hz => (mem_covSet A r z).mpr
        ⟨((mem_covSet A s z).mp hz).1, scope_of_longest_word hq hlq hqr z hz⟩)
  have heq : ScopeLe A r s ∧ ScopeLe A s r := by
    rcases comparable with h | h
    · exact ⟨h, hr.2.1.2 s hs.1 h⟩
    · exact ⟨hs.2.1.2 r hr.1 h, h⟩
  rcases Nat.lt_trichotomy r s with hlt | he | hgt
  · exact False.elim (hs.2.2 r hr.1 hlt hr.2.1 ⟨heq.2, heq.1⟩)
  · exact he
  · exact False.elim (hr.2.2 s hs.1 hgt hs.2.1 heq)

/-- Conditional class dynamics: if each old class has a longest requirement
that survives, every new suffixient set has at least one position per old class.
This counts distinct classes, not the generally nonmonotone witness positions. -/
theorem maxClassCount_le_of_surviving_longest (A J : Text)
    (hpersist : ∀ r, IsRep A r → ∃ p ∈ covSet A r,
      maxW A r ≤ wlen p ∧ p ∈ requirements J)
    (S : List Nat) (hS : suffixient S J = true) :
    maxClassCount A ≤ S.length := by
  classical
  rw [maxClassCount_eq_reps]
  let D : Nat → Nat → Prop := fun r x => ∃ p ∈ covSet A r,
    maxW A r ≤ wlen p ∧ coversAt (p.1 ++ [p.2]) x J = true
  have rep_of_mem : ∀ r ∈ reps A, IsRep A r := by
    intro r hr
    exact of_decide_eq_true (List.mem_filter.mp hr).2
  refine count_le_of_disjoint_witnesses S (reps A) D
    ((nodup_positionsT A).filter _) ?_ ?_
  · intro r hr
    obtain ⟨p, hp, hl, hpJ⟩ := hpersist r (rep_of_mem r hr)
    obtain ⟨x, hx, hc⟩ := (suffixient_iff_cover S J).mp hS p hpJ
    exact ⟨x, hx, p, hp, hl, hc⟩
  · intro r hr s hs hne x hx hy
    obtain ⟨p, hp, hlp, hpx⟩ := hx
    obtain ⟨q, hq, hlq, hqx⟩ := hy
    exact hne (longest_words_disjoint (rep_of_mem r hr) (rep_of_mem s hs)
      hp hq hlp hlq hpx hqx)

/-- Requirements inclusion suffices, even for texts not related by append. -/
theorem chi_le_of_requirements_subset (A J : Text)
    (hA : positive A = true) (hJ : positive J = true)
    (hsub : ∀ p ∈ requirements A, p ∈ requirements J) : chi A ≤ chi J := by
  rw [chi_eq_maxClasses A hA, chi_eq_maxClasses J hJ, maxClassCount_eq_reps J]
  apply maxClassCount_le_of_surviving_longest A J _ (reps J) (reps_suffixient J)
  intro r hr
  obtain ⟨p, hp, hl⟩ := exists_maxW hr.2.1.1
  exact ⟨p, hp, hl, hsub p ((mem_covSet A r p).mp hp).1⟩

/-- A finite certificate of pairwise suffix-incomparable requirements gives a
lower bound on chi. Checking a certificate avoids powerset computation. -/
theorem chi_lower_certificate (T : Text) (hT : positive T = true)
    (P : List (List Nat × Nat))
    (hreq : ∀ i ∈ List.range P.length, P.getD i ([], 0) ∈ requirements T)
    (hanti : ∀ i ∈ List.range P.length, ∀ j ∈ List.range P.length, i ≠ j →
      let a := (P.getD i ([], 0)).1 ++ [(P.getD i ([], 0)).2]
      let b := (P.getD j ([], 0)).1 ++ [(P.getD j ([], 0)).2]
      a.length ≤ b.length → b.drop (b.length - a.length) ≠ a) :
    P.length ≤ chi T := by
  rw [chi_eq_maxClasses T hT, maxClassCount_eq_reps]
  let D : Nat → Nat → Prop := fun i x =>
    coversAt ((P.getD i ([], 0)).1 ++ [(P.getD i ([], 0)).2]) x T = true
  have h := count_le_of_disjoint_witnesses (reps T) (List.range P.length) D
    List.nodup_range
  simp only [List.length_range] at h
  apply h
  · intro i hi
    exact (suffixient_iff_cover (reps T) T).mp (reps_suffixient T)
      (P.getD i ([], 0)) (hreq i hi)
  · intro i hi j hj hne x hx hy
    let a := (P.getD i ([], 0)).1 ++ [(P.getD i ([], 0)).2]
    let b := (P.getD j ([], 0)).1 ++ [(P.getD j ([], 0)).2]
    rcases Nat.le_total a.length b.length with hab | hba
    · exact hanti i hi j hj hne hab (coversAt_suffix_of_coversAt hx hy hab)
    · exact hanti j hj i hi hne.symm hba (coversAt_suffix_of_coversAt hy hx hba)

/-- Document-aligned counterexample: every 30 is a separator, with no empty
or unterminated documents. The appended document has one content symbol. -/
def docLoss : Text := [1, 30, 3, 4, 30, 3, 30, 4, 30, 5, 1, 30]
def docAppend : Text := [3, SEP]

def docPrivate : List (List Nat × Nat) :=
  [([], 1), ([1, 30], 3), ([30, 3], 4), ([4, 30], 3),
   ([30, 3], 30), ([30], 4), ([4, 30], 5)]

set_option maxRecDepth 100000 in
set_option maxHeartbeats 4000000 in
theorem chi_docLoss : chi docLoss = 7 := by
  apply Nat.le_antisymm
  · exact chi_le_of_suffixient_mem docLoss [1, 3, 4, 6, 7, 8, 10]
      (by decide) (by decide)
  · exact chi_lower_certificate docLoss (by decide) docPrivate (by decide) (by decide)

set_option maxRecDepth 100000 in
set_option maxHeartbeats 4000000 in
theorem chi_docJoined_le : chi (docLoss ++ docAppend) ≤ 6 :=
  chi_le_of_suffixient_mem (docLoss ++ docAppend) [1, 4, 6, 8, 10, 14]
    (by decide) (by decide)

/-- Banked refutation even for a SEP-aligned single-symbol-document append. -/
theorem chi_single_symbol_document_decreases :
    chi (docLoss ++ [3, SEP]) < chi docLoss := by
  have := chi_docJoined_le
  change chi (docLoss ++ [3, SEP]) ≤ 6 at this
  rw [chi_docLoss]
  omega

set_option maxRecDepth 100000 in
set_option maxHeartbeats 4000000 in
theorem chi_docJoined : chi (docLoss ++ docAppend) = 6 := by
  apply Nat.le_antisymm chi_docJoined_le
  exact chi_lower_certificate (docLoss ++ docAppend) (by decide)
    [([], 1), ([1, 30, 3], 4), ([4, 30], 3), ([30, 3, 30], 4),
     ([4, 30], 5), ([1, 30, 3], 30)] (by decide) (by decide)

/-- A prefix-extension counterexample, where the appended document is also
literally a prefix of the old text. Both chunks remain document-aligned. -/
def prefixLoss : Text := [3, SEP] ++ docLoss

set_option maxRecDepth 100000 in
set_option maxHeartbeats 4000000 in
theorem chi_prefixLoss : chi prefixLoss = 7 := by
  apply Nat.le_antisymm
  · exact chi_le_of_suffixient_mem prefixLoss [3, 5, 6, 8, 9, 10, 12]
      (by decide) (by decide)
  · exact chi_lower_certificate prefixLoss (by decide)
      [([3, 30], 1), ([1, 30], 3), ([30, 3], 4), ([4, 30], 3),
       ([30, 3], 30), ([3, 30], 4), ([4, 30], 5)] (by decide) (by decide)

set_option maxRecDepth 100000 in
set_option maxHeartbeats 4000000 in
theorem chi_prefixJoined : chi (prefixLoss ++ [3, SEP]) = 6 := by
  apply Nat.le_antisymm
  · exact chi_le_of_suffixient_mem (prefixLoss ++ [3, SEP]) [3, 6, 8, 10, 12, 16]
      (by decide) (by decide)
  · exact chi_lower_certificate (prefixLoss ++ [3, SEP]) (by decide)
      [([3, 30], 1), ([1, 30, 3], 4), ([4, 30], 3), ([30, 3, 30], 4),
       ([4, 30], 5), ([1, 30, 3], 30)] (by decide) (by decide)

theorem chi_prefix_extension_decreases :
    ([3, SEP] <+: prefixLoss) ∧ chi (prefixLoss ++ [3, SEP]) < chi prefixLoss := by
  refine ⟨⟨docLoss, rfl⟩, ?_⟩
  rw [chi_prefixLoss, chi_prefixJoined]; decide

/-- Occurrences survive finite append. -/
theorem occurs_append (A B w : Text) (hw : occurs w A = true) :
    occurs w (A ++ B) = true := by
  by_cases he : w = []
  · subst w; simp [occurs]
  obtain ⟨i, hi, heq⟩ := (occurs_eq_true w A he).mp hw
  apply (occurs_eq_true w (A ++ B) he).mpr
  refine ⟨i, by simp only [List.length_append]; omega, ?_⟩
  rw [List.drop_append, List.take_append_of_le_length]
  · exact heq
  · simp only [List.length_drop]; omega

/-- The enumerated substring list retains every old substring. -/
theorem subStrings_append (A B w : Text) (hw : w ∈ subStrings A) :
    w ∈ subStrings (A ++ B) := by
  induction A with
  | nil =>
    have he : w = [] := by simpa [subStrings] using hw
    subst w
    cases B <;> simp [subStrings]
  | cons a A ih =>
    simp only [subStrings, List.cons_append, List.mem_cons, List.mem_append, List.mem_map] at hw
    rcases hw with he | ⟨i, hi, he⟩ | hw
    · subst w; simp [subStrings]
    · have hib : i + 1 ≤ (a :: A).length := by
        simp only [List.mem_range] at hi; omega
      have hie : i ∈ List.range (a :: (A ++ B)).length := by
        simp only [List.mem_range, List.length_cons, List.length_append] at hi ⊢
        omega
      have ht : (a :: (A ++ B)).take (i + 1) = (a :: A).take (i + 1) :=
        List.take_append_of_le_length hib
      simp only [List.cons_append, subStrings, List.mem_cons, List.mem_append, List.mem_map]
      exact Or.inr (Or.inl ⟨i, hie, ht.trans he⟩)
    · simp only [List.cons_append, subStrings, List.mem_cons, List.mem_append]
      exact Or.inr (Or.inr (ih hw))

theorem dedupAux_nodup (seen xs : List Nat) : (dedup.dedupAux seen xs).Nodup := by
  induction xs generalizing seen with
  | nil => simp [dedup.dedupAux]
  | cons a xs ih =>
    simp only [dedup.dedupAux]
    split
    · exact ih seen
    · rw [List.nodup_cons]
      exact ⟨fun h => (mem_dedupAux (a :: seen) xs a).mp h |>.2 (by simp), ih _⟩

theorem rightExts_nodup (A w : Text) : (rightExts w A).Nodup :=
  dedupAux_nodup [] _

theorem rightExts_append_subset (A B w : Text) :
    ∀ c ∈ rightExts w A, c ∈ rightExts w (A ++ B) := by
  intro c hc
  exact (mem_rightExts w c (A ++ B)).mpr
    (occurs_append A B _ ((mem_rightExts w c A).mp hc))

theorem rightExts_length_append (A B w : Text) :
    (rightExts w A).length ≤ (rightExts w (A ++ B)).length :=
  nodup_length_le_of_subset (rightExts_nodup A w) (rightExts_append_subset A B w)

/-- A lost old requirement MUST have used terminal-suffix maximality and
at most one right extension. This is the exact finite-text obstruction. -/
theorem lost_requirement_is_terminal (A B : Text) (w : Text) (c : Nat)
    (hp : (w, c) ∈ requirements A) (hn : (w, c) ∉ requirements (A ++ B)) :
    w ≠ [] ∧ rightMaximal.isSuffix w A = true ∧ (rightExts w A).length < 2 := by
  obtain ⟨hs, hr, hc⟩ := (mem_requirements w c A).mp hp
  have hnr : rightMaximal w (A ++ B) ≠ true := by
    intro hrJ
    exact hn ((mem_requirements w c (A ++ B)).mpr
      ⟨subStrings_append A B w hs, hrJ, rightExts_append_subset A B w c hc⟩)
  cases w with
  | nil => exact False.elim (hnr rfl)
  | cons a w =>
    simp only [rightMaximal, Bool.and_eq_true, Bool.or_eq_true, decide_eq_true_eq] at hr
    have ho := occurs_append A B (a :: w) hr.1
    have hlen : (rightExts (a :: w) A).length < 2 := by
      apply Classical.byContradiction
      intro hge
      have he : 2 ≤ (rightExts (a :: w) (A ++ B)).length :=
        Nat.le_trans (by omega) (rightExts_length_append A B (a :: w))
      apply hnr
      simp only [rightMaximal, Bool.and_eq_true, Bool.or_eq_true, decide_eq_true_eq]
      exact ⟨ho, Or.inr he⟩
    refine ⟨by simp, ?_, hlen⟩
    rcases hr.2 with ht | ht
    · exact ht
    · omega

/-- Appending a fresh leading symbol makes every old suffix requirement
branch instead of losing right-maximality. The remainder is arbitrary. -/
theorem requirements_append_fresh (A B : Text) (d : Nat) (hd : d ∉ A) :
    ∀ p ∈ requirements A, p ∈ requirements (A ++ d :: B) := by
  intro p hp
  apply Classical.byContradiction
  intro hn
  obtain ⟨hw, hterm, hsmall⟩ := lost_requirement_is_terminal A (d :: B) p.1 p.2 hp hn
  have hsplit := suffix_of_isSuffix p.1 A hterm
  obtain ⟨pre, heq⟩ := hsplit
  have hnew : occurs (p.1 ++ [d]) (A ++ d :: B) = true := by
    rw [heq]
    apply (occurs_eq_true _ _ (by simp)).mpr
    refine ⟨pre.length, by simp, ?_⟩
    simp only [List.append_assoc, List.drop_left, List.length_append, List.length_singleton]
    rw [List.take_append, List.take_of_length_le (by omega)]
    simp
  have hdext := (mem_rightExts p.1 d (A ++ d :: B)).mpr hnew
  have hcext := rightExts_append_subset A (d :: B) p.1 p.2
    ((mem_requirements p.1 p.2 A).mp hp).2.2
  have hcd : p.2 ≠ d := by
    intro he
    have hm := occurs_append_last p.1 p.2 A
      ((mem_rightExts p.1 p.2 A).mp ((mem_requirements p.1 p.2 A).mp hp).2.2)
    exact hd (he ▸ hm)
  have hlen : 2 ≤ (rightExts p.1 (A ++ d :: B)).length := by
    cases he : rightExts p.1 (A ++ d :: B) with
    | nil => simp [he] at hdext
    | cons a xs =>
      cases xs with
      | nil => simp only [he, List.mem_singleton] at hdext hcext; omega
      | cons b xs => simp
  have hocc : occurs p.1 A = true := by
    cases he : p.1 with
    | nil => contradiction
    | cons a xs =>
      have hh := ((mem_requirements p.1 p.2 A).mp hp).2.1
      simp only [he, rightMaximal, Bool.and_eq_true] at hh
      exact hh.1
  have hr : rightMaximal p.1 (A ++ d :: B) = true := by
    cases he : p.1 with
    | nil => rfl
    | cons a xs =>
      simp only [rightMaximal, Bool.and_eq_true, Bool.or_eq_true, decide_eq_true_eq]
      constructor
      · simpa only [he] using occurs_append A (d :: B) p.1 hocc
      · exact Or.inr (by simpa only [he] using hlen)
  exact hn ((mem_requirements p.1 p.2 _).mpr
    ⟨subStrings_append A (d :: B) p.1 ((mem_requirements p.1 p.2 A).mp hp).1,
     hr, hcext⟩)

/-- Banked special case: fresh leading appended symbol; its tail may contain
old symbols, including the shared document separator. -/
theorem chi_append_fresh (A B : Text) (d : Nat)
    (hA : positive A = true) (hJ : positive (A ++ d :: B) = true) (hd : d ∉ A) :
    chi A ≤ chi (A ++ d :: B) :=
  chi_le_of_requirements_subset A _ hA hJ (requirements_append_fresh A B d hd)

/-- Nonempty appended texts over a completely disjoint alphabet satisfy
monotonicity; the empty append is equality. -/
theorem chi_append_disjoint (A B : Text)
    (hA : positive A = true) (hJ : positive (A ++ B) = true)
    (hdisjoint : ∀ c ∈ A, c ∉ B) : chi A ≤ chi (A ++ B) := by
  cases B with
  | nil => simp
  | cons d B =>
    exact chi_append_fresh A B d hA hJ
      (fun hd => hdisjoint d hd (by simp))

/-- SEP-separated content alphabets may share SEP. For a nonempty first
appended document, its leading content symbol is fresh, so the count cannot
fall. The rest of the appended documents may contain the shared separator. -/
theorem chi_append_disjoint_content (A B : Text) (d : Nat)
    (hA : positive A = true) (hJ : positive (A ++ d :: B) = true)
    (hd : d ≠ SEP)
    (hdisjoint : ∀ c ∈ A, c ∈ d :: B → c = SEP) :
    chi A ≤ chi (A ++ d :: B) := by
  apply chi_append_fresh A B d hA hJ
  intro hm
  exact hd (hdisjoint d hm (by simp))

/-- A fresh single-symbol document is a positive subcase of (a), although
arbitrary single-symbol-document append is refuted above. -/
theorem chi_append_fresh_document (A : Text) (c : Nat)
    (hA : positive A = true) (hJ : positive (A ++ [c, SEP]) = true)
    (hc : c ∉ A) : chi A ≤ chi (A ++ [c, SEP]) :=
  chi_append_fresh A [SEP] c hA hJ hc

/-- If the old text also occurs as a suffix of its extension, every old
terminal context remains terminal, so no old requirement is lost. -/
theorem requirements_append_of_suffix (A B : Text)
    (hborder : SuffixOf A (A ++ B)) :
    ∀ p ∈ requirements A, p ∈ requirements (A ++ B) := by
  intro p hp
  obtain ⟨hsub, hr, hc⟩ := (mem_requirements p.1 p.2 A).mp hp
  refine (mem_requirements p.1 p.2 (A ++ B)).mpr
    ⟨subStrings_append A B p.1 hsub, ?_, rightExts_append_subset A B p.1 p.2 hc⟩
  cases he : p.1 with
  | nil => rfl
  | cons a w =>
    simp only [he, rightMaximal, Bool.and_eq_true, Bool.or_eq_true, decide_eq_true_eq] at hr ⊢
    refine ⟨occurs_append A B _ hr.1, ?_⟩
    rcases hr.2 with ht | hb
    · obtain ⟨pre, heq⟩ := suffix_of_isSuffix (a :: w) A ht
      obtain ⟨left, hleft⟩ := hborder
      have hdecomp : A ++ B = (left ++ pre) ++ (a :: w) := by
        rw [hleft, heq, List.append_assoc]
      apply Or.inl
      unfold rightMaximal.isSuffix
      rw [hdecomp, List.length_append, Nat.add_sub_cancel, List.drop_left]
      simp only [beq_self_eq_true, Bool.and_true, decide_eq_true_eq]
      omega
    · exact Or.inr (Nat.le_trans hb (rightExts_length_append A B _))

/-- A banked border-preserving append theorem, distinct from the false
claim that appending any prefix of A always preserves the count. -/
theorem chi_append_of_suffix (A B : Text)
    (hA : positive A = true) (hJ : positive (A ++ B) = true)
    (hborder : SuffixOf A (A ++ B)) : chi A ≤ chi (A ++ B) :=
  chi_le_of_requirements_subset A _ hA hJ (requirements_append_of_suffix A B hborder)

/-- In particular, doubling a positive text cannot decrease its count. -/
theorem chi_append_self (A : Text) (hA : positive A = true) :
    chi A ≤ chi (A ++ A) := by
  have hJ : positive (A ++ A) = true := by simpa [positive] using hA
  exact chi_append_of_suffix A A hA hJ ⟨A, rfl⟩

end SxgcChiMono
