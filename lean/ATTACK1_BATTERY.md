# Attack-1 go/no-go battery report (LOWER_BOUND_PLAN Part 4, Step 4)

Workspace `/tmp/sxgc-laneY`, base `5e24cc5`. Family under test: the
witness-perturbation family `famText k L ps` — `k` blocks of run length
`L`, block `i` = `x_i^{p_i} · b_i · x_i^{L−p_i}` with per-block-distinct
letters (`x_i = 2i+2`, `b_i = 2i+3`), so the marker of block `i` sits at
position `i(L+1) + p_i + 1` and MOVES with `p_i`.  Grids: `halfGrid` =
`2 p_i ≤ L` per block; `grid` = unrestricted.  Scales: `k ≤ 4`, `n =
k(L+1) ≤ 12` (χ is the brute powerset oracle).  Everything below is
`#eval` output from `Attack1Eval.lean` / `Attack1Diag*.lean` (repro:
`lake env lean Attack1Eval.lean` after `lake build`).

## Verdict: **GO (reformulated)** — natural-tier floor attack proceeds on the
uniform-bounding form; the original strict-invariance form is REFUTED with
the obstruction fully characterized below.

## (a) χ across the grids — `(members, min χ, max χ)`

Half grids (the Attack-1 family spec):

| k | L | n | members | χ range | invariant? |
|---|---|---|---|---|---|
| 1 | 4 | 5 | 3 | 2 | YES |
| 1 | 5 | 6 | 3 | 2 | YES |
| 2 | 4 | 10 | 9 | 4–5 | NO |
| 2 | 5 | 12 | 9 | 4–5 | NO |
| 3 | 2 | 9 | 8 | 6 | YES (χ = 2k) |
| 3 | 3 | 12 | 8 | 6–8 | NO |
| 4 | 2 | 12 | 16 | 8 | YES (χ = 2k) |

Full grids (obstruction documentation): (1,2): χ=2 constant; (2,2):
4–5; (2,3): 4–5; (3,2): 6–7.  χ linearity in k (L=2 half grid):
χ = 2k exactly for k = 1,2,3,4 (2, 4, 6, 8).

**Empirical law (verified on every grid run):** for `L ≥ 3`,

    χ(famText k L ps) = 2k + #{ i ≥ 2 : p_i ≥ 1 }

and at `L = 2` the correction term vanishes (χ = 2k flat).  In
particular, on every half grid at every tested (k, L):

    2k ≤ χ ≤ 3k − 1        (uniformly Θ(k))

## The obstruction, characterized (why strict invariance fails)

Diffing requirement lists and brute minimal witness sets at
`k=2, L=3` — `[0,0]` = `[3,2,2,2 | 5,4,4,4]` (χ=4, witnesses
{1,4,5,8}) vs `[0,1]` = `[3,2,2,2 | 4,5,4,4]` (χ=5, witnesses
{1,4,5,6,8}):

- At `p_i = 0` block `i` STARTS with its marker, so the block's
  junction position (its first char) and its marker position COINCIDE —
  one coverage class serves the junction requirements
  `(x_{i−1}^j, b_i)` AND the marker requirements `(ε, b_i)`.
- At `p_i ≥ 1` the junction position (first `x_i`) and the marker
  position (offset `p_i+1`) are DISTINCT, each with private
  requirements — the new ones at `[0,1]` are exactly `(x_1, x_2)`,
  `(x_1 x_1, x_2)` (junction, now into `x_2` instead of `b_2`) and
  `(x_2, b_2)` (the marker's left extension, which exists only when a
  left run does) — so the class SPLITS: +1 per non-first block with
  `p_i ≥ 1`.
- At `L = 2` the runs are too short to give the junction position a
  private requirement, so the split does not happen (χ = 2k flat).

This is a *real combinatorial property of the family*, not a battery
deficiency: the brute oracle is the ground truth and the law above is
exact on every member tested.

## (b) unique forced answers — PASS (half AND full grids, all tested)

For every member and every block `i`: `validCovers [b_i] T = [marker_i]`
and, whenever `p_i ≥ 1`, also `validCovers [x_i, b_i] T = [marker_i]`.
The correct answer for the marker query (and the perturbed two-letter
query) is UNIQUELY forced — the marker position.

## (c) pairwise answer-incompatibility — PASS (half AND full grids)

Forced-answer vectors `(f[b_1], …, f[b_k])` are pairwise distinct across
every grid: no single answer function is correct for two distinct
members (by `incompat_of_forced`, since distinct `p_i` ⇒ distinct
forced marker answers on the shared query `[b_i]`, which occurs in both
texts).

## (d) requirement status — PASS

`([x_i], b_i) ∈ requirements T` whenever `p_i ≥ 1` (verified on all
half-grid members): the perturbed word is requirement-relevant, so the
markers are tied into χ through the S1 bridge (`emitted_suffixient`),
not merely occurring.

## (e) bridge instance — PASS

`suffixient (emitted (fFirst T) T) T = true` on every half-grid member
tested (a concrete instance of the kernel-checked
`emitted_suffixient`/`fFirst_correct`).

## (f) positivity — PASS

All members are `positive` (letters `2..9`, well inside `1..127`).

## Why this is a GO for the floor program

`family_counting` (kernel-checked, no sorry) does not need χ-invariance
— it needs (i) pairwise answer-incompatibility (PASS, (c)) and (ii) the
space bound to be expressible in χ.  The uniform bound
`2k ≤ χ ≤ 3k − 1` supplies (ii): with `|family| = (⌊L/2⌋+1)^k`
independent choices and `n = k(L+1)`,

    s ≥ log₂ |family| = k · log₂(⌊L/2⌋+1) ≥ (χ/3) · log₂(⌊L/2⌋+1)
                      = Ω(χ · log(n/χ))

on the family (for `L` a growing function of `k`; the additive constant
is standard family-specific-floor bookkeeping).  The natural-tier
statement (S2 with `f = Θ(log(n/χ)))` is therefore reachable by this
attack with the uniform-bounding formulation, at the cost of the exact
class-counting lemma for the family (next Lean target:
`chi_fam_bounds : 2k ≤ chi (famText k L ps) ≤ 3k − 1` on the half
grid — the battery's exact law makes this a concrete, battery-checked
statement, not a hope).

The strictly-invariant formulation ("χ = k + const across the grid")
would have been more convenient but is false for this gadget family;
any future family wanting strict invariance must merge the junction and
marker classes (e.g. separators that make junction requirements
impossible) — noted as the family-design lesson, not pursued here.

## Scale honesty

χ is the brute powerset minimality oracle — only computable at
`n ≤ 12`.  All grids respect that; the heaviest single run is
`k=4, L=2` (16 members, n = 12).  The checks (b)–(f) are polynomial and
were additionally run on the FULL grids (not just half) where noted.
No claim is made beyond the tested grid sizes; the empirical law is
exact on every tested member.
