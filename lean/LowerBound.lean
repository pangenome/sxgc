import Sxgc

/-!
# Lower-bound scaffold (LOWER_BOUND_PLAN, Part 4 Steps 1–3 + Step-4 machinery)

Statement-locked scaffold for the χ-necessity program (see `LOWER_BOUND_PLAN.md`
and `ATTACK1_BATTERY.md`):

  * Step 1 — the M0 index model and the suffixient operation class
    (definitions only; alphabet-honest: texts and patterns are bare
    `List Nat`, no SIGMA bound is used anywhere).
  * Step 2 — the S1 bridge: any correct locate-one oracle emits a
    suffixient position set on the requirement words, hence
    `chi T ≤ |emitted f T|` (kernel-checked, no sorry).
  * Step 3 — `family_counting`: the pigeonhole core of the eventual floor
    proof — a fixed decoder, bit-space indexes, and pairwise-incompatible
    correct answers force one distinct index per family member
    (kernel-checked, no sorry).
  * Step 4 machinery — the executable pieces the Attack-1 battery drives:
    `validCovers` (the positions a correct oracle may answer), `fFirst`
    (the canonical first-cover oracle), and the witness-perturbation
    family definitions (in `Attack1Eval.lean`, alongside the battery).

Refinements relative to the plan (definitional correctness only; documented,
never adopted to dodge difficulty):

  1. `CorrectLocateOne` requires the answer to be a genuine 1-based text
     position (`1 ≤ x ∧ x ≤ T.length`). The plan's coversAt-only variant
     admits degenerate oracles — e.g. always answering `T.length + 1`,
     where `pref T x = T` so every word is "covered" — which is not
     locating an occurrence in any operational sense. What `coversAt`
     alone guarantees is recorded separately as `le_of_coversAt`.
  2. `Realizes` is parameterized by a FIXED decoder `dec : Index → Answer`.
     The plan's per-index existential decoder makes counting impossible
     (a single bit string can realize every answer function through its
     own decoder); one fixed decoder is what "an index format" means.
  3. `family_counting` assumes `F.Nodup` (a family of texts is a set) and
     concludes `F.length ≤ 2^(s+1) - 1` from `space D ≤ s`; the plan's
     `≤ 2^s` shape is the corollary `family_counting_lt` under
     `space D + 1 ≤ s`.
-/

namespace Sxgc.LowerBound

open Sxgc

/-! ## Step 1: the index model and the suffixient operation class -/

/-- An answer function: pattern ↦ optional 1-based text position. -/
abbrev Answer := List Nat → Option Nat

/-- O_req / O_loc1 correctness: every occurring word is answered with a
genuine 1-based text position at which that occurrence of the word ends
(the `coversAt` covering predicate).  Alphabet-honest: any `List Nat`
word; no SIGMA restriction.  (For the empty text no correct oracle exists
— there is no position to report — so all statements below are vacuous
there, which is fine: the empty text has no locate-one operation.) -/
def CorrectLocateOne (f : Answer) (T : Text) : Prop :=
  ∀ w, occurs w T = true →
    ∃ x, f w = some x ∧ 1 ≤ x ∧ x ≤ T.length ∧ coversAt w x T = true

/-- Matching-statistics length for the suffix of `P` starting at `i`:
the largest `k ≤ P.length - i` whose prefix of that suffix occurs in `T`.
(Max over occurring prefixes — monotonicity in `k` is a theorem, not a
definitional necessity; the spec is correct without it.) -/
def msLen (P T : Text) (i : Nat) : Nat :=
  ((List.range (P.length - i + 1)).filter
    (fun k => occurs ((P.drop i).take k) T)).foldl (fun acc k => max acc k) 0

/-- O_MS answer: pattern ↦ per-suffix-start list of (length, position). -/
abbrev MSAnswer := Text → List (Nat × Nat)

/-- The pair an MS oracle returns for suffix start `i` (default junk). -/
def msAnsAt (g : MSAnswer) (P : Text) (i : Nat) : Nat × Nat :=
  (g P)[i]?.getD (0, 0)

/-- O_MS correctness (Step-1 sketch, mirroring `lcpOf` maximality): for
every pattern and every suffix start, the answer pairs the true MS length
with a genuine 1-based position covering the matched prefix.  Definition
only — no proof claimed here. -/
def CorrectMS (g : MSAnswer) (T : Text) : Prop :=
  ∀ P : Text, (g P).length = P.length ∧
    ∀ i, i < P.length →
      (msAnsAt g P i).1 = msLen P T i ∧
        1 ≤ (msAnsAt g P i).2 ∧ (msAnsAt g P i).2 ≤ T.length ∧
          coversAt ((P.drop i).take (msAnsAt g P i).1) (msAnsAt g P i).2 T = true

/-- M0 bit-space index: an arbitrary bit string. -/
def Index := List Bool

/-- Index size in bits. -/
def space (D : Index) : Nat := D.length

/-- M0 decode: a fixed decoder realizes `f` when it decodes `D` to `f`. -/
def Realizes (dec : Index → Answer) (D : Index) (f : Answer) : Prop := dec D = f

/-! ## Step 2: the S1 bridge -/

/-- The requirement words: `w ++ [c]` for every requirement `(w, c)`. -/
def reqWords (T : Text) : List (List Nat) :=
  (requirements T).map (fun p => p.1 ++ [p.2])

/-- The positions an oracle emits on the requirement words, in requirement
order.  Duplicates allowed: `chi_le_of_suffixient_mem` tolerates them. -/
def emitted (f : Answer) (T : Text) : List Nat :=
  (reqWords T).flatMap (fun w => match f w with | some x => [x] | none => [])

theorem mem_reqWords {w : List Nat} {T : Text} :
    w ∈ reqWords T ↔ ∃ p, p ∈ requirements T ∧ p.1 ++ [p.2] = w := by
  rw [reqWords, List.mem_map]

theorem mem_emitted {f : Answer} {T : Text} {x : Nat} :
    x ∈ emitted f T ↔ ∃ w, w ∈ reqWords T ∧ f w = some x := by
  constructor
  · intro hx
    rw [emitted, List.mem_flatMap] at hx
    obtain ⟨w, hw, hxw⟩ := hx
    cases hfw : f w with
    | none => rw [hfw] at hxw; simp at hxw
    | some y =>
      rw [hfw] at hxw
      simp only [List.mem_singleton] at hxw
      exact ⟨w, hw, by rw [hfw, hxw]⟩
  · rintro ⟨w, hw, hfw⟩
    rw [emitted, List.mem_flatMap]
    refine ⟨w, hw, ?_⟩
    rw [hfw]; simp

/-- What `coversAt` alone guarantees for a nonempty word: `1 ≤ x`.  The
upper bound `x ≤ T.length` does NOT follow from `coversAt` (a position
past the end sees the full prefix), which is exactly why
`CorrectLocateOne` carries its range constraint explicitly. -/
theorem le_of_coversAt {wc : List Nat} {x : Nat} {T : Text}
    (hne : wc ≠ []) (hc : coversAt wc x T = true) : 1 ≤ x := by
  obtain ⟨h1, _⟩ := (coversAt_eq_true wc x T).mp hc
  rw [pref_length] at h1
  have hlen : 1 ≤ wc.length := by
    cases wc with
    | nil => exact absurd rfl hne
    | cons a t => simp
  omega

/-- **S1 bridge, part 1** — any correct locate-one oracle emits a
suffixient position set on the requirement words alone. -/
theorem emitted_suffixient (f : Answer) (T : Text) (h : CorrectLocateOne f T) :
    suffixient (emitted f T) T = true := by
  refine suffixient_of_witnesses (emitted f T) T ?_
  intro p hp
  obtain ⟨_, _, hext⟩ := (mem_requirements p.1 p.2 T).mp hp
  have hocc : occurs (p.1 ++ [p.2]) T = true := (mem_rightExts p.1 p.2 T).mp hext
  obtain ⟨x, hx, _, _, hc⟩ := h (p.1 ++ [p.2]) hocc
  refine ⟨x, ?_, hc⟩
  exact (mem_emitted).mpr ⟨p.1 ++ [p.2],
    (mem_reqWords).mpr ⟨p, hp, rfl⟩, hx⟩

/-- **S1 bridge, part 2** — any correct locate-one oracle emits at least
`chi T` positions on the requirement words alone. -/
theorem chi_le_of_oracle (f : Answer) (T : Text) (h : CorrectLocateOne f T) :
    chi T ≤ (emitted f T).length := by
  refine chi_le_of_suffixient_mem T (emitted f T) ?_ (emitted_suffixient f T h)
  intro x hx
  obtain ⟨w, hw, hfx⟩ := (mem_emitted).mp hx
  obtain ⟨p, hp, rfl⟩ := (mem_reqWords).mp hw
  obtain ⟨_, _, hext⟩ := (mem_requirements p.1 p.2 T).mp hp
  have hocc : occurs (p.1 ++ [p.2]) T = true := (mem_rightExts p.1 p.2 T).mp hext
  obtain ⟨y, hy, h1, h2, _⟩ := h (p.1 ++ [p.2]) hocc
  have hxy : x = y := by
    rw [hfx] at hy
    exact Option.some.inj hy
  subst hxy
  exact mem_positionsT h1 h2

/-! ## Step 3: `family_counting` (the pigeonhole core) -/

/-- Generalized counting lemma (the repo's `nodup_length_le_of_subset` is
`List Nat`-specific; identical proof, arbitrary carrier). -/
theorem nodup_length_le_of_subset' {α : Type} {l m : List α} (hnd : l.Nodup)
    (hsub : ∀ x ∈ l, x ∈ m) : l.length ≤ m.length := by
  induction l generalizing m with
  | nil => simp
  | cons a t ih =>
    rw [List.nodup_cons] at hnd
    obtain ⟨hnotin, hndt⟩ := hnd
    have ham : a ∈ m := hsub a (List.mem_cons_self)
    obtain ⟨s, t', rfl⟩ := List.mem_iff_append.mp ham
    have hsub' : ∀ x ∈ t, x ∈ s ++ t' := by
      intro x hx
      have hxam : x ∈ s ++ a :: t' := hsub x (List.mem_cons_of_mem a hx)
      rw [List.mem_append] at hxam
      rcases hxam with h | h
      · exact List.mem_append_left _ h
      · rw [List.mem_cons] at h
        rcases h with h | h
        · subst h; exact absurd hx hnotin
        · exact List.mem_append_right _ h
    have hthis : t.length ≤ s.length + t'.length := by
      have := ih hndt hsub'
      simpa [List.length_append] using this
    simp only [List.length_cons, List.length_append]
    omega

/-- `Nodup` is preserved by mapping an injective-on-`l` function. -/
theorem nodup_map_of_injOn {α β : Type} (f : α → β) : ∀ (l : List α), l.Nodup →
    (∀ a ∈ l, ∀ b ∈ l, f a = f b → a = b) → (l.map f).Nodup := by
  intro l
  induction l with
  | nil => intro _ _; simp
  | cons a t ih =>
    intro hnd hinj
    rw [List.nodup_cons] at hnd
    obtain ⟨hnotin, hndt⟩ := hnd
    rw [List.map_cons, List.nodup_cons]
    constructor
    · intro hy
      obtain ⟨b, hb, hbeq⟩ := List.mem_map.mp hy
      have hab : a = b := hinj a (List.mem_cons_self) b (List.mem_cons_of_mem a hb)
          hbeq.symm
      subst hab
      exact hnotin hb
    · exact ih hndt (fun a' ha' b hb hab =>
        hinj a' (List.mem_cons_of_mem a ha') b (List.mem_cons_of_mem a hb) hab)

/-- Finite choice over a list of texts, fully constructively (decidable
equality): from a per-member existential one gets a global assignment. -/
theorem finite_choice_index {P : Text → Index → Prop} : ∀ (F : List Text),
    (∀ T ∈ F, ∃ D : Index, P T D) → ∃ g : Text → Index, ∀ T ∈ F, P T (g T) := by
  intro F
  induction F with
  | nil => intro _; exact ⟨fun _ => [], by simp⟩
  | cons T rest ih =>
    intro hex
    obtain ⟨D, hD⟩ := hex T (List.mem_cons_self)
    obtain ⟨g', hg'⟩ := ih (fun T' hT' => hex T' (List.mem_cons_of_mem T hT'))
    refine ⟨fun T' => if T' = T then D else g' T', ?_⟩
    intro T' hT'
    by_cases hEq : T' = T
    · show P T' (if T' = T then D else g' T')
      rw [if_pos hEq]
      rw [← hEq] at hD
      exact hD
    · show P T' (if T' = T then D else g' T')
      rw [if_neg hEq]
      rcases List.mem_cons.mp hT' with h | h
      · exact absurd h hEq
      · exact hg' T' h

/-- All Bool lists of an exact length (by head-cons, so membership
inducts smoothly on the list's own structure). -/
def boolListsExact : Nat → List (List Bool)
  | 0 => [[]]
  | k + 1 =>
    let r := boolListsExact k
    r.map (fun l => false :: l) ++ r.map (fun l => true :: l)

/-- Every Bool list of length at most `s`: the M0 indexes of `space ≤ s`. -/
def allShort : Nat → List (List Bool)
  | 0 => [[]]
  | s + 1 => boolListsExact (s + 1) ++ allShort s

theorem length_boolListsExact (k : Nat) : (boolListsExact k).length = 2 ^ k := by
  induction k with
  | zero => simp [boolListsExact]
  | succ n ih =>
    rw [boolListsExact, List.length_append, List.length_map, List.length_map, ih]
    rw [Nat.pow_succ]
    omega

theorem mem_boolListsExact : ∀ (D : List Bool), D ∈ boolListsExact D.length
  | [] => by simp [boolListsExact]
  | b :: t => by
    have ih := mem_boolListsExact t
    have hl : (b :: t).length = t.length + 1 := by simp
    rw [hl]
    unfold boolListsExact
    cases b with
    | false => exact List.mem_append_left _ ((List.mem_map).mpr ⟨t, ih, rfl⟩)
    | true => exact List.mem_append_right _ ((List.mem_map).mpr ⟨t, ih, rfl⟩)

theorem length_allShort (s : Nat) : (allShort s).length = 2 ^ (s + 1) - 1 := by
  induction s with
  | zero => simp [allShort, Nat.pow_one]
  | succ n ih =>
    rw [allShort, List.length_append, length_boolListsExact, ih]
    have h2 : 2 ^ (n + 1 + 1) = 2 * 2 ^ (n + 1) := by rw [Nat.pow_succ]; omega
    rw [h2]
    omega

theorem mem_allShort : ∀ (D : List Bool) (s : Nat), D.length ≤ s → D ∈ allShort s
  | D, 0, hs => by
    have hnil : D = [] := by
      cases D with
      | nil => rfl
      | cons b bs => simp at hs
    subst hnil; simp [allShort]
  | D, s + 1, hs => by
    rcases Nat.lt_or_ge D.length (s + 1) with h | h
    · have him := mem_allShort D s (by omega)
      exact List.mem_append_right _ him
    · have hlen : D.length = s + 1 := by omega
      rw [allShort]
      refine List.mem_append_left _ ?_
      rw [← hlen]
      exact mem_boolListsExact D

/-- **Pigeonhole core (Step 3)** — if every member of a family of distinct
texts has a bit-index of `space ≤ s` (under one fixed decoder) realizing a
correct locate-one oracle, and no single answer function is correct for
two distinct members, then the family has at most `2^(s+1) - 1` members.

This is the shape of the eventual χ-floor proof: Attack 1 supplies a
family of size `(L/2 + 1)^k` with `χ = Θ(k)` and `n = k(L+1)`, and this
lemma turns it into `s ≥ Ω(k log(n/k)) = Ω(χ log(n/χ))` bits. -/
theorem family_counting (dec : Index → Answer) (F : List Text) (s : Nat)
    (hnd : F.Nodup)
    (hex : ∀ T ∈ F, ∃ D : Index, space D ≤ s ∧
      ∃ f, Realizes dec D f ∧ CorrectLocateOne f T)
    (hincompat : ∀ T ∈ F, ∀ T' ∈ F, T ≠ T' →
        ∀ f, CorrectLocateOne f T → ¬ CorrectLocateOne f T') :
    F.length ≤ 2 ^ (s + 1) - 1 := by
  have hex2 : ∀ T ∈ F, ∃ D : Index, space D ≤ s ∧ CorrectLocateOne (dec D) T := by
    intro T hT
    obtain ⟨D, hsD, f, hr, hc⟩ := hex T hT
    have hr' : dec D = f := hr
    exact ⟨D, hsD, by rw [hr']; exact hc⟩
  obtain ⟨g, hg⟩ := finite_choice_index F hex2
  have hinj : ∀ a ∈ F, ∀ b ∈ F, g a = g b → a = b := by
    intro a ha b hb hab
    by_cases hEq : a = b
    · exact hEq
    · obtain ⟨_, hca⟩ := hg a ha
      obtain ⟨_, hcb⟩ := hg b hb
      exact absurd (hincompat a ha b hb hEq (dec (g a)) hca
        (by rw [hab]; exact hcb)) (fun h => h)
  have hmapnd : (F.map g).Nodup := nodup_map_of_injOn g F hnd hinj
  have hsub : ∀ D ∈ F.map g, D ∈ allShort s := by
    intro D hD
    rw [List.mem_map] at hD
    obtain ⟨T, hT, hTD⟩ := hD
    rw [← hTD]
    obtain ⟨hsD, _⟩ := hg T hT
    exact mem_allShort _ s hsD
  have hle := nodup_length_le_of_subset' hmapnd hsub
  calc F.length = (F.map g).length := (List.length_map g).symm
    _ ≤ (allShort s).length := hle
    _ = 2 ^ (s + 1) - 1 := length_allShort s

/-- The plan's exact `2^s` shape: `space D + 1 ≤ s` bounds the family by
`2^s`. -/
theorem family_counting_lt (dec : Index → Answer) (F : List Text) (s : Nat)
    (hnd : F.Nodup)
    (hex : ∀ T ∈ F, ∃ D : Index, space D + 1 ≤ s ∧
      ∃ f, Realizes dec D f ∧ CorrectLocateOne f T)
    (hincompat : ∀ T ∈ F, ∀ T' ∈ F, T ≠ T' →
        ∀ f, CorrectLocateOne f T → ¬ CorrectLocateOne f T') :
    F.length ≤ 2 ^ s := by
  rcases Nat.eq_zero_or_pos s with h0 | hpos
  · subst h0
    cases F with
    | nil => simp
    | cons T rest =>
      obtain ⟨D, hs, _, _, _⟩ := hex T (List.mem_cons_self)
      exact absurd hs (by omega)
  · have hmain := family_counting dec F (s - 1) hnd ?_ hincompat
    · have hsnz : s - 1 + 1 = s := by omega
      rw [hsnz] at hmain
      have h2s : 1 ≤ 2 ^ s := Nat.one_le_two_pow
      omega
    · intro T hT
      obtain ⟨D, hs, f, hr, hc⟩ := hex T hT
      exact ⟨D, by omega, f, hr, hc⟩

/-! ## Step 4 machinery: executable oracle pieces for the battery -/

/-- The positions a correct oracle MAY answer for `w`: genuine 1-based
positions (the `range (n+1)` filter bounds `x ≤ T.length`) whose prefix
ends with `w`. -/
def validCovers (w : List Nat) (T : Text) : List Nat :=
  ((List.range (T.length + 1)).filter (fun x => 1 ≤ x ∧ coversAt w x T))

theorem mem_validCovers {w : List Nat} {T : Text} {x : Nat} :
    x ∈ validCovers w T ↔ (1 ≤ x ∧ x ≤ T.length ∧ coversAt w x T = true) := by
  constructor
  · intro h
    rw [validCovers, List.mem_filter] at h
    obtain ⟨h1, h2⟩ := h
    rw [List.mem_range] at h1
    rw [decide_eq_true_eq] at h2
    exact ⟨h2.1, by omega, h2.2⟩
  · rintro ⟨h1, h2, hc⟩
    rw [validCovers, List.mem_filter]
    refine ⟨by rw [List.mem_range]; omega, ?_⟩
    rw [decide_eq_true_eq]
    exact ⟨h1, hc⟩

theorem head?_mem {α : Type} {l : List α} {a : α} (h : l.head? = some a) :
    a ∈ l := by
  cases l with
  | nil => simp at h
  | cons b bs =>
    simp only [List.head?] at h
    rw [Option.some.injEq] at h
    subst h
    exact List.mem_cons_self

theorem head?_ne_none {α : Type} {l : List α} (h : ∃ a, a ∈ l) :
    l.head? ≠ none := by
  obtain ⟨a, ha⟩ := h
  cases l with
  | nil => exact absurd ha (by simp)
  | cons b bs => simp

theorem occurs_mem_validCovers {w : List Nat} {T : Text}
    (hw : occurs w T = true) (hne : T ≠ []) :
    ∃ x, x ∈ validCovers w T := by
  cases w with
  | nil =>
    have h1 : 1 ≤ T.length := by
      cases T with
      | nil => exact absurd rfl hne
      | cons b bs => simp
    refine ⟨1, (mem_validCovers).mpr ⟨by omega, h1, ?_⟩⟩
    exact (coversAt_iff_suffix [] 1 T).mpr ⟨pref T 1, by simp⟩
  | cons a t =>
    obtain ⟨x, hx, hc⟩ := exists_covers_of_occurs (a :: t) T hw (by simp)
    have hxp : 1 ≤ x ∧ x ≤ T.length := by
      rw [positionsT, List.mem_map] at hx
      obtain ⟨y, hy, hyx⟩ := hx
      rw [List.mem_range] at hy
      omega
    exact ⟨x, (mem_validCovers).mpr ⟨hxp.1, hxp.2, hc⟩⟩

/-- The canonical first-cover oracle: answer each occurring word with
the smallest valid position covering it. -/
def fFirst (T : Text) : Answer := fun w => (validCovers w T).head?

/-- `fFirst` is a correct locate-one oracle for every nonempty text —
the battery's non-vacuity witness for `CorrectLocateOne`. -/
theorem fFirst_correct (T : Text) (hne : T ≠ []) : CorrectLocateOne (fFirst T) T := by
  intro w hw
  have hne' := occurs_mem_validCovers hw hne
  cases h : (validCovers w T).head? with
  | none => exact absurd h (head?_ne_none hne')
  | some y =>
    have hy : y ∈ validCovers w T := head?_mem h
    obtain ⟨h1, h2, hc⟩ := (mem_validCovers).mp hy
    exact ⟨y, h, h1, h2, hc⟩

/-- A unique valid cover forces the answer of any correct oracle
(the uniqueness-to-incompatibility bridge for Attack 1). -/
theorem forced_unique {f : Answer} {T : Text} {w : List Nat} {m : Nat}
    (hocc : occurs w T = true)
    (huniq : ∀ x, 1 ≤ x → x ≤ T.length → coversAt w x T = true → x = m)
    (h : CorrectLocateOne f T) : f w = some m := by
  obtain ⟨x, hx, h1, h2, hc⟩ := h w hocc
  rw [hx]
  congr 1
  exact huniq x h1 h2 hc

/-- Distinct forced answers on a shared query make two texts admit no
common correct oracle — the pairwise-incompatibility engine. -/
theorem incompat_of_forced (f : Answer) (T T' : Text) (w : List Nat) (m m' : Nat)
    (hoccT : occurs w T = true) (hoccT' : occurs w T' = true)
    (hT : ∀ x, 1 ≤ x → x ≤ T.length → coversAt w x T = true → x = m)
    (hT' : ∀ x, 1 ≤ x → x ≤ T'.length → coversAt w x T' = true → x = m')
    (hne : m ≠ m')
    (h : CorrectLocateOne f T) : ¬ CorrectLocateOne f T' := by
  intro h'
  have h1 := forced_unique hoccT hT h
  have h2 := forced_unique hoccT' hT' h'
  rw [h1] at h2
  exact hne (Option.some.inj h2)



/-! ## Attack 1: the witness-perturbation family — `chi_fam_bounds`

`LOWER_BOUND_PLAN` Part 4, Step 5 (the battery-pre-validated next target).
The family `famText k L ps`: `k` blocks of run length `L`, block `i` =
`x_i^{p_i} · b_i · x_i^{L-p_i}` with per-block-distinct letters
(`x_i = 2i+2`, `b_i = 2i+3`).  On the half grid (`2 p_i ≤ L`), with `L ≥ 1`
and `k ≥ 1` (every right run nonempty, every letter occurring):

    2k ≤ χ(famText k L ps) ≤ 3k − 1

Lower bound: the `2k` letter requirements `(ε, x_i)`, `(ε, b_i)` are pairwise
uncoverable (no position ends in two distinct letters), so every suffixient
candidate set carries one position per letter.  Upper bound: the explicit
`3k − 1` positions {M_i} ∪ {E_i} ∪ {P_i} (marker, block-end, block-boundary)
are suffixient, applied through `chi_le_of_suffixient` via the filter trick.
Both halves rest on the structural analysis: every requirement word is `ε`
or a pure run word `x_i^j` — every other occurring word fails right-maximality
by unique-window arguments (the battery's obstruction, made a theorem). -/

namespace Fam

/-! ### Generic helpers -/

theorem replicate_add' {α : Type} (m n : Nat) (a : α) :
    List.replicate (m + n) a = List.replicate m a ++ List.replicate n a := by
  induction m with
  | zero => rw [Nat.zero_add, List.replicate_zero, List.nil_append]
  | succ m ih =>
    rw [Nat.add_right_comm, List.replicate_succ, List.replicate_succ, List.cons_append, ← ih]

theorem nodup_filter' {α : Type} {p : α → Bool} : ∀ {l : List α}, l.Nodup → (l.filter p).Nodup := by
  intro l
  induction l with
  | nil => intro _; exact List.nodup_nil
  | cons a t ih =>
    intro h
    rw [List.nodup_cons] at h
    rw [List.filter_cons]
    by_cases hp : p a
    · rw [if_pos hp, List.nodup_cons]
      constructor
      · intro hmem
        rw [List.mem_filter] at hmem
        exact h.1 hmem.1
      · exact ih h.2
    · rw [if_neg hp]
      exact ih h.2

theorem getElem_append_mid {α : Type} {l₁ l₂ l₃ : List α} {i : Nat}
    (h1 : l₁.length ≤ i) (h2 : i - l₁.length < l₂.length)
    (h0 : i < (l₁ ++ (l₂ ++ l₃)).length) :
    (l₁ ++ (l₂ ++ l₃))[i]'h0 = l₂[i - l₁.length]'h2 := by
  have hlr : i - l₁.length < (l₂ ++ l₃).length := by
    have hl : (l₂ ++ l₃).length = l₂.length + l₃.length := by simp
    omega
  have g1 : (l₁ ++ (l₂ ++ l₃))[i]'h0 = (l₂ ++ l₃)[i - l₁.length]'hlr :=
    List.getElem_append_right h1
  rw [g1]
  exact List.getElem_append_left h2

/-- Uniqueness of division-with-remainder representations. -/
theorem div_mod_unique {q b i r : Nat} (hb : 0 < b) (hr : r < b)
    (h : i * b + r = q) : q / b = i ∧ q % b = r := by
  have hd : (q / b) * b + q % b = q := by
    have := Nat.div_add_mod q b
    rw [Nat.mul_comm] at this
    exact this
  have hmb : q % b < b := Nat.mod_lt q hb
  rcases Nat.lt_trichotomy i (q / b) with hlt | heq | hgt
  · have hle : (i + 1) * b ≤ (q / b) * b := Nat.mul_le_mul (by omega) (by omega)
    rw [Nat.succ_mul] at hle
    omega
  · subst heq; omega
  · have hle : (q / b + 1) * b ≤ i * b := Nat.mul_le_mul (by omega) (by omega)
    rw [Nat.succ_mul] at hle
    omega

/-- Pointwise access into an occurrence window. -/
theorem window_le {T w : Text} {s : Nat} (hw : w ≠ [])
    (h : (T.drop s).take w.length = w) : s + w.length ≤ T.length := by
  rcases Nat.le_total s T.length with hs | hs
  · have h0 : ((T.drop s).take w.length).length = w.length := by rw [h]
    rw [List.length_take] at h0
    have hd : (T.drop s).length = T.length - s := List.length_drop
    rcases Nat.le_total w.length (T.drop s).length with hle | hge
    · omega
    · rw [Nat.min_eq_right hge] at h0
      omega
  · exfalso; apply hw
    have hd0 : (T.drop s).length = 0 := by
      have := List.length_drop (l := T) (i := s)
      omega
    have hd1 : T.drop s = [] := List.eq_nil_of_length_eq_zero hd0
    rw [hd1, List.take_nil] at h
    exact h.symm

theorem window_get {T w : Text} {s : Nat} (h : (T.drop s).take w.length = w)
    {t : Nat} (ht : t < w.length) (hst : s + t < T.length) :
    T[s + t]'hst = w[t] := by
  have htw : t < w.length := ht
  have htk : t < ((T.drop s).take w.length).length := by rw [h]; exact htw
  have hdrop : t < (T.drop s).length := by
    have h0 : ((T.drop s).take w.length).length = w.length := by rw [h]
    rw [List.length_take] at h0
    have hd : (T.drop s).length = T.length - s := List.length_drop
    rcases Nat.le_total w.length (T.drop s).length with hle | hge
    · omega
    · rw [Nat.min_eq_right hge] at h0
      omega
  have h2 : ((T.drop s).take w.length)[t]'htk = (T.drop s)[t]'hdrop := List.getElem_take
  have h3 : (T.drop s)[t]'hdrop = T[s + t]'hst := List.getElem_drop
  have h1 : ((T.drop s).take w.length)[t]'htk = w[t] := List.getElem_of_eq h htk
  rw [← h1, h2, h3]

/-- Occurrences in split form: `w` sits in `T` at 0-based offset `s`. -/
theorem occurs_split (w T : Text) (hw : w ≠ []) (hocc : occurs w T = true) :
    ∃ s, s + w.length ≤ T.length ∧ T = T.take s ++ (w ++ T.drop (s + w.length)) := by
  obtain ⟨s, hs, hwin⟩ := (occurs_eq_true w T hw).mp hocc
  refine ⟨s, hs, ?_⟩
  have h1 : T.take (s + w.length) = T.take s ++ w := by
    rw [List.take_add, hwin]
  calc T = T.take (s + w.length) ++ T.drop (s + w.length) :=
        (List.take_append_drop (s + w.length) T).symm
    _ = (T.take s ++ w) ++ T.drop (s + w.length) := by rw [h1]
    _ = T.take s ++ (w ++ T.drop (s + w.length)) := List.append_assoc _ _ _

/-- A pointwise-constant list is a replicate. -/
theorem eq_replicate_of_const : ∀ (w : List Nat) (c : Nat),
    (∀ t, (ht : t < w.length) → w[t] = c) → w = List.replicate w.length c := by
  intro w
  induction w with
  | nil => intro c _; rfl
  | cons a t ih =>
    intro c hc
    have hc1 : a = c := hc 0 (by simp)
    have key : ∀ j, (hj : j < t.length) → t[j] = c := by
      intro j hj
      have hj' := hc (j + 1) (by simp; omega)
      rw [List.getElem_cons_succ] at hj'
      exact hj'
    rw [List.length_cons, hc1, List.replicate_succ]
    exact congr (rfl) (ih c key)

/-- Index congruence for `getElem` (the instance follows by proof irrelevance). -/
theorem getElem_idx_congr {α : Type} {l : List α} {i j : Nat} (h : i = j)
    (hi : i < l.length) : l[i]'hi = l[j]'(h ▸ hi) := by
  subst h
  rfl

/-! ### The dedup all-equal bound -/

theorem dedupAux_mem : ∀ (l seen : List Nat) (a : Nat),
    a ∈ Sxgc.dedup.dedupAux seen l → a ∈ l ∧ ¬seen.contains a := by
  intro l
  induction l with
  | nil =>
    intro seen a h
    rw [Sxgc.dedup.dedupAux] at h
    exact absurd h List.not_mem_nil
  | cons b t ih =>
    intro seen a h
    rw [Sxgc.dedup.dedupAux] at h
    by_cases hb : seen.contains b
    · rw [if_pos hb] at h
      obtain ⟨h1, h2⟩ := ih seen a h
      exact ⟨List.mem_cons_of_mem _ h1, h2⟩
    · rw [if_neg hb, List.mem_cons] at h
      rcases h with h | h
      · subst h
        refine ⟨List.mem_cons_self, hb⟩
      · obtain ⟨h1, h2⟩ := ih (b :: seen) a h
        refine ⟨List.mem_cons_of_mem _ h1, fun hc => h2 ?_⟩
        rw [List.contains_cons, Bool.or_eq_true]
        exact Or.inr hc

theorem dedup_all_eq_length_le (e : Nat) : ∀ (l : List Nat),
    (∀ a ∈ l, a = e) → (dedup l).length ≤ 1 := by
  intro l
  induction l with
  | nil =>
    intro _
    change (Sxgc.dedup.dedupAux [] []).length ≤ 1
    rw [Sxgc.dedup.dedupAux]
    simp
  | cons a t ih =>
    intro hall
    have hte : ∀ b ∈ t, b = e := fun b hb => hall b (List.mem_cons_of_mem _ hb)
    have hae : a = e := hall a (List.mem_cons_self)
    change (Sxgc.dedup.dedupAux [] (a :: t)).length ≤ 1
    rw [Sxgc.dedup.dedupAux]
    by_cases h0 : ([] : List Nat).contains a
    · simp at h0
    · rw [if_neg h0, List.length_cons]
      have hzero : (Sxgc.dedup.dedupAux [a] t).length ≤ 0 := by
        cases hlen : Sxgc.dedup.dedupAux [a] t with
        | nil => simp
        | cons b bs =>
          have hmem : b ∈ Sxgc.dedup.dedupAux [a] t := by
            rw [hlen]; exact List.mem_cons_self
          obtain ⟨hin, hnb⟩ := dedupAux_mem t [a] b hmem
          have hbe : b = e := hte b hin
          rw [hbe, hae] at hnb
          exact absurd (by simp) hnb
      omega

/-! ### The family: definitions and block/position structure -/

def runL (i : Nat) : Nat := 2 * i + 2
def markL (i : Nat) : Nat := 2 * i + 3

def block (i L p : Nat) : Text :=
  (List.replicate p (runL i)) ++ ([markL i] ++ (List.replicate (L - p) (runL i)))

def famText (k L : Nat) (ps : List Nat) : Text :=
  (((List.range k).map (fun i => block i L (ps.getD i 0))).flatten)

theorem runL_inj {i j : Nat} (h : runL i = runL j) : i = j := by
  unfold runL at h; omega
theorem markL_inj {i j : Nat} (h : markL i = markL j) : i = j := by
  unfold markL at h; omega
theorem runL_ne_markL {i j : Nat} (h : runL i = markL j) : False := by
  unfold runL markL at h; omega

theorem block_length {i L p : Nat} (hp : p ≤ L) : (block i L p).length = L + 1 := by
  simp only [block, List.length_append, List.length_replicate, List.length_singleton]
  omega

theorem range_split : ∀ (m i k : Nat), k = i + 1 + m →
    List.range k = (List.range i) ++ ([i] ++ ((List.range m).map (fun j => i + 1 + j))) := by
  intro m
  induction m with
  | zero =>
    intro i k hk
    subst hk
    simp [List.range_succ]
  | succ m ih =>
    intro i k hk
    have hstep : List.range k = (List.range (i + 1 + m)) ++ [i + 1 + m] := by
      have hk' : (i + 1 + m) + 1 = k := by omega
      rw [← hk']
      exact List.range_succ
    rw [hstep, ih i (i + 1 + m) rfl, List.range_succ]
    rw [List.map_append, List.map_singleton]
    simp only [List.append_assoc]

theorem famText_split {k L : Nat} {ps : List Nat} {i : Nat} (hik : i < k) :
    famText k L ps = (famText i L ps) ++ ((block i L (ps.getD i 0)) ++
      (((List.range (k - i - 1)).map (fun j => block (i + 1 + j) L (ps.getD (i + 1 + j) 0))).flatten)) := by
  unfold famText
  rw [range_split (k - i - 1) i k (by omega)]
  simp only [List.map_append, List.map_singleton, List.flatten_append,
    List.flatten_cons, List.flatten_nil, List.append_nil, List.map_map]
  rfl

theorem famText_length {k L : Nat} {ps : List Nat} (hp : ∀ i, i < k → ps.getD i 0 ≤ L) :
    (famText k L ps).length = k * (L + 1) := by
  induction k with
  | zero => simp [famText]
  | succ k ih =>
    have hp' : ∀ i, i < k → ps.getD i 0 ≤ L := fun i hi => hp i (Nat.lt_succ_of_lt hi)
    rw [famText_split (Nat.lt_succ_self k)]
    have hpost : (((List.range (k + 1 - k - 1)).map (fun j =>
        block (k + 1 + j) L (ps.getD (k + 1 + j) 0))).flatten) = [] := by
      simp
    rw [hpost]
    simp only [List.length_append, List.length_nil, block_length (hp k (Nat.lt_succ_self k))]
    rw [ih hp', Nat.succ_mul]

/-- The letter of a block at 0-based offset `t ≤ L`: the marker exactly at `p`. -/
theorem getElem_singleton_idx {a idx : Nat} (h : idx < [a].length) :
    [a][idx]'h = a := by
  cases idx with
  | zero => rfl
  | succ n => exact absurd h (by simp)

theorem block_get {i L p t : Nat} (hp : p ≤ L) (ht : t ≤ L)
    (htb : t < (block i L p).length) :
    (block i L p)[t]'htb = if t = p then markL i else runL i := by
  have hrep : (List.replicate p (runL i)).length = p := List.length_replicate
  have hdef : block i L p =
      ((List.replicate p (runL i)) ++ ([markL i] ++ (List.replicate (L - p) (runL i)))) := rfl
  have hop : (block i L p)[t]'htb =
      ((List.replicate p (runL i)) ++ ([markL i] ++ (List.replicate (L - p) (runL i))))[t]'(by
        rw [← hdef]; exact htb) := List.getElem_of_eq hdef htb
  rw [hop]
  by_cases htp : t < p
  · rw [if_neg (by omega)]
    rw [List.getElem_append_left]
    · rw [List.getElem_replicate]
    · omega
  · by_cases hte : t = p
    · subst hte
      rw [if_pos rfl]
      rw [List.getElem_append_right]
      · rw [List.getElem_append_left]
        · exact getElem_singleton_idx _
        · rw [hrep]; simp
      · rw [hrep]; omega
    · rw [if_neg (by omega)]
      rw [List.getElem_append_right]
      · rw [List.getElem_append_right]
        · rw [List.getElem_replicate]
        · rw [List.length_singleton]
          omega
      · rw [hrep]; omega

/-- Position access: the letter at 0-based `q` is block `q/(L+1)`'s letter at
offset `q%(L+1)`. -/
theorem fam_get {k L : Nat} {ps : List Nat} (hp : ∀ i, i < k → ps.getD i 0 ≤ L)
    {q : Nat} (hq : q < (famText k L ps).length) :
    (famText k L ps)[q]'hq =
      (block (q / (L + 1)) L (ps.getD (q / (L + 1)) 0))[q % (L + 1)]'(by
        have hlen : (famText k L ps).length = k * (L + 1) := famText_length hp
        rw [hlen] at hq
        have hi : q / (L + 1) < k := by
          rw [Nat.div_lt_iff_lt_mul (by omega : 0 < L + 1)]
          omega
        rw [block_length (hp _ hi)]
        exact Nat.mod_lt q (by omega)) := by
  have hlen : (famText k L ps).length = k * (L + 1) := famText_length hp
  have hL1 : 0 < L + 1 := by omega
  have hi : q / (L + 1) < k := by
    rw [hlen] at hq
    rw [Nat.div_lt_iff_lt_mul hL1]
    omega
  have ho : q % (L + 1) ≤ L := Nat.lt_succ_iff.mp (Nat.mod_lt q hL1)
  have hdm : (L + 1) * (q / (L + 1)) + q % (L + 1) = q := Nat.div_add_mod q (L + 1)
  have hpre : (famText (q / (L + 1)) L ps).length = (q / (L + 1)) * (L + 1) :=
    famText_length (fun j hj => hp j (Nat.lt_trans hj hi))
  have hblk : (block (q / (L + 1)) L (ps.getD (q / (L + 1)) 0)).length = L + 1 :=
    block_length (hp _ hi)
  have hhsplit := famText_split (k := k) (L := L) (ps := ps) (i := q / (L + 1)) hi
  have h0' : q < ((famText (q / (L + 1)) L ps) ++ ((block (q / (L + 1)) L
      (ps.getD (q / (L + 1)) 0)) ++ (((List.range (k - q / (L + 1) - 1)).map (fun j =>
      block (q / (L + 1) + 1 + j) L (ps.getD (q / (L + 1) + 1 + j) 0))).flatten))).length := by
    rw [← hhsplit]
    exact hq
  have h1' : (famText (q / (L + 1)) L ps).length ≤ q := by
    rw [hpre]
    have hmc : (q / (L + 1)) * (L + 1) = (L + 1) * (q / (L + 1)) := Nat.mul_comm _ _
    omega
  have h2' : q - (famText (q / (L + 1)) L ps).length <
      (block (q / (L + 1)) L (ps.getD (q / (L + 1)) 0)).length := by
    rw [hpre, hblk]
    have hmc : (q / (L + 1)) * (L + 1) = (L + 1) * (q / (L + 1)) := Nat.mul_comm _ _
    omega
  have hcong : (famText k L ps)[q]'hq = ((famText (q / (L + 1)) L ps) ++ ((block (q / (L + 1)) L
      (ps.getD (q / (L + 1)) 0)) ++ (((List.range (k - q / (L + 1) - 1)).map (fun j =>
      block (q / (L + 1) + 1 + j) L (ps.getD (q / (L + 1) + 1 + j) 0))).flatten)))[q]'h0' :=
    List.getElem_of_eq hhsplit hq
  rw [hcong, getElem_append_mid h1' h2' h0']
  congr 2
  have hmc : (q / (L + 1)) * (L + 1) = (L + 1) * (q / (L + 1)) := Nat.mul_comm _ _
  omega

/-- Full position/letter classification. -/
theorem fam_letter {k L : Nat} {ps : List Nat} (hp : ∀ i, i < k → ps.getD i 0 ≤ L)
    {q : Nat} (hq : q < (famText k L ps).length) :
    (q / (L + 1) < k ∧ q % (L + 1) ≤ L) ∧
    ((famText k L ps)[q]'hq = markL (q / (L + 1)) ∧ q % (L + 1) = ps.getD (q / (L + 1)) 0 ∨
     (famText k L ps)[q]'hq = runL (q / (L + 1)) ∧ q % (L + 1) ≠ ps.getD (q / (L + 1)) 0) := by
  have hlen : (famText k L ps).length = k * (L + 1) := famText_length hp
  have hL1 : 0 < L + 1 := by omega
  have hi : q / (L + 1) < k := by
    rw [hlen] at hq
    rw [Nat.div_lt_iff_lt_mul hL1]
    omega
  have ho : q % (L + 1) ≤ L := Nat.lt_succ_iff.mp (Nat.mod_lt q hL1)
  refine ⟨⟨hi, ho⟩, ?_⟩
  rw [fam_get hp hq, block_get (hp _ hi) ho (by
    rw [block_length (hp _ hi)]
    omega)]
  by_cases hop : q % (L + 1) = ps.getD (q / (L + 1)) 0
  · left
    rw [if_pos hop]
    exact ⟨rfl, hop⟩
  · right
    rw [if_neg hop]
    exact ⟨rfl, hop⟩

/-- Run letters sit exactly in their own block, off the marker. -/
theorem fam_run_iff {k L : Nat} {ps : List Nat} (hp : ∀ i, i < k → ps.getD i 0 ≤ L)
    {q i : Nat} (hq : q < (famText k L ps).length) :
    (famText k L ps)[q]'hq = runL i ↔
      (i < k ∧ i = q / (L + 1) ∧ q % (L + 1) ≠ ps.getD i 0) := by
  obtain ⟨⟨hi, ho⟩, hcls⟩ := fam_letter hp hq
  constructor
  · intro h
    rcases hcls with ⟨hm, _⟩ | ⟨hr, hop⟩
    · exact absurd (runL_ne_markL (h.symm.trans hm)) (by simp)
    · have hie : i = q / (L + 1) := runL_inj (h.symm.trans hr)
      rw [← hie] at hop
      exact ⟨by rw [hie]; exact hi, hie, hop⟩
  · rintro ⟨hi', hie, hop⟩
    rcases hcls with ⟨hm, hopm⟩ | ⟨hr, hop'⟩
    · rw [← hie] at hopm
      exact absurd hopm hop
    · rw [hr, hie]

/-- The marker letter occurs exactly at its block's marker offset. -/
theorem fam_mark_iff {k L : Nat} {ps : List Nat} (hp : ∀ i, i < k → ps.getD i 0 ≤ L)
    {q i : Nat} (hq : q < (famText k L ps).length) :
    (famText k L ps)[q]'hq = markL i ↔
      (i < k ∧ q = i * (L + 1) + ps.getD i 0) := by
  obtain ⟨⟨hi, ho⟩, hcls⟩ := fam_letter hp hq
  constructor
  · intro h
    rcases hcls with ⟨hm, hop⟩ | ⟨hr, _⟩
    · have hie : i = q / (L + 1) := markL_inj (h.symm.trans hm)
      refine ⟨by rw [hie]; exact hi, ?_⟩
      rw [← hie] at hop
      have hdm : (L + 1) * (q / (L + 1)) + q % (L + 1) = q := Nat.div_add_mod q (L + 1)
      have hmc : i * (L + 1) = (L + 1) * (q / (L + 1)) := by rw [hie, Nat.mul_comm]
      omega
    · exact absurd (runL_ne_markL (hr.symm.trans h)) (by simp)
  · rintro ⟨hi', hqeq⟩
    have hpi : ps.getD i 0 ≤ L := hp i hi'
    obtain ⟨hdiv, hmod⟩ := div_mod_unique (by omega : 0 < L + 1)
      (by omega : ps.getD i 0 < L + 1) hqeq.symm
    rcases hcls with ⟨hm, _⟩ | ⟨hr, hop⟩
    · rw [hm, ← hdiv]
    · rw [← hdiv] at hmod
      exact absurd hmod hop

/-- Same letter at two positions forces the same block. -/
theorem fam_letter_block {k L : Nat} {ps : List Nat} (hp : ∀ i, i < k → ps.getD i 0 ≤ L)
    {q q' : Nat} (hq : q < (famText k L ps).length) (hq' : q' < (famText k L ps).length)
    (h : (famText k L ps)[q]'hq = (famText k L ps)[q']'hq') :
    q / (L + 1) = q' / (L + 1) := by
  obtain ⟨_, hcls⟩ := fam_letter hp hq
  obtain ⟨_, hcls'⟩ := fam_letter hp hq'
  rcases hcls with ⟨hm, _⟩ | ⟨hr, _⟩ <;> rcases hcls' with ⟨hm', _⟩ | ⟨hr', _⟩
  · exact markL_inj (hm.symm.trans (h.trans hm'))
  · exact absurd (runL_ne_markL ((hm.symm.trans (h.trans hr')).symm)) (by simp)
  · exact absurd (runL_ne_markL (hr.symm.trans (h.trans hm'))) (by simp)
  · exact runL_inj (hr.symm.trans (h.trans hr'))

/-! ### The lower bound: one position per distinct letter -/

theorem mem_letter_occurs {T : Text} {c : Nat} (h : c ∈ T) : occurs [c] T = true := by
  obtain ⟨u, v, hv⟩ := List.mem_iff_append.mp h
  refine (occurs_eq_true [c] T (by simp)).mpr ⟨u.length, by rw [hv]; simp, ?_⟩
  rw [hv]
  simp [List.drop]

theorem nodup_append' : ∀ {l1 l2 : List Nat}, l1.Nodup → l2.Nodup →
    (∀ a ∈ l1, a ∉ l2) → (l1 ++ l2).Nodup := by
  intro l1
  induction l1 with
  | nil => intro l2 _ h2 _; exact h2
  | cons a t ih =>
    intro l2 h1 h2 hd
    rw [List.nodup_cons] at h1
    refine List.nodup_cons.mpr ⟨?_, ih h1.2 h2 (fun b hb => hd b (List.mem_cons_of_mem _ hb))⟩
    intro hmem
    rcases List.mem_append.mp hmem with hmem | hmem
    · exact h1.1 hmem
    · exact hd a (List.mem_cons_self) hmem

/-- Finite choice over a list of letters into positions. -/
theorem finite_choice_pos {P : Nat → Nat → Prop} : ∀ (l : List Nat),
    (∀ c ∈ l, ∃ x, P c x) → ∃ g : Nat → Nat, ∀ c ∈ l, P c (g c) := by
  intro l
  induction l with
  | nil => intro _; exact ⟨fun _ => 0, by simp⟩
  | cons c t ih =>
    intro hex
    obtain ⟨x, hx⟩ := hex c (List.mem_cons_self)
    obtain ⟨g', hg'⟩ := ih (fun c' hC' => hex c' (List.mem_cons_of_mem c hC'))
    refine ⟨fun c' => if c' = c then x else g' c', ?_⟩
    intro c' hC'
    by_cases hEq : c' = c
    · simp only []
      rw [if_pos hEq]
      rw [← hEq] at hx
      exact hx
    · simp only []
      rw [if_neg hEq]
      rcases List.mem_cons.mp hC' with h | h
      · exact absurd h hEq
      · exact hg' c' h

/-- Two distinct letters cannot both be covered at the same position. -/
theorem coversAt_letter_inj {T : Text} {a b x : Nat}
    (ha : coversAt [a] x T = true) (hb : coversAt [b] x T = true) : a = b := by
  obtain ⟨u1, hu1⟩ := (coversAt_iff_suffix [a] x T).mp ha
  obtain ⟨u2, hu2⟩ := (coversAt_iff_suffix [b] x T).mp hb
  have heq : u1 ++ [a] = u2 ++ [b] := hu1.symm.trans hu2
  have hlen : u1.length = u2.length := by
    have h3 : (u1 ++ [a]).length = (u2 ++ [b]).length := by rw [heq]
    simp only [List.length_append, List.length_singleton] at h3
    omega
  have hA : u1.length < (u1 ++ [a]).length := by
    simp only [List.length_append, List.length_singleton]; omega
  have hB' : u2.length < (u2 ++ [b]).length := by
    simp only [List.length_append, List.length_singleton]; omega
  have hc := getElem_idx_congr (l := u2 ++ [b]) (i := u2.length) (j := u1.length) hlen.symm hB'
  have h1 : (u1 ++ [a])[u1.length]'hA = a := by
    rw [List.getElem_append_right (Nat.le_refl u1.length)]
    exact getElem_singleton_idx _
  have h2 : (u2 ++ [b])[u2.length]'hB' = b := by
    rw [List.getElem_append_right (Nat.le_refl u2.length)]
    exact getElem_singleton_idx _
  have hconv : (u1 ++ [a])[u1.length]'hA = (u2 ++ [b])[u1.length]'(by
      simp only [List.length_append, List.length_singleton, hlen]
      omega) := List.getElem_of_eq heq hA
  calc a = (u1 ++ [a])[u1.length]'hA := h1.symm
    _ = (u2 ++ [b])[u1.length]'(by
        simp only [List.length_append, List.length_singleton, hlen]
        omega) := hconv
    _ = b := (hc.symm.trans h2)

/-- **Letter lower bound**: χ is at least the number of distinct letters
(each letter's `(ε, c)` requirement is covered only at positions holding `c`). -/
theorem chi_ge_letters (T : Text) (cs : List Nat) (hcs : ∀ c ∈ cs, c ∈ T)
    (hnd : cs.Nodup) : cs.length ≤ chi T := by
  have hocc : ∀ c ∈ cs, occurs [c] T = true := fun c hc => mem_letter_occurs (hcs c hc)
  -- the (ε, c) requirements
  have hreq : ∀ c ∈ cs, ([], c) ∈ requirements T := by
    intro c hc
    refine (mem_requirements [] c T).mpr ⟨?_, by simp [rightMaximal], ?_⟩
    · clear hc
      induction T with
      | nil => simp [subStrings]
      | cons t ts ih =>
        rw [subStrings]
        exact List.mem_cons_self
    · exact (mem_rightExts [] c T).mpr (hocc c hc)
  unfold chi
  dsimp only
  split
  · rename_i d hd
    have hdmem : d ∈ List.flatMap (blk T) (List.range (T.length + 1)) := List.mem_of_mem_head? hd
    rw [List.mem_flatMap] at hdmem
    obtain ⟨k, _hk, hdk⟩ := hdmem
    obtain ⟨_hsub_seq, _hlen_d, hsuff_d⟩ := (mem_blk T k d).mp hdk
    -- finite choice of covering positions
    have hex : ∀ c ∈ cs, ∃ x, x ∈ d ∧ coversAt [c] x T = true := by
      intro c hc
      obtain ⟨x, hxd, hxc⟩ := (suffixient_iff_cover d T).mp hsuff_d ([], c) (hreq c hc)
      exact ⟨x, hxd, hxc⟩
    obtain ⟨g, hg⟩ := finite_choice_pos cs hex
    -- injectivity of c ↦ g c
    have hinj : ∀ a ∈ cs, ∀ b ∈ cs, g a = g b → a = b := by
      intro a ha b hb hab
      obtain ⟨_, hca⟩ := hg a ha
      obtain ⟨_, hcb⟩ := hg b hb
      rw [← hab] at hcb
      exact coversAt_letter_inj hca hcb
    have hmapnd : (cs.map g).Nodup := nodup_map_of_injOn g cs hnd hinj
    have hsub : ∀ x ∈ cs.map g, x ∈ d := by
      intro x hx
      rw [List.mem_map] at hx
      obtain ⟨c, hc, hxc⟩ := hx
      rw [← hxc]
      exact (hg c hc).1
    have hle := nodup_length_le_of_subset' hmapnd hsub
    have : cs.length = (cs.map g).length := (List.length_map g).symm
    omega
  · rename_i _hd
    -- no candidate: chi = T.length; cs ⊆ letters of T
    have hsub : ∀ c ∈ cs, c ∈ T := hcs
    have hle : cs.length ≤ T.length :=
      nodup_length_le_of_subset' hnd (fun c hc => hsub c hc)
    omega

/-! ### The family letter set -/

/-- Membership of a block letter in the family text. -/
theorem fam_mem_of_block_mem {k L : Nat} {ps : List Nat} {i : Nat} (hik : i < k)
    (hp : ∀ j, j < k → ps.getD j 0 ≤ L) {c : Nat} (hcm : c ∈ block i L (ps.getD i 0)) :
    c ∈ famText k L ps := by
  rw [famText_split hik]
  rw [List.mem_append, List.mem_append]
  exact Or.inr (Or.inl hcm)

theorem runL_mem {k L : Nat} {ps : List Nat} {i : Nat} (hik : i < k)
    (hp : ∀ j, j < k → ps.getD j 0 ≤ L)
    (hlt : ps.getD i 0 < L) : runL i ∈ famText k L ps := by
  refine fam_mem_of_block_mem hik hp ?_
  rw [block, List.mem_append, List.mem_append]
  refine Or.inr (Or.inr ?_)
  rw [List.mem_replicate]
  refine ⟨by omega, rfl⟩

theorem markL_mem {k L : Nat} {ps : List Nat} {i : Nat} (hik : i < k)
    (hp : ∀ j, j < k → ps.getD j 0 ≤ L) : markL i ∈ famText k L ps := by
  refine fam_mem_of_block_mem hik hp ?_
  rw [block, List.mem_append, List.mem_append]
  exact Or.inr (Or.inl (List.mem_cons_self))

/-- The `2k` family letters, as a nodup list. -/
def famLetters (k : Nat) : List Nat :=
  ((List.range k).map runL) ++ ((List.range k).map markL)

theorem famLetters_nodup (k : Nat) : (famLetters k).Nodup := by
  have h1 : ((List.range k).map runL).Nodup := by
    apply nodup_map_of_injOn
    · exact List.nodup_range
    · intro a ha b hb hab
      exact runL_inj hab
  have h2 : ((List.range k).map markL).Nodup := by
    apply nodup_map_of_injOn
    · exact List.nodup_range
    · intro a ha b hb hab
      exact markL_inj hab
  refine nodup_append' h1 h2 ?_
  intro a ha
  rw [List.mem_map] at ha
  obtain ⟨i, _, rfl⟩ := ha
  intro hb
  rw [List.mem_map] at hb
  obtain ⟨j, _, hj⟩ := hb
  exact runL_ne_markL hj.symm

theorem famLetters_length (k : Nat) : (famLetters k).length = 2 * k := by
  unfold famLetters
  rw [List.length_append, List.length_map, List.length_map, List.length_range]
  omega

/-- **Family lower bound**: `2k ≤ χ` — the `2k` per-block-distinct letters are
pairwise uncoverable. -/
theorem chi_fam_lower {k L : Nat} {ps : List Nat} (hL : 1 ≤ L)
    (hhalf : ∀ j, j < k → 2 * ps.getD j 0 ≤ L)
    (hp : ∀ j, j < k → ps.getD j 0 ≤ L) :
    2 * k ≤ chi (famText k L ps) := by
  have hge := chi_ge_letters (famText k L ps) (famLetters k) (fun c hc => by
    rw [famLetters] at hc
    rw [List.mem_append] at hc
    rcases hc with hc | hc
    · rw [List.mem_map] at hc
      obtain ⟨i, hi, rfl⟩ := hc
      exact runL_mem (List.mem_range.mp hi) hp (by
        have := hhalf i (List.mem_range.mp hi)
        have := hL
        omega)
    · rw [List.mem_map] at hc
      obtain ⟨i, hi, rfl⟩ := hc
      exact markL_mem (List.mem_range.mp hi) hp) (famLetters_nodup k)
  rw [famLetters_length] at hge
  exact hge

/-! ### The unique-window engine -/

/-- Taking `|w|` letters of a `w ++ [c]`-window yields `w`. -/
theorem take_of_append_window {T : Text} {w : List Nat} {c : Nat} {s : Nat}
    (hW : ((T.drop s).take (w.length + 1)) = (w ++ [c])) :
    (T.drop s).take w.length = w := by
  have h1 : ((T.drop s).take (w.length + 1)).take w.length = w := by
    rw [hW, List.take_append]
    cases w with
    | nil => rfl
    | cons x t => simp
  rw [List.take_take, Nat.min_eq_left (Nat.le_succ w.length)] at h1
  exact h1

/-- If `w ++ [c]` has a window at `s`, then `c` is the letter at `s + |w|`. -/
theorem letter_of_append_window {T : Text} {w : List Nat} {c : Nat} {s : Nat}
    (hslen : s + w.length < T.length)
    (hW : ((T.drop s).take (w.length + 1)) = (w ++ [c])) :
    T[s + w.length]'hslen = c := by
  have hWlen : (T.drop s).take ((w ++ [c]).length) = (w ++ [c]) := by
    rw [List.length_append, List.length_singleton]; exact hW
  have hgt : T[s + w.length]'hslen = (w ++ [c])[w.length]'(by
      simp) := window_get hWlen (by simp) hslen
  rw [hgt]
  cases w with
  | nil => rfl
  | cons x t =>
    rw [List.getElem_append_right (Nat.le_refl (x :: t).length)]
    exact getElem_singleton_idx _

/-- **The unique-window engine**: a nonempty word occurring at a *unique* window
contributes no requirements — any right extension is forced to the single letter
after the window, so the word is either a suffix (no extensions) or has one. -/
theorem notreq_of_unique_window {T : Text} {w : List Nat} {s : Nat} (hw : w ≠ [])
    (hwin : (T.drop s).take w.length = w) (hsle : s + w.length ≤ T.length)
    (huniq : ∀ s' : Nat, s' + w.length ≤ T.length ∧
      (T.drop s').take w.length = w → s' = s) :
    ∀ c : Nat, (w, c) ∉ requirements T := by
  intro c hreq
  obtain ⟨_, hrm, hrx⟩ := (mem_requirements w c T).mp hreq
  simp only [rightMaximal, Bool.and_eq_true, Bool.or_eq_true] at hrm
  obtain ⟨_, hdisj⟩ := hrm
  have hoccW : occurs (w ++ [c]) T = true := (mem_rightExts w c T).mp hrx
  have hWne : w ++ [c] ≠ [] := by cases w with
    | nil => exact absurd rfl hw
    | cons x t => simp
  obtain ⟨s'', hs''le, hwin''⟩ := (occurs_eq_true _ T hWne).mp hoccW
  simp only [List.length_append, List.length_singleton] at hs''le hwin''
  have hwins : (T.drop s'').take w.length = w :=
    take_of_append_window hwin''
  have hs''eq : s'' = s := huniq s'' ⟨by omega, hwins⟩
  have hslen : s + w.length < T.length := by omega
  have hcW : T[s + w.length]'hslen = c := by
    have hcc := letter_of_append_window (w := w) (c := c) (s := s'') (by omega) hwin''
    calc T[s + w.length]'hslen = T[s + w.length]'(by omega) := rfl
      _ = T[s'' + w.length]'(by omega) := (getElem_idx_congr (l := T)
          (i := s'' + w.length) (j := s + w.length) (by rw [hs''eq]) (by omega)).symm
      _ = c := hcc
  rcases hdisj with hsuf | hge
  · exfalso
    have hweq : w = T.drop (T.length - w.length) := by
      simp only [rightMaximal.isSuffix, Bool.and_eq_true] at hsuf
      simpa using hsuf.2
    have hsuf' : (T.drop (T.length - w.length)).take w.length = w := by
      have hlen : (T.drop (T.length - w.length)).length = w.length := by
        have hd : (T.drop (T.length - w.length)).length
            = T.length - (T.length - w.length) := List.length_drop
        omega
      have h1 : (T.drop (T.length - w.length)).take w.length
          = (T.drop (T.length - w.length)).take (T.drop (T.length - w.length)).length := by
        rw [hlen]
      rw [h1, List.take_length]
      exact hweq.symm
    have hsu : T.length - w.length = s := huniq _ ⟨by omega, hsuf'⟩
    omega
  · exfalso
    have hge' : 2 ≤ (rightExts w T).length := by simpa using hge
    have hall : ∀ c' ∈ rightExts w T, c' = c := by
      intro c' hc'
      have hoccW' : occurs (w ++ [c']) T = true := (mem_rightExts w c' T).mp hc'
      have hWne' : w ++ [c'] ≠ [] := by cases w with
        | nil => exact absurd rfl hw
        | cons x t => simp
      obtain ⟨s3, hs3le, hwin3⟩ := (occurs_eq_true _ T hWne').mp hoccW'
      simp only [List.length_append, List.length_singleton] at hs3le hwin3
      have hwins3 : (T.drop s3).take w.length = w := take_of_append_window hwin3
      have hs3eq : s3 = s := huniq s3 ⟨by omega, hwins3⟩
      have hc3 : T[s3 + w.length]'(by omega) = c' :=
        letter_of_append_window (w := w) (c := c') (by omega) hwin3
      have hc3' := getElem_idx_congr (l := T) (i := s3 + w.length) (j := s + w.length)
        (by rw [hs3eq]) (by omega)
      calc c' = T[s3 + w.length]'(by omega) := hc3.symm
        _ = T[s + w.length]'(by omega) := hc3'
        _ = c := hcW
    have hlen : (rightExts w T).length ≤ 1 := by
      unfold rightExts
      refine dedup_all_eq_length_le c _ ?_
      intro a ha
      exact hall a ((mem_dedup _ _).mpr ha)
    omega

/-! ### The two obstruction cases -/

/-- A word containing a marker letter has a unique window:
the marker occurs at exactly one position. -/
theorem unique_window_of_mark {k L : Nat} {ps : List Nat}
    (hp : ∀ i, i < k → ps.getD i 0 ≤ L) {w : List Nat} {t i : Nat}
    (ht : t < w.length) (hmark : w[t]'ht = markL i)
    {s s' : Nat} (hwin : ((famText k L ps).drop s).take w.length = w)
    (hsle : s + w.length ≤ (famText k L ps).length)
    (hwin' : ((famText k L ps).drop s').take w.length = w)
    (hs'le : s' + w.length ≤ (famText k L ps).length) : s = s' := by
  have hq : s + t < (famText k L ps).length := by omega
  have hq' : s' + t < (famText k L ps).length := by omega
  have h1 : (famText k L ps)[s + t]'hq = w[t]'ht := window_get hwin ht hq
  have h2 : (famText k L ps)[s' + t]'hq' = w[t]'ht := window_get hwin' ht hq'
  rw [hmark] at h1 h2
  obtain ⟨hik, hpos⟩ := (fam_mark_iff hp hq).mp h1
  obtain ⟨_, hpos'⟩ := (fam_mark_iff hp hq').mp h2
  omega

/-- A word containing two distinct run letters has a unique window:
the two windows agree pointwise on blocks, and the block boundary
between the two letters pins the window start. -/
theorem unique_window_of_two_run_letters {k L : Nat} {ps : List Nat}
    (hp : ∀ i, i < k → ps.getD i 0 ≤ L) {w : List Nat} {t1 t2 i j : Nat}
    (ht1 : t1 < w.length) (ht2 : t2 < w.length) (hlt : t1 ≤ t2)
    (hr1 : w[t1]'ht1 = runL i) (hr2 : w[t2]'ht2 = runL j) (hij : i < j)
    {s s' : Nat} (hwin : ((famText k L ps).drop s).take w.length = w)
    (hsle : s + w.length ≤ (famText k L ps).length)
    (hwin' : ((famText k L ps).drop s').take w.length = w)
    (hs'le : s' + w.length ≤ (famText k L ps).length) : s = s' := by
  -- pointwise block agreement between the two windows
  have H : ∀ t, (ht : t < w.length) → (s + t) / (L + 1) = (s' + t) / (L + 1) := by
    intro t ht
    have hq : s + t < (famText k L ps).length := by omega
    have hq' : s' + t < (famText k L ps).length := by omega
    have h1 : (famText k L ps)[s + t]'hq = w[t]'ht := window_get hwin ht hq
    have h2 : (famText k L ps)[s' + t]'hq' = w[t]'ht := window_get hwin' ht hq'
    exact fam_letter_block hp hq hq' (h1.trans h2.symm)
  have hq1 : s + t1 < (famText k L ps).length := by omega
  have hq2 : s + t2 < (famText k L ps).length := by omega
  have hb1 : (s + t1) / (L + 1) = i := by
    have h1 : (famText k L ps)[s + t1]'hq1 = runL i := by
      rw [window_get hwin ht1 hq1]; exact hr1
    obtain ⟨_, hi, _⟩ := (fam_run_iff hp hq1).mp h1
    exact hi.symm
  have hb2 : (s + t2) / (L + 1) = j := by
    have h2 : (famText k L ps)[s + t2]'hq2 = runL j := by
      rw [window_get hwin ht2 hq2]; exact hr2
    obtain ⟨_, hj, _⟩ := (fam_run_iff hp hq2).mp h2
    exact hj.symm
  have hL1 : 0 < L + 1 := by omega
  -- the boundary B := (i+1)(L+1) lies inside the window: s + t1 < B ≤ s + t2
  have hlt1 : s + t1 < (i + 1) * (L + 1) := by
    refine (Nat.div_lt_iff_lt_mul hL1).mp ?_
    rw [hb1]
    omega
  have hge2 : (i + 1) * (L + 1) ≤ s + t2 := by
    rcases Nat.lt_or_ge (s + t2) ((i + 1) * (L + 1)) with hlt | hge
    · exfalso
      have hh : (s + t2) / (L + 1) < i + 1 :=
        Nat.div_lt_of_lt_mul (by rw [Nat.mul_comm]; exact hlt)
      rw [hb2] at hh
      omega
    · exact hge
  obtain ⟨σ, hσ⟩ : ∃ σ, (i + 1) * (L + 1) = s + σ := ⟨(i + 1) * (L + 1) - s, by omega⟩
  have hσ1 : σ - 1 < w.length := by omega
  have hσm : σ < w.length := by omega
  have hBm1 : ((i + 1) * (L + 1) - 1) / (L + 1) = i := by
    have hexp : (i + 1) * (L + 1) - 1 = i * (L + 1) + L := by
      have hA : (i + 1) * (L + 1) = i * (L + 1) + (L + 1) := by
        rw [Nat.add_mul, Nat.one_mul]
      omega
    rw [hexp, Nat.mul_comm i (L + 1), Nat.add_comm, Nat.add_mul_div_left _ _ hL1]
    simp
  have hB0 : ((i + 1) * (L + 1)) / (L + 1) = i + 1 := Nat.mul_div_cancel _ hL1
  have hsσ : (s + (σ - 1)) / (L + 1) = (s' + (σ - 1)) / (L + 1) := H _ hσ1
  have hsm : s + (σ - 1) = (i + 1) * (L + 1) - 1 := by omega
  rw [hsm, hBm1] at hsσ
  have hsσ' : (s + σ) / (L + 1) = (s' + σ) / (L + 1) := H _ hσm
  have hs0 : s + σ = (i + 1) * (L + 1) := by omega
  rw [hs0, hB0] at hsσ'
  have h1' : s' + (σ - 1) < (i + 1) * (L + 1) := by
    refine (Nat.div_lt_iff_lt_mul hL1).mp ?_
    rw [hsσ]
    omega
  have h2' : (i + 1) * (L + 1) ≤ s' + σ := by
    rcases Nat.lt_or_ge (s' + σ) ((i + 1) * (L + 1)) with hlt | hge
    · exfalso
      have hh : (s' + σ) / (L + 1) < i + 1 :=
        Nat.div_lt_of_lt_mul (by rw [Nat.mul_comm]; exact hlt)
      rw [hsσ'] at hh
      omega
    · exact hge
  omega

/-! ### Requirement word shape -/

/-- **Requirement word shape**: any word contributing requirements is empty or a
pure run `x_i^m` (`m ≥ 1`). Words containing a marker, or two distinct run letters
(crossing a block boundary), have unique windows and hence no requirements. -/
theorem req_word_shape {k L : Nat} {ps : List Nat} (hp : ∀ i, i < k → ps.getD i 0 ≤ L)
    {w : List Nat} {c : Nat} (hreq : (w, c) ∈ requirements (famText k L ps))
    (hw : w ≠ []) : ∃ i m, i < k ∧ 1 ≤ m ∧ w = List.replicate m (runL i) := by
  obtain ⟨_, hrm, _⟩ := (mem_requirements w c (famText k L ps)).mp hreq
  simp only [rightMaximal, Bool.and_eq_true] at hrm
  obtain ⟨hoccw, _⟩ := hrm
  obtain ⟨s, hsle, hwin⟩ := (occurs_eq_true w (famText k L ps) hw).mp hoccw
  have hw0 : 0 < w.length := by
    cases w with
    | nil => exact absurd rfl hw
    | cons x tl => simp
  have hq0 : s < (famText k L ps).length := by omega
  have hwL0 : (famText k L ps)[s]'hq0 = w[0]'hw0 := window_get hwin hw0 hq0
  obtain ⟨⟨hbk, _⟩, hcls⟩ := fam_letter hp hq0
  rcases hcls with ⟨hm0, _⟩ | ⟨hr0, _⟩
  · -- first letter is a marker: the whole word has a unique window → no requirements
    exact absurd (notreq_of_unique_window hw hwin hsle
      (fun s' ⟨hs'le, hwin'⟩ =>
        (unique_window_of_mark hp hw0 (hwL0.symm.trans hm0)
          hwin hsle hwin' hs'le).symm) c hreq)
      (by simp)
  · -- first letter is a run letter of block i₀ := s / (L+1)
    have hi0 : (famText k L ps)[s]'hq0 = runL (s / (L + 1)) := hr0
    rcases Classical.em (∃ t, ∃ h : t < w.length, w[t]'h ≠ w[0]'hw0) with
      ⟨t, ht, hne⟩ | hall
    · exfalso
      have hqt : s + t < (famText k L ps).length := by omega
      have hwLt : (famText k L ps)[s + t]'hqt = w[t]'ht := window_get hwin ht hqt
      obtain ⟨⟨_, _⟩, hclst⟩ := fam_letter hp hqt
      rcases hclst with ⟨hmt, _⟩ | ⟨hrt, _⟩
      · -- differing letter is a marker: unique window → no requirements
        exact absurd (notreq_of_unique_window hw hwin hsle
          (fun s' ⟨hs'le, hwin'⟩ =>
            (unique_window_of_mark hp ht (hwLt.symm.trans hmt)
              hwin hsle hwin' hs'le).symm) c hreq)
          (by simp)
      · -- differing letter is a run letter of a different block: engine B
        have hne' : s / (L + 1) ≠ (s + t) / (L + 1) := by
          intro heq
          exact hne (by
            have hcc : (famText k L ps)[s]'hq0 = (famText k L ps)[s + t]'hqt :=
              hi0.trans (by rw [heq]; exact hrt.symm)
            rw [hwL0, hwLt] at hcc
            exact hcc.symm)
        have hle : s / (L + 1) ≤ (s + t) / (L + 1) :=
          Nat.div_le_div_right (by omega)
        exact absurd (notreq_of_unique_window hw hwin hsle
          (fun s' ⟨hs'le, hwin'⟩ =>
            (unique_window_of_two_run_letters hp hw0 ht (by omega)
              (hwL0.symm.trans hi0) (hwLt.symm.trans hrt) (by omega)
              hwin hsle hwin' hs'le).symm) c hreq) (by simp)
    · -- all letters equal the first: the word is a pure run
      refine ⟨s / (L + 1), w.length, hbk, by omega, ?_⟩
      refine eq_replicate_of_const w (runL (s / (L + 1))) (fun t ht => ?_)
      have hcc : w[t]'ht = w[0]'hw0 :=
        Classical.byContradiction (fun hne => hall ⟨t, ht, hne⟩)
      rw [hcc, ← hwL0]
      exact hi0

/-! ### The cover -/

/-- Covering a word via an explicit window at the cover position. -/
theorem coversAt_of_window {T : Text} {W : List Nat} {x : Nat} (hge : W.length ≤ x)
    (hxle : x ≤ T.length)
    (hwin : ((T.drop (x - W.length)).take W.length) = W) :
    coversAt W x T = true := by
  rw [coversAt_iff_suffix]
  refine ⟨T.take (x - W.length), ?_⟩
  have hpref : pref T x = T.take x := rfl
  have h1 : T.take x = T.take (x - W.length) ++ ((T.drop (x - W.length)).take W.length) := by
    rw [← List.take_add, Nat.sub_add_cancel hge]
  rw [hpref, h1, hwin]

/-- Covering a single letter at position `x` (the prefix's last letter). -/
theorem coversAt_single {T : Text} {c x : Nat} (h1 : 1 ≤ x) (hxle : x ≤ T.length)
    (hlast : T[x-1]'(by omega) = c) : coversAt [c] x T = true := by
  rw [coversAt_iff_suffix]
  refine ⟨T.take (x - 1), ?_⟩
  have hpref : pref T x = T.take x := rfl
  rw [hpref]
  obtain ⟨y, hy⟩ : ∃ y, x = y + 1 := ⟨x - 1, by omega⟩
  subst hy
  have h2 : T.take (y + 1) = T.take y ++ [T[y]'(by omega)] := by
    simpa using List.take_add_one (by omega : y < T.length)
  have hysub : y + 1 - 1 = y := by omega
  have hlast' : T[y]'(by omega) = c :=
    (getElem_idx_congr (l := T) (i := y + 1 - 1) (j := y) (by omega) (by omega)).symm.trans hlast
  rw [hysub, h2]
  exact congrArg (fun z => T.take y ++ [z]) hlast'

/-- A window is determined by its letters. -/
theorem window_of_letters {T : Text} {a : Nat} {W : List Nat}
    (hale : a + W.length ≤ T.length)
    (hletters : ∀ t, (ht : t < W.length) → T[a + t]'(by omega) = W[t]'ht) :
    ((T.drop a).take W.length) = W := by
  have hd : (T.drop a).length = T.length - a := List.length_drop
  refine List.ext_get ?_ ?_
  · rw [List.length_take]
    omega
  · intro n h₁ h₂
    have hget : ((T.drop a).take W.length)[n]'h₁ = T[a + n]'(by omega) := by
      rw [List.getElem_take, List.getElem_drop]
    exact hget.trans (hletters n h₂)

/-- An occurring pure run of `x_i` of length `m+1` fits in the right run
(half grid: `2p_i ≤ L`), so `m + 1 ≤ L - p_i`. -/
theorem fam_run_occ_le {k L : Nat} {ps : List Nat} (hp : ∀ i, i < k → ps.getD i 0 ≤ L)
    {i m s : Nat} (hik : i < k) (hhalf : 2 * ps.getD i 0 ≤ L)
    (hsle : s + (m + 1) ≤ (famText k L ps).length)
    (hwin : ((famText k L ps).drop s).take (m + 1) = List.replicate (m + 1) (runL i)) :
    m + 1 ≤ L - ps.getD i 0 := by
  -- every letter of the window is x_i, so all positions lie in block i, off p_i
  have hblk : ∀ t, (ht : t < m + 1) → (s + t) / (L + 1) = i ∧
      (s + t) % (L + 1) ≠ ps.getD i 0 := by
    intro t ht
    have hst : s + t < (famText k L ps).length := by omega
    have hlen : (List.replicate (m + 1) (runL i)).length = m + 1 := by simp
    have hwin' : ((famText k L ps).drop s).take
        ((List.replicate (m + 1) (runL i)).length) = List.replicate (m + 1) (runL i) := by
      rw [hlen]; exact hwin
    have h := window_get hwin' (by rw [hlen]; exact ht) hst
    rw [List.getElem_replicate] at h
    sorry
  sorry

/-- **The cover**: every requirement of the family text is covered by one of
the `3k-1` marker / block-end / boundary positions. -/
theorem fam_cover {k L : Nat} {ps : List Nat} (hp : ∀ i, i < k → ps.getD i 0 ≤ L)
    (hL : 1 ≤ L) (hhalf : ∀ i, i < k → 2 * ps.getD i 0 ≤ L) :
    ∀ p, p ∈ requirements (famText k L ps) → ∃ x, x ≤ (famText k L ps).length ∧
      coversAt (p.1 ++ [p.2]) x (famText k L ps) = true ∧
      ( (∃ i, i < k ∧ x = i * (L + 1) + ps.getD i 0 + 1) ∨
        (∃ i, i < k ∧ x = (i + 1) * (L + 1)) ∨
        (∃ i, i + 1 < k ∧ x = (i + 1) * (L + 1) + 1) ) := by
  sorry

/-! ### Composition skeleton (statement-locked toward the χ-floor) -/

/-- The family text length (the `n = k(L+1)` input of the floor shape). -/
theorem fam_half_grid_size {k L : Nat} {ps : List Nat} (hp : ∀ i, i < k → ps.getD i 0 ≤ L) :
    (famText k L ps).length = k * (L + 1) := famText_length hp

/-- Half-grid parameters place every marker in the left half of its block,
giving the `L/2` width that makes marker shifts pairwise-incompatible. -/
theorem fam_half_grid {k L : Nat} {ps : List Nat} (hL : 1 ≤ L)
    (hhalf : ∀ i, i < k → 2 * ps.getD i 0 ≤ L) (i : Nat) (hik : i < k) :
    ps.getD i 0 ≤ L / 2 := by
  have := hhalf i hik
  omega

/-- **Skeleton stub** — the family's member texts (marker-perturbation class):
one text per assignment of half-grid marker positions. Size `(L/2 + 1)^k`,
pairwise distinguishable at markers. The construction and the incompatibility
argument (single-marker flip forces distinct locate-one answers) are the
remaining work; the statement is locked to the shape the floor needs. -/
theorem fam_forced_incompat {k L : Nat} {ps ps' : List Nat}
    (hL : 1 ≤ L)
    (hhalf : ∀ i, i < k → 2 * ps.getD i 0 ≤ L)
    (hhalf' : ∀ i, i < k → 2 * ps'.getD i 0 ≤ L)
    (hps : ∃ i, i < k ∧ ps.getD i 0 ≠ ps'.getD i 0)
    (f : Answer) : ¬ (CorrectLocateOne f (famText k L ps) ∧
                       CorrectLocateOne f (famText k L ps')) := by
  sorry

/-- **Skeleton stub** — the family's witness size: with `χ = Θ(k)` members'
requirements all distinct at markers, any correct locate-one oracle on all
`(L/2 + 1)^k` members must emit distinct answers, so the counting lemma
applies with `F.length = (L/2 + 1)^k`. -/
theorem fam_oracle_witness {k L : Nat} (hL : 1 ≤ L)
    (dec : Index → Answer) (s : Nat)
    (hex : ∀ ps, (∀ i, i < k → 2 * ps.getD i 0 ≤ L) → ∃ D : Index,
      space D ≤ s ∧ CorrectLocateOne (dec D) (famText k L ps)) :
    False := by
  sorry

/-- **Floor theorem shape (statement-locked)**: `family_counting` applied to
the witness-perturbation family yields `s ≥ Ω(χ · log(n/χ))`. The hypotheses
are exactly: the family bound (`chi_fam_bounds`, the remaining piece), the
member count `(#assignments)^k`, and pairwise incompatibility. With
`2^s ≥ (L/2 + 1)^k` and `χ ≤ 3k−1`, `n = k(L+1)`, this reads
`s ≥ k log(L/2) = Ω(χ log(n/χ))` bits. -/
theorem floor_theorem_shape {k L : Nat} (hL : 1 ≤ L) (hk : 1 ≤ k)
    (s : Nat)
    (hcount : (L / 2 + 1) ^ k ≤ 2 ^ (s + 1) - 1)
    (hchi : 2 * k ≤ χk ∧ χk ≤ 3 * k - 1)
    (hn : n = k * (L + 1)) :
    χk * (Nat.log2 (n / χk)) / 3 ≤ s + 1 := by
  sorry

end Fam

end Sxgc.LowerBound
