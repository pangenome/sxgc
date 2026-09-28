import Sxgc
import LowerBound

/-!
# The continuation-answering bridge (LM_CHI_BACKEND_SPEC §5.1)

The locate-one bridge (`Sxgc.LowerBound.emitted_suffixient`,
`Sxgc.LowerBound.chi_le_of_oracle`) proves that any oracle answering EVERY
occurring word with a covering position emits a suffixient set on the
requirement words alone, hence `chi T ≤ |emitted f T|`.

The LM setting (docs/LM_CHI_BACKEND_SPEC.md, open question §5.1) does not
need locate-one: a continuation-answering system only has to answer the
requirements — for each right-maximal context `w` and each occurring
right-extension `c`, a position where `w ++ [c]` is covered.  This file
closes that gap: the bridge holds verbatim for the strictly weaker
continuation-only oracle, in all three forms the spec's framing needs:

  * `chi_le_of_cont_oracle`            — `chi T ≤ |emitted f T|`
      (duplicates allowed; the `chi_le_of_oracle` analog);
  * `chi_le_of_cont_oracle_distinct`   — `chi T ≤ |(dedup (emitted f T))|`
      (the precise LM form: the system must consult `chi T` DISTINCT
      positions over the course of answering all the fork queries);
  * `chi_le_of_realized_cont`          — the fixed-decoder index form
      (`Realizes dec D f ∧ CorrectContinuation f T` forces the decoded
      answer's distinct emitted set past `chi T`).

Faithfulness notes (statements are new; designed to the spec's §5.1
wording, not weakened to make proofs easy):

  * `CorrectContinuation` demands a genuine 1-based text position
    (`1 ≤ x ∧ x ≤ T.length`) exactly as `CorrectLocateOne` does.  That is
    the "corpus position" reading AND the constraint the membership step
    of `chi_le_of_suffixient_mem` consumes; `coversAt` alone admits
    past-the-end positions (`pref T x = T`), the same degeneracy the
    locate-one template already records (`le_of_coversAt` and the M0
    refinement note in `LowerBound.lean`).
  * `CorrectLocateOne` answers all occurring words; requirement words
    occur, so `correctLocateOne_imp_cont` holds — the continuation oracle
    is strictly the weaker demand, and the bridge below therefore covers
    the locate-one case while standing on its own.
-/

namespace Sxgc.LM

open Sxgc Sxgc.LowerBound

/-! ## O_cont: the requirement-driven answer form

`f` need only be defined on the continued requirement words `w ++ [c]` —
the fork queries of the generation loop.  Nothing is demanded of `f` on
other words. -/

/-- Continuation-answering correctness: every requirement `(w, c)` of `T`
is answered with a genuine 1-based text position at which the continued
word `w ++ [c]` is covered (ends).  The requirement-driven answer form of
LM_CHI_BACKEND_SPEC §5.1 — strictly weaker than `CorrectLocateOne`
(which answers every occurring word), yet still forced past `chi T`. -/
def CorrectContinuation (f : Answer) (T : Text) : Prop :=
  ∀ w c, (w, c) ∈ requirements T →
    ∃ x, f (w ++ [c]) = some x ∧ 1 ≤ x ∧ x ≤ T.length ∧
      coversAt (w ++ [c]) x T = true

/-- Locate-one correctness is the stronger demand: its answers on the
requirement words satisfy `CorrectContinuation` verbatim. -/
theorem correctLocateOne_imp_cont {f : Answer} {T : Text}
    (h : CorrectLocateOne f T) : CorrectContinuation f T := by
  intro w c hp
  obtain ⟨_, _, hext⟩ := (mem_requirements w c T).mp hp
  exact h (w ++ [c]) ((mem_rightExts w c T).mp hext)

/-! ## The bridge, continuation form -/

/-- Every position a continuation oracle emits on the requirement words is
a genuine text position — the membership step of
`chi_le_of_suffixient_mem`, shared by both counting forms below. -/
theorem cont_emitted_mem_positionsT {f : Answer} {T : Text}
    (h : CorrectContinuation f T) : ∀ x ∈ emitted f T, x ∈ positionsT T := by
  intro x hx
  obtain ⟨w, hw, hfx⟩ := (mem_emitted).mp hx
  obtain ⟨p, hp, rfl⟩ := (mem_reqWords).mp hw
  obtain ⟨y, hy, h1, h2, _⟩ := h p.1 p.2 hp
  have hxy : x = y := by
    rw [hfx] at hy
    exact Option.some.inj hy
  subst hxy
  exact mem_positionsT h1 h2

/-- **Continuation bridge, part 1** — any correct continuation oracle
emits a suffixient position set on the requirement words alone (the analog
of `emitted_suffixient`; mechanical from the existing machinery). -/
theorem emitted_suffixient_of_cont (f : Answer) (T : Text)
    (h : CorrectContinuation f T) : suffixient (emitted f T) T = true := by
  refine suffixient_of_witnesses (emitted f T) T ?_
  intro p hp
  obtain ⟨x, hx, _, _, hc⟩ := h p.1 p.2 hp
  refine ⟨x, ?_, hc⟩
  exact (mem_emitted).mpr ⟨p.1 ++ [p.2],
    (mem_reqWords).mpr ⟨p, hp, rfl⟩, hx⟩

/-- **Continuation bridge, part 2** — any correct continuation oracle
emits at least `chi T` positions on the requirement words alone
(duplicates allowed; the `chi_le_of_oracle` analog). -/
theorem chi_le_of_cont_oracle (f : Answer) (T : Text)
    (h : CorrectContinuation f T) : chi T ≤ (emitted f T).length :=
  chi_le_of_suffixient_mem T (emitted f T)
    (cont_emitted_mem_positionsT h) (emitted_suffixient_of_cont f T h)

/-! ## The distinct-count form (the precise LM statement)

"Must consult ≥ `chi T` positions" means DISTINCT positions: a system that
re-emits one position `chi T` times has not consulted `chi T` positions.
Removing duplicates preserves both the witness property (`mem_dedup`) and
positional membership, so the deduplicated emitted set is itself a
suffixient set of genuine positions. -/

/-- Removing duplicates preserves suffixience: every requirement still
has a covering witness, now among the distinct positions. -/
theorem suffixient_dedup {S : List Nat} {T : Text}
    (h : suffixient S T = true) : suffixient (dedup S) T = true := by
  refine suffixient_of_witnesses (dedup S) T ?_
  intro p hp
  rw [show suffixient S T =
        (requirements T).all (fun q => S.any (fun x => coversAt (q.1 ++ [q.2]) x T)) from rfl,
    List.all_eq_true] at h
  obtain ⟨x, hx, hc⟩ := List.any_eq_true.mp (h p hp)
  exact ⟨x, (mem_dedup S x).mpr hx, hc⟩

/-- Duplicate removal never grows a list; with it, the plain counting form
is a corollary of the distinct form. -/
theorem dedupAux_length_le : ∀ (l seen : List Nat),
    (dedup.dedupAux seen l).length ≤ l.length := by
  intro l
  induction l with
  | nil => intro seen; simp [dedup.dedupAux]
  | cons a rest ih =>
    intro seen
    simp only [dedup.dedupAux]
    by_cases hx : (seen.contains a) = true
    · rw [if_pos hx]
      simp only [List.length_cons]
      have hle := ih seen
      omega
    · rw [if_neg hx]
      have hle := ih (a :: seen)
      simp only [List.length_cons]
      omega

theorem dedup_length_le (l : List Nat) : (dedup l).length ≤ l.length :=
  dedupAux_length_le l []

/-- **The precise LM form** — any correct continuation oracle consults at
least `chi T` DISTINCT positions across the requirement words. -/
theorem chi_le_of_cont_oracle_distinct (f : Answer) (T : Text)
    (h : CorrectContinuation f T) : chi T ≤ ((dedup (emitted f T))).length := by
  refine chi_le_of_suffixient_mem T ((dedup (emitted f T))) ?_
    (suffixient_dedup (emitted_suffixient_of_cont f T h))
  intro x hx
  exact cont_emitted_mem_positionsT h x ((mem_dedup (emitted f T) x).mp hx)

/-- **The index form** — a fixed-decoder bit-space index whose decoded
answer function is a correct continuation oracle is forced past `chi T`
distinct emitted positions: "any continuation-answering system must
consult ≥ `chi T` positions" in its precise, kernel-checked form
(`Realizes dec D f` is by definition `dec D = f`). -/
theorem chi_le_of_realized_cont (dec : Index → Answer) (D : Index) (T : Text)
    (h : CorrectContinuation (dec D) T) :
    chi T ≤ ((dedup (emitted (dec D) T))).length :=
  chi_le_of_cont_oracle_distinct _ T h

end Sxgc.LM
