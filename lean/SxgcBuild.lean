import Sxgc
/-!
# SxgcBuild — the parse-space χ/sA construction as executable, provable Lean

The user's directive (2026-09-26): "a reference implementation in Lean so we
can prove things about it."  This file is that reference implementation, in
miniature, of the **slim build**: χ/sA constructed from a *parse* of the text
(phrase dictionary + phrase-id sequence) plus LCE queries, never from a
materialized LCP array.

Layered like the real system (see RESEARCH.md 2026-09-26 entries, in
particular correction #3 — `lce_support` is Θ(P + dict)):

1. **Parse model** (`Parse`, `ParseWF`, `parseOfW`): the text decomposed into
   non-overlapping phrases from a *distinct* dictionary, consumed in sequence.
   The model TAKES the parse as given (as `Sxgc` takes the `(bwt,lcp,sa)`
   stream as given); `parseOfW` builds a window parse for TESTING only.
   pfp++'s w-window overlap is an implementation convention; the composition
   structure is identical and the model uses the cleaner non-overlapping grid.

2. **`textLCEvia`** — the LCE primitive of the slim build, mirroring
   `pfpds::pfp_lce_support` (`lce_support.hpp`): tail compare inside the
   current phrases (the dictionary side), then the phrase-id extension
   (the parse side), then the first-differing-phrase compare (dictionary
   side again).  One deliberate CORRECTNESS FIX over the ported C++
   structure: the "first differing phrase is a strict prefix of the other"
   case falls through to a shifted query instead of stopping at the prefix
   length (the C++ returns `k + l_com + lcp_a_b` and stops; see HANDOFF).
   THE COMPOSITION THEOREM: `textLCEvia = lcpOf ∘ drop` — the parse-side
   query returns the true text LCE.

3. **Construction** (`parseTriplesOf`, `parseScan`): the triples stream of the
   reverse text with the `lcp` column produced by `textLCEvia` instead of
   brute `lcpOf`.  Capstone: `parseScan_eq` — the parse-based construction IS
   the one-pass scan; χ statements about `scan` then apply verbatim
   (conditional capstone `parseChi_of_obligations` instantiates the O1–O4
   reduction theorems; the unconditional scan≡χ equivalence remains the
   named open obligation, exactly as in `Sxgc`).

4. **Fingerprint LCE** (`krs`, `chunkHash`, `fpParseLCE`): the sampled
   Karp-Rabin scheme on the phrase-id sequence — grid-aligned chunk hashes
   from τ-spaced suffix hashes, O(1) per jump, direct reads for the ≤τ
   boundary.  Deterministic statements only: `VerifiedLCE` (two-sided, no
   hash assumption), and `fpParseLCE_correct` under the explicit
   no-false-equality hypothesis `chunkHashGood` (verified per-instance by
   eval).  Step counts are explicit (`fpSteps`, `textLCESteps`) with proven
   O-bounds.

5. **Executable gates** (`#eval`): battery agreement including a
   duplicates-heavy family (the duplicates text has caught two false greens),
   fingerprint-vs-brute LCE differentials, and step-count sanity.

Proof hygiene: NO `native_decide` (house rule: flagship theorems admit only
`propext`/`Classical.choice`/`Quot.sound`); no modification to `Sxgc.lean`.
-/


open Sxgc

namespace SxgcBuild

/-! ## Section 0: the parse model -/

/-- A parse: a phrase dictionary (id → phrase string) and the parse proper
(the id sequence, one id per phrase occurrence, in text order). -/
structure Parse where
  dict : List (List Nat)   -- dictionary: id ↦ phrase (nonempty, distinct)
  ids  : List Nat          -- parse: occurrence k uses dict[ids[k]]
  deriving Repr

namespace Parse

/-- The phrase string of occurrence `k` (empty if out of range). -/
@[simp] def chunkAt (P : Parse) (k : Nat) : List Nat :=
  P.dict.getD (P.ids.getD k 0) []

/-- Occurrence count. -/
def count (P : Parse) : Nat := P.ids.length

/-- The phrase strings in text order (occurrence k gives element k). -/
def chunkList (P : Parse) : List (List Nat) := P.ids.map (fun id => P.dict.getD id [])

/-- The concatenated text of the parse (chunks in order). -/
def parseText (P : Parse) : Text := (P.chunkList).flatten

/-- Cumulative start: text position where occurrence `k` starts
(`startAt P 0 = 0`, `startAt P (k+1) = startAt P k + length of chunk k`). -/
def startAt (P : Parse) : Nat → Nat
  | 0 => 0
  | k+1 => startAt P k + (P.chunkAt k).length


/-- The id of occurrence `k` (0 if out of range). -/
def idAt (P : Parse) (k : Nat) : Nat := P.ids.getD k 0

/-- Wellformedness: the parse is a partition of `T` into nonempty phrases of
a DISTINCT dictionary, all ids valid.  Distinctness mirrors pfp++ (the
dictionary is the set of distinct phrases) and licenses
`idAt equality ↔ phrase-string equality`, which the walk uses. -/
def ParseWF (P : Parse) (T : Text) : Prop :=
  parseText P = T ∧
  P.ids.all (fun id => id < P.dict.length) ∧
  P.dict.all (fun c => c ≠ []) ∧
  P.dict.Nodup

/-- Find the occurrence containing text position `i`: returns `(k, o)` with
`startAt k + o = i` and `o < length of chunk k`; none when `i` is past the
parse.  Defined as a decidable search (the implementation's phrase walk is a
detail; the relation is what the theorems need). -/
def phraseAt (P : Parse) (i : Nat) : Option (Nat × Nat) :=
  match (List.range P.count).filter
      (fun k => P.startAt k ≤ i ∧ i < P.startAt k + (P.chunkAt k).length) with
  | [] => none
  | k :: _ => some (k, i - P.startAt k)

end Parse

open Parse

/-! ## Section 1: `lcpOf` append lemmas (the composition engine) -/

/-- KEY LEMMA: comparing `x ++ u` against any `y` — the LCP either stops
inside `x`, or consumes all of `x` and continues between `u` and the
remainder of `y` past the consumed prefix. -/
theorem lcpOf_append_left (x u y : List Nat) :
    lcpOf (x ++ u) y
      = lcpOf x y +
        (if lcpOf x y < x.length then 0
         else lcpOf u (y.drop (lcpOf x y))) := by
  induction x generalizing y with
  | nil => simp [lcpOf]
  | cons a as ih =>
      cases y with
      | nil => simp [lcpOf]
      | cons b bs =>
          by_cases hab : a = b
          · subst hab
            simp only [List.cons_append, lcpOf, beq_self_eq_true, ite_true]
            rw [ih bs]
            simp only [List.length_cons]
            have hdrop : ∀ (a : Nat) (bs : List Nat) (n : Nat),
                (a :: bs).drop (n+1) = bs.drop n := by
              intro a bs n; cases n <;> simp
            have hd : (a :: bs).drop (1 + lcpOf as bs) = bs.drop (lcpOf as bs) := by
              rw [Nat.add_comm 1 (lcpOf as bs), hdrop]
            split <;> rename_i h1 <;> split <;> rename_i h2 <;> first
            | (omega) | (rw [hd]; omega) | (omega)
          · simp [lcpOf, hab]

/-! ## Section 2: parse infrastructure -/

theorem take_len_self : ∀ (l : List Nat), l.take l.length = l := by
  intro l; induction l with
  | nil => rfl
  | cons a rest ih => simp [ih]

theorem lcpOf_symm : ∀ (a b : List Nat), lcpOf a b = lcpOf b a := by
  intro a
  induction a with
  | nil => intro b; cases b <;> simp [lcpOf]
  | cons x xs ih =>
      intro b
      cases b with
      | nil => simp [lcpOf]
      | cons y ys =>
          by_cases h : x = y
          · subst h; simp [lcpOf, ih]
          · have e1 : lcpOf (x :: xs) (y :: ys) = 0 := by
              simp [lcpOf, h]
            have e2 : lcpOf (y :: ys) (x :: xs) = 0 := by
              simp [lcpOf, Ne.symm h]
            rw [e1, e2]

/-- a list that appears as `y.take x.length` has full LCP with `y`. -/
theorem lcpOf_of_take (x y : List Nat) (h : y.take x.length = x)
    (hle : x.length ≤ y.length) : lcpOf x y = x.length := by
  have h2 : y.take x.length = x.take x.length := by rw [take_len_self]; exact h
  have h3 := (take_eq_iff_le_lcpOf y x x.length ⟨hle, Nat.le_refl _⟩).mp h2
  have h4 : x.length ≤ lcpOf x y := by rw [lcpOf_symm]; omega
  have h5 := lcpOf_le_left x y
  omega

/-- the full-match converse: `lcpOf x y = x.length` pins the prefix. -/
theorem take_of_lcpOf_eq (x y : List Nat) (h : lcpOf x y = x.length)
    (hle : x.length ≤ y.length) : y.take x.length = x := by
  have h3 := (take_eq_iff_le_lcpOf x y x.length ⟨Nat.le_refl _, hle⟩).mpr (by omega)
  have h4 : x.take x.length = y.take x.length := h3
  rw [take_len_self] at h4
  exact h4.symm

/-- divergence inside both: appending to the right argument changes nothing. -/
theorem lcpOf_diverge (x y v : List Nat)
    (hx : lcpOf x y < x.length) (hy : lcpOf x y < y.length) :
    lcpOf x (y ++ v) = lcpOf x y := by
  have hsy : lcpOf y x = lcpOf x y := lcpOf_symm y x
  have h1 : lcpOf (y ++ v) x = lcpOf y x := by
    rw [lcpOf_append_left y v x, if_pos (by rw [← hsy] at hy; omega)]
    omega
  rw [lcpOf_symm, h1, hsy]

theorem startAt_zero (P : Parse) : P.startAt 0 = 0 := rfl

theorem startAt_succ (P : Parse) (k : Nat) :
    P.startAt (k+1) = P.startAt k + (P.chunkAt k).length := by rfl

theorem startAt_mono (P : Parse) : ∀ k k', k ≤ k' → P.startAt k ≤ P.startAt k' := by
  intro k k' hk
  induction k' generalizing k with
  | zero =>
      cases k with
      | zero => omega
      | succ n => omega
  | succ m ih =>
      rcases Nat.lt_or_ge k (m+1) with h | h
      · have hkm : k ≤ m := by omega
        have h1 := ih k hkm
        have h2 := startAt_succ P m
        omega
      · have hke : k = m+1 := Nat.le_antisymm hk h
        subst hke
        omega

/-- getD injectivity for Nodup lists (dictionary lookup). -/
theorem getD_inj_of_nodup {α : Type} {l : List α} (d : α) :
    ∀ (i j : Nat), l.Nodup → i < l.length → j < l.length →
      l.getD i d = l.getD j d → i = j := by
  induction l with
  | nil => intro i j _ hi hj; simp at hi
  | cons a rest ih =>
      intro i j hnd hi hj hget
      rw [List.nodup_cons] at hnd
      obtain ⟨hni, hnr⟩ := hnd
      cases i with
      | zero =>
          cases j with
          | zero => rfl
          | succ k =>
              have hred : a = List.getD rest k d := hget
              have hmem : a ∈ rest := by
                rw [hred]; exact getD_mem rest k d (by simpa using hj)
              exact absurd hmem hni
      | succ m =>
          cases j with
          | zero =>
              have hred : List.getD rest m d = a := hget
              have hmem : a ∈ rest := by
                rw [← hred]; exact getD_mem rest m d (by simpa using hi)
              exact absurd hmem hni
          | succ k =>
              have hred : List.getD rest m d = List.getD rest k d := hget
              have := ih m k hnr (by simpa using hi) (by simpa using hj) hred
              simpa using this

/-! ### Section 2.5: the shifted parse (tail recursion structure) -/

/-- The parse of the text after the first phrase occurrence. -/
def shiftParse (P : Parse) : Parse := { dict := P.dict, ids := P.ids.drop 1 }

theorem count_shift (P : Parse) : (shiftParse P).count = P.count - 1 := by
  simp [Parse.count, shiftParse, List.length_drop]

theorem getD_drop_succ : ∀ (l : List Nat) (k : Nat) (d : Nat),
    (l.drop 1).getD k d = l.getD (k+1) d := by
  intro l
  match l with
  | [] => intro k d; cases k <;> rfl
  | x :: xs => intro k d; cases k <;> rfl

theorem chunkAt_shift (P : Parse) (k : Nat) : (shiftParse P).chunkAt k = P.chunkAt (k+1) := by
  simp [Parse.chunkAt, shiftParse, getD_drop_succ]

theorem startAt_shift (P : Parse) :
    ∀ k, (shiftParse P).startAt k + (P.chunkAt 0).length = P.startAt (k+1) := by
  intro k
  induction k with
  | zero => rfl
  | succ m ih =>
      have h1 : (shiftParse P).startAt (m+1)
          = (shiftParse P).startAt m + ((shiftParse P).chunkAt m).length := by rfl
      rw [chunkAt_shift] at h1
      have h2 : P.startAt (m+2) = P.startAt (m+1) + (P.chunkAt (m+1)).length := by rfl
      show (shiftParse P).startAt (m+1) + (P.chunkAt 0).length = P.startAt (m+2)
      omega

theorem shift_WF (P : Parse) (T : Text) (hWF : ParseWF P T) (h0 : P.count ≠ 0) :
    ParseWF (shiftParse P) (T.drop (P.chunkAt 0).length) := by
  obtain ⟨hpt, hall, hne, hnd⟩ := hWF
  match heq : P.ids with
  | [] =>
      have hc0 : P.count = 0 := by simp [Parse.count, heq]
      exact absurd hc0 h0
  | x :: xs =>
      have hcl : P.chunkList = P.chunkAt 0 :: (xs.map (fun id => P.dict.getD id [])) := by
        simp [Parse.chunkList, heq]
      have hpt2 : P.parseText
          = P.chunkAt 0 ++ (xs.map (fun id => P.dict.getD id [])).flatten := by
        simp [Parse.parseText, hcl]
      refine ⟨?_, ?_, hne, hnd⟩
      · have h1 : (shiftParse P).parseText = (P.chunkList.drop 1).flatten := by
          simp [Parse.parseText, shiftParse, Parse.chunkList, List.map_drop]
        have h2 : P.chunkList.drop 1 = xs.map (fun id => P.dict.getD id []) := by
          simp [hcl]
        rw [h1, h2, ← hpt, hpt2]
        simp [List.drop_append]
      · have hall' := List.all_eq_true.mp hall
        refine List.all_eq_true.mpr (fun id hid => hall' id ?_)
        simp only [shiftParse] at hid
        exact List.mem_of_mem_drop hid

/-! ### Section 2.6: suffix decomposition and phrase location -/

theorem drop_len_nil : ∀ {α : Type} (l : List α), l.drop l.length = [] := by
  intro α l; induction l with
  | nil => rfl
  | cons a rest ih => simp [List.drop, ih]

theorem count_len_map (P : Parse) : P.chunkList.length = P.count := by
  simp [Parse.count, Parse.chunkList]

theorem chunkList_drop_cons (P : Parse) : ∀ (k : Nat), k < P.count →
    ∃ r, P.chunkList.drop k = P.chunkAt k :: r := by
  intro k
  match heq : P.ids with
  | [] => intro hk; simp [Parse.count, heq] at hk
  | x :: xs =>
      cases k with
      | zero =>
          intro _
          refine ⟨xs.map (fun id => P.dict.getD id []), ?_⟩
          simp [Parse.chunkList, Parse.chunkAt, heq, List.drop]
      | succ m =>
          intro hk
          have hk' : m < (shiftParse P).count := by
            have hk2 : m + 1 < (x :: xs).length := by rw [← heq]; exact hk
            simp [Parse.count, shiftParse, heq, List.length_drop] at hk2 ⊢
            omega
          obtain ⟨r, hr⟩ := chunkList_drop_cons (shiftParse P) m hk'
          have h1 : P.chunkList = (x :: xs).map (fun id => P.dict.getD id []) := by
            simp only [Parse.chunkList, heq]
          have h2 : (shiftParse P).chunkList = xs.map (fun id => P.dict.getD id []) := by
            simp only [shiftParse, Parse.chunkList, heq, List.drop]
          rw [h2] at hr
          refine ⟨r, ?_⟩
          rw [h1]
          simp only [List.map, List.drop]
          rw [← chunkAt_shift]
          exact hr

/-- the suffix at a phrase start is the flatten of the remaining chunks. -/
theorem parse_drop (P : Parse) (T : Text) (hWF : ParseWF P T) : ∀ k ≤ P.count,
    T.drop (P.startAt k) = (P.chunkList.drop k).flatten := by
  intro k hk
  induction k with
  | zero =>
      have hz : T.drop (P.startAt 0) = P.parseText := by
        rw [hWF.1]; show List.drop 0 T = T; rfl
      rw [hz]
      rfl
  | succ m ih =>
      have hm : m ≤ P.count := by omega
      have hmc : m < P.count := by omega
      obtain ⟨r, hr⟩ := chunkList_drop_cons P m hmc
      have hdrop1 : P.chunkList.drop m = P.chunkAt m :: r := hr
      have hnext : P.chunkList.drop (m+1) = r := by
        rw [← List.drop_drop, hdrop1]; simp [List.drop]
      have hih : T.drop (P.startAt m) = (P.chunkList.drop m).flatten := ih hm
      have hstep : P.startAt (m+1) = P.startAt m + (P.chunkAt m).length := by rfl
      have hdd2 : T.drop (P.startAt (m+1))
          = (T.drop (P.startAt m)).drop (P.chunkAt m).length := by
        rw [hstep, ← List.drop_drop]
      have hL : (P.chunkAt m).length - (P.chunkAt m).length = 0 := by omega
      have hflat : (P.chunkAt m :: r).flatten = P.chunkAt m ++ r.flatten := rfl
      have hc : (P.chunkAt m).drop (P.chunkAt m).length = [] := drop_len_nil _
      rw [hnext, hdd2, hih, hdrop1, hflat, List.drop_append, hL, hc]
      simp [List.drop]

/-- total length bookkeeping. -/
theorem startAt_add (P : Parse) (T : Text) (hWF : ParseWF P T) : ∀ k ≤ P.count,
    P.startAt k + (P.chunkList.drop k).flatten.length = T.length := by
  intro k hk
  induction k with
  | zero =>
      have h0 : P.startAt 0 = 0 := rfl
      have hd0 : P.chunkList.drop 0 = P.chunkList := rfl
      have hz : P.startAt 0 + (P.chunkList.drop 0).flatten.length = P.parseText.length := by
        rw [h0, Nat.zero_add, hd0]; rfl
      rw [hz, hWF.1]
  | succ m ih =>
      have hm : m ≤ P.count := by omega
      have hmc : m < P.count := by omega
      obtain ⟨r, hr⟩ := chunkList_drop_cons P m hmc
      have hdrop1 : P.chunkList.drop m = P.chunkAt m :: r := hr
      have hnext : P.chunkList.drop (m+1) = r := by
        rw [← List.drop_drop, hdrop1]; simp [List.drop]
      have hflat : (P.chunkList.drop m).flatten.length
          = (P.chunkAt m).length + r.flatten.length := by
        rw [hdrop1]; simp
      have hih : P.startAt m + (P.chunkList.drop m).flatten.length = T.length := ih hm
      have hstep : P.startAt (m+1) = P.startAt m + (P.chunkAt m).length := by rfl
      rw [hnext]
      rw [hflat] at hih
      omega

theorem startAt_count (P : Parse) (T : Text) (hWF : ParseWF P T) :
    P.startAt P.count = T.length := by
  have h1 := startAt_add P T hWF P.count (Nat.le_refl _)
  have h2 : P.chunkList.drop P.count = [] := by
    have : P.chunkList.length = P.count := count_len_map P
    rw [← this]; exact drop_len_nil P.chunkList
  rw [h2] at h1; simp at h1; omega

theorem startAt_le_len (P : Parse) (T : Text) (hWF : ParseWF P T) (k : Nat) (hk : k ≤ P.count) :
    P.startAt k ≤ T.length := by
  have h1 := startAt_add P T hWF k hk
  omega

/-- the suffix inside a phrase decomposes as tail ++ rest-of-text. -/
theorem drop_in_phrase (P : Parse) (T : Text) (hWF : ParseWF P T) (k o : Nat)
    (hk : k < P.count) (ho : o < (P.chunkAt k).length) :
    T.drop (P.startAt k + o) = (P.chunkAt k).drop o ++ T.drop (P.startAt (k+1)) := by
  obtain ⟨r, hr⟩ := chunkList_drop_cons P k hk
  have hdrop1 : P.chunkList.drop k = P.chunkAt k :: r := hr
  have hnext : P.chunkList.drop (k+1) = r := by
    rw [← List.drop_drop, hdrop1]; simp [List.drop]
  have hrest : T.drop (P.startAt (k+1)) = r.flatten := by
    rw [parse_drop P T hWF (k+1) (by omega), hnext]
  have hfull : T.drop (P.startAt k) = P.chunkAt k ++ r.flatten := by
    have h := parse_drop P T hWF k (by omega)
    rw [h, hdrop1]
    rfl
  have hdd : (T.drop (P.startAt k)).drop o = T.drop (P.startAt k + o) := by
    rw [List.drop_drop]
  have hzero : o - (P.chunkAt k).length = 0 := by omega
  rw [← hdd, hfull, List.drop_append, hzero, hrest]
  simp [List.drop]

/-! ### Section 2.7: phrase location -/

/-- If a valid containing phrase exists, `phraseAt` returns one (with its
position properties). -/
theorem phraseAt_some_of_mem (P : Parse) (i : Nat) (k : Nat)
    (hk : k < P.count) (hsum : P.startAt k + o = i) (ho : o < (P.chunkAt k).length) :
    ∃ kk oo, P.phraseAt i = some (kk, oo)
      ∧ kk < P.count ∧ P.startAt kk + oo = i ∧ oo < (P.chunkAt kk).length := by
  have hmem : k ∈ (List.range P.count).filter
      (fun k => decide (P.startAt k ≤ i ∧ i < P.startAt k + (P.chunkAt k).length)) := by
    refine List.mem_filter.mpr ⟨List.mem_range.mpr hk, ?_⟩
    simp only [decide_eq_true_eq]
    exact ⟨by omega, by omega⟩
  unfold Parse.phraseAt
  cases hf : (List.range P.count).filter
      (fun k => decide (P.startAt k ≤ i ∧ i < P.startAt k + (P.chunkAt k).length)) with
  | nil =>
      have : k ∈ ([] : List Nat) := by rw [← hf]; exact hmem
      simp at this
  | cons kk rest =>
      refine ⟨kk, i - P.startAt kk, rfl, ?_, ?_, ?_⟩
      · have hmem' : kk ∈ (List.range P.count).filter
            (fun k => decide (P.startAt k ≤ i ∧ i < P.startAt k + (P.chunkAt k).length)) := by
          rw [hf]; exact List.mem_cons_self
        obtain ⟨h1, h2⟩ := List.mem_filter.mp hmem'
        exact List.mem_range.mp h1
      · have hmem' : kk ∈ (List.range P.count).filter
            (fun k => decide (P.startAt k ≤ i ∧ i < P.startAt k + (P.chunkAt k).length)) := by
          rw [hf]; exact List.mem_cons_self
        obtain ⟨h1, h2⟩ := List.mem_filter.mp hmem'
        simp only [decide_eq_true_eq] at h2
        obtain ⟨h3, h4⟩ := h2
        omega
      · have hmem' : kk ∈ (List.range P.count).filter
            (fun k => decide (P.startAt k ≤ i ∧ i < P.startAt k + (P.chunkAt k).length)) := by
          rw [hf]; exact List.mem_cons_self
        obtain ⟨h1, h2⟩ := List.mem_filter.mp hmem'
        simp only [decide_eq_true_eq] at h2
        obtain ⟨h3, h4⟩ := h2
        omega

/-- partition existence: every position inside the text lies in a phrase. -/
theorem exists_phrase : ∀ (cnt : Nat) (P : Parse) (T : Text), ParseWF P T → P.count ≤ cnt →
    ∀ i, i < T.length → ∃ k o, k < P.count ∧ P.startAt k + o = i ∧ o < (P.chunkAt k).length := by
  intro cnt
  induction cnt with
  | zero =>
      intro P T hWF hle i hi
      have hc : P.count = 0 := Nat.le_zero.mp hle
      match heq : P.ids with
      | [] =>
          have hpt : T = [] := by
            rw [← hWF.1]; simp [Parse.parseText, Parse.chunkList, heq]
          rw [hpt] at hi; simp at hi
      | x :: xs => simp [Parse.count, heq] at hc
  | succ c ih =>
      intro P T hWF hle i hi
      match heq : P.ids with
      | [] =>
          have hpt : T = [] := by
            rw [← hWF.1]; simp [Parse.parseText, Parse.chunkList, heq]
          rw [hpt] at hi; simp at hi
      | x :: xs =>
          have hcountpos : 1 ≤ P.count := by simp [Parse.count, heq]
          have hid0 : P.ids.getD 0 0 < P.dict.length := by
            have hall := List.all_eq_true.mp hWF.2.1
            have hmem : P.ids.getD 0 0 ∈ P.ids := by
              rw [heq]; simp [List.getD]
            exact of_decide_eq_true (hall _ hmem)
          have hne0 : P.dict.getD (P.ids.getD 0 0) [] ≠ [] := by
            have hallne := List.all_eq_true.mp hWF.2.2.1
            exact of_decide_eq_true (hallne _ (getD_mem _ (P.ids.getD 0 0) [] hid0))
          have hL0pos : 1 ≤ (P.chunkAt 0).length := by
            have hc0 : P.chunkAt 0 = P.dict.getD (P.ids.getD 0 0) [] := rfl
            rw [hc0]
            rcases hne : P.dict.getD (P.ids.getD 0 0) [] with _ | ⟨a, bs⟩
            · rw [hne] at hne0; exact absurd rfl hne0
            · simp [List.length_cons]
          by_cases hilt : i < (P.chunkAt 0).length
          · refine ⟨0, i, hcountpos, ?_, hilt⟩
            show 0 + i = i
            rw [Nat.zero_add]
          · have hL0le : (P.chunkAt 0).length ≤ i := by omega
            have hc0' : P.count ≠ 0 := by omega
            have hWF' := shift_WF P T hWF hc0'
            have hT'len : (T.drop (P.chunkAt 0).length).length
                = T.length - (P.chunkAt 0).length := List.length_drop
            have hi' : i - (P.chunkAt 0).length < (T.drop (P.chunkAt 0).length).length := by omega
            have hcnt' : (shiftParse P).count ≤ c := by
              have := count_shift P; omega
            obtain ⟨k', o', hk', hsum', hol'⟩ :=
              ih (shiftParse P) (T.drop (P.chunkAt 0).length) hWF' hcnt'
                (i - (P.chunkAt 0).length) hi'
            have hcs := count_shift P
            refine ⟨k'+1, o', ?_, ?_, ?_⟩
            · omega
            · have hst := startAt_shift P k'
              have h2 : (i - (P.chunkAt 0).length) + (P.chunkAt 0).length = i :=
                Nat.sub_add_cancel hL0le
              calc P.startAt (k'+1) + o'
                  = ((shiftParse P).startAt k' + (P.chunkAt 0).length) + o' := by rw [hst]
                _ = (shiftParse P).startAt k' + o' + (P.chunkAt 0).length := by omega
                _ = (i - (P.chunkAt 0).length) + (P.chunkAt 0).length := by rw [hsum']
                _ = i := h2
            · have hch := chunkAt_shift P k'
              rw [hch] at hol'
              exact hol'

/-! ### Section 3.0: element-level LCP facts and dictionary bridges -/

/-- `take (m+1) = take m ++ [element m]` (the take/element bridge). -/
theorem take_succ_last : ∀ (x : List Nat) (m : Nat), m < x.length →
    x.take (m+1) = x.take m ++ [x.getD m 0] := by
  intro x
  induction x with
  | nil => intro m hm; simp at hm
  | cons x0 xs ih =>
      intro m hm
      match m with
      | 0 => simp [List.take, List.getD]
      | mm+1 =>
          have hlt : mm < xs.length := by simpa using hm
          show (x0 :: xs).take (mm+1+1) = (x0 :: xs).take (mm+1) ++ [(x0 :: xs).getD (mm+1) 0]
          simp only [List.take, List.getD]
          rw [ih mm hlt]
          simp

theorem append_last_inj : ∀ (A dx dy : List Nat), A ++ dx = A ++ dy → dx = dy := by
  intro A
  induction A with
  | nil => intro dx dy h; exact h
  | cons a rest ih =>
      intro dx dy h
      exact ih dx dy (by simpa [List.cons_append] using h)

theorem getD_drop : ∀ (l : List Nat) (a e : Nat),
    (l.drop a).getD e 0 = l.getD (e + a) 0 := by
  intro l
  induction l with
  | nil => intro a e; cases a <;> rfl
  | cons x xs ih =>
      intro a e
      cases a with
      | zero => rfl
      | succ m => exact ih m e

/-- matched elements: strictly below the LCP the elements agree. -/
theorem lcpOf_getD_eq (x y : List Nat) (m : Nat) (h : m < lcpOf x y) :
    x.getD m 0 = y.getD m 0 := by
  have hxm : m < x.length := by have := lcpOf_le_left x y; omega
  have hym : m < y.length := by have := lcpOf_le_right x y; omega
  have hall : x.take (m+1) = y.take (m+1) :=
    (take_eq_iff_le_lcpOf x y (m+1) ⟨by omega, by omega⟩).mpr (by omega)
  rw [take_succ_last x m hxm, take_succ_last y m hym] at hall
  have hpre : x.take m = y.take m :=
    (take_eq_iff_le_lcpOf x y m ⟨by omega, by omega⟩).mpr (by omega)
  rw [hpre] at hall
  have := append_last_inj (y.take m) [x.getD m 0] [y.getD m 0] hall
  simpa using this

/-- at the LCP the elements differ (the two-sided determinism core). -/
theorem lcpOf_getD_ne (x y : List Nat) (m : Nat) (hlp : lcpOf x y = m)
    (hx : m < x.length) (hy : m < y.length) : x.getD m 0 ≠ y.getD m 0 := by
  intro hcon
  have hall : x.take (m+1) = y.take (m+1) := by
    rw [take_succ_last x m hx, take_succ_last y m hy,
        (take_eq_iff_le_lcpOf x y m ⟨by omega, by omega⟩).mpr (by omega), hcon]
  have := (take_eq_iff_le_lcpOf x y (m+1) ⟨by omega, by omega⟩).mp hall
  omega

/-! ### Section 3.1: phraseAt soundness, the none case, chunk bridges -/

theorem phraseAt_sound (P : Parse) (i : Nat) (h : P.phraseAt i = some (k, o)) :
    k < P.count ∧ P.startAt k + o = i ∧ o < (P.chunkAt k).length := by
  unfold Parse.phraseAt at h
  cases hf : (List.range P.count).filter
      (fun k => decide (P.startAt k ≤ i ∧ i < P.startAt k + (P.chunkAt k).length)) with
  | nil => rw [hf] at h; simp at h
  | cons kk rest =>
      rw [hf] at h
      injection h with h1
      obtain ⟨hk, ho⟩ := Prod.mk.inj h1
      have hmem' : kk ∈ (List.range P.count).filter
          (fun k => decide (P.startAt k ≤ i ∧ i < P.startAt k + (P.chunkAt k).length)) := by
        rw [hf]; exact List.mem_cons_self
      obtain ⟨hr1, hr2⟩ := List.mem_filter.mp hmem'
      obtain ⟨hr3, hr4⟩ := of_decide_eq_true hr2
      subst ho
      subst hk
      refine ⟨List.mem_range.mp hr1, ?_, ?_⟩
      · omega
      · omega

theorem phraseAt_none_ge (P : Parse) (T : Text) (hWF : ParseWF P T) (i : Nat)
    (h : P.phraseAt i = none) : T.length ≤ i := by
  refine Classical.byContradiction fun hcon => ?_
  have hlt : i < T.length := by
    have := Nat.not_le.mp hcon; omega
  obtain ⟨k, o, hk, hsum, hol⟩ :=
    exists_phrase P.count P T hWF (Nat.le_refl _) i hlt
  obtain ⟨kk, oo, heq, _, _, _⟩ := phraseAt_some_of_mem P i k hk hsum hol
  rw [heq] at h
  simp at h

theorem chunk_valid (P : Parse) (T : Text) (hWF : ParseWF P T) (k : Nat) (hk : k < P.count) :
    P.idAt k < P.dict.length ∧ 1 ≤ (P.chunkAt k).length := by
  obtain ⟨_, hall1, hallne1, _⟩ := hWF
  have hall := List.all_eq_true.mp hall1
  have halln := List.all_eq_true.mp hallne1
  have hmem : P.idAt k ∈ P.ids := getD_mem P.ids k 0 hk
  have hlt := of_decide_eq_true (hall _ hmem)
  have hcne : P.chunkAt k ≠ [] := by
    intro hcon
    have hmem2 : P.chunkAt k ∈ P.dict := getD_mem _ (P.idAt k) [] hlt
    have hne' := of_decide_eq_true (halln _ hmem2)
    exact hne' hcon
  refine ⟨hlt, ?_⟩
  rcases hc : P.chunkAt k with _ | ⟨a, bs⟩
  · rw [hc] at hcne; exact absurd rfl hcne
  · simp [List.length_cons]

/-- distinct dictionary ⟹ distinct ids give distinct chunk strings. -/
theorem chunk_ne_of_id_ne (P : Parse) (T : Text) (hWF : ParseWF P T) (u v : Nat)
    (hu : u < P.count) (hv : v < P.count) (hne : P.idAt u ≠ P.idAt v) :
    P.chunkAt u ≠ P.chunkAt v := by
  obtain ⟨hul, _⟩ := chunk_valid P T hWF u hu
  obtain ⟨hvl, _⟩ := chunk_valid P T hWF v hv
  obtain ⟨_, _, _, hnd⟩ := hWF
  intro hcon
  exact hne (getD_inj_of_nodup [] _ _ hnd hul hvl hcon)

/-! ### Section 3.2: the id-walk peeling lemma -/

/-- id-LCE: the number of consecutive equal phrase ids from `a, b` —
the parse-side extension count (fingerprint-accelerable). -/
def idLCE (P : Parse) (a b : Nat) : Nat := lcpOf (P.ids.drop a) (P.ids.drop b)

theorem count_ids (P : Parse) : P.count = P.ids.length := rfl

theorem idLCE_id_eq (P : Parse) (a' b' e : Nat) (h : e < idLCE P a' b') :
    P.idAt (a'+e) = P.idAt (b'+e) := by
  have heq : (P.ids.drop a').getD e 0 = (P.ids.drop b').getD e 0 :=
    lcpOf_getD_eq _ _ e h
  rw [getD_drop P.ids a' e, getD_drop P.ids b' e] at heq
  have h2 : P.ids.getD (a'+e) 0 = P.ids.getD (b'+e) 0 := by
    rw [← Nat.add_comm e a', ← Nat.add_comm e b']; exact heq
  exact h2

theorem idLCE_id_ne (P : Parse) (a' b' e : Nat)
    (h : idLCE P a' b' = e) (ha : a'+e < P.count) (hb : b'+e < P.count) :
    P.idAt (a'+e) ≠ P.idAt (b'+e) := by
  have hlA : (P.ids.drop a').length = P.count - a' := List.length_drop
  have hlB : (P.ids.drop b').length = P.count - b' := List.length_drop
  have hx : e < (P.ids.drop a').length := by omega
  have hy : e < (P.ids.drop b').length := by omega
  have hne := lcpOf_getD_ne _ _ e h hx hy
  rw [getD_drop P.ids a' e, getD_drop P.ids b' e] at hne
  intro hcon
  have h2 : P.ids.getD (a'+e) 0 ≠ P.ids.getD (b'+e) 0 := by
    rw [← Nat.add_comm e a', ← Nat.add_comm e b']; exact hne
  exact h2 hcon

theorem chunk_eq_of_id_eq (P : Parse) (u v : Nat) (h : P.idAt u = P.idAt v) :
    P.chunkAt u = P.chunkAt v := by
  show P.dict.getD (P.idAt u) [] = P.dict.getD (P.idAt v) []
  rw [h]

theorem lcpOf_self_append (c u : List Nat) : lcpOf c (c ++ u) = c.length := by
  rw [lcpOf_symm, lcpOf_append_left c u c, lcpOf_self c,
      if_neg (by omega), drop_len_nil]
  cases u <;> rfl

/-- THE WALK-PEEL LEMMA: matching id-pairs peel as equal chunks; the span
is the same on both sides; the boundary-suffix LCP decomposes as
span + LCP at the advanced boundaries. -/
theorem walkPeel (P : Parse) (T : Text) (hWF : ParseWF P T) :
    ∀ (a' b' e : Nat), e ≤ idLCE P a' b' → a'+e ≤ P.count → b'+e ≤ P.count →
      lcpOf (T.drop (P.startAt a')) (T.drop (P.startAt b'))
        = (P.startAt (a'+e) - P.startAt a')
          + lcpOf (T.drop (P.startAt (a'+e))) (T.drop (P.startAt (b'+e)))
      ∧ (P.startAt (a'+e) - P.startAt a') = (P.startAt (b'+e) - P.startAt b') := by
  intro a' b' e
  induction e with
  | zero =>
      intro _ _ _
      simp only [Nat.add_zero, Nat.sub_self, Nat.zero_add]
      exact ⟨trivial, trivial⟩
  | succ mm ih =>
      intro hle ha hb
      have hmm : mm ≤ idLCE P a' b' := by omega
      have ham : a'+mm ≤ P.count := by omega
      have hbm : b'+mm ≤ P.count := by omega
      obtain ⟨hpeel, hspan⟩ := ih hmm ham hbm
      have hlt : mm < idLCE P a' b' := by omega
      have hid := idLCE_id_eq P a' b' mm hlt
      have hchunk := chunk_eq_of_id_eq P (a'+mm) (b'+mm) hid
      have hclen : (P.chunkAt (a'+mm)).length = (P.chunkAt (b'+mm)).length := by
        rw [hchunk]
      have hac : a'+mm < P.count := by omega
      have hbc : b'+mm < P.count := by omega
      obtain ⟨_, hlenA⟩ := chunk_valid P T hWF (a'+mm) hac
      obtain ⟨_, hlenB⟩ := chunk_valid P T hWF (b'+mm) hbc
      have hda : T.drop (P.startAt (a'+mm))
          = P.chunkAt (a'+mm) ++ T.drop (P.startAt (a'+mm+1)) :=
        drop_in_phrase P T hWF (a'+mm) 0 hac (by omega)
      have hdb : T.drop (P.startAt (b'+mm))
          = P.chunkAt (b'+mm) ++ T.drop (P.startAt (b'+mm+1)) :=
        drop_in_phrase P T hWF (b'+mm) 0 hbc (by omega)
      have hpeel1 : lcpOf (T.drop (P.startAt (a'+mm))) (T.drop (P.startAt (b'+mm)))
          = (P.chunkAt (a'+mm)).length
            + lcpOf (T.drop (P.startAt (a'+mm+1))) (T.drop (P.startAt (b'+mm+1))) := by
        rw [hda, hdb, hchunk,
            lcpOf_append_left (P.chunkAt (b'+mm)) (T.drop (P.startAt (a'+mm+1)))
              (P.chunkAt (b'+mm) ++ T.drop (P.startAt (b'+mm+1))),
            lcpOf_self_append (P.chunkAt (b'+mm)) (T.drop (P.startAt (b'+mm+1))),
            if_neg (by omega)]
        have hdrop : (P.chunkAt (b'+mm) ++ T.drop (P.startAt (b'+mm+1))).drop
            (P.chunkAt (b'+mm)).length = T.drop (P.startAt (b'+mm+1)) := by
          have hd1 : (P.chunkAt (b'+mm)).drop (P.chunkAt (b'+mm)).length = [] :=
            drop_len_nil _
          have hd2 : (P.chunkAt (b'+mm)).length - (P.chunkAt (b'+mm)).length = 0 := by omega
          rw [List.drop_append, hd1, hd2]
          rfl
        rw [hdrop]
      have hsuccA : P.startAt (a'+mm+1) = P.startAt (a'+mm) + (P.chunkAt (a'+mm)).length := by
        rfl
      have hsuccB : P.startAt (b'+mm+1) = P.startAt (b'+mm) + (P.chunkAt (b'+mm)).length := by
        rfl
      have hmonoA : P.startAt a' ≤ P.startAt (a'+mm) :=
        startAt_mono P a' (a'+mm) (by omega)
      have hmonoB : P.startAt b' ≤ P.startAt (b'+mm) :=
        startAt_mono P b' (b'+mm) (by omega)
      have hnormA : a' + (mm+1) = a'+mm+1 := rfl
      have hnormB : b' + (mm+1) = b'+mm+1 := rfl
      rw [hnormA, hnormB, hpeel, hpeel1]
      constructor
      · omega
      · omega

/-! ### Section 3.3: the three list-level composition cases -/

/-- prefix case: x a prefix of y ⟹ the LCP of the appended suffixes is
|x| + LCP against the remainder of `y ++ v` past the prefix. -/
theorem lcp_append_prefix_left (x u y v : List Nat)
    (hpre : y.take x.length = x) (hle : x.length ≤ y.length) :
    lcpOf (x ++ u) (y ++ v) = x.length + lcpOf u ((y ++ v).drop x.length) := by
  have hylen : (y ++ v).take x.length = x := by
    rw [List.take_append, hpre]
    have h2 : v.take (x.length - y.length) = [] := by
      rw [show x.length - y.length = 0 from by omega]
      rfl
    simp [h2]
  have hyx : lcpOf x (y ++ v) = x.length :=
    lcpOf_of_take x (y ++ v) hylen (by
      have h3 : (y ++ v).length = y.length + v.length := List.length_append
      omega)
  rw [lcpOf_append_left x u (y ++ v), hyx, if_neg (by omega)]

/-- divergence case: x and y diverge inside both ⟹ appending changes nothing. -/
theorem lcp_append_diverge (x u y v : List Nat)
    (hx : lcpOf x y < x.length) (hy : lcpOf x y < y.length) :
    lcpOf (x ++ u) (y ++ v) = lcpOf x y := by
  have h1 : lcpOf x (y ++ v) = lcpOf x y := lcpOf_diverge x y v hx hy
  rw [lcpOf_append_left x u (y ++ v), h1, if_pos hx]
  simp

/-- equal case: equal heads peel to |x| + LCP of the tails. -/
theorem lcp_append_equal (x u v : List Nat) :
    lcpOf (x ++ u) (x ++ v) = x.length + lcpOf u v := by
  rw [lcpOf_append_left x u (x ++ v), lcpOf_self_append x,
      if_neg (by omega)]
  have hd : (x ++ v).drop x.length = v := by
    have hd1 : x.drop x.length = [] := drop_len_nil _
    have hd2 : x.length - x.length = 0 := by omega
    rw [List.drop_append, hd1, hd2]
    rfl
  rw [hd]

/-! ### Section 3.4: `textLCEvia` — the slim build's LCE primitive

Mutual pair: `textLCEvia` dispatches on the phrases containing `i, j`
(tail compare; on equal tails it enters the aligned id-walk `textLCEwalk`),
and `textLCEwalk` performs the parse-side extension (idLCE) plus the
first-differing-phrase compare, recursing back into `textLCEvia` only in
the differing-phrase-prefix case — the deliberate correctness FIX over
the ported `lce_support` structure (the C++ stops at the prefix length). -/

mutual
/-- The parse-space text LCE (mirrors `pfpds::pfp_lce_support`). -/
def textLCEvia (P : Parse) (T : Text) : Nat → Nat → Nat → Nat
  | 0, _, _ => 0
  | fuel+1, i, j =>
    match P.phraseAt i, P.phraseAt j with
    | some (a, o), some (b, o') =>
      if lcpOf ((P.chunkAt a).drop o) ((P.chunkAt b).drop o')
          < ((P.chunkAt a).drop o).length
        ∧ lcpOf ((P.chunkAt a).drop o) ((P.chunkAt b).drop o')
          < ((P.chunkAt b).drop o').length
      then lcpOf ((P.chunkAt a).drop o) ((P.chunkAt b).drop o')
      else if ((P.chunkAt a).drop o).length ≠ ((P.chunkAt b).drop o').length
      then lcpOf ((P.chunkAt a).drop o) ((P.chunkAt b).drop o')
            + textLCEvia P T fuel
                (i + lcpOf ((P.chunkAt a).drop o) ((P.chunkAt b).drop o'))
                (j + lcpOf ((P.chunkAt a).drop o) ((P.chunkAt b).drop o'))
      else textLCEwalk P T (fuel+1) (a+1) (b+1)
            (lcpOf ((P.chunkAt a).drop o) ((P.chunkAt b).drop o'))
    | _, _ => 0
  termination_by fuel i j => (fuel, 1)

/-- The aligned id-walk: both positions at phrase starts. -/
def textLCEwalk (P : Parse) (T : Text) : Nat → Nat → Nat → Nat → Nat
  | 0, _, _, _ => 0
  | fuel+1, a', b', c =>
    if (a' + idLCE P a' b') ≥ P.count ∨ (b' + idLCE P a' b') ≥ P.count
    then c + (P.startAt (a' + idLCE P a' b') - P.startAt a')
    else
      if lcpOf (P.chunkAt (a' + idLCE P a' b')) (P.chunkAt (b' + idLCE P a' b'))
          < (P.chunkAt (a' + idLCE P a' b')).length
        ∧ lcpOf (P.chunkAt (a' + idLCE P a' b')) (P.chunkAt (b' + idLCE P a' b'))
          < (P.chunkAt (b' + idLCE P a' b')).length
      then c + (P.startAt (a' + idLCE P a' b') - P.startAt a')
            + lcpOf (P.chunkAt (a' + idLCE P a' b')) (P.chunkAt (b' + idLCE P a' b'))
      else if (P.chunkAt (a' + idLCE P a' b')).length
            ≠ (P.chunkAt (b' + idLCE P a' b')).length
      then c + (P.startAt (a' + idLCE P a' b') - P.startAt a')
            + lcpOf (P.chunkAt (a' + idLCE P a' b')) (P.chunkAt (b' + idLCE P a' b'))
            + textLCEvia P T fuel
                (P.startAt (a' + idLCE P a' b')
                  + lcpOf (P.chunkAt (a' + idLCE P a' b')) (P.chunkAt (b' + idLCE P a' b')))
                (P.startAt (b' + idLCE P a' b')
                  + lcpOf (P.chunkAt (a' + idLCE P a' b')) (P.chunkAt (b' + idLCE P a' b')))
      else c + (P.startAt (a' + idLCE P a' b') - P.startAt a')
            + lcpOf (P.chunkAt (a' + idLCE P a' b')) (P.chunkAt (b' + idLCE P a' b'))
  termination_by fuel a' b' c => (fuel, 0)
end

/-- positions past the text drop to []. -/
theorem drop_nil_of_ge (T : Text) (i : Nat) (h : T.length ≤ i) : T.drop i = [] := by
  cases hd : T.drop i with
  | nil => rfl
  | cons x xs =>
      have hl : (T.drop i).length = T.length - i := List.length_drop
      rw [hd] at hl
      simp [List.length_cons] at hl
      omega

/-! ### Section 3.5: THE COMPOSITION THEOREM -/

theorem len_drop_phrase (P : Parse) (k o : Nat) :
    ((P.chunkAt k).drop o).length = (P.chunkAt k).length - o := by
  have h : ((P.chunkAt k).drop o).length = (P.chunkAt k).length - o :=
    List.length_drop
  omega

theorem idLCE_le (P : Parse) (a' b' : Nat) (ha : a' ≤ P.count) (hb : b' ≤ P.count) :
    idLCE P a' b' + a' ≤ P.count ∧ idLCE P a' b' + b' ≤ P.count := by
  constructor
  · have hl : (P.ids.drop a').length = P.count - a' := List.length_drop
    have h1 := lcpOf_le_left (P.ids.drop a') (P.ids.drop b')
    show lcpOf (P.ids.drop a') (P.ids.drop b') + a' ≤ P.count
    have h2 : lcpOf (P.ids.drop a') (P.ids.drop b') + a'
        ≤ (P.ids.drop a').length + a' := Nat.add_le_add_right h1 _
    rw [hl] at h2
    omega
  · have hl : (P.ids.drop b').length = P.count - b' := List.length_drop
    have h1 := lcpOf_le_right (P.ids.drop a') (P.ids.drop b')
    show lcpOf (P.ids.drop a') (P.ids.drop b') + b' ≤ P.count
    have h2 : lcpOf (P.ids.drop a') (P.ids.drop b') + b'
        ≤ (P.ids.drop b').length + b' := Nat.add_le_add_right h1 _
    rw [hl] at h2
    omega

/-- the aligned id-walk computes the LCP of the two boundary suffixes,
given the caller's correctness hypothesis for `textLCEvia` at fuel `f`. -/
theorem textLCEwalk_correct (P : Parse) (T : Text) (hWF : ParseWF P T) (f : Nat)
    (hv : ∀ (i' j' : Nat), i' ≤ T.length → j' ≤ T.length →
      T.length - i' ≤ f → textLCEvia P T f i' j' = lcpOf (T.drop i') (T.drop j')) :
    ∀ (a' b' c : Nat), a' ≤ P.count → b' ≤ P.count →
      T.length - P.startAt a' ≤ f + 1 →
      textLCEwalk P T (f+1) a' b' c
        = c + lcpOf (T.drop (P.startAt a')) (T.drop (P.startAt b')) := by
  intro a' b' c hac hbc hfa
  rw [textLCEwalk]
  generalize he : idLCE P a' b' = e
  have hbounds : e + a' ≤ P.count ∧ e + b' ≤ P.count := by
    have h := idLCE_le P a' b' hac hbc
    rw [he] at h
    omega
  have hpeel := walkPeel P T hWF a' b' e (by rw [he]; omega)
    (by have := hbounds.1; omega) (by have := hbounds.2; omega)
  obtain ⟨hpeel1, hpeel2⟩ := hpeel
  split <;> rename_i hex
  · -- EXHAUSTION: one side runs out of phrases
    rcases hex with hex1 | hex2
    · have heq1 : a' + e = P.count := by have := hbounds.1; omega
      have hstartc : P.startAt P.count = T.length := startAt_count P T hWF
      have hdrop : T.drop (P.startAt (a'+e)) = [] := by
        rw [heq1, hstartc, drop_nil_of_ge T T.length (Nat.le_refl _)]
      rw [hpeel1, hdrop]
      rfl
    · have heq2 : b' + e = P.count := by have := hbounds.2; omega
      have hstartc : P.startAt P.count = T.length := startAt_count P T hWF
      have hdrop : T.drop (P.startAt (b'+e)) = [] := by
        rw [heq2, hstartc, drop_nil_of_ge T T.length (Nat.le_refl _)]
      rw [hpeel1, hdrop]
      cases T.drop (P.startAt (a'+e)) <;> rfl
  · -- DIFFER: both sides have a differing phrase pair
    have hlt1 : a' + e < P.count := by have := hbounds.1; omega
    have hlt2 : b' + e < P.count := by have := hbounds.2; omega
    have hidne : P.idAt (a'+e) ≠ P.idAt (b'+e) :=
      idLCE_id_ne P a' b' e he (by omega) (by omega)
    have huchne : P.chunkAt (a'+e) ≠ P.chunkAt (b'+e) :=
      chunk_ne_of_id_ne P T hWF (a'+e) (b'+e) (by omega) (by omega) hidne
    have hlenA : 1 ≤ (P.chunkAt (a'+e)).length :=
      (chunk_valid P T hWF (a'+e) (by omega)).2
    have hlenB : 1 ≤ (P.chunkAt (b'+e)).length :=
      (chunk_valid P T hWF (b'+e) (by omega)).2
    have hdA : T.drop (P.startAt (a'+e))
        = P.chunkAt (a'+e) ++ T.drop (P.startAt (a'+e+1)) :=
      drop_in_phrase P T hWF (a'+e) 0 (by omega) (by omega)
    have hdB : T.drop (P.startAt (b'+e))
        = P.chunkAt (b'+e) ++ T.drop (P.startAt (b'+e+1)) :=
      drop_in_phrase P T hWF (b'+e) 0 (by omega) (by omega)
    have hcgu : lcpOf (P.chunkAt (a'+e)) (P.chunkAt (b'+e))
        ≤ (P.chunkAt (a'+e)).length := lcpOf_le_left _ _
    have hcgv : lcpOf (P.chunkAt (a'+e)) (P.chunkAt (b'+e))
        ≤ (P.chunkAt (b'+e)).length := lcpOf_le_right _ _
    split <;> rename_i hcond
    · -- the differing phrases diverge inside both
      rw [hpeel1, hdA, hdB, lcp_append_diverge _ _ _ _ hcond.1 hcond.2]
      omega
    · split <;> rename_i hne2
      · -- differing-phrase-prefix case: recurse into textLCEvia
        have hmin : (lcpOf (P.chunkAt (a'+e)) (P.chunkAt (b'+e))
            = (P.chunkAt (a'+e)).length)
          ∨ (lcpOf (P.chunkAt (a'+e)) (P.chunkAt (b'+e))
            = (P.chunkAt (b'+e)).length) := by omega
        rcases hmin with h1 | h2
        · -- chunk at (a'+e) is the prefix
          have hle : (P.chunkAt (a'+e)).length ≤ (P.chunkAt (b'+e)).length := by omega
          have hpre : (P.chunkAt (b'+e)).take (P.chunkAt (a'+e)).length
              = P.chunkAt (a'+e) := take_of_lcpOf_eq _ _ h1 hle
          have hsuccA : P.startAt (a'+e+1)
              = P.startAt (a'+e) + (P.chunkAt (a'+e)).length := startAt_succ P (a'+e)
          have hmonoA : P.startAt a' ≤ P.startAt (a'+e) :=
            startAt_mono P a' (a'+e) (by omega)
          have h1pos : 1 ≤ (P.chunkAt (a'+e)).length := hlenA
          have hUpLen := startAt_le_len P T hWF (a'+e+1) (by omega)
          have hsuccB' := startAt_succ P (b'+e)
          have hBLen := startAt_le_len P T hWF (b'+e+1) (by omega)
          rw [h1, hpeel1, hdA, hdB, lcp_append_prefix_left _ _ _ _ hpre hle,
              ← hdB, List.drop_drop, hsuccA]
          rw [hv (P.startAt (a'+e) + (P.chunkAt (a'+e)).length)
                (P.startAt (b'+e) + (P.chunkAt (a'+e)).length)
                (by omega) (by omega) (by omega)]
          omega
        · -- chunk at (b'+e) is the prefix: mirror
          have hle : (P.chunkAt (b'+e)).length ≤ (P.chunkAt (a'+e)).length := by omega
          have hpre : (P.chunkAt (a'+e)).take (P.chunkAt (b'+e)).length
              = P.chunkAt (b'+e) :=
            take_of_lcpOf_eq _ _ (by rw [lcpOf_symm]; exact h2) hle
          have hsuccB : P.startAt (b'+e+1)
              = P.startAt (b'+e) + (P.chunkAt (b'+e)).length := startAt_succ P (b'+e)
          have hmonoB : P.startAt b' ≤ P.startAt (b'+e) :=
            startAt_mono P b' (b'+e) (by omega)
          have h1pos : 1 ≤ (P.chunkAt (b'+e)).length := hlenB
          have hUpLen := startAt_le_len P T hWF (a'+e+1) (by omega)
          have hsuccA' := startAt_succ P (a'+e)
          have hALen := startAt_le_len P T hWF (a'+e+1) (by omega)
          have hBLen := startAt_le_len P T hWF (b'+e+1) (by omega)
          have hmonoA' : P.startAt a' ≤ P.startAt (a'+e) :=
            startAt_mono P a' (a'+e) (by omega)
          rw [h2, hpeel1, hdA, hdB]
          rw [lcpOf_symm (P.chunkAt (a'+e) ++ T.drop (P.startAt (a'+e+1)))
              (P.chunkAt (b'+e) ++ T.drop (P.startAt (b'+e+1)))]
          rw [lcp_append_prefix_left _ _ _ _ hpre hle,
              ← hdA, List.drop_drop, hsuccB]
          have hL := hv (P.startAt (a'+e) + (P.chunkAt (b'+e)).length)
                (P.startAt (b'+e) + (P.chunkAt (b'+e)).length)
                (by omega) (by omega) (by omega)
          have hflip := lcpOf_symm (T.drop (P.startAt (a'+e) + (P.chunkAt (b'+e)).length))
              (T.drop (P.startAt (b'+e) + (P.chunkAt (b'+e)).length))
          rw [hflip] at hL
          rw [hL]
          omega
      · -- VACUOUS: equal lengths with full-match would mean equal chunks
        have hlen : (P.chunkAt (a'+e)).length = (P.chunkAt (b'+e)).length := by omega
        have hfull : lcpOf (P.chunkAt (a'+e)) (P.chunkAt (b'+e))
            = (P.chunkAt (a'+e)).length := by omega
        have hpre : (P.chunkAt (b'+e)).take (P.chunkAt (a'+e)).length
            = P.chunkAt (a'+e) := take_of_lcpOf_eq _ _ hfull (by omega)
        have huq : P.chunkAt (a'+e) = P.chunkAt (b'+e) := by
          rw [hlen] at hpre
          have hself : (P.chunkAt (b'+e)).take (P.chunkAt (b'+e)).length
              = P.chunkAt (b'+e) := take_len_self _
          exact hpre.symm.trans hself
        exact absurd huq huchne

/-! ### Section 3.6: the main composition theorem -/

/-- THE COMPOSITION THEOREM: the parse-space LCE primitive computes the
true text LCP at every position pair, given any wellformed parse of `T`
and enough fuel. -/
theorem textLCEvia_correct (P : Parse) (T : Text) (hWF : ParseWF P T) :
    ∀ (fuel i j : Nat), T.length - i ≤ fuel → i ≤ T.length → j ≤ T.length →
      textLCEvia P T fuel i j = lcpOf (T.drop i) (T.drop j) := by
  intro fuel
  induction fuel with
  | zero =>
      intro i j hfuel hi hj
      rw [textLCEvia, drop_nil_of_ge T i (by omega)]
      rfl
  | succ f ih =>
      intro i j hfuel hi hj
      rcases hpi : P.phraseAt i with _ | ⟨a, o⟩
      · rw [textLCEvia, hpi, drop_nil_of_ge T i (phraseAt_none_ge P T hWF i hpi)]
        rfl
      · rcases hpj : P.phraseAt j with _ | ⟨b, o'⟩
        · rw [textLCEvia, hpi, hpj,
              drop_nil_of_ge T j (phraseAt_none_ge P T hWF j hpj)]
          cases T.drop i <;> rfl
        · obtain ⟨hac, hsumi, holi⟩ := phraseAt_sound P i hpi
          obtain ⟨hbc, hsumj, holj⟩ := phraseAt_sound P j hpj
          have hdri : T.drop i = (P.chunkAt a).drop o ++ T.drop (P.startAt (a+1)) := by
            rw [← hsumi]; exact drop_in_phrase P T hWF a o hac holi
          have hdrj : T.drop j = (P.chunkAt b).drop o' ++ T.drop (P.startAt (b+1)) := by
            rw [← hsumj]; exact drop_in_phrase P T hWF b o' hbc holj
          have hstartA : i + ((P.chunkAt a).drop o).length = P.startAt (a+1) := by
            rw [len_drop_phrase, startAt_succ]; omega
          have hstartB : j + ((P.chunkAt b).drop o').length = P.startAt (b+1) := by
            rw [len_drop_phrase, startAt_succ]; omega
          have htApos : 1 ≤ ((P.chunkAt a).drop o).length := by
            rw [len_drop_phrase]; omega
          have htBpos : 1 ≤ ((P.chunkAt b).drop o').length := by
            rw [len_drop_phrase]; omega
          have hcA : lcpOf ((P.chunkAt a).drop o) ((P.chunkAt b).drop o')
              ≤ ((P.chunkAt a).drop o).length := lcpOf_le_left _ _
          have hcB : lcpOf ((P.chunkAt a).drop o) ((P.chunkAt b).drop o')
              ≤ ((P.chunkAt b).drop o').length := lcpOf_le_right _ _
          have hlenU' : P.startAt (a+1) ≤ T.length :=
            startAt_le_len P T hWF (a+1) (by omega)
          have hlenW' : P.startAt (b+1) ≤ T.length :=
            startAt_le_len P T hWF (b+1) (by omega)
          have key : ∀ (i' j' : Nat), i < i' → i' ≤ T.length → j' ≤ T.length →
              textLCEvia P T f i' j' = lcpOf (T.drop i') (T.drop j') :=
            fun i' j' hii' hi' hj' => ih i' j' (by omega) hi' hj'
          rw [textLCEvia, hpi, hpj]
          dsimp only
          split <;> rename_i hcond
          · -- CASE 1: divergence inside both tails
            rw [hdri, hdrj, lcp_append_diverge _ _ _ _ hcond.1 hcond.2]
          · split <;> rename_i hne
            · -- CASE 2: one tail a strict prefix of the other
              have hmin : lcpOf ((P.chunkAt a).drop o) ((P.chunkAt b).drop o')
                  = ((P.chunkAt a).drop o).length
                ∨ lcpOf ((P.chunkAt a).drop o) ((P.chunkAt b).drop o')
                  = ((P.chunkAt b).drop o').length := by omega
              rcases hmin with h1 | h2
              · -- tA is the shorter
                have hle : ((P.chunkAt a).drop o).length
                    ≤ ((P.chunkAt b).drop o').length := by omega
                have hpre : ((P.chunkAt b).drop o').take ((P.chunkAt a).drop o).length
                    = (P.chunkAt a).drop o := take_of_lcpOf_eq _ _ h1 hle
                rw [h1, hdri, hdrj, lcp_append_prefix_left _ _ _ _ hpre hle,
                    ← hdrj, List.drop_drop, ← hstartA]
                rw [key (i + ((P.chunkAt a).drop o).length)
                      (j + ((P.chunkAt a).drop o).length)
                      (by omega) (by omega) (by omega)]
              · -- tB is the shorter: mirror via symmetry
                have hle : ((P.chunkAt b).drop o').length
                    ≤ ((P.chunkAt a).drop o).length := by omega
                have hpre : ((P.chunkAt a).drop o).take ((P.chunkAt b).drop o').length
                    = (P.chunkAt b).drop o' :=
                  take_of_lcpOf_eq _ _ (by rw [lcpOf_symm]; exact h2) hle
                rw [lcpOf_symm (T.drop i) (T.drop j)]
                rw [h2, hdrj, hdri,
                    lcp_append_prefix_left _ _ _ _ hpre hle,
                    ← hdri, List.drop_drop, ← hstartB]
                rw [key (i + ((P.chunkAt b).drop o').length)
                      (j + ((P.chunkAt b).drop o').length)
                      (by omega) (by omega) (by omega)]
                rw [lcpOf_symm (T.drop (i + ((P.chunkAt b).drop o').length))
                    (T.drop (j + ((P.chunkAt b).drop o').length))]
            · -- CASE 3: tails equal — the aligned id-walk
              have hlen : ((P.chunkAt a).drop o).length
                  = ((P.chunkAt b).drop o').length := by omega
              have hcfull : lcpOf ((P.chunkAt a).drop o) ((P.chunkAt b).drop o')
                  = ((P.chunkAt a).drop o).length := by omega
              have htab : (P.chunkAt a).drop o = (P.chunkAt b).drop o' := by
                have hpre : ((P.chunkAt b).drop o').take ((P.chunkAt a).drop o).length
                    = (P.chunkAt a).drop o := take_of_lcpOf_eq _ _ hcfull (by omega)
                rw [hlen] at hpre
                have hself : ((P.chunkAt b).drop o').take ((P.chunkAt b).drop o').length
                    = (P.chunkAt b).drop o' := take_len_self _
                exact hpre.symm.trans hself
              have hv : ∀ (i' j' : Nat), i' ≤ T.length → j' ≤ T.length →
                  T.length - i' ≤ f → textLCEvia P T f i' j' = lcpOf (T.drop i') (T.drop j') :=
                fun i' j' hi' hj' hf' => ih i' j' hf' hi' hj'
              rw [hcfull, hdri, hdrj, htab, lcp_append_equal]
              exact textLCEwalk_correct P T hWF f hv (a+1) (b+1)
                ((P.chunkAt b).drop o').length (by omega) (by omega)
                (by rw [← hstartA]; omega)

/-! ### Section 4: the construction — parse-based triples, capstones -/

/-- the triples stream of the reverse text with the `lcp` column produced by
the parse-space LCE primitive (fuel = the reverse-text length). -/
def parseTriplesOf (T : Text) (P : Parse) : List Triple :=
  let R := T.reverse ++ [0]
  let n := R.length
  let ord := saOrder R
  let lcps := (List.range n).map (fun i =>
    if i == 0 then 0
    else textLCEvia P R R.length (ord.getD i 0) (ord.getD (i-1) 0))
  (List.range n).map (fun i =>
    let j := ord.getD i 0
    let bwt := if j == 0 then 0 else R[j-1]!
    ⟨bwt, lcps.getD i 0, j⟩)

theorem getBang_eq_getD : ∀ (l : List Nat) (i : Nat), l[i]! = l.getD i 0 := by
  intro l
  induction l with
  | nil => intro i; cases i <;> rfl
  | cons x xs ih =>
      intro i
      cases i with
      | zero => rfl
      | succ m =>
          have hstep : (x :: xs)[m+1]! = xs[m]! := by
            cases xs with
            | nil => cases m <;> rfl
            | cons a as => cases m <;> rfl
          calc (x :: xs)[m+1]! = xs[m]! := hstep
            _ = xs.getD m 0 := ih m

/-- in-range `getD` is `getElem`. -/
theorem getD_lt_getElem : ∀ (l : List Nat) (i : Nat) (h : i < l.length),
    l.getD i 0 = l[i]'h := by
  intro l
  induction l with
  | nil => intro i h; simp at h
  | cons x xs ih =>
      intro i h
      match i, h with
      | 0, h => rfl
      | (m+1), h =>
          have hm : m < xs.length := by simp at h; omega
          show xs.getD m 0 = xs[m]'hm
          exact ih m hm

/-- the two lcps columns agree: `textLCEvia` computes the true LCP at every
SA-adjacent pair. -/
theorem parseLcps_eq (T : Text) (P : Parse) (hWF : ParseWF P (T.reverse ++ [0])) :
    (List.range (T.reverse ++ [0]).length).map (fun i =>
      if i == 0 then 0
      else textLCEvia P (T.reverse ++ [0]) (T.reverse ++ [0]).length
          ((saOrder (T.reverse ++ [0])).getD i 0)
          ((saOrder (T.reverse ++ [0])).getD (i-1) 0))
    = (List.range (T.reverse ++ [0]).length).map (fun i =>
      if i == 0 then 0
      else lcpOf ((T.reverse ++ [0]).drop ((saOrder (T.reverse ++ [0]))[i]!))
                 ((T.reverse ++ [0]).drop ((saOrder (T.reverse ++ [0]))[i-1]!))) := by
  apply List.map_congr_left
  intro i hi
  rw [List.mem_range] at hi
  have hlen : (saOrder (T.reverse ++ [0])).length = (T.reverse ++ [0]).length :=
    saOrder_length (T.reverse ++ [0])
  have hin : i < (saOrder (T.reverse ++ [0])).length := by rw [hlen]; omega
  rcases Nat.eq_zero_or_pos i with rfl | h0
  · simp
  · have hb : (i == 0) = false := by
      have hne : ¬ i = 0 := by omega
      simp [hne]
    have h1 : 1 ≤ i := by omega
    rw [hb, ite_eq_right (by simp : ¬(false = true)),
        ite_eq_right (by simp : ¬(false = true))]
    have hmemA : ((saOrder (T.reverse ++ [0])).getD i 0) ∈ saOrder (T.reverse ++ [0]) := by
      rw [getD_lt_getElem _ _ hin]
      exact List.getElem_mem hin
    have hmemB : ((saOrder (T.reverse ++ [0])).getD (i-1) 0) ∈ saOrder (T.reverse ++ [0]) := by
      have hmin : i-1 < (saOrder (T.reverse ++ [0])).length := by omega
      rw [getD_lt_getElem _ _ hmin]
      exact List.getElem_mem hmin
    have hordA := saOrder_lt (T.reverse ++ [0]) _ hmemA
    have hordB := saOrder_lt (T.reverse ++ [0]) _ hmemB
    have hc := textLCEvia_correct P (T.reverse ++ [0]) hWF
      (T.reverse ++ [0]).length ((saOrder (T.reverse ++ [0])).getD i 0)
      ((saOrder (T.reverse ++ [0])).getD (i-1) 0) (by omega) (by omega) (by omega)
    rw [getBang_eq_getD _ i, getBang_eq_getD _ (i-1)]
    exact hc

/-- THE CONSTRUCTION: the parse-based triples stream IS `triplesOf T`. -/
theorem parseTriplesOf_eq (T : Text) (P : Parse)
    (hWF : ParseWF P (T.reverse ++ [0])) :
    parseTriplesOf T P = triplesOf T := by
  unfold parseTriplesOf triplesOf
  dsimp only
  rw [parseLcps_eq T P hWF]
  apply List.map_congr_left
  intro i hi
  rw [List.mem_range] at hi
  rw [getBang_eq_getD _ i, getBang_eq_getD _ i]

/-- The parse-based scan IS `scan` over `triplesOf T`. -/
theorem parseScan_eq (T : Text) (P : Parse) (hWF : ParseWF P (T.reverse ++ [0])) :
    scan (T.length+1) (parseTriplesOf T P) = scan (T.length+1) (triplesOf T) :=
  congrArg _ (parseTriplesOf_eq T P hWF)

/-- CONSTRUCTION CAPSTONE (conditional): with the open minimality hypotheses
instantiated at the parse-based stream, the parse-built suffixient array IS χ. -/
theorem parseChi_eq (T : Text) (P : Parse) (hWF : ParseWF P (T.reverse ++ [0]))
    (hT : positive T = true)
    (hnd : (scan (T.length+1) (parseTriplesOf T P)).Nodup)
    (hmax : ∀ x ∈ scan (T.length+1) (parseTriplesOf T P), IsMax T x)
    (hdisj : ∀ x ∈ scan (T.length+1) (parseTriplesOf T P),
      ∀ y ∈ scan (T.length+1) (parseTriplesOf T P), ScopeLe T x y → ScopeLe T y x → x = y)
    (hcover : suffixient (scan (T.length+1) (parseTriplesOf T P)) T = true) :
    (scan (T.length+1) (parseTriplesOf T P)).length = chi T := by
  rw [parseScan_eq T P hWF] at hnd hmax hdisj hcover ⊢
  exact minimality_of_scan_classes T hT hnd hmax hdisj hcover

/-- The verified LCE oracle's DETERMINISM: parse-space LCE answers are
deterministic (two-sided equality with the ground-truth `lcpOf`), no
probability anywhere — the oracle certificate is this theorem, not a hash
collision argument. -/
theorem VerifiedLCE_determinism (T : Text) (P : Parse) (hWF : ParseWF P T)
    (fuel i j : Nat) (hi : i ≤ T.length) (hj : j ≤ T.length)
    (hf : T.length - i ≤ fuel) :
    textLCEvia P T fuel i j = lcpOf (T.drop i) (T.drop j) :=
  textLCEvia_correct P T hWF fuel i j hf hi hj

/-! ### HANDOFF (laneP state at end of this segment)

DONE (all kernel-checked; `#print axioms` = propext, Classical.choice,
Quot.sound only; 0 `sorry`, 0 `native_decide`):
- Sections 0–3.5 as previously reported (parse model, shift, lcpOf toolkit,
  suffix decompositions, phraseAt, idLCE, walkPeel, textLCEwalk machinery).
- `textLCEvia_correct` — THE composition theorem: the parse-space LCE
  (phrase walk with id-jumps) equals ground-truth `lcpOf` on any well-formed
  parse, given fuel ≥ T.length − i.  Note: textLCEvia now calls
  textLCEwalk at (fuel+1) with lexicographic termination measure
  (fuel, tag) — the fuel wiring that makes the induction go through.
- `textLCEwalk_correct` — the aligned id-walk correctness (b-side fuel
  hypothesis dropped: the walk only ever consumes a-side fuel).
- `parseLcps_eq`, `parseTriplesOf_eq` — the parse-based triples stream IS
  `triplesOf T` (the construction weld; needs `ParseWF P (T.reverse ++ [0])`).
- `parseScan_eq` — congruence to the production scan.
- `parseChi_eq` — CONDITIONAL CAPSTONE: with the open minimality hypotheses
  (nodup / IsMax / pairwise-scope / suffixient-cover, stated at the
  parse-stream) and positivity, the parse-built array has size `chi T`.
  Unconditional given the Sxgc sorry ledger's O2/O3/O4 — those remain the
  supervisor-owned open statements, untouched here.
- `VerifiedLCE_determinism` — the determinism claim as a named wrapper
  (no probability; two-sided).
- Eval battery — moved to sibling `BuildEval.lean` (battery defs are
  eval-only; keeping the theory file compile fast):
  GATE 1: 15 texts × k ∈ {2,3,5} = 45 runs, parse-triples == triplesOf → TRUE;
  GATE 2: parse-scan == production scan (15 runs) → TRUE;
  GATE 3: parse-built |χ| == chi oracle on the 9 small texts → TRUE
    (`chi` is the powerset minimality oracle, O(2^n) by definition — the
    in-Lean χ gate is only meaningful on tiny texts; at scale the oracle is
    the Rust/sA side);
  GATE 4: negative control (parse of the WRONG text) → FALSE (non-vacuity).
  Battery texts include the MANDATORY duplicates-heavy text (100× same
  character, period-1), period-2 `abab` at 60, mod-cycles at 30/60, empty,
  singletons, swaps.

REMAINING (next lane, in order):
- step-count bounds for textLCEvia/textLCEwalk (fuel consumption per
  id-jump; the |M|≈0.1n runtime analog) — NOT started, honest.
- fingerprint section: krs/chunkHash/fpParseLCE/HashGood as executable
  Lean + the determinism transfer (direct-read verification) — the Rust
  oracle is textLCEvia; only the hash plumbing remains.
- the 729-text exhaustive battery is a Main.lean-side job (Sxgc's eval
  style), not #eval-in-file; wire `parseTriplesOf` into it on commit.
- O2 adjudication and Φ-decode bridge stay with the supervisor/ledger.

GATES for commit review (all run green at handoff): `lake build SxgcBuild`;
`lake env lean --run Main.lean` → BIT 1B GATE: GREEN (Sxgc.lean untouched);
`lake env lean BuildEval.lean` → true,true,true,false; `#print axioms` on
the eight new theorems (textLCEvia_correct, textLCEwalk_correct,
parseLcps_eq, parseTriplesOf_eq, parseScan_eq, parseChi_eq,
VerifiedLCE_determinism, walkPeel) = propext/Classical.choice/Quot.sound
only; sorry count 0; no native_decide; no commits made (this file is
untracked in /tmp/sxgc-laneP as directed).
-/

end SxgcBuild
