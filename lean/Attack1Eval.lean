import LowerBound
import Sxgc

/-!
# Attack-1 go/no-go battery (LOWER_BOUND_PLAN Part 4, Step 4)

The witness-perturbation family: `k` blocks of run length `L`, each block
`i` using its own run letter `2i+2` and marker letter `2i+3` (all letters
distinct across blocks, all in the `positive` range).  Block `i` embeds
its marker at offset `p_i` inside the run:

    B_i(p) = x_i^{p} · b_i · x_i^{L-p}

so the marker position `i(L+1) + p_i + 1` MOVES with `p_i`, and the word
`x_i^j b_i` occurs exactly once (at the marker).  The checks:

  (a) χ-invariance across the parameter grid (the go/no-go core);
  (b) unique covering: the forced answer for `[b_i]` (and for
      `[x_i, b_i]` when `p_i ≥ 1`) is exactly the marker position;
  (c) pairwise answer-incompatibility: distinct parameter vectors force
      distinct correct-answer vectors (via (b) + `incompat_of_forced`);
  (d) requirement status: `([x_i], b_i)` is a genuine requirement whenever
      `p_i ≥ 1` (the marker word is not merely occurring but
      requirement-relevant, tying the markers into `χ` through the bridge);
  (e) bridge instance: `fFirst`'s emitted set is suffixient (a concrete
      instance of `emitted_suffixient` on family members);
  (f) positivity.

Grids: `halfGrid L k` restricts `2 p ≤ L` per block (the Attack-1 family
spec — see ATTACK1_BATTERY.md for why the unrestricted grid fails
χ-invariance and how that failure is characterized, not hidden).
-/

open Sxgc Sxgc.LowerBound

namespace Attack1

def runL (i : Nat) : Nat := 2 * i + 2
def markL (i : Nat) : Nat := 2 * i + 3

def block (i L p : Nat) : Text :=
  (List.replicate p (runL i)) ++ [markL i] ++ (List.replicate (L - p) (runL i))

def famText (k L : Nat) (ps : List Nat) : Text :=
  (((List.range k).map (fun i => block i L (ps.getD i 0))).flatten)

/-- The (0-based) marker position of block `i` in `famText k L ps`. -/
def markerPos (L : Nat) (ps : List Nat) (i : Nat) : Nat :=
  i * (L + 1) + (ps.getD i 0) + 1

/-- Full parameter grid: `p ∈ [0..L]^k`. -/
def grid (L : Nat) : Nat → List (List Nat)
  | 0 => [[]]
  | k + 1 => (grid L k).flatMap (fun s => (List.range (L + 1)).map (fun p => p :: s))

/-- Attack-1 family grid: `p ∈ [0..L/2]^k` (the half-space where the
junction structure is parameter-invariant; see ATTACK1_BATTERY.md). -/
def halfGrid (L : Nat) : Nat → List (List Nat)
  | 0 => [[]]
  | k + 1 => (halfGrid L k).flatMap (fun s =>
      ((List.range (L + 1)).filter (fun p => 2 * p ≤ L)).map (fun p => p :: s))

/-- (a) χ statistics over a grid: (members, min χ, max χ).  Invariance
holds iff min = max. -/
def chiStats (k L : Nat) (pss : List (List Nat)) : Nat × Nat × Nat :=
  let chis := pss.map (fun ps => chi (famText k L ps))
  (pss.length, chis.foldl (fun a b => min a b) 999, chis.foldl (fun a b => max a b) 0)

/-- (b) unique forced answers: for every block, the bare-marker word and
(when the left run is nonempty) the two-letter perturbed word have
exactly one valid cover — the marker position. -/
def chkUnique (k L : Nat) (ps : List Nat) : Bool :=
  (List.range k).all (fun i =>
    let T := famText k L ps
    let m := markerPos L ps i
    validCovers [markL i] T == [m] ∧
      (ps.getD i 0 == 0 ∨ validCovers [runL i, markL i] T == [m]))

/-- The forced-answer vector of a member: one entry per block. -/
def ansVec (k L : Nat) (ps : List Nat) : List (Option Nat) :=
  (List.range k).map (fun i => (validCovers [markL i] (famText k L ps)).head?)

/-- (c) pairwise incompatibility: forced-answer vectors are pairwise
distinct across the grid (with (b), no single f is correct for two
members — the concrete witness of `incompat_of_forced`). -/
def chkInjective (k L : Nat) (pss : List (List Nat)) : Bool :=
  let vs := pss.map (ansVec k L)
  vs.all (fun v => (vs.filter (fun w => w == v)).length == 1)

/-- (d) requirement status of the perturbed word `x_i b_i`. -/
def chkReq (k L : Nat) (ps : List Nat) : Bool :=
  (List.range k).all (fun i =>
    ps.getD i 0 == 0 ∨ ([runL i], markL i) ∈ requirements (famText k L ps))

/-- (e) bridge instance: the canonical oracle's emitted set is
suffixient on this member. -/
def chkBridge (k L : Nat) (ps : List Nat) : Bool :=
  let T := famText k L ps
  suffixient (emitted (fFirst T) T) T

/-- (f) positivity of a member. -/
def chkPos (k L : Nat) (ps : List Nat) : Bool := positive (famText k L ps)

/-! ### χ-invariance on the Attack-1 (half) grids — the go/no-go core.
Expect `min = max` in every row.  `n = k(L+1)` per member. -/

#eval chiStats 1 4 (halfGrid 4 1)   -- n = 5,   3 members
#eval chiStats 1 5 (halfGrid 5 1)   -- n = 6,   3 members
#eval chiStats 2 4 (halfGrid 4 2)   -- n = 10,  9 members
#eval chiStats 2 5 (halfGrid 5 2)   -- n = 12,  9 members
#eval chiStats 3 2 (halfGrid 2 3)   -- n = 9,   8 members
#eval chiStats 3 3 (halfGrid 3 3)   -- n = 12,  8 members
#eval chiStats 4 2 (halfGrid 2 4)   -- n = 12, 16 members

/-! ### Full-grid χ behavior (obstruction documentation, not the family):
χ varies with the number of blocks having `2 p > L` (extra junction
class) — reported in ATTACK1_BATTERY.md. -/

#eval chiStats 1 2 (grid 2 1)       -- n = 3,   3 members
#eval chiStats 2 2 (grid 2 2)       -- n = 6,   9 members
#eval chiStats 2 3 (grid 3 2)       -- n = 8,  16 members
#eval chiStats 3 2 (grid 2 3)       -- n = 9,  27 members

/-! ### (b) unique forced answers — on half grids AND full grids. -/

#eval (halfGrid 4 2).all (chkUnique 2 4) ∧ (halfGrid 5 2).all (chkUnique 2 5) ∧
      (halfGrid 2 3).all (chkUnique 3 2) ∧ (halfGrid 3 3).all (chkUnique 3 3) ∧
      (halfGrid 2 4).all (chkUnique 4 2)
#eval (grid 3 2).all (chkUnique 2 3) ∧ (grid 2 3).all (chkUnique 3 2)

/-! ### (c) pairwise incompatibility — half and full grids. -/

#eval chkInjective 2 4 (halfGrid 4 2) ∧ chkInjective 2 5 (halfGrid 5 2) ∧
      chkInjective 3 3 (halfGrid 3 3) ∧ chkInjective 4 2 (halfGrid 2 4)
#eval chkInjective 2 3 (grid 3 2) ∧ chkInjective 3 2 (grid 2 3)

/-! ### (d), (e), (f) on the half grids. -/

#eval (halfGrid 5 2).all (chkReq 2 5) ∧ (halfGrid 3 3).all (chkReq 3 3) ∧
      (halfGrid 2 4).all (chkReq 4 2)
#eval (halfGrid 5 2).all (chkBridge 2 5) ∧ (halfGrid 3 3).all (chkBridge 3 3) ∧
      (halfGrid 2 4).all (chkBridge 4 2)
#eval (halfGrid 5 2).all (chkPos 2 5) ∧ (halfGrid 3 3).all (chkPos 3 3) ∧
      (halfGrid 2 4).all (chkPos 4 2)

/-! ### χ linearity in k (fixed L = 2, half grid): expect χ = 2k. -/

#eval chiStats 1 2 (halfGrid 2 1)
#eval chiStats 2 2 (halfGrid 2 2)
#eval chiStats 3 2 (halfGrid 2 3)
#eval chiStats 4 2 (halfGrid 2 4)

end Attack1
