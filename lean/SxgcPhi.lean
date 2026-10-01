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

/-! ## Criterion C: gcd-one primitivity and the singleton-domain branch

Criterion C (SXI2 v3 [DESIGN.md](../bit6/sxi_logs/sxi2-v3/DESIGN.md)) is the hybrid
safety test `C(i) := (byte-frequency gcd g = 1) OR (cyclic SA domain of run i is a
singleton)`. This section formalizes the two halves that do not require a cyclic
rotation model of the text:

* `gcdOne_not_isPower`: `g = 1` rules out `T = U^m` with `m > 1`. That periodicity
  is exactly the hypothesis under which the order-preserving LF/Phi interval map
  gains equal-rotation ties, so `g = 1` is the certificate the publisher records on
  real corpora (all three retained artifacts have `g = 1`).
* `criterion_singleton_branch`: on a singleton domain the affine Phi formula is
  exact — the single point is the run tail `u`, and the formula returns the
  directly sampled successor `v = phiInv u` (`headFromTail`), with no periodicity
  hypothesis. This is the branch a periodic text falls back to.

The `g = 1` obligation has two bridges: (a) a cyclic-rotation model and the
primitive-implies-distinct-rotations direction (Lyndon--Schützenberger), and (b) order
preservation of the LF map for a tie-free suffix order, which yields the
Nishimoto--Tabei exactness — for run `i` with tail value `u`, head value `v = phiInv R u`
and cyclic SA domain `D_i`, every `x ∈ D_i` satisfies `phiFormula R u v x = phiInv R x`.
Bridge (a) is now proven in the section `Criterion C bridge (a)` below; bridge (b),
with the suffix-array order model it needs, remains statement-locked. This section
introduces no proof hole. -/

/-- Gcd of the byte frequencies over the distinct symbols of `T`. `0` when `T` is empty. -/
def byteFreqGcd (T : Text) : Nat :=
  T.eraseDups.foldl (fun acc c => Nat.gcd acc (T.count c)) 0

theorem foldl_gcd_dvd_acc (a : Nat) (l : List Nat) : l.foldl Nat.gcd a ∣ a := by
  induction l generalizing a with
  | nil => simp
  | cons b t ih =>
      simp only [List.foldl_cons]
      exact Nat.dvd_trans (ih (Nat.gcd a b)) (Nat.gcd_dvd_left a b)

/-- If `m` divides the accumulator and every mapped entry, it divides the mapped gcd fold. -/
theorem dvd_foldl_gcd (m a : Nat) (l : List Nat) (f : Nat → Nat)
    (ha : m ∣ a) (hl : ∀ x ∈ l, m ∣ f x) :
    m ∣ l.foldl (fun acc c => Nat.gcd acc (f c)) a := by
  induction l generalizing a with
  | nil => simpa using ha
  | cons b t ih =>
      simp only [List.foldl_cons]
      exact ih (Nat.gcd a (f b)) (Nat.dvd_gcd ha (hl b (by simp)))
        (fun x hx => hl x (by simp [hx]))

/-- A byte appearing `m` times in a `U`-repeat appears `m * count` times in `U^m`. -/
theorem count_flatten_replicate (U : Text) (m c : Nat) :
    ((List.replicate m U).flatten).count c = m * U.count c := by
  induction m with
  | zero => simp
  | succ k ih =>
      rw [List.replicate_succ, List.flatten_cons, List.count_append, ih,
          Nat.add_mul, Nat.one_mul]
      exact Nat.add_comm _ _

/-- `T` is a proper power `U^m`, `m > 1` (i.e. `T` is not primitive). -/
def IsPower (T : Text) : Prop := ∃ U m, 1 < m ∧ T = (List.replicate m U).flatten

/-- Every byte frequency of a proper power is divisible by the exponent. -/
theorem isPower_dvd_byteFreqGcd {T : Text} (h : IsPower T) :
    ∃ m, 1 < m ∧ m ∣ byteFreqGcd T := by
  obtain ⟨U, m, hm, hT⟩ := h
  refine ⟨m, hm, ?_⟩
  unfold byteFreqGcd
  apply dvd_foldl_gcd m 0 T.eraseDups (fun c => T.count c)
  · exact ⟨0, by simp⟩
  · intro c _
    rw [hT, count_flatten_replicate]
    exact ⟨U.count c, rfl⟩

/-- Criterion C, `g = 1` half at the frequency level: gcd one rules out proper powers,
hence the equal-rotation periodicity the LF-map argument needs to exclude. -/
theorem gcdOne_not_isPower (T : Text) (h : byteFreqGcd T = 1) : ¬ IsPower T := by
  intro hp
  obtain ⟨m, hm, hdvd⟩ := isPower_dvd_byteFreqGcd hp
  rw [h] at hdvd
  have hmle : m ≤ 1 := Nat.le_of_dvd Nat.one_pos hdvd
  omega

/-- The affine Phi interval formula (DESIGN.md): `phi^-1(x) = (v + x - u) mod n`. -/
def phiFormula (R : Text) (u v x : Nat) : Nat := (v + (x - u)) % R.length

/-- Criterion C, singleton half: when a run's cyclic SA domain is the singleton `{u}`,
the affine formula at `u` returns exactly the sampled successor `phiInv u`, with no
periodicity hypothesis. Here `u = rowPos R (a-1)` is the run tail and `v = rowPos R a`
the next run's head. -/
theorem criterion_singleton_branch (R : Text) (a : Nat) (hpos : 0 < a) (ha : a < R.length) :
    phiFormula R (rowPos R (a-1)) (rowPos R a) (rowPos R (a-1))
      = phiInv R (rowPos R (a-1)) := by
  have hv : rowPos R a < R.length := rowPos_lt R a ha
  unfold phiFormula
  rw [Nat.sub_self, Nat.add_zero, Nat.mod_eq_of_lt hv]
  exact headFromTail R a hpos ha

/-! ## Criterion C bridge (a): primitive texts have distinct cyclic rotations

Discharges obligation (a) of the `g = 1` half of Criterion C (see the
`Criterion C` section above and `FLEET_CRITERIONC_REPORT.md`): if
`byteFreqGcd T = 1` then all `T.length` cyclic rotations of `T` are pairwise
distinct.

The route is the classical one, formalized from scratch on `List Nat` with no
imports beyond `Sxgc`:

* `rot T k = T.drop k ++ T.take k` is the cyclic rotation by `k`;
* two equal rotations with distinct shifts force a nontrivial *cyclic-shift
  symmetry* `CyclicSym T d` (`rot_eq_cyclicSym`);
* symmetries are closed under addition, complement and subtraction
  (`cyclicSym_add/neg/sub`, the cyclic-group arithmetic done by hand);
* strong induction on the shift `d` with the Euclidean step `d ↦ n % d`
  (`cyclicSym_sub_mul`) reaches `d ∣ n`, where `T` is literally a power
  (`cyclicSym_isPower`), contradicting `gcdOne_not_isPower`.

Obligation (b) — order preservation of the LF/Phi interval map for the tie-free
suffix order, yielding `phiFormula R u v x = phiInv R x` on a cyclic SA domain —
is *not* addressed here and remains statement-locked: it needs the suffix-array
order model, which `SxgcPhi` deliberately abstracts behind `PieceSound`, and the
r-space spliced-output equivalence. -/

theorem add_self_mod (x n : Nat) : (x + n) % n = x % n := by
  have h : x + n = x + 1 * n := by omega
  rw [h, Nat.add_mul_mod_self_right]

theorem length_flatten_replicate' {α : Type} (U : List α) (m : Nat) :
    (List.replicate m U).flatten.length = m * U.length := by
  induction m with
  | zero => simp
  | succ k ih => rw [List.replicate_succ, List.flatten_cons, List.length_append, ih, Nat.succ_mul]; omega

theorem flatten_replicate_getElem? {α : Type} (U : List α) (m i : Nat) (_hU : 0 < U.length) :
    ((List.replicate m U).flatten)[i]? = if i < m * U.length then U[i % U.length]? else none := by
  induction m generalizing i with
  | zero => simp [List.replicate_zero]
  | succ k ih =>
      rw [List.replicate_succ, List.flatten_cons, List.getElem?_append]
      by_cases hik : i < U.length
      · rw [ite_eq_left hik]
        have hle : U.length ≤ (k + 1) * U.length := by
          have := Nat.mul_le_mul_right U.length (show 1 ≤ k + 1 by omega)
          simpa using this
        rw [ite_eq_left (by omega : i < (k + 1) * U.length), Nat.mod_eq_of_lt hik]
      · rw [ite_eq_right hik]
        have hle : U.length ≤ i := by omega
        by_cases hbig : i < (k + 1) * U.length
        · rw [ite_eq_left hbig]
          have hlt : i - U.length < k * U.length := by
            have hmul : (k + 1) * U.length = U.length + k * U.length := by rw [Nat.succ_mul]; omega
            omega
          rw [ih (i - U.length), ite_eq_left hlt]
          have hmod : (i - U.length) % U.length = i % U.length := by
            calc (i - U.length) % U.length
                = ((i - U.length) + U.length) % U.length := (add_self_mod (i - U.length) U.length).symm
              _ = i % U.length := by rw [Nat.sub_add_cancel hle]
          rw [hmod]
        · rw [ite_eq_right hbig]
          apply List.getElem?_eq_none
          rw [length_flatten_replicate']
          have hmul : (k + 1) * U.length = U.length + k * U.length := by rw [Nat.succ_mul]; omega
          omega

def rot (T : Text) (k : Nat) : Text := T.drop k ++ T.take k

def CyclicSym (T : Text) (r : Nat) : Prop :=
  ∀ x, x < T.length → T[x]? = T[(x + r) % T.length]?

theorem rot_getElem? (T : Text) (k i : Nat) (hk : k ≤ T.length) (hi : i < T.length) :
    (rot T k)[i]? = T[(i + k) % T.length]? := by
  unfold rot
  rw [List.getElem?_append, List.length_drop]
  by_cases h : i < T.length - k
  · rw [ite_eq_left h, List.getElem?_drop]
    have hki : k + i < T.length := by omega
    congr 1
    rw [Nat.add_comm i k, Nat.mod_eq_of_lt hki]
  · rw [ite_eq_right h, List.getElem?_take]
    have hlt : i - (T.length - k) < k := by omega
    rw [ite_eq_left hlt]
    have hidx : i - (T.length - k) = (i + k) % T.length := by
      have hge : T.length ≤ i + k := by omega
      have hmod : (i + k) % T.length = i + k - T.length := by
        calc (i + k) % T.length = (i + k - T.length + T.length) % T.length := by
              rw [Nat.sub_add_cancel hge]
          _ = (i + k - T.length) % T.length := add_self_mod _ _
          _ = i + k - T.length := Nat.mod_eq_of_lt (by omega)
      omega
    rw [hidx]

theorem cyclicSym_length (T : Text) : CyclicSym T T.length := by
  intro x hx
  have h : (x + T.length) % T.length = x := by
    rw [show x + T.length = x + 1 * T.length by omega, Nat.add_mul_mod_self_right,
        Nat.mod_eq_of_lt hx]
  rw [h]

theorem cyclicSym_add (T : Text) (r s : Nat) (hr : CyclicSym T r) (hs : CyclicSym T s) :
    CyclicSym T (r + s) := by
  intro x hx
  have hy : (x + r) % T.length < T.length := Nat.mod_lt _ (by omega)
  calc T[x]? = T[(x + r) % T.length]? := hr x hx
    _ = T[((x + r) % T.length + s) % T.length]? := hs _ hy
    _ = T[(x + (r + s)) % T.length]? := by
        congr 1
        rw [Nat.mod_add_mod]
        congr 1
        omega

theorem cyclicSym_neg (T : Text) (r : Nat) (hr : CyclicSym T r) (hrn : r ≤ T.length) :
    CyclicSym T (T.length - r) := by
  intro x hx
  have hx' : (x + (T.length - r)) % T.length < T.length := Nat.mod_lt _ (by omega)
  have h := hr _ hx'
  have hmod : ((x + (T.length - r)) % T.length + r) % T.length = x := by
    rw [Nat.mod_add_mod]
    have hxr : x + (T.length - r) + r = x + T.length := by omega
    rw [hxr, show x + T.length = x + 1 * T.length by omega,
        Nat.add_mul_mod_self_right, Nat.mod_eq_of_lt hx]
  rw [hmod] at h
  exact h.symm

theorem cyclicSym_sub (T : Text) (a b : Nat) (ha : CyclicSym T a) (hb : CyclicSym T b)
    (hba : b ≤ a) (hbn : b ≤ T.length) : CyclicSym T (a - b) := by
  have h1 : CyclicSym T (a + (T.length - b)) :=
    cyclicSym_add T a (T.length - b) ha (cyclicSym_neg T b hb hbn)
  intro x hx
  have hx1 := h1 x hx
  have heq : (x + (a + (T.length - b))) % T.length = (x + (a - b)) % T.length := by
    have hxa : x + (a + (T.length - b)) = (x + (a - b)) + T.length := by omega
    rw [hxa, show (x + (a - b)) + T.length = (x + (a - b)) + 1 * T.length by omega,
        Nat.add_mul_mod_self_right]
  rwa [heq] at hx1

theorem cyclicSym_mod_eq (T : Text) (d : Nat) (hd : 0 < d) (hsym : CyclicSym T d) (hdle : d ≤ T.length) :
    ∀ x, x < T.length → T[x]? = T[x % d]? := by
  intro x
  induction x using Nat.strongRecOn with
  | ind x ih =>
    intro hx
    by_cases hxd : x < d
    · rw [Nat.mod_eq_of_lt hxd]
    · have hxdlt : x - d < T.length := by omega
      have hxa : x - d + d = x := Nat.sub_add_cancel (by omega : d ≤ x)
      have h1 : T[x - d]? = T[x]? := by
        have h := hsym (x - d) hxdlt
        rw [hxa, Nat.mod_eq_of_lt hx] at h
        exact h
      have hmod : (x - d) % d = x % d := by
        calc (x - d) % d = (x - d + d) % d := (add_self_mod (x - d) d).symm
          _ = x % d := by rw [hxa]
      rw [← h1, ih (x - d) (by omega) hxdlt, hmod]

theorem cyclicSym_sub_mul (T : Text) (d : Nat) (hdle : d ≤ T.length) (hsym : CyclicSym T d) :
    ∀ k, k ≤ T.length / d → CyclicSym T (T.length - k * d) := by
  intro k
  induction k with
  | zero => simpa using cyclicSym_length T
  | succ j ihj =>
    intro hk
    have hj : j ≤ T.length / d := by omega
    have hprev : CyclicSym T (T.length - j * d) := ihj hj
    have hge : d ≤ T.length - j * d := by
      have h1 : (j + 1) * d ≤ T.length := by
        calc (j + 1) * d ≤ (T.length / d) * d := Nat.mul_le_mul_right d (by omega : j + 1 ≤ T.length / d)
          _ ≤ T.length := Nat.div_mul_le_self T.length d
      have h2 : (j + 1) * d = j * d + d := by rw [Nat.succ_mul]
      omega
    have hstep := cyclicSym_sub T (T.length - j * d) d hprev hsym hge hdle
    have heq : T.length - j * d - d = T.length - (j + 1) * d := by rw [Nat.succ_mul]; omega
    rwa [heq] at hstep

theorem cyclicSym_isPower (T : Text) :
    ∀ d, 0 < d → d < T.length → CyclicSym T d → IsPower T := by
  intro d
  induction d using Nat.strongRecOn with
  | ind d ih =>
    intro hd hdn hsym
    have hdle : d ≤ T.length := Nat.le_of_lt hdn
    by_cases hdiv : d ∣ T.length
    · have hnd' : T.length = d * (T.length / d) := by
        have h := Nat.div_add_mod T.length d
        have h0 : T.length % d = 0 := Nat.mod_eq_zero_of_dvd hdiv
        omega
      have hnd : T.length = (T.length / d) * d := hnd'.trans (Nat.mul_comm _ _)
      have hq1 : 1 < T.length / d := by
        have h : d * 1 < d * (T.length / d) := by
          rw [Nat.mul_one, ← hnd']
          exact hdn
        exact (Nat.mul_lt_mul_left hd).mp h
      refine ⟨T.take d, T.length / d, hq1, ?_⟩
      apply List.ext_getElem?
      intro i
      have htake : (T.take d).length = d := by rw [List.length_take, Nat.min_eq_left hdle]
      rw [flatten_replicate_getElem? (T.take d) (T.length / d) i (by rw [htake]; exact hd)]
      rw [htake]
      by_cases hi : i < T.length
      · have hmod := cyclicSym_mod_eq T d hd hsym hdle i hi
        rw [hmod]
        have hib : i < (T.length / d) * d := by omega
        rw [ite_eq_left hib, List.getElem?_take, ite_eq_left (Nat.mod_lt _ hd)]
      · rw [List.getElem?_eq_none (by omega : T.length ≤ i)]
        rw [ite_eq_right (by omega : ¬ i < (T.length / d) * d)]
    · have hrpos : 0 < T.length % d := by
        rcases Nat.eq_zero_or_pos (T.length % d) with h0 | hp
        · exact absurd (Nat.dvd_of_mod_eq_zero h0) hdiv
        · exact hp
      have hsym' : CyclicSym T (T.length % d) := by
        have hk : T.length / d ≤ T.length / d := Nat.le_refl _
        have hsub := cyclicSym_sub_mul T d hdle hsym (T.length / d) hk
        have heq : T.length - T.length / d * d = T.length % d := by
          rw [Nat.mod_eq_sub_mul_div, Nat.mul_comm d (T.length / d)]
        rwa [heq] at hsub
      have hrd : T.length % d < d := Nat.mod_lt _ hd
      have hrn : T.length % d < T.length := by omega
      exact ih (T.length % d) hrd hrpos hrn hsym'

theorem rot_eq_cyclicSym (T : Text) (s t : Nat) (hs : s < T.length) (ht : t < T.length)
    (hst : s ≤ t) (h : rot T s = rot T t) : CyclicSym T (t - s) := by
  intro x hx
  have hsle : s ≤ T.length := Nat.le_of_lt hs
  have htle : t ≤ T.length := Nat.le_of_lt ht
  have hil : (x + T.length - s) % T.length < T.length := Nat.mod_lt _ (by omega)
  have hxi : (((x + T.length - s) % T.length) + s) % T.length = x := by
    rw [Nat.mod_add_mod]
    have h1 : x + T.length - s + s = x + T.length := by omega
    rw [h1, show x + T.length = x + 1 * T.length by omega,
        Nat.add_mul_mod_self_right, Nat.mod_eq_of_lt hx]
  have hxt : (((x + T.length - s) % T.length) + t) % T.length = (x + (t - s)) % T.length := by
    rw [Nat.mod_add_mod]
    have h1 : x + T.length - s + t = x + (t - s) + T.length := by omega
    rw [h1, add_self_mod]
  calc T[x]? = T[(((x + T.length - s) % T.length) + s) % T.length]? := by rw [hxi]
    _ = (rot T s)[(x + T.length - s) % T.length]? :=
        (rot_getElem? T s ((x + T.length - s) % T.length) hsle hil).symm
    _ = (rot T t)[(x + T.length - s) % T.length]? := by rw [h]
    _ = T[(((x + T.length - s) % T.length) + t) % T.length]? :=
        rot_getElem? T t ((x + T.length - s) % T.length) htle hil
    _ = T[(x + (t - s)) % T.length]? := by rw [hxt]

theorem rot_eq_imp_not_gcdOne (T : Text) (s t : Nat) (hs : s < T.length) (ht : t < T.length)
    (hst : s ≠ t) (h : rot T s = rot T t) : byteFreqGcd T ≠ 1 := by
  intro hg
  rcases Nat.lt_or_gt_of_ne hst with hlt | hgt
  · have hsym := rot_eq_cyclicSym T s t hs ht (Nat.le_of_lt hlt) h
    exact gcdOne_not_isPower T hg (cyclicSym_isPower T (t - s) (by omega) (by omega) hsym)
  · have hsym := rot_eq_cyclicSym T t s ht hs (Nat.le_of_lt hgt) h.symm
    exact gcdOne_not_isPower T hg (cyclicSym_isPower T (s - t) (by omega) (by omega) hsym)

theorem byteFreqGcd_one_rot_injective (T : Text) (hg : byteFreqGcd T = 1) :
    ∀ s t, s < T.length → t < T.length → rot T s = rot T t → s = t := by
  intro s t hs ht h
  by_cases hne : s = t
  · exact hne
  · exact absurd hg (rot_eq_imp_not_gcdOne T s t hs ht hne h)

end SxgcPhi

