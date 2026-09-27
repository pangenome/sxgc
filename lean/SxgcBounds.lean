import SxgcBuild

/-!
# Explicit work accounting and verified fingerprint substitution

Cost convention: one indexed phrase-location, dictionary LCE, prefix-sum,
or fixed-size control block is one primitive. The instrumented evaluator
shares repeated dictionary/id LCE subexpressions with `let`; its erasure is
proved equal to the reference. Steps are charged work (including terminal
checks and a fixed allowance per branch), not interpreter instruction counts. List traversal implementing
these specifications in SxgcBuild is NOT constant time. These are bounds
for that explicit primitive-call model, not for Lean's list evaluator.
Dictionary byte comparisons, rank/select construction, SA construction,
and LF resolution need separate accounting in a machine-level theorem.

`pieces` counts the actual successful phrase comparisons along the query:
equal ids in an aligned walk, and consumed phrase tails in prefix cases.
Thus it measures the answer in phrase pieces (both grids may contribute),
not text bytes, fuel, or a worst-case cap. The terminal mismatch costs a
constant. For the linear verifier an id extension of e costs e+1 probes.
-/
open Sxgc SxgcBuild SxgcBuild.Parse
namespace SxgcBounds

/-- Actual recursion count of the direct verifier: one pair comparison per
matching symbol, plus the mismatch/end test. Independent of the answer. -/
def lcpProbes : List Nat → List Nat → Nat
  | [], _ => 1
  | _ :: _, [] => 1
  | a :: x, b :: y => if a == b then 1 + lcpProbes x y else 1

theorem lcpProbes_eq (x y : List Nat) : lcpProbes x y = lcpOf x y + 1 := by
  induction x generalizing y with
  | nil => rfl
  | cons a x ih =>
    cases y with
    | nil => rfl
    | cons b y =>
      simp only [lcpProbes, lcpOf]
      split
      · rw [ih]; omega
      · rfl

/-- Justifies the e+1 verification charge used by the aligned walk. -/
theorem idLCE_probes (P : Parse) (a b : Nat) :
    lcpProbes (P.ids.drop a) (P.ids.drop b) = idLCE P a b + 1 :=
  lcpProbes_eq _ _

structure LCERun where
  answer : Nat
  steps : Nat
  pieces : Nat
  deriving Repr

/-- Record a consumed prefix and its work before a recursive call. -/
def charge (bytes work pieces : Nat) (r : LCERun) : LCERun :=
  ⟨bytes + r.answer, work + r.steps, pieces + r.pieces⟩

mutual
/-- Instrumentation of exactly the reference's branches, with a replaceable
phrase-id extension oracle. Each dictionary query is charged as one primitive. -/
def viaRun (ext : Parse → Nat → Nat → Nat) (P : Parse) : Nat → Nat → Nat → LCERun
  | 0, _, _ => ⟨0, 1, 0⟩
  | f+1, i, j =>
    match P.phraseAt i, P.phraseAt j with
    | some (a,o), some (b,o') =>
      let x := (P.chunkAt a).drop o
      let y := (P.chunkAt b).drop o'
      let c := lcpOf x y
      if c < x.length ∧ c < y.length then ⟨c, 4, 0⟩
      else if x.length ≠ y.length then
        charge c 4 1 (viaRun ext P f (i+c) (j+c))
      else charge 0 4 1 (walkRun ext P (f+1) (a+1) (b+1) c)
    | _, _ => ⟨0, 3, 0⟩
  termination_by f _i _j => (f, 1)
/-- e+1 accounts for the actual direct id verification including its boundary;
four more units pay for dictionary/span/branch primitives. -/
def walkRun (ext : Parse → Nat → Nat → Nat) (P : Parse) :
    Nat → Nat → Nat → Nat → LCERun
  | 0, _, _, _ => ⟨0, 1, 0⟩
  | f+1, a, b, c =>
    let e := ext P a b
    let span := P.startAt (a+e) - P.startAt a
    if a+e ≥ P.count ∨ b+e ≥ P.count then ⟨c+span, e+5, e⟩
    else
      let x := P.chunkAt (a+e)
      let y := P.chunkAt (b+e)
      let d := lcpOf x y
      if d < x.length ∧ d < y.length then ⟨c+span+d, e+5, e⟩
      else if x.length ≠ y.length then
        charge (c+span+d) (e+5) (e+1)
          (viaRun ext P f (P.startAt (a+e)+d) (P.startAt (b+e)+d))
      else ⟨c+span+d, e+5, e⟩
  termination_by f _a _b _c => (f, 0)
end

/-- Erasing the instrumentation yields the committed reference, including
its strict-prefix continuation and its exact fuel wiring. -/
theorem runs_answer (P : Parse) (T : Text) (f : Nat) :
    (∀ i j, (viaRun idLCE P f i j).answer = textLCEvia P T f i j) ∧
    (∀ a b c, (walkRun idLCE P f a b c).answer = textLCEwalk P T f a b c) := by
  induction f with
  | zero => constructor <;> intros <;> simp [viaRun, walkRun, textLCEvia, textLCEwalk]
  | succ f ih =>
    have hw : ∀ a b c, (walkRun idLCE P (f+1) a b c).answer =
        textLCEwalk P T (f+1) a b c := by
      intro a b c
      rw [walkRun, textLCEwalk]
      dsimp only
      split
      · rfl
      · split
        · rfl
        · split
          · exact congrArg (fun z => _ + z) (ih.1 _ _)
          · rfl
    refine ⟨?_, hw⟩
    intro i j
    rw [viaRun, textLCEvia]
    cases P.phraseAt i with
    | none => rfl
    | some ao =>
      rcases ao with ⟨a,o⟩
      cases P.phraseAt j with
      | none => rfl
      | some bo =>
        rcases bo with ⟨b,o'⟩
        dsimp only
        split
        · rfl
        · split
          · exact congrArg (fun z => _ + z) (ih.1 _ _)
          · simpa only [charge, Nat.zero_add] using hw (a+1) (b+1) _

/-- Named universal constant in the statement-locked primitive model. -/
def workConstant : Nat := 8

/-- A bound for every fuel, including prematurely exhausted queries.
No fuel term occurs on the right: only comparisons that actually matched. -/
theorem runs_steps (ext : Parse → Nat → Nat → Nat) (P : Parse) (f : Nat) :
    (∀ i j, (viaRun ext P f i j).steps ≤
      workConstant * (1 + (viaRun ext P f i j).pieces)) ∧
    (∀ a b c, (walkRun ext P f a b c).steps ≤
      workConstant * (1 + (walkRun ext P f a b c).pieces)) := by
  induction f with
  | zero => constructor <;> intros <;> simp [viaRun, walkRun, workConstant]
  | succ f ih =>
    have hw : ∀ a b c, (walkRun ext P (f+1) a b c).steps ≤
        workConstant * (1 + (walkRun ext P (f+1) a b c).pieces) := by
      intro a b c
      rw [walkRun]
      dsimp only
      split
      · simp only [workConstant]; omega
      · split
        · simp only [workConstant]; omega
        · split
          · have h := ih.1 (P.startAt (a+ext P a b) +
                lcpOf (P.chunkAt (a+ext P a b)) (P.chunkAt (b+ext P a b)))
              (P.startAt (b+ext P a b) +
                lcpOf (P.chunkAt (a+ext P a b)) (P.chunkAt (b+ext P a b)))
            simp only [charge, workConstant] at h ⊢
            omega
          · simp only [workConstant]; omega
    refine ⟨?_, hw⟩
    intro i j
    rw [viaRun]
    cases P.phraseAt i with
    | none => simp [workConstant]
    | some ao =>
      rcases ao with ⟨a,o⟩
      cases P.phraseAt j with
      | none => simp [workConstant]
      | some bo =>
        rcases bo with ⟨b,o'⟩
        dsimp only
        split
        · simp [workConstant]
        · split
          · have h := ih.1 (i+lcpOf ((P.chunkAt a).drop o) ((P.chunkAt b).drop o'))
              (j+lcpOf ((P.chunkAt a).drop o) ((P.chunkAt b).drop o'))
            simp only [charge, workConstant] at h ⊢
            omega
          · have h := hw (a+1) (b+1)
              (lcpOf ((P.chunkAt a).drop o) ((P.chunkAt b).drop o'))
            simp only [charge, workConstant] at h ⊢
            omega

def textLCESteps (P : Parse) (fuel i j : Nat) : Nat :=
  (viaRun idLCE P fuel i j).steps

def verificationLength (P : Parse) (fuel i j : Nat) : Nat :=
  (viaRun idLCE P fuel i j).pieces

theorem textLCESteps_bound (P : Parse) (fuel i j τ : Nat) :
    textLCESteps P fuel i j ≤
      workConstant * (1 + τ + verificationLength P fuel i j) := by
  have h := (runs_steps idLCE P fuel).1 i j
  simp only [textLCESteps, verificationLength, workConstant] at h ⊢
  omega

/-- Actual calls: each query is a pair of text positions, with common fuel. -/
def querySteps (P : Parse) (fuel : Nat) (qs : List (Nat × Nat)) : Nat :=
  (qs.map (fun q => textLCESteps P fuel q.1 q.2)).sum

def verificationSum (P : Parse) (fuel : Nat) (qs : List (Nat × Nat)) : Nat :=
  (qs.map (fun q => verificationLength P fuel q.1 q.2)).sum

theorem querySteps_bound (P : Parse) (fuel τ : Nat) (qs : List (Nat × Nat)) :
    querySteps P fuel qs ≤ workConstant *
      (qs.length * (1+τ) + verificationSum P fuel qs) := by
  induction qs with
  | nil => simp [querySteps, verificationSum]
  | cons q qs ih =>
    have h := textLCESteps_bound P fuel q.1 q.2 τ
    simp only [querySteps, verificationSum, List.map_cons, List.sum_cons,
      List.length_cons, Nat.add_mul, Nat.mul_add, workConstant] at *
    omega

/-- Dictionary size is the number of stored symbols, not number of phrases. -/
def dictionarySize (P : Parse) : Nat := (P.dict.map List.length).sum

/-- Linear preprocessing charge + the actual r-query trace. -/
def totalWork (P : Parse) (fuel : Nat) (qs : List (Nat × Nat)) : Nat :=
  P.count + dictionarySize P + querySteps P fuel qs

/-- Paper-shaped capstone. τ≥1 absorbs the unavoidable per-query constant.
No claim that verification lengths are O(τ); the sum remains explicit. -/
theorem totalWork_bound (P : Parse) (fuel τ : Nat) (hτ : 1 ≤ τ)
    (qs : List (Nat × Nat)) :
    totalWork P fuel qs ≤ 16 *
      (P.count + dictionarySize P + qs.length * τ + verificationSum P fuel qs) := by
  have h := querySteps_bound P fuel τ qs
  have hr : qs.length ≤ qs.length * τ := by
    simpa using Nat.mul_le_mul_left qs.length hτ
  simp only [totalWork, workConstant, Nat.mul_add, Nat.mul_one] at h ⊢
  omega

/-- The nonzero rows of parseTriplesOf make exactly these adjacent-SA
queries. The zero row uses the literal LCP 0 and makes no LCE call. -/
def parseTripleQueries (T : Text) : List (Nat × Nat) :=
  let ord := saOrder (T.reverse ++ [0])
  (List.range T.length).map (fun k => (ord.getD (k+1) 0, ord.getD k 0))

theorem parseTripleQueries_length (T : Text) :
    (parseTripleQueries T).length = T.length := by
  simp [parseTripleQueries]

/-- LCE-support work of the actual reference construction's query log.
This excludes constructing/sorting the suffix array and emitting triples. -/
def parseTriplesLCEWork (T : Text) (P : Parse) : Nat :=
  totalWork P (T.reverse ++ [0]).length (parseTripleQueries T)

theorem parseTriplesLCEWork_bound (T : Text) (P : Parse) (τ : Nat) (hτ : 1 ≤ τ) :
    parseTriplesLCEWork T P ≤ 16 *
      (P.count + dictionarySize P + T.length * τ +
        verificationSum P (T.reverse ++ [0]).length (parseTripleQueries T)) := by
  have h := totalWork_bound P (T.reverse ++ [0]).length τ hτ (parseTripleQueries T)
  simpa only [parseTriplesLCEWork, parseTripleQueries_length] using h

/-- The counted query still computes the proven oracle, not a synthetic cost
unrelated to an execution. -/
theorem viaRun_correct (P : Parse) (T : Text) (hWF : ParseWF P T)
    (f i j : Nat) (hf : T.length-i ≤ f) (hi : i ≤ T.length) (hj : j ≤ T.length) :
    (viaRun idLCE P f i j).answer = lcpOf (T.drop i) (T.drop j) := by
  rw [(runs_answer P T f).1 i j]
  exact VerifiedLCE_determinism T P hWF f i j hi hj hf

/-! ## Abstract sampled suffix hashes and deterministic verification -/

/-- Algebra supplied by a hash implementation. `cut` removes a suffix hash
from a whole hash to recover the prefix hash of the requested length. -/
structure HashOps (β : Type) where
  hash : List Nat → β
  prepend : Nat → β → β
  cut : β → β → Nat → β

/-- Algebraic hash laws; these do not assume collision freedom. -/
structure HashLaws (H : HashOps β) : Prop where
  cons : ∀ x xs, H.prepend x (H.hash xs) = H.hash (x :: xs)
  cut : ∀ xs n, H.cut (H.hash xs) (H.hash (xs.drop n)) n = H.hash (xs.take n)

/-- τ-spaced suffix samples (a function specification of the stored table). -/
def krs (H : HashOps β) (xs : List Nat) (τ k : Nat) : β :=
  H.hash (xs.drop (k*τ))

/-- Reconstruct an arbitrary suffix by at most τ direct reads before the
next stored sample. Out-of-range samples denote the empty suffix. -/
def suffixHash (H : HashOps β) (xs : List Nat) (τ i : Nat) : β :=
  let endPos := (i/τ+1)*τ
  ((xs.drop i).take (endPos-i)).foldr H.prepend (krs H xs τ (i/τ+1))

/-- Two reconstructed suffix hashes determine a chunk hash. -/
def chunkHash (H : HashOps β) (xs : List Nat) (τ i n : Nat) : β :=
  H.cut (suffixHash H xs τ i) (suffixHash H xs τ (i+n)) n

theorem hash_foldr (H : HashOps β) (hH : HashLaws H) (xs ys : List Nat) :
    xs.foldr H.prepend (H.hash ys) = H.hash (xs ++ ys) := by
  induction xs with
  | nil => rfl
  | cons x xs ih => simp only [List.foldr_cons, ih, hH.cons, List.cons_append]

theorem next_sample_bounds (i τ : Nat) (hτ : 0 < τ) :
    i ≤ (i/τ+1)*τ ∧ (i/τ+1)*τ-i ≤ τ := by
  have he := Nat.mod_add_div i τ
  have hm := Nat.mod_lt i hτ
  rw [Nat.add_mul, Nat.one_mul, Nat.mul_comm (i/τ)]
  omega

theorem suffixHash_correct (H : HashOps β) (hH : HashLaws H)
    (xs : List Nat) (τ i : Nat) (hτ : 0 < τ) :
    suffixHash H xs τ i = H.hash (xs.drop i) := by
  have hb := (next_sample_bounds i τ hτ).1
  unfold suffixHash krs
  rw [hash_foldr H hH]
  have hd : xs.drop ((i/τ+1)*τ) = (xs.drop i).drop ((i/τ+1)*τ-i) := by
    rw [List.drop_drop]
    congr 1
    omega
  rw [hd, List.take_append_drop]

theorem chunkHash_correct (H : HashOps β) (hH : HashLaws H)
    (xs : List Nat) (τ i n : Nat) (hτ : 0 < τ) :
    chunkHash H xs τ i n = H.hash ((xs.drop i).take n) := by
  unfold chunkHash
  rw [suffixHash_correct H hH xs τ i hτ,
    suffixHash_correct H hH xs τ (i+n) hτ]
  have hd : xs.drop (i+n) = (xs.drop i).drop n := by rw [List.drop_drop]
  rw [hd, hH.cut]

/-- No false equality among chunks of THESE two finite inputs. This is an
instance-specific hypothesis, not global injectivity of a finite-word hash.
Offsets/lengths outside the inputs merely repeat the empty/full chunks. -/
def HashGood (H : HashOps β) (x y : List Nat) : Prop :=
  ∀ a b n, H.hash ((x.drop a).take n) = H.hash ((y.drop b).take n) →
    (x.drop a).take n = (y.drop b).take n

theorem hashGood_drop (H : HashOps β) {x y : List Nat}
    (hg : HashGood H x y) (a b : Nat) : HashGood H (x.drop a) (y.drop b) := by
  intro u v n he
  simp only [List.drop_drop] at he ⊢
  exact hg (a+u) (b+v) n he

/-- Two-sided certificate: every proposed symbol agrees AND the next symbols
differ (or a suffix ends). A final-block-only check would be insufficient. -/
def VerifiedLCE (x y : List Nat) (n : Nat) : Prop :=
  n ≤ x.length ∧ n ≤ y.length ∧ x.take n = y.take n ∧
    (n = x.length ∨ n = y.length ∨ x.getD n 0 ≠ y.getD n 0)

instance (x y : List Nat) (n : Nat) : Decidable (VerifiedLCE x y n) :=
  inferInstanceAs (Decidable (_ ∧ _ ∧ _ ∧ _))

theorem verified_iff (x y : List Nat) (n : Nat) :
    VerifiedLCE x y n ↔ n = lcpOf x y := by
  constructor
  · rintro ⟨hx,hy,hpre,hend⟩
    have hn := (take_eq_iff_le_lcpOf x y n ⟨hx,hy⟩).mp hpre
    have hl := lcpOf_le_left x y
    have hr := lcpOf_le_right x y
    rcases hend with he | he | he
    · omega
    · omega
    · apply Classical.byContradiction
      intro hne
      have hlt : n < lcpOf x y := by omega
      exact he (lcpOf_getD_eq x y n hlt)
  · intro hn
    subst n
    have hx := lcpOf_le_left x y
    have hy := lcpOf_le_right x y
    refine ⟨hx,hy,(take_eq_iff_le_lcpOf x y _ ⟨hx,hy⟩).mpr (by omega), ?_⟩
    by_cases h1 : lcpOf x y = x.length
    · exact Or.inl h1
    by_cases h2 : lcpOf x y = y.length
    · exact Or.inr (Or.inl h2)
    exact Or.inr (Or.inr (lcpOf_getD_ne x y _ rfl (by omega) (by omega)))

/-- A bad proposal fails loudly (`none`), as the production verifier does;
it never silently turns a hash collision into a wrong LCE. -/
def verifyLCE (x y : List Nat) (guess : Nat) : Option Nat :=
  if VerifiedLCE x y guess then some guess else none

theorem verifyLCE_exact (x y : List Nat) (guess n : Nat)
    (h : verifyLCE x y guess = some n) : n = lcpOf x y := by
  unfold verifyLCE at h
  split at h
  · rename_i hc
    cases h
    exact (verified_iff x y guess).mp hc
  · contradiction

/-- A block-jump variant: equal hash blocks advance τ phrases. The first
unequal or short block is directly read; the separate verifier then checks
ALL skipped blocks and the boundary. Fuel exhaustion also directly reads.
The jump schedule is deliberately simple, not the C++ exponential search. -/
def fpGuess [DecidableEq β] (H : HashOps β) (τ : Nat) :
    Nat → List Nat → List Nat → Nat
  | 0, x, y => lcpOf x y
  | f+1, x, y =>
    if τ ≤ x.length ∧ τ ≤ y.length ∧
        chunkHash H x τ 0 τ = chunkHash H y τ 0 τ then
      τ + fpGuess H τ f (x.drop τ) (y.drop τ)
    else lcpOf x y

/-- Peeling an equal, complete block. -/
theorem lcpOf_peel_take (x y : List Nat) (τ : Nat)
    (_hx : τ ≤ x.length) (hy : τ ≤ y.length) (he : x.take τ = y.take τ) :
    lcpOf x y = τ + lcpOf (x.drop τ) (y.drop τ) := by
  have h := lcp_append_equal (x.take τ) (x.drop τ) (y.drop τ)
  rw [List.take_append_drop, he, List.take_append_drop] at h
  simpa only [List.length_take, Nat.min_eq_left hy] using h

theorem fpGuess_correct [DecidableEq β] (H : HashOps β) (hH : HashLaws H)
    {x y : List Nat} (hg : HashGood H x y) (τ : Nat) (hτ : 0 < τ) (f : Nat) :
    fpGuess H τ f x y = lcpOf x y := by
  induction f generalizing x y with
  | zero => rfl
  | succ f ih =>
    rw [fpGuess]
    split
    · rename_i h
      have he := h.2.2
      rw [chunkHash_correct H hH x τ 0 τ hτ,
        chunkHash_correct H hH y τ 0 τ hτ] at he
      simp only [List.drop_zero] at he
      rw [ih (hashGood_drop H hg τ τ), lcpOf_peel_take x y τ h.1 h.2.1 (hg 0 0 τ he)]
    · rfl

def fpParseLCE [DecidableEq β] (H : HashOps β) (τ : Nat)
    (P : Parse) (a b : Nat) : Option Nat :=
  let x := P.ids.drop a
  let y := P.ids.drop b
  verifyLCE x y (fpGuess H τ P.count x y)

/-- Collision-independent soundness: accepted answers are the reference idLCE. -/
theorem fpParseLCE_sound [DecidableEq β] (H : HashOps β) (τ : Nat)
    (P : Parse) (a b n : Nat) (h : fpParseLCE H τ P a b = some n) :
    n = idLCE P a b :=
  verifyLCE_exact _ _ _ n h

/-- HashGood gives successful return, not just soundness conditional on success. -/
theorem fpParseLCE_correct [DecidableEq β] (H : HashOps β) (hH : HashLaws H)
    (P : Parse) (hg : HashGood H P.ids P.ids) (τ : Nat) (hτ : 0 < τ) (a b : Nat) :
    fpParseLCE H τ P a b = some (idLCE P a b) := by
  unfold fpParseLCE
  dsimp only
  rw [fpGuess_correct H hH (hashGood_drop H hg a b) τ hτ]
  simp only [verifyLCE, verified_iff, ite_true, idLCE]

/-- A total adapter for composition; on failure it directly recomputes the id
LCE. Production chooses fail-loud instead; accepted answers coincide either way. -/
def fpExtension [DecidableEq β] (H : HashOps β) (τ : Nat)
    (P : Parse) (a b : Nat) : Nat :=
  (fpParseLCE H τ P a b).getD (idLCE P a b)

theorem fpExtension_eq [DecidableEq β] (H : HashOps β) (τ : Nat) :
    fpExtension H τ = idLCE := by
  funext P a b
  unfold fpExtension
  cases h : fpParseLCE H τ P a b with
  | none => rfl
  | some n => exact fpParseLCE_sound H τ P a b n h

def fpTextLCE [DecidableEq β] (H : HashOps β) (τ : Nat)
    (P : Parse) (fuel i j : Nat) : Nat :=
  (viaRun (fpExtension H τ) P fuel i j).answer

/-- The functional weld, independent of collisions, with explicit fallback. -/
theorem fpTextLCE_eq [DecidableEq β] (H : HashOps β) (τ : Nat)
    (P : Parse) (T : Text) (fuel i j : Nat) :
    fpTextLCE H τ P fuel i j = textLCEvia P T fuel i j := by
  unfold fpTextLCE
  rw [fpExtension_eq]
  exact (runs_answer P T fuel).1 i j

/-- Oracle substitution only needs agreement on the parse being queried. -/
theorem runs_oracle_congr (ext ext' : Parse → Nat → Nat → Nat) (P : Parse)
    (he : ext P = ext' P) (f : Nat) :
    (∀ i j, viaRun ext P f i j = viaRun ext' P f i j) ∧
    (∀ a b c, walkRun ext P f a b c = walkRun ext' P f a b c) := by
  induction f with
  | zero => constructor <;> intros <;> simp [viaRun, walkRun]
  | succ f ih =>
    have hw : ∀ a b c, walkRun ext P (f+1) a b c = walkRun ext' P (f+1) a b c := by
      intro a b c
      rw [walkRun, walkRun]
      simp only [he]
      split
      · rfl
      · split
        · rfl
        · split
          · rw [ih.1]
          · rfl
    refine ⟨?_, hw⟩
    intro i j
    rw [viaRun, viaRun]
    cases P.phraseAt i with
    | none => rfl
    | some ao =>
      rcases ao with ⟨a,o⟩
      cases P.phraseAt j with
      | none => rfl
      | some bo =>
        rcases bo with ⟨b,o'⟩
        dsimp only
        split
        · rfl
        · split
          · rw [ih.1]
          · rw [hw]

/-- No-fallback weld: any extension returning accepted fingerprint answers
on THIS parse can be substituted into the reference. This is the contract
usable by a fail-loud implementation; failing executions need not return. -/
theorem verifiedExtension_weld [DecidableEq β] (H : HashOps β) (τ : Nat)
    (ext : Parse → Nat → Nat → Nat) (P : Parse)
    (haccept : ∀ a b, fpParseLCE H τ P a b = some (ext P a b))
    (T : Text) (fuel i j : Nat) :
    (viaRun ext P fuel i j).answer = textLCEvia P T fuel i j := by
  have he : ext P = idLCE P := by
    funext a b
    exact fpParseLCE_sound H τ P a b (ext P a b) (haccept a b)
  rw [(runs_oracle_congr ext idLCE P he fuel).1 i j]
  exact (runs_answer P T fuel).1 i j

/-- Ties the hash scheme to the existing named determinism theorem. -/
theorem fpTextLCE_determinism [DecidableEq β] (H : HashOps β) (τ : Nat)
    (P : Parse) (T : Text) (hWF : ParseWF P T) (fuel i j : Nat)
    (hi : i ≤ T.length) (hj : j ≤ T.length) (hf : T.length-i ≤ fuel) :
    fpTextLCE H τ P fuel i j = lcpOf (T.drop i) (T.drop j) := by
  rw [fpTextLCE_eq H τ P T]
  exact VerifiedLCE_determinism T P hWF fuel i j hi hj hf

/-! ## Hash work: actual probes plus complete verification

A probe uses four suffix reconstructions (≤τ reads each), two cuts and
control. Direct comparisons charge one unit per compared pair, plus one
terminal test. The stored-sample/hash algebra operations are unit cost.
This is a bound for the block schedule above, not the different exponential
schedule or machine arithmetic in bit6/slim_lce.hpp.
-/

def fpProbeSteps [DecidableEq β] (H : HashOps β) (τ : Nat) :
    Nat → List Nat → List Nat → Nat
  | 0, x, y => lcpOf x y + 1
  | f+1, x, y =>
    if τ ≤ x.length ∧ τ ≤ y.length ∧
        chunkHash H x τ 0 τ = chunkHash H y τ 0 τ then
      (4*τ+6) + fpProbeSteps H τ f (x.drop τ) (y.drop τ)
    else (4*τ+6) + (lcpOf x y + 1)

theorem fpProbeSteps_bound [DecidableEq β] (H : HashOps β) (hH : HashLaws H)
    {x y : List Nat} (hg : HashGood H x y) (τ : Nat) (hτ : 0 < τ) (f : Nat) :
    fpProbeSteps H τ f x y ≤ 10 * (1 + τ + lcpOf x y) := by
  induction f generalizing x y with
  | zero => simp only [fpProbeSteps]; omega
  | succ f ih =>
    rw [fpProbeSteps]
    split
    · rename_i h
      have he := h.2.2
      rw [chunkHash_correct H hH x τ 0 τ hτ,
        chunkHash_correct H hH y τ 0 τ hτ] at he
      simp only [List.drop_zero] at he
      have hp := lcpOf_peel_take x y τ h.1 h.2.1 (hg 0 0 τ he)
      have hb := ih (hashGood_drop H hg τ τ)
      omega
    · omega

/-- Includes every proposed prefix symbol and its terminal boundary test. -/
def fpSteps [DecidableEq β] (H : HashOps β) (τ f : Nat) (x y : List Nat) : Nat :=
  fpProbeSteps H τ f x y + fpGuess H τ f x y + 1

/-- The l_i term here is precisely the query answer in phrase ids. -/
theorem fpSteps_bound [DecidableEq β] (H : HashOps β) (hH : HashLaws H)
    {x y : List Nat} (hg : HashGood H x y) (τ : Nat) (hτ : 0 < τ) (f : Nat) :
    fpSteps H τ f x y ≤ 12 * (1 + τ + lcpOf x y) := by
  have h := fpProbeSteps_bound H hH hg τ hτ f
  unfold fpSteps
  rw [fpGuess_correct H hH hg τ hτ]
  omega

def fpQuerySteps [DecidableEq β] (H : HashOps β) (τ : Nat) (P : Parse)
    (qs : List (Nat × Nat)) : Nat :=
  (qs.map (fun q => fpSteps H τ P.count (P.ids.drop q.1) (P.ids.drop q.2))).sum

def idVerificationSum (P : Parse) (qs : List (Nat × Nat)) : Nat :=
  (qs.map (fun q => idLCE P q.1 q.2)).sum

theorem fpQuerySteps_bound [DecidableEq β] (H : HashOps β) (hH : HashLaws H)
    (P : Parse) (hg : HashGood H P.ids P.ids) (τ : Nat) (hτ : 0 < τ)
    (qs : List (Nat × Nat)) :
    fpQuerySteps H τ P qs ≤ 12 * (qs.length * (1+τ) + idVerificationSum P qs) := by
  induction qs with
  | nil => simp [fpQuerySteps, idVerificationSum]
  | cons q qs ih =>
    have h := fpSteps_bound H hH (hashGood_drop H hg q.1 q.2) τ hτ P.count
    simp only [fpQuerySteps, idVerificationSum, idLCE, List.map_cons,
      List.sum_cons, List.length_cons, Nat.add_mul, Nat.mul_add] at *
    omega

def fpTotalWork [DecidableEq β] (H : HashOps β) (τ : Nat) (P : Parse)
    (qs : List (Nat × Nat)) : Nat :=
  P.count + dictionarySize P + fpQuerySteps H τ P qs

/-- r is the length of the actual fingerprint-query log, l_i its LCE answer.
Preprocessing is charged once. Constants and τ's positivity are explicit. -/
theorem fpTotalWork_bound [DecidableEq β] (H : HashOps β) (hH : HashLaws H)
    (P : Parse) (hg : HashGood H P.ids P.ids) (τ : Nat) (hτ : 0 < τ)
    (qs : List (Nat × Nat)) :
    fpTotalWork H τ P qs ≤ 24 *
      (P.count + dictionarySize P + qs.length * τ + idVerificationSum P qs) := by
  have h := fpQuerySteps_bound H hH P hg τ hτ qs
  have hr : qs.length ≤ qs.length * τ := by
    have h1 : 1 ≤ τ := hτ
    simpa using Nat.mul_le_mul_left qs.length h1
  simp only [fpTotalWork, Nat.mul_add, Nat.mul_one] at h ⊢
  omega

/-! ## O2 statement lock (2026-09-27)

WARRANT RUN FIRST: `lake env lean O2Eval.lean` returned
("O2 bounded warrant: entries, bounded, violations = ", 729, 729, 0).
The generator has 729 entries / 127 distinct texts; all lcp≤MAXINT.
This licenses the successor STATEMENT below, not a proof by finite testing.

The saturation witness is an arbitrary six-triple stream, not `triplesOf T`.
It refutes the universal STREAM claim. It does not refute the original
TEXT-restricted O2: realizing that witness as a valid suffix stream is a
separate obligation. No false-as-stated label is attached to that narrower
claim without such evidence. Sxgc.lean's existing ledger is untouched.
-/

/-- RETIRED-FALSE record: unbounded arbitrary-stream version of O2. -/
def O2_stream_unbounded_FALSE_AS_STATED : Prop :=
  ∀ (N : Nat) (ts : List Triple), (scan N ts).Nodup

def saturationWitness : List Triple :=
  [⟨1,9223372036854775811,6⟩, ⟨2,9223372036854775810,5⟩,
   ⟨1,9223372036854775809,4⟩, ⟨2,9223372036854775808,3⟩,
   ⟨1,9223372036854775807,2⟩, ⟨2,9223372036854775806,1⟩]

set_option maxRecDepth 100000 in
set_option maxHeartbeats 2000000 in
theorem saturation_not_nodup : ¬ (scan 100 saturationWitness).Nodup := by decide

theorem O2_stream_unbounded_refuted : ¬ O2_stream_unbounded_FALSE_AS_STATED := by
  intro h
  exact saturation_not_nodup (h 100 saturationWitness)

/-- Statement-locked bounded TEXT successor; remains an open proposition,
not an asserted theorem or an additional axiom. -/
def O2_bounded : Prop :=
  ∀ (T : Text), positive T = true →
    (∀ t ∈ triplesOf T, t.lcp ≤ MAXINT.toNat) →
    (scan (T.length+1) (triplesOf T)).Nodup

/-! ## HANDOFF (laneR, 2026-09-27)

IMPLEMENTED, kernel checked, no new proof holes:
- `lcpProbes_eq` / `idLCE_probes`: independent recursive comparison counter
  is exactly answer+1, justifying the walk's actual direct-verification charge.
- `runs_answer`: instrumentation erases to both committed mutual functions.
- `runs_steps`, `textLCESteps_bound`: explicit constant 8, actual matched
  phrase-piece trace, every fuel; correctness at sufficient fuel is
  `viaRun_correct`. `totalWork_bound`: constant 16, τ≥1, r=qs.length.
  `parseTriplesLCEWork_bound` specializes to the reference's adjacent-SA
  query log; its length is exactly T.length (sentinel row makes no query).
- `krs`/`suffixHash`/`chunkHash`: sampled suffix algebra proved from HashLaws;
  reconstruction reads at most τ symbols (`next_sample_bounds`).
- `VerifiedLCE` iff exact LCP; `fpParseLCE_sound` independent of hash laws or
  collision freedom. Prefix AND boundary verification is required, across
  the ENTIRE proposal. `fpParseLCE_correct` guarantees return under HashGood.
- `fpTextLCE_eq`/`fpTextLCE_determinism`: total adapter weld; fallback is
  explicitly recomputation on rejection. `verifiedExtension_weld`: separate
  no-fallback contract for implementations whose calls all return accepted
  answers. HashGood supplies that acceptance condition.
- `fpSteps_bound`: 12*(1+τ+l_i), l_i literally the id-LCE answer.
  `fpTotalWork_bound`: 24*(P+|D|+r*τ+Σl_i) for the actual id-query log.
- O2 measured FIRST: 729 entries, all bounded, 0 violations. O2_bounded is
  a locked OPEN PROPOSITION, not a theorem. The arbitrary-stream unbounded
  record is kernel-refuted by saturation_not_nodup; no claim that this
  arbitrary stream is triplesOf a text. The original text-only unbounded
  claim has NOT thereby been refuted. Sxgc.lean remains byte-identical.

SCOPE / REMAINING BRIDGES (do not hide these in a runtime claim):
- These are explicit indexed-primitive charge models, not Lean List runtime
  bounds. Preprocessing is a linear charge definition, not an implemented
  index-builder complexity proof. Phrase location/dictionary LCE/prefix sums
  are supplied primitives. The reference also materializes SA/triples;
  proving their cost is outside this LCE accounting.
- The reference l_i is matched phrase pieces across both grids, including
  strict-prefix restarts; it is not asserted equal to the yeast telemetry's
  per-seed `verified_phrases` field. Fingerprint l_i is exactly the id answer;
  its step charge includes the extra boundary comparison. Both sums stay
  explicit, with no O(τ) worst-case claim.
- The sampled scheme uses fixed-block jumps. bit6/slim_lce.hpp uses an
  exponential/binary guess schedule and two fingerprint levels. Their
  accepted-answer contract is the same two-sided certificate. No compiled
  C++/Rust refinement, overlap-coordinate conversion, source-level runtime
  bound, or bound on its hash probes is proved here. The FULL dictionary
  symbol-verification cost must also be charged for that implementation.
- Hash cost bounds assume HashLaws + instance-local HashGood. Soundness on accepted answers
  does not. No adversarial-collision runtime bound is asserted.
- No proof of O2_bounded is supplied; finite testing is its statement warrant.
  No change to the four existing Sxgc proof holes, nor to BuildEval gates.
- The requested RESEARCH.md "Lean reference implementation lands" entry is
  absent in this ca06712 snapshot (the commit subject records the landing).
  The measured slim-build entry records 15.33 phrases/seed and 22,262 max.
  No contact_supervisor tool was available in the supplied tool catalog.

REVIEW COMMANDS (run from lean/):
  lake build SxgcBuild SxgcBounds
  lake env lean --run Main.lean
  lake env lean BuildEval.lean
  lake env lean BoundsEval.lean
  lake env lean O2Eval.lean
  lake env lean BoundsAxioms.lean
  lake env lean counterexamples/saturation_refutation.lean
RESULTS: targeted and default builds successful; Main: 511 pass / 0 fail, BIT 1B GREEN;
BuildEval: true/true/true/false unchanged. BoundsEval: all 11 assert gates
GREEN, including modular hash instance checks and injected bad proposals.
Numerical fingerprint (answer, steps, RHS at τ=2): (0,16,36), (1,18,48),
(2,32,60), (5,50,96), (12,112,180). Reference spot: steps 9 ≤ 32 for
one matched phrase piece at τ=2. O2Eval: 729/729 bounded, 0 violations,
127 distinct texts; saturation sorted emissions [94,95,95,96,96,97].
The axiom audit enumerates EVERY new theorem (32); permitted axioms only
propext, Classical.choice, Quot.sound. Sxgc.lean and BuildEval.lean are
byte-identical to HEAD; SxgcBuild's executable/proof code is unchanged.
Proof-hole counts: Sxgc 4 (unchanged), SxgcBuild 0, SxgcBounds 0.
No native evaluation proof tactic, git commits, or staging. Independent
reviewer acceptance remains required; these checked gates do not replace it.
-/

end SxgcBounds
