# Formal assembly lane report — 2026-09-26

TARGET `fm_equivalence_bounded`: **PROVEN. Sorry ledger 5 → 4.**

## What was done

`fm_equivalence_bounded` (the bounded successor of the retired-false unbounded
`fm_equivalence`) is now a complete theorem in Sxgc.lean — no sorry, no axioms:
the scan machine and the fmSpec machine emit the same suffixient set for every
positive text whose LCP values respect the model's Int arithmetic
(side-condition vacuous for real texts: lcp ≤ |T| < 2^63).

FmJoint.lean (the committed joint-induction machinery) was MERGED into
Sxgc.lean (Sxgc cannot import a downstream file, so the assembly had to live
with the machinery) and then extended with the assembly layer:

- `coupled_unpack` — three-case view of the `Coupled` invariant
  ((c) never-involved / (a) coupled-active / (b) delayed).
- `hemOf` + `hemOf_cases`/`hemOf_mem_S` — the FM emission as an Option value
  plus its extraction lemmas.
- `slot_composite` / `upd_getR_self` / `Ls_getD` / `getFM_lt` — the scan/FM
  slot-level shapes of the full boundary composite (evalStep + two upds;
  fmStep + fmStep), each derived from the machine definitions.
- `bound_coupled` — the boundary row-step assembly for the `Coupled` part of
  the invariant: applies `coupled_step_inv` to the two involved chars
  (ending char p, starting char t.c) and `coupled_step_notinv` to every
  uninvolved char.
- `bound_mem` — the MEM part (∀ x, x ∈ out₁ ↔ x ∈ S' ∨ Delayed-post):
  the per-character emission accounting across the boundary, including the
  impossibility argument for "FM overwrites without emitting while the scan
  drops without re-arming" (prevSmaller witness vs the armed NSV; uses the
  (a)-invariant's past condition and `nextSmaller_le`).
- `scanAux_cons_bnd/nonbnd`, `fmAux_cons_bnd/nonbnd` — one-step machine
  unfoldings.
- `joint_stream` — the main lockstep induction: both machines over the stream
  carrying `JointInv` (per-char Coupled + out ↔ S ∨ Delayed) and the
  running-min alignment `m = winOpen (Ls ts) (prevBnd ts k) k`; non-boundary
  rows preserve the invariant by monotonicity (`coupled_nonbnd`); boundary
  rows consume `bound_coupled` + `bound_mem`; at stream end the flush
  (`evalStep (-1)` vs `finalEmit`) converts the invariant into the membership
  equivalence (`evalStep_mem`, `finalEmit_mem`).
- `fm_equiv_stream` — the wrapper: machine-start state satisfies
  `jointInv_base`; `prevBnd ts 1 = 0` and `winOpen _ 0 1 = MAXINT` align the
  initial running min.

The top-level proof of `fm_equivalence_bounded` converts the hypothesis
`hsat : ∀ t ∈ triplesOf T, t.lcp ≤ MAXINT.toNat` to the stream form
(via `getD_mem` + `Ls_getD`), case-splits on `triplesOf T` (nil is trivial:
both machines return []), and applies `fm_equiv_stream`.

## Gates

- `lake build`: 0 errors.
- `lake env lean --run Main.lean`: **BIT 1B GATE: GREEN**
  (511-text exhaustive covering+minimality; all battery evals print their
  recorded counts).
- `lake env lean Sxgc.lean`: exactly 4 `declaration uses 'sorry'` warnings —
  the ledger: O1 (`covering_given_stream`, line ~1888), minimality (~1917),
  `chi_from_events` (~5419), and the retired-false record
  `witnesses_at_boundaries_FALSE_AS_STATED` (~5706, kept as record only).

## Diff summary

- `lean/FmJoint.lean`: DELETED — its 1,743 lines (machinery, all sorry-free)
  were merged verbatim into Sxgc.lean (minus the stale HANDOFF block and the
  debug `trace_state`s), because the assembly could not import a downstream
  file and Sxgc.lean hosts the target statement.
- `lean/Sxgc.lean`: +2,986 / −13 lines.  All pre-existing theorems, statements
  and evals are untouched; the only change to `fm_equivalence_bounded` is that
  its `sorry` is replaced by the proof (statement verified byte-identical in
  the diff).

## Secondary target (O2 Nodup): not attempted

Budget went to the primary target (the boundary MEM assembly was the single
largest block of the session).  Route note for a future lane: O2's statement is
unbounded; the saturation record already shows scan duplicates positions in
the above-MAXINT regime, so an unbounded proof is presumably FALSE as stated —
first step would be the eval-check below saturation on the 729 battery
(already 0 counterexamples) and then the sa-distinctness route (for
`triplesOf T`, sa values form a permutation, so emitted positions N − sa are
distinct per row) — but per house rules any hsat-side-condition change to O2
is a statement-lock event requiring supervisor adjudication before an edit.

## Obstructions hit (for the next lane's context)

None remaining for this target.  The two notable Lean-without-Mathlib traps
that cost iterations (now recorded in the file for the future): `cases h : e`
substitutes the scrutinee everywhere in the goal but does not iota-reduce the
surrounding match (use `simp only [h]`), and pair-projection through an
unreduced `ite` is not defeq (eliminate ifs one at a time with `rw` before
`show`-collapsing projections).
