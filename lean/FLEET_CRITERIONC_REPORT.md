# Fleet lane: Criterion C termination (partial — two provable halves closed)

## Outcome

**Two of the three criterion-C obligations are now Lean-proven, with zero new proof holes.**
The full `g = 1` order-preservation obligation is stated precisely but not proven — the
honest boundary is below.

## What was proven (added to `lean/SxgcPhi.lean`, new "Criterion C" section)

Criterion C (SXI2 v3 `DESIGN.md`) is `C(i) := (byte-frequency gcd g = 1) OR (cyclic SA
domain of run i is a singleton)`. The section formalizes the two halves that need no
cyclic-rotation model:

1. **`gcdOne_not_isPower` — `g = 1` rules out proper powers.**
   - `byteFreqGcd` = gcd of byte frequencies over `T`'s distinct symbols (`T.eraseDups`).
   - `foldl_gcd_dvd_acc`, `dvd_foldl_gcd` — fold/gcd divisibility infrastructure.
   - `count_flatten_replicate` — a byte appearing `m` times in `U` appears `m · count`
     in `U^m` (`((replicate m U).flatten).count c = m * U.count c`).
   - `isPower_dvd_byteFreqGcd` — a proper power `U^m` (`IsPower T := ∃ U m, 1 < m ∧ T =
     (replicate m U).flatten`) has every frequency divisible by `m`, so `m ∣ byteFreqGcd T`.
   - `gcdOne_not_isPower` — `byteFreqGcd T = 1` and `IsPower T` give `m ∣ 1` with `1 < m`,
     impossible. This is exactly the DESIGN.md reduction "a separate lemma could derive
     `Primitive T` from `gcd(byteFrequencies T) = 1`."

2. **`criterion_singleton_branch` — the singleton disjunct is exact.**
   - `phiFormula R u v x := (v + (x - u)) % R.length` is the affine interval formula.
   - On a singleton domain the only point is the run tail `u = rowPos R (a-1)`, and the
     formula returns `v = rowPos R a = phiInv R u` (`headFromTail`), with **no periodicity
     hypothesis**. This is the fallback branch a periodic text takes.

## What is NOT proven (precise remaining obligation)

For primitive `R` (e.g. `byteFreqGcd R = 1` by the theorem above), all `n` cyclic rotations
of `R` are pairwise distinct, and hence the Nishimoto–Tabei order-preserving interval map is
exact: for run `i` with tail value `u`, head value `v = phiInv R u`, and cyclic SA domain
`D_i`, every `x ∈ D_i` satisfies `phiFormula R u v x = phiInv R x`.

Missing bridges (not asserted anywhere, not stubbed with `sorry`):
- **(a)** a cyclic-rotation model of `R` plus primitive ⟹ distinct rotations
  (Lyndon–Schützenberger / Fine–Wilf direction);
- **(b)** order preservation of the LF map for a tie-free suffix order, yielding the affine
  interval image.

Both require suffix-array/BWT infrastructure that `SxgcPhi.lean` deliberately abstracts
behind `PieceSound`/interval certificates. No `sorry` was introduced: this section adds
6 theorems/lemmas, all closed.

## Gates (all green in worktree `cb9fb61b`)

| Gate | Result |
|---|---|
| `lake build SxgcPhi` | GREEN |
| `lake build` (default exe) | GREEN, 6 jobs |
| `lake env lean --run Main.lean` | 511 pass / 0 fail; BIT 1B GATE GREEN |
| `lake env lean PhiEval.lean` | 17 texts, 381 positions, 0 Phi mismatches; 0 run-boundary |
| `lake env lean PhiAxioms.lean` | 6 new theorems audited; only `propext`, `Classical.choice`, `Quot.sound`; no `sorryAx` |
| `grep -c sorry SxgcPhi.lean` | 0 |
| `git diff --stat` | `SxgcPhi.lean` +96, `PhiAxioms.lean` +8 — **104 insertions, 0 deletions** (no existing statement or definition touched) |

## Files changed (left UNSTAGED)

- `lean/SxgcPhi.lean` — new "Criterion C" section (insertion only).
- `lean/PhiAxioms.lean` — 6 appended `#print axioms` audit lines.

No commit, no `git add`. No running data job or `.sxi` artifact was touched.
