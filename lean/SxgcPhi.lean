import SxgcBuild

/-!
# SA predecessor and run-head extraction

Positions and rows are zero based, on the *indexed* text R (typically
T.reverse ++ [0]). Phi is cyclic: the predecessor of SA row zero is the
last row. This is a permutation, unlike the partial/noncyclic `phiOf` used
for PLCP in Sxgc. No positivity or unique-sentinel assumption is needed.

Statement correspondence to the interval-image extractor: a run occupies
consecutive SA rows [a,b]. For every run except the first, the preceding
run's tail is row a-1, so its unmirrored tail sample is SA[a-1]. The
extractor looks up that position among sorted Phi interval images, obtains
Phi^-1(SA[a-1]), and writes SA[a]. `headFromTail` proves exactly this
identity. A mirrored .ri4 sample must first be decoded as (n-1)-sample;
the first head SA[0] follows from the cyclic last-row case proved below
(or may be supplied separately). File decoding, interval-table
provenance, machine arithmetic and the compiled search are separate bridges.
-/

open Sxgc SxgcBuild
namespace SxgcPhi

def rowPos (R : Text) (a : Nat) : Nat := (saOrder R).getD a 0
def saRank (R : Text) (p : Nat) : Nat := (saOrder R).idxOf p
def prevRow (n a : Nat) : Nat := if a = 0 then n - 1 else a - 1
def nextRow (n a : Nat) : Nat := if a + 1 < n then a + 1 else 0
def phi (R : Text) (p : Nat) : Nat := rowPos R (prevRow R.length (saRank R p))
def phiInv (R : Text) (p : Nat) : Nat := rowPos R (nextRow R.length (saRank R p))

theorem rank_lt (R : Text) (p : Nat) (hp : p < R.length) :
    saRank R p < R.length := by
  have := List.idxOf_lt_length_of_mem (saOrder_mem R p hp)
  simpa [saRank, saOrder_length] using this

theorem rowPos_lt (R : Text) (a : Nat) (ha : a < R.length) :
    rowPos R a < R.length := by
  exact saOrder_lt R _ (getD_mem _ a 0 (by simpa [saOrder_length] using ha))

theorem rowPos_rank (R : Text) (p : Nat) (hp : p < R.length) :
    rowPos R (saRank R p) = p := by
  unfold rowPos saRank
  rw [getD_lt_getElem _ _ (by simpa [saRank, saOrder_length] using rank_lt R p hp)]
  exact List.getElem_idxOf _

theorem rank_rowPos (R : Text) (a : Nat) (ha : a < R.length) :
    saRank R (rowPos R a) = a := by
  unfold saRank rowPos
  rw [getD_lt_getElem _ _ (by simpa [saOrder_length] using ha)]
  exact (saOrder_nodup R).idxOf_getElem _ _

theorem prevRow_lt (n a : Nat) (ha : a < n) : prevRow n a < n := by
  unfold prevRow; split <;> omega

theorem nextRow_lt (n a : Nat) (ha : a < n) : nextRow n a < n := by
  unfold nextRow; split <;> omega

theorem next_prev (n a : Nat) (ha : a < n) : nextRow n (prevRow n a) = a := by
  unfold prevRow nextRow
  split <;> split <;> omega

theorem prev_next (n a : Nat) (ha : a < n) : prevRow n (nextRow n a) = a := by
  unfold nextRow prevRow
  split <;> split <;> omega

/-- Row adjacency is precisely the condition needed by head extraction. -/
theorem headFromTail (R : Text) (a : Nat) (hpos : 0 < a) (ha : a < R.length) :
    rowPos R a = phiInv R (rowPos R (a - 1)) := by
  unfold phiInv
  rw [rank_rowPos R (a-1) (by omega)]
  have hn : nextRow R.length (a-1) = a := by
    unfold nextRow; split <;> omega
  rw [hn]

def bwtRow (R : Text) (a : Nat) : Nat :=
  let p := rowPos R a
  if p = 0 then 0 else R.getD (p-1) 0

/-- A nonfirst BWT-run head: its preceding row ends the preceding run. -/
def RunBoundary (R : Text) (a : Nat) : Prop :=
  0 < a ∧ a < R.length ∧ bwtRow R (a-1) ≠ bwtRow R a

/-- Explicit sample contract for adjacent run tails/heads (all run lengths). -/
theorem runHeadFromTail (R : Text) (a tailSample headSample : Nat)
    (hb : RunBoundary R a) (ht : tailSample = rowPos R (a-1))
    (hh : headSample = rowPos R a) : headSample = phiInv R tailSample := by
  rw [ht, hh]
  exact headFromTail R a hb.1 hb.2.1

theorem phiInv_phi (R : Text) (p : Nat) (hp : p < R.length) :
    phiInv R (phi R p) = p := by
  unfold phiInv phi
  rw [rank_rowPos R _ (prevRow_lt _ _ (rank_lt R p hp)),
      next_prev _ _ (rank_lt R p hp), rowPos_rank R p hp]

theorem phi_phiInv (R : Text) (p : Nat) (hp : p < R.length) :
    phi R (phiInv R p) = p := by
  unfold phiInv phi
  rw [rank_rowPos R _ (nextRow_lt _ _ (rank_lt R p hp)),
      prev_next _ _ (rank_lt R p hp), rowPos_rank R p hp]

/-- Includes the first head: its cyclic predecessor is the last run's tail. -/
theorem headFromCyclicTail (R : Text) (a : Nat) (ha : a < R.length) :
    rowPos R a = phiInv R (rowPos R (prevRow R.length a)) := by
  unfold phiInv
  rw [rank_rowPos R _ (prevRow_lt _ _ ha), next_prev _ _ ha]

/-! ## Interval image semantics (list-level search contract)

The extractor represents each affine Phi piece by (dest, start, len), sorts
by dest, and searches the half-open image containing q. `imageLookup` is a
linear list specification with the same early-stop/half-open decisions.
The proof below does NOT claim to verify C++ binary search or its cost.
`piece_inverse` is independent of the search implementation: any search
returning a containing piece can use it. `PieceSound` is the explicit bridge
from the decoded move table to the suffix-array Phi permutation.
-/

structure ImagePiece where
  dest : Nat
  start : Nat
  len : Nat
  deriving Repr, BEq

def Contains (e : ImagePiece) (q : Nat) : Prop := e.dest ≤ q ∧ q < e.dest + e.len

def PieceSound (R : Text) (e : ImagePiece) : Prop :=
  ∀ k, k < e.len → e.start+k < R.length ∧ phi R (e.start+k) = e.dest+k

theorem piece_inverse (R : Text) (e : ImagePiece) (q : Nat)
    (hs : PieceSound R e) (hq : Contains e q) :
    e.start + (q-e.dest) = phiInv R q := by
  obtain ⟨hl, hu⟩ := hq
  obtain ⟨hp, he⟩ := hs (q-e.dest) (by omega)
  have hsum : e.dest + (q-e.dest) = q := by omega
  rw [hsum] at he
  calc
    e.start + (q-e.dest) = phiInv R (phi R (e.start + (q-e.dest))) :=
      (phiInv_phi R _ hp).symm
    _ = phiInv R q := congrArg (phiInv R) he

def imageLookup : List ImagePiece → Nat → Option Nat
  | [], _ => none
  | e :: es, q =>
      if q < e.dest then none
      else if q < e.dest + e.len then some (e.start + (q-e.dest))
      else imageLookup es q

theorem imageLookup_sound (R : Text) (es : List ImagePiece) (q p : Nat)
    (hs : ∀ e ∈ es, PieceSound R e) (h : imageLookup es q = some p) :
    p = phiInv R q := by
  induction es with
  | nil => simp [imageLookup] at h
  | cons e es ih =>
      simp only [imageLookup] at h
      split at h
      · contradiction
      · rename_i hlow
        split at h
        · rename_i hhigh
          have hp : e.start + (q-e.dest) = p := Option.some.inj h
          rw [← hp]
          exact piece_inverse R e q (hs e (by simp)) ⟨by omega, hhigh⟩
        · exact ih (fun f hf => hs f (by simp [hf])) h

theorem imageLookup_complete (es : List ImagePiece) (q : Nat)
    (horder : es.Pairwise (fun e f => e.dest ≤ f.dest))
    (hcover : ∃ e ∈ es, Contains e q) :
    ∃ p, imageLookup es q = some p := by
  induction es with
  | nil => simp at hcover
  | cons e es ih =>
      obtain ⟨hfirst, hrest⟩ := List.pairwise_cons.mp horder
      have hlow : e.dest ≤ q := by
        obtain ⟨f, hf, hl, _⟩ := hcover
        rcases List.mem_cons.mp hf with he | hf
        · subst f; exact hl
        · exact Nat.le_trans (hfirst f hf) hl
      simp only [imageLookup, ite_eq_right (by omega : ¬ q < e.dest)]
      by_cases hhigh : q < e.dest + e.len
      · rw [ite_eq_left hhigh]; exact ⟨_, rfl⟩
      · rw [ite_eq_right hhigh]
        apply ih hrest
        obtain ⟨f, hf, hq⟩ := hcover
        refine ⟨f, ?_, hq⟩
        rcases List.mem_cons.mp hf with he | hf
        · subst f; exact False.elim (hhigh hq.2)
        · exact hf

/-- A sorted, covering, sound interval-image index answers Phi inverse. -/
theorem imageLookup_eq_phiInv (R : Text) (es : List ImagePiece) (q : Nat)
    (hs : ∀ e ∈ es, PieceSound R e)
    (horder : es.Pairwise (fun e f => e.dest ≤ f.dest))
    (hcover : ∃ e ∈ es, Contains e q) :
    imageLookup es q = some (phiInv R q) := by
  obtain ⟨p, hp⟩ := imageLookup_complete es q horder hcover
  rw [imageLookup_sound R es q p hs hp] at hp
  exact hp

/-- The list-index extractor yields the head, including the cyclic first run. -/
theorem extractHead_correct (R : Text) (es : List ImagePiece) (a : Nat)
    (ha : a < R.length) (hs : ∀ e ∈ es, PieceSound R e)
    (horder : es.Pairwise (fun e f => e.dest ≤ f.dest))
    (hcover : ∃ e ∈ es, Contains e (rowPos R (prevRow R.length a))) :
    imageLookup es (rowPos R (prevRow R.length a)) = some (rowPos R a) := by
  rw [imageLookup_eq_phiInv R es _ hs horder hcover,
      ← headFromCyclicTail R a ha]

/-- .ri4's (n-1)-position representation is an involution on valid samples. -/
theorem unmirror_tail (n p : Nat) (hp : p < n) :
    n-1-(n-1-p) = p := by omega

/-! ### HANDOFF
All claims are over Nat/Lean lists. No serialized move-table refinement or
compiled binary-loop proof is asserted. Sorted-image lookup is proved at
the list-function level; source-table PieceSound and coverage must be
established by a decoder/refinement layer. Sorting cost is outside scope.
See PhiEval.lean, PhiAxioms.lean and PHI_ACCEPTANCE.md for gates.
-/

end SxgcPhi
