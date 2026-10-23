import Sxgc

/-!
# PROB-CHI — proving the VERIFIER, not the constructor

The operator's question: *"How probabilistic could we go? chi can be a bit wrong
- if the errors are really rare it might still be a powerful object. We can keep
the text around and check. We could emit errors in some structured way."*

The pipeline's answer (`tools/binbwt/PROB_CHI_ANALYSIS.md`, `bit6/pile_finish.cpp`,
`bit6/slim_lce.hpp`) is a split: the CONSTRUCTOR may become probabilistic (it
stops byte-confirming every LCE answer and emits a claims list), and exactness
moves to a CHECKER that reads the text at the claimed positions. This file
formalizes the half of that argument that is a proof obligation at all: **the
verifier**. The constructor's probability is a *performance* claim; the product's
certainty is a *soundness* claim, and soundness is what a proof can carry.

## What is claimed here (and what is honestly not)

A claim, in the exact shape the C++ emits (`PfClaimView` in
`bit6/pile_finish.cpp`), is a quadruple `(i, j, g, truncated)` meaning

* `T[i..i+g) = T[j..j+g)`                                  (`SpanEq`)
* and either the answer was capped by the QUERY (`g = cap`, truncated), or the
  next symbols separate the two suffixes: `T[i+g] ≠ T[j+g]`    (`Sep`)

Theorems proved here, with no `sorry`:

* `checkClaim_sound`      — the checker accepts only true claims (the verifier's
  soundness; this is the theorem about the C++ loop `pf_claim_violation`).
* `sep_iff_not_span_succ` — `Sep` is exactly "the span stops growing at `g`".
* `spanEq_mono`           — span equality is monotone in the length.
* `certified_unique`      — **the product statement at the fact level**: two
  answers that both satisfy the certified-fact predicate are EQUAL. So the
  hash-trusted constructor's answer and the full tier's byte-verified answer
  cannot differ once the claims list is accepted.
* `full_tier_is_certified` — the FULL tier's own verifier (prefix + boundary,
  `slim_lce.hpp` `lce()`) implies the same predicate, so the uniqueness theorem
  applies to it.
* `answers_determine_output` — any downstream function of the answer list gives
  the same output for two pointwise-equal answer lists. Combined with the above:
  **an accepted claims list forces the same product as the exact constructor.**

## The honest gaps (not proved, deliberately stated)

1. EXTRACTION. The constructor, the checker and the sweep are C++ that is not
   extracted into Lean. What is proved is the *logic* of the verifier and the
   uniqueness of the certified fact; the bridge to the running binary is
   (a) the completeness ledger (`PILE_FINISH_CLAIMS_LEDGER`: claims written ==
   hash-derived answers, asserted at runtime, fail-loud) and (b) the
   `pf_claim_violation` code being a literal transcription of `checkClaim`
   below. A proved compiled verifier would need extraction/refinement; that is
   the next lever, not this file.
2. THE SWEEP. `answers_determine_output` is generic in the downstream function.
   The concrete sweep (`Sweep::advance`, `bit6/pile_finish.cpp`) is a
   deterministic state machine over `(char, topLcp, interiorMin)` with no text
   access at all (its own docstring says so), so "the product is a function of
   the answers" is a property of that code, not something this file proves.
3. CYCLIC COMPOSITION. The C++ LCE composes segment claims around the cyclic
   text (`LceWrap::operator()`, which restarts at wrapped coordinates). The
   theorems here are per SEGMENT; the composition is a simple loop whose
   correctness is `min`-arithmetic over already-certified segments and is left
   to the same extraction step as (1).
4. ARITHMETIC. `sym` reads out of bounds as `0` (a `List` totalization), so every
   theorem that needs bounds carries them as hypotheses. The C++ asserts them
   (`pf_check` in `pf_claim_violation`), fail-loud.
5. Bit-level equality of the container also depends on the encoder; that is
   covered by the standing byte-identity gates at 100 MB / 1 GB, not by this file.
-/

namespace SxgcProb

open Sxgc

/-! ## The text model and the claim language -/

/-- Total byte read: out-of-bounds is `0` (see gap 4). -/
def sym (T : Text) (p : Nat) : Nat := T.getD p 0

/-- One claim, exactly the C++ record: positions, claimed answer, and whether the
answer was truncated by the query's cap rather than by the text. -/
structure Claim where
  i : Nat
  j : Nat
  g : Nat
  truncated : Bool
deriving Repr, DecidableEq

/-- `T[i..i+g) = T[j..j+g)`, symbol by symbol. -/
def SpanEq (T : Text) (i j g : Nat) : Prop := ∀ k, k < g → sym T (i + k) = sym T (j + k)

/-- The suffixes separate exactly at distance `g`: the maximality fact. -/
def Sep (T : Text) (i j g : Nat) : Prop := sym T (i + g) ≠ sym T (j + g)

/-- A claim is TRUE of the text: both of its facts hold. -/
def ClaimTrue (T : Text) (c : Claim) : Prop :=
  SpanEq T c.i c.j c.g ∧ (c.truncated = true ∨ Sep T c.i c.j c.g)

/-! ## The checker (a literal transcription of `pf_claim_violation`) -/

/-- Decidable span equality, the `for (k = 0; k < g; ++k)` loop. -/
def spanEqB (T : Text) (i j g : Nat) : Bool :=
  (List.range g).all (fun k => sym T (i + k) == sym T (j + k))

/-- THE CHECKER. Returns `true` iff the text satisfies both claim facts. This is
`pf_claim_violation(...) == UINT64_MAX` in the C++. -/
def checkClaim (T : Text) (c : Claim) : Bool :=
  spanEqB T c.i c.j c.g &&
    (c.truncated || sym T (c.i + c.g) != sym T (c.j + c.g))

/-- The record's own bounds check: the C++ `pf_check`s that the claimed span is
inside the text, and that a non-truncated claim has its boundary byte inside it.
A record that fails this is not a claim at all (the C++ aborts, it does not
return a verdict), so the batteries below treat it as rejected. -/
def inRange (T : Text) (c : Claim) : Bool :=
  decide (c.i + c.g ≤ T.length ∧ c.j + c.g ≤ T.length ∧
          (c.truncated = true ∨ (c.i + c.g + 1 ≤ T.length ∧ c.j + c.g + 1 ≤ T.length)))

/-- THE CHECKER AS THE PILE RUNS IT: bounds assertion + fact check. -/
def checkClaimRec (T : Text) (c : Claim) : Bool := inRange T c && checkClaim T c

/-- A whole claims list is accepted iff every claim in it is. -/
def checkClaims (T : Text) (cs : List Claim) : Bool := cs.all (checkClaimRec T)

/-! ## Basic lemmas -/

theorem spanEqB_sound {T : Text} {i j g : Nat} (h : spanEqB T i j g = true) :
    SpanEq T i j g := by
  intro k hk
  have hmem : k ∈ List.range g := List.mem_range.mpr hk
  have hall : ∀ x ∈ List.range g, (sym T (i + x) == sym T (j + x)) = true :=
    List.all_eq_true.mp h
  have := hall k hmem
  simpa using this

theorem spanEqB_complete {T : Text} {i j g : Nat} (h : SpanEq T i j g) :
    spanEqB T i j g = true := by
  refine List.all_eq_true.mpr ?_
  intro k hk
  have hk' : k < g := List.mem_range.mp hk
  simpa using h k hk'

theorem spanEqB_iff (T : Text) (i j g : Nat) : spanEqB T i j g = true ↔ SpanEq T i j g :=
  ⟨spanEqB_sound, spanEqB_complete⟩

/-- **VERIFIER SOUNDNESS.** An accepted claim is a true claim. -/
theorem checkClaim_sound {T : Text} {c : Claim} (h : checkClaim T c = true) :
    ClaimTrue T c := by
  unfold checkClaim at h
  rw [Bool.and_eq_true] at h
  obtain ⟨hspan, hrest⟩ := h
  refine ⟨spanEqB_sound hspan, ?_⟩
  rw [Bool.or_eq_true] at hrest
  rcases hrest with hb | hb
  · exact Or.inl hb
  · refine Or.inr ?_
    have hne : (sym T (c.i + c.g) != sym T (c.j + c.g)) = true := hb
    unfold Sep
    exact bne_iff_ne.mp hne

/-- A span that stops growing, GIVEN that it matched up to `g`: `Sep` is exactly
the negation of one more symbol. (The hypothesis is essential: without it a
shorter span can already have failed.) -/
theorem sep_iff_not_span_succ {T : Text} {i j g : Nat} (hg : SpanEq T i j g) :
    Sep T i j g ↔ ¬ SpanEq T i j (g + 1) := by
  constructor
  · intro hsep hspan
    exact hsep (hspan g (Nat.lt_succ_self g))
  · intro hnot heq
    apply hnot
    intro k hk
    rcases Nat.lt_or_ge k g with hlt | hge
    · exact hg k hlt
    · have hke : k = g := by omega
      subst hke
      exact heq

/-- Span equality is monotone in the length. -/
theorem spanEq_mono {T : Text} {i j : Nat} : ∀ {g g' : Nat}, g ≤ g' → SpanEq T i j g' →
    SpanEq T i j g := by
  intro g g' hle h k hk
  exact h k (Nat.lt_of_lt_of_le hk hle)

/-! ## The certified-answer predicate and its uniqueness

The finish consumes exactly one number per query: `min (lce, cap)`. The predicate
below is what the checker certifies about that number, stated so that it does not
need the recursive `lce` function at all: the span of length `g` matches, and the
answer is either capped by the query or the span does not extend. -/

/-- `g` is a CERTIFIED answer for `(i, j, cap)`. -/
def Certified (T : Text) (i j cap g : Nat) : Prop :=
  g ≤ cap ∧ SpanEq T i j g ∧ (g = cap ∨ ¬ SpanEq T i j (g + 1))

/-- **THE PRODUCT STATEMENT AT THE FACT LEVEL.** Two answers that both satisfy
the certified predicate for the same query are equal: the hash-trusted tier's
answer and the full tier's byte-verified answer cannot disagree. -/
theorem certified_unique {T : Text} {i j cap g g' : Nat}
    (hg : Certified T i j cap g) (hg' : Certified T i j cap g') : g = g' := by
  obtain ⟨hgcap, hgspan, hgext⟩ := hg
  obtain ⟨hg'cap, hg'span, hg'ext⟩ := hg'
  rcases Nat.lt_trichotomy g g' with hlt | heq | hgt
  · -- g < g': the longer certified answer forces the shorter one to extend
    exfalso
    have hstep : SpanEq T i j (g + 1) := spanEq_mono (Nat.succ_le_of_lt hlt) hg'span
    rcases hgext with heq | hnot
    · omega          -- g = cap and g' ≤ cap with g < g' is impossible
    · exact hnot hstep
  · exact heq
  · -- g' < g: symmetric
    exfalso
    have hstep : SpanEq T i j (g' + 1) := spanEq_mono (Nat.succ_le_of_lt hgt) hgspan
    rcases hg'ext with heq | hnot
    · omega
    · exact hnot hstep

/-- The FULL tier's verifier (`slim_lce.hpp` `lce()`: it walks the proposed
prefix byte by byte and then requires the boundary symbols to differ) certifies
the same predicate, PROVIDED the claim is the truncated one exactly when the
answer hit the query's cap - which is how the C++ builds the record
(`truncated = (g == cap)`). -/
theorem full_tier_is_certified {T : Text} {c : Claim} {cap : Nat}
    (htr : c.truncated = true ↔ c.g = cap) (hle : c.g ≤ cap)
    (h : checkClaim T c = true) : Certified T c.i c.j cap c.g := by
  obtain ⟨hspan, hext⟩ := checkClaim_sound h
  refine ⟨hle, hspan, ?_⟩
  rcases hext with hb | hsep
  · exact Or.inl (htr.mp hb)
  · exact Or.inr ((sep_iff_not_span_succ hspan).mp hsep)

/-! ## From facts to the product

The sweep is a deterministic function of the list of answers (it reads no text:
see `Sweep`'s docstring). So equality of the answer lists is equality of the
container, for ANY downstream encoder. Stated generically because the encoder is
not extracted (gap 1/2). -/

/-- Two pointwise-equal answer lists produce the same output downstream. -/
theorem answers_determine_output {α β : Type} (encode : List α → β) {l₁ l₂ : List α}
    (h : l₁ = l₂) : encode l₁ = encode l₂ := by
  rw [h]

/-- The claims list carries the query's cap, so the accepted-claims hypothesis of
the two theorems above is exactly `checkClaims T cs = true` over the emitted
records. -/
theorem accepted_claims_are_true {T : Text} {cs : List Claim}
    (h : checkClaims T cs = true) : ∀ c ∈ cs, ClaimTrue T c := by
  intro c hc
  have hall : ∀ x ∈ cs, checkClaimRec T x = true := List.all_eq_true.mp h
  have hc2 : checkClaim T c = true := by
    have hrec := hall c hc
    unfold checkClaimRec at hrec
    rw [Bool.and_eq_true] at hrec
    exact hrec.2
  exact checkClaim_sound hc2

/-! ## Executable battery: the checker against a brute-force oracle

Tiny-scale, adversarial-first, in the spirit of the repo's other batteries
(`PhiEval.lean`): for every pair of positions of every text in the family, the
checker must ACCEPT exactly the claims whose answer equals the brute-force
`min (lce, cap)`, and must REJECT the deliberately corrupted neighbours (off by
-1 and by +1). The corrupted direction is the whole point: a hash-trusted
constructor that is wrong by one symbol must not survive the checker. -/

/-- Brute-force LCE with explicit fuel, independent of the checker's loop. -/
def bruteLceFuel (T : Text) : Nat → Nat → Nat → Nat
  | 0, _, _ => 0
  | f + 1, i, j =>
      if h : i < T.length ∧ j < T.length then
        (if T[i] = T[j] then 1 + bruteLceFuel T f (i + 1) (j + 1) else 0)
      else 0

/-- The C++ cap (`slim_lce.hpp` `lce()`): `cap = min (limit, n - i, n - j)`.
Every real record satisfies this, and it is what makes a suffix that runs into
the end of the text a TRUNCATED claim (no boundary byte exists). -/
def clampCap (T : Text) (i j cap : Nat) : Nat :=
  min cap (min (T.length - i) (T.length - j))

/-- The true answer the finish consumes: `min (lce, cap)`. -/
def trueAnswer (T : Text) (i j cap : Nat) : Nat :=
  min (bruteLceFuel T (T.length - i) i j) (clampCap T i j cap)

/-- The claim a constructor emits for a given answer (the C++ record). -/
def claimOf (i j cap g : Nat) : Claim := ⟨i, j, g, decide (g = cap)⟩

def texts : List Text :=
  [[], [1], [1, 1], [1, 2], [2, 1], [0, 0, 0], [1, 1, 1, 1], [1, 2, 1, 2, 1, 2],
   [2, 2, 1, 2, 2, 1, 2, 2], [1, 2, 3, 1, 2, 3, 1, 2, 3],
   (List.range 24).map (fun x => x % 2 + 1),
   (List.range 24).map (fun x => x % 3 + 1),
   (List.range 24).map (fun x => x * x % 5 + 1),
   (List.range 40).map (fun _ => 7)]

/-- All (i, j, cap) cases of one text, with the answers the constructor would
emit and the two corrupted neighbours. -/
def cases (T : Text) : List (Nat × Nat × Nat) :=
  (List.range (T.length + 1)).flatMap (fun i =>
    (List.range (T.length + 1)).flatMap (fun j =>
      [0, 1, 2, T.length, T.length + 3].map (fun cap => (i, j, cap))))

/-- Violations of the checker's contract over one text: (accepted-but-false,
rejected-but-true, corruption-survived). -/
def audit (T : Text) : Nat × Nat × Nat :=
  cases T |>.foldl (fun (acc : Nat × Nat × Nat) (q : Nat × Nat × Nat) =>
    let (i, j, cap0) := q
    let cap := clampCap T i j cap0
    let g := trueAnswer T i j cap
    let ok := claimOf i j cap g
    let low := claimOf i j cap (g - 1)
    let high := claimOf i j cap (g + 1)
    let A := (checkClaimRec T ok)            -- must be true
    let B := (checkClaimRec T low)           -- must be false when g > 0
    let C := (checkClaimRec T high && decide (high.g ≤ cap)) -- must be false when g < cap
    let (a, b, c) := acc
    (a + (if A then 0 else 1),
     b + (if B && decide (g > 0) then 1 else 0),
     c + (if C then 1 else 0))) (0, 0, 0)

#eval do
  let res := texts.map (fun (T : Text) => audit T)
  let acc := res.foldl (fun (x : Nat × Nat × Nat) (y : Nat × Nat × Nat) =>
    (x.1 + y.1, x.2.1 + y.2.1, x.2.2 + y.2.2)) (0, 0, 0)
  let positions := (texts.map (fun (T : Text) => (T.length + 1) * (T.length + 1) * 5)).foldl (·+·) 0
  IO.println s!"PROB-CHI VERIFIER: {texts.length} texts, {positions} (i,j,cap) cases; \
    rejected-true={acc.1} accepted-false={acc.2.1} corruption-survived={acc.2.2}"
  unless acc.1 == 0 && acc.2.1 == 0 && acc.2.2 == 0 do
    throw (IO.userError "PROB-CHI VERIFIER RED")

/-- The uniqueness theorem, checked executably: for every case, the TRUE answer
is the only answer the checker accepts among `g-1, g, g+1, g+2` THAT THE QUERY
COULD HAVE PRODUCED.

The `x ≤ cap` filter is not a convenience: the checker certifies FACTS, and it
never sees the query's cap (the record carries only the `truncated` bit). A
`g > cap` claim can be a perfectly true fact - it is a non-answer, and the
constructor emits it never. Cap compliance (`g ≤ cap`, and `truncated ↔ g = cap`)
is a constructor invariant; it is what `Certified` carries and what makes
`certified_unique` a statement about ANSWERS rather than about facts. -/
def uniqueAudit (T : Text) : Nat :=
  (cases T).foldl (fun bad (q : Nat × Nat × Nat) =>
    let (i, j, cap0) := q
    let cap := clampCap T i j cap0
    let g := trueAnswer T i j cap
    -- every accepted neighbour must equal the true answer
    let nb := [g - 1, g, g + 1, g + 2].filter (fun x => x ≤ cap)
    let survived := (nb.filter (fun x => checkClaimRec T (claimOf i j cap x))).filter (fun x => x != g)
    bad + survived.length) 0

#eval do
  let bad := (texts.map (fun (T : Text) => uniqueAudit T)).foldl (·+·) 0
  IO.println s!"PROB-CHI UNIQUENESS: {texts.length} texts; accepted-answer-is-unique violations={bad}"
  unless bad == 0 do throw (IO.userError "PROB-CHI UNIQUENESS RED")

end SxgcProb
