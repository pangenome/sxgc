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

end Sxgc.LowerBound
