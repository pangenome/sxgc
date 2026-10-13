import Sxgc

/-!
# Permutation inversion through a cyclic stream

Space is in words (values ≤ n), plus O(1) scalar control registers. Output
is write-only. Oracle input is TEXT-order row numbers; output is their
inverse in RANK order. This is a sequential cyclic-stream model, not random
SA access. Period discovery and ordering p representatives are preprocessing;
O(1) periodic emission means constant word-RAM arithmetic after that work.
-/
namespace Sxgc.PiWall

structure Permutation (n : Nat) where
  forward : Fin n → Fin n
  backward : Fin n → Fin n
  left_inv : ∀ i, backward (forward i) = i
  right_inv : ∀ i, forward (backward i) = i

def inversionTask {n : Nat} (π : Permutation n) : List Nat :=
  (List.finRange n).map (fun k => (π.backward k).val)

/-- Retained payload; scalar cursor, output counter, program counter excluded. -/
structure Store (n S : Nat) where
  words : List (Fin (n + 1))
  bounded : words.length ≤ S

def space {n S : Nat} (s : Store n S) : Nat := s.words.length

inductive Action (n : Nat) where
  | advance
  | emit (position : Fin n)
  | halt

/-- Code fixed independently of π; only current stream item visible.
Advancing costs one oracle step; wrapping allowed, jumping forbidden. -/
structure Enumerator (n S : Nat) where
  initial : Store n S
  transition : Fin 16 → Fin n → Fin (n + 1) → Store n S → Fin n →
    Fin 16 × Store n S × Action n

/-- Accumulated output is never passed to transition. Fuel is a termination
witness, NOT visible working state. The output counter saturates at n. -/
def run {n S : Nat} (π : Permutation n) (A : Enumerator n S)
    (fuel : Nat) (pc : Fin 16) (cursor : Fin n) (k : Nat)
    (s : Store n S) : List Nat × Nat :=
  match fuel with
  | 0 => ([], 0)
  | fuel + 1 =>
    let (pc', s', action) := A.transition pc cursor ⟨min k n, by omega⟩ s (π.forward cursor)
    match action with
    | .halt => ([], 0)
    | .advance =>
      let next : Fin n := ⟨(cursor.val + 1) % n, Nat.mod_lt _ (by have := cursor.isLt; omega)⟩
      let (out, t) := run π A fuel pc' next k s'
      (out, t + 1)
    | .emit x =>
      let (out, t) := run π A fuel pc' cursor (k + 1) s'
      (x.val :: out, t)

def Correct {n S : Nat} (A : Enumerator n S) : Prop :=
  ∀ π : Permutation n, ∀ start : Fin n, ∃ fuel,
    (run π A fuel 0 start 0 A.initial).1 = inversionTask π

def WorstCostAtLeast {n S : Nat} (A : Enumerator n S) (bound : Nat) : Prop :=
  ∃ π : Permutation n, ∃ start : Fin n, ∀ fuel,
    (run π A fuel 0 start 0 A.initial).1 = inversionTask π →
    bound ≤ (run π A fuel 0 start 0 A.initial).2

/-! ## EXPLICIT AXIOM BLOCK — model-transfer assumption

Motivation: Borodin, Fischer, Kirkpatrick, Lynch, **Tompa**,
“A time-space tradeoff for sorting on non-oblivious machines”, JCSS 22
(1981), 351–364, DOI 10.1016/0022-0000(81)90037-4;
https://groups.csail.mit.edu/tds/papers/Lynch/jcss81-bfklt.pdf.
Munro and Paterson, “Selection and sorting with limited storage”, TCS 12
(1980), 315–323, DOI 10.1016/0304-3975(80)90061-4;
https://wrap.warwick.ac.uk/id/eprint/46321/.

Those citations DO NOT prove this exact numerical cyclic-oracle statement.
This is an explicitly trusted transfer assumption requiring a separate proof
or reduction: comparison-sorting bounds cannot silently be applied to
numeric permutations. Constants 64 fix an asymptotic convention; S excludes
O(1) control words. No claim about compressed/text subclasses follows.
-/
axiom cycle_inversion_wall (n S : Nat) (hn : 64 ≤ n)
    (hS : 1 ≤ S) (hSn : S ≤ n) (A : Enumerator n S) (hA : Correct A) :
    WorstCostAtLeast A (n * n / (64 * S))
/-! ## END AXIOM BLOCK -/

/-- Conditional quadratic lower endpoint; axiom dependence explicit. -/
theorem wall_one (n : Nat) (hn : 64 ≤ n) (A : Enumerator n 1) (hA : Correct A) :
    WorstCostAtLeast A (n * n / 64) := by
  simpa using cycle_inversion_wall n 1 hn (by omega) (by omega) A hA

/-- Whole cyclic stream starting at text position zero. -/
def cycleScan {n : Nat} (π : Permutation n) : List (Nat × Nat) :=
  (List.finRange n).map (fun i => (i.val, (π.forward i).val))

/-- Restart a scan for each rank, retaining only a candidate. Lists specify
the trace; an implementation can stream them without materializing them. -/
def scanInverse {n : Nat} (π : Permutation n) : List Nat :=
  (List.finRange n).map (fun k =>
    (((cycleScan π).find? (fun item => item.2 == k.val)).map Prod.fst).getD 0)

/-- Dense scatter of one scan into n cells. Array writes have unit RAM cost. -/
def denseInverse {n : Nat} (π : Permutation n) : Array Nat :=
  (cycleScan π).foldl (fun a item => a.set! item.2 item.1) (Array.replicate n 0)

/-- Conservative query budgets: n full scans versus one. Counts observations
including the first item; `run` charges advances instead. -/
def scalarSteps (n : Nat) : Nat := n * n
def denseSteps (n : Nat) : Nat := n

theorem scalar_quadratic (n : Nat) : scalarSteps n = n * n := rfl
theorem dense_linear (n : Nat) : denseSteps n = n := rfl
theorem dense_space (n : Nat) : (Array.replicate n (0 : Nat)).size = n := by simp

/-! ## Periodic break: no use of the wall axiom

A cyclic text is its n-periodic infinite extension. A minimal cyclic period
is a positive preserving shift with no smaller positive preserving shift.
Equal rotations compare the first n symbols (`finite_iff` bridges the
extension definition). The residue classification is proved from minimality,
not assumed in a certificate.
-/
structure CyclicText where
  n : Nat
  positive : 0 < n
  symbol : Nat → Nat
  wraps : ∀ i, symbol (i + n) = symbol i

def ShiftPeriod (T : CyclicText) (d : Nat) : Prop := ∀ i, T.symbol (i + d) = T.symbol i

def MinimalPeriod (T : CyclicText) (p : Nat) : Prop :=
  0 < p ∧ ShiftPeriod T p ∧ ∀ d, 0 < d → ShiftPeriod T d → p ≤ d

def EqualRotation (T : CyclicText) (i j : Nat) : Prop :=
  ∀ k, T.symbol (i + k) = T.symbol (j + k)

/-- Reduce extension index modulo any positive period. -/
theorem at_mod (T : CyclicText) {p : Nat} (hp : 0 < p) (h : ShiftPeriod T p)
    (i : Nat) : T.symbol i = T.symbol (i % p) := by
  have many : ∀ a m, T.symbol (a + m * p) = T.symbol a := by
    intro a m
    induction m with
    | zero => simp
    | succ m ih => rw [Nat.succ_mul, ← Nat.add_assoc, h, ih]
  have he : i % p + (i / p) * p = i := by
    simpa [Nat.mul_comm] using Nat.mod_add_div i p
  calc T.symbol i = T.symbol (i % p + (i / p) * p) := congrArg T.symbol he.symm
       _ = T.symbol (i % p) := many (i % p) (i / p)

/-- Equal rotations yield a preserving shift equal to their nonnegative gap. -/
theorem gap_period (T : CyclicText) {i j : Nat} (hij : i ≤ j)
    (h : EqualRotation T i j) : ShiftPeriod T (j - i) := by
  intro k
  let q := i * T.n
  have hq : i ≤ q := by
    dsimp [q]
    have he := Nat.mul_le_mul_left i (show 1 ≤ T.n from T.positive)
    simpa using he
  have wrapMany : ∀ a m, T.symbol (a + m * T.n) = T.symbol a := by
    intro a m
    induction m with
    | zero => simp
    | succ m ih =>
      rw [Nat.succ_mul, ← Nat.add_assoc, T.wraps, ih]
  have e := h (k + q - i)
  have ei : i + (k + q - i) = k + q := by omega
  have ej : j + (k + q - i) = (k + (j - i)) + q := by omega
  rw [ei, ej] at e
  rw [wrapMany k i, wrapMany (k + (j - i)) i] at e
  exact e.symm

/-- Euclidean remainder of a preserving shift is preserving. -/
theorem period_mod (T : CyclicText) {p d : Nat} (hp : 0 < p)
    (h : ShiftPeriod T p) (hd : ShiftPeriod T d) : ShiftPeriod T (d % p) := by
  intro k
  have e := hd k
  rw [at_mod T hp h (k + d), at_mod T hp h k] at e
  rw [at_mod T hp h (k + d % p), at_mod T hp h k]
  simpa [Nat.add_mod] using e

/-- Minimality forces every preserving shift to be divisible by p. -/
theorem period_dvd (T : CyclicText) {p d : Nat} (h : MinimalPeriod T p)
    (hd : ShiftPeriod T d) : d % p = 0 := by
  have hr := Nat.mod_lt d h.1
  by_cases hz : d % p = 0
  · exact hz
  · have := h.2.2 (d % p) (by omega) (period_mod T h.1 h.2.1 hd)
    omega

/-- THE CLASSIFICATION: equal rotations exactly equal residues. -/
theorem equalRotation_iff_residue (T : CyclicText) {p : Nat}
    (h : MinimalPeriod T p) (i j : Nat) :
    EqualRotation T i j ↔ i % p = j % p := by
  constructor
  · intro he
    by_cases hij : i ≤ j
    · have hz := period_dvd T h (gap_period T hij he)
      have hj : j = i + (j - i) := by omega
      rw [hj, Nat.add_mod, hz, Nat.add_zero, Nat.mod_mod]
    · have he' : EqualRotation T j i := fun k => (he k).symm
      have hz := period_dvd T h (gap_period T (by omega) he')
      have hi : i = j + (i - j) := by omega
      rw [hi, Nat.add_mod, hz, Nat.add_zero, Nat.mod_mod]
  · intro hr k
    rw [at_mod T h.1 h.2.1 (i + k), at_mod T h.1 h.2.1 (j + k)]
    congr 1
    rw [Nat.add_mod i k p, Nat.add_mod j k p, hr]

/-- A finite comparison suffices for equality of infinite rotations. -/
theorem finite_iff (T : CyclicText) (i j : Nat) :
    (∀ k, k < T.n → T.symbol (i + k) = T.symbol (j + k)) ↔ EqualRotation T i j := by
  constructor
  · intro h k
    have hi := at_mod T T.positive T.wraps (i + k)
    have hj := at_mod T T.positive T.wraps (j + k)
    have hi' := at_mod T T.positive T.wraps (i + k % T.n)
    have hj' := at_mod T T.positive T.wraps (j + k % T.n)
    rw [hi, hj]
    have e := h (k % T.n) (Nat.mod_lt _ T.positive)
    rw [hi', hj'] at e
    simpa [Nat.add_mod] using e
  · intro h k _; exact h k

theorem minimalPeriod_divides_length (T : CyclicText) {p : Nat}
    (h : MinimalPeriod T p) : T.n % p = 0 ∧ p ≤ T.n :=
  ⟨period_dvd T h T.wraps, h.2.2 T.n T.positive T.wraps⟩

/-- Constant arithmetic per row after the p residue representatives are ordered.
An array of p residues occupies p WORDS, not p bits. Space is O(p) words,
or O(p log n + log n) bits; O(log n + p) with p measured in bits needs an
additional compressed representation theorem and is not claimed here. -/
def emitRow (p copies : Nat) (residues : Array Nat) (row : Nat) : Nat :=
  residues[row / copies]! + (row % copies) * p

/-- C++ ascending-position tie convention. -/
def emitClass (p copies residue : Nat) : List Nat :=
  (List.range copies).map (fun j => residue + j * p)

theorem emitted_residue {p copies residue j : Nat} (hp : 0 < p) (hr : residue < p)
    (_hj : j < copies) : (residue + j * p) % p = residue := by
  simp [Nat.add_mod, Nat.mod_eq_of_lt hr]

theorem emitted_ascending {p residue j k : Nat} (hp : 0 < p) (hjk : j < k) :
    residue + j * p < residue + k * p := by
  exact Nat.add_lt_add_left (Nat.mul_lt_mul_of_pos_right hjk hp) residue

theorem emitted_complete (p i : Nat) : i % p + (i / p) * p = i := by
  have := Nat.mod_add_div i p
  simpa [Nat.mul_comm] using this

theorem emitted_class_iff {p copies residue i : Nat} (hp : 0 < p)
    (hr : residue < p) :
    i ∈ emitClass p copies residue ↔ i < copies * p ∧ i % p = residue := by
  simp only [emitClass, List.mem_map, List.mem_range]
  constructor
  · rintro ⟨j, hj, rfl⟩
    constructor
    · have := Nat.mul_le_mul_right p (show j + 1 ≤ copies by omega)
      rw [Nat.add_mul] at this
      omega
    · exact emitted_residue hp hr hj
  · rintro ⟨hi, hm⟩
    refine ⟨i / p, ?_, ?_⟩
    · exact (Nat.div_lt_iff_lt_mul hp).2 hi
    · rw [← hm]; exact emitted_complete p i

theorem no_repair_when_aperiodic (T : CyclicText) (h : MinimalPeriod T T.n)
    {i j : Nat} (hi : i < T.n) (hj : j < T.n) : EqualRotation T i j ↔ i = j := by
  simpa [Nat.mod_eq_of_lt hi, Nat.mod_eq_of_lt hj] using
    equalRotation_iff_residue T h i j

end Sxgc.PiWall
