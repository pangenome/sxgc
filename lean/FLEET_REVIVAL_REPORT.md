# FLEET_REVIVAL_REPORT — witness revival under concatenation (formalized)

Lane: LEAN GRIND — WITNESS REVIVAL FORMALIZATION.
Deliverable: `lean/SxgcRevival.lean` (`lean_lib SxgcRevival`), all changes UNSTAGED.
Status: **main goal PROVED** (both invariant refutations + χ values). One sub-goal
(the requested χ-monotonicity corollary) is **NOT** established and is reported as
such — no false statement was proved.

## Result

The chunk-merge premise audit's counterexample is now formalized against the
repository's own finite-text suffixient definitions (imported from `Sxgc`, not
re-defined). With `SEP = 30`, `A = [1,2,2,SEP]`, `B = [1,SEP]`, `J = A ++ B`:

| Fact | Theorem |
| --- | --- |
| pos 1 of `A` is a maximal coverage class | `isMax_A_one` |
| pos 2 of `A` is NOT maximal (dominated by pos 3) | `not_isMax_A_two` |
| pos 2 of `J` IS maximal — **revival** | `isMax_J_two` |
| pos 1 of `J` is NOT maximal (destroyed) | `not_isMax_J_one` |
| "new witnesses only in the added chunk" is FALSE | `new_witnesses_not_confined_to_old_chunk` |
| "old witnesses only get dominated away" is FALSE | `old_witnesses_need_not_survive` |
| pos 2 is not a canonical representative in `A` | `not_isRep_A_two` |
| pos 2 IS a canonical representative in `J` | `isRep_J_two` |
| `chi A = 3`, `chi J = 5` | `chi_A`, `chi_J` |
| `maxClassCount A = 3`, `maxClassCount J = 5` | `maxClassCount_A`, `maxClassCount_J` |
| χ is not preserved (`chi A ≠ chi J`) | `chi_changes` |

Both refutations are stated as negations of the *general* invariant shape,
instantiated at the counterexample, so no monotone pruning rule of that form can
be sound.

## Mechanism, confirmed by `#eval`

* Revival: in `A`, `covSet A 2 = {((),2)}`, strictly dominated by
  `covSet A 3 = {((),2),((2,),2)}`. In `J`, `covSet J 2 = {((),2),((1,),2)}`.
  The requirement `((1,),2)` is covered by **position 2 only** (`#eval` position
  filter over `J` returns `[2]`; over `A` returns `[]`). Appending `B` makes the
  context `1` right-maximal (two right-extensions `2` and `SEP`), creating a
  requirement whose only witness is the old, previously dominated position 2.
* Destruction: `covSet A 1 = {((),1)}` was maximal; in `J` the appended position 5
  has `covSet J 5 = {((),1),((30,),1)}`, which strictly dominates it, so position
  1 is no longer maximal. (`((30,),1)` is covered only by position 5.)
* `chi A = 3`, `chi J = 5`; `positive A = positive J = true` (so the
  `chi_eq_maxClasses` bridge applies to both).

## The requested chi corollary is NOT supported (honest boundary)

The brief's parenthetical — "there is no lemma of the form
`chi T1 ≤ chi (T1 ++ T2)` in general" — could **not** be established and is
probably false as a *refutation target*: here `chi A = 3 ≤ 5 = chi J`, so the
candidate inequality holds for this pair. Searches for a chi-**decreasing** pair
all came up empty:

| search | range | chi-decrease found |
| --- | --- | --- |
| exhaustive, alphabet {1,2} | \|A\|,\|B\| ≤ 7, separator-aligned | none |
| exhaustive, alphabet {1,2,3} | \|A\|,\|B\| ≤ 5 | none |
| exhaustive, alphabet {1,2,3} | \|A\| ≤ 6, \|B\| ≤ 3 | none |
| random, alphabet {1,2,3} | \|A\| ≤ 14, \|B\| ≤ 10, 40k pairs | none |
| requirement-set removal, alphabet {1,2} | \|A\| ≤ 7, \|B\| ≤ 5 | none |

(χ changed in 19790/20000 random pairs, always non-decreasing in the samples
inspected.) So we did **not** prove any chi-decrease statement, and we did not
prove the monotone inequality either — it is left open, precisely. What the merge
algorithm actually needed was monotonicity of the **witness set**, and that is
what the theorems above refute. This distinction is recorded in the file header.

Caveat: My earlier ad-hoc Python search (document-aligned, small) also found no
requirement-set removal, even though the audit report asserts append can remove a
suffix-only right-maximal context. The structural argument for removal is sound in
general (appending can destroy `isSuffix w` status), but I did not exhibit a
document-aligned instance, and did not formalize that claim. Not claimed here.

## Gates

| Gate | Result |
| --- | --- |
| `lake env lean SxgcRevival.lean` | PASS (2.1 s) |
| `lake build SxgcRevival` | PASS (1.0 s, `lean_lib SxgcRevival` added) |
| `lake build` (whole project) | PASS (6 jobs) |
| no `sorry` / `axiom` / `native_decide` / `admit` in new file | PASS (grep: NONE) |
| existing statements/definitions unchanged | PASS (only `lakefile.lean` has an appended `lean_lib` line; no theorem touched) |
| `#eval` gates matching claimed numbers | PASS (`chi A = 3`, `chi J = 5`; revived/destroyed coverage sets printed) |

## Files

* `lean/SxgcRevival.lean` — new, the deliverable.
* `lean/lakefile.lean` — one appended line `lean_lib SxgcRevival`.
* No other file touched; all changes left UNSTAGED.
