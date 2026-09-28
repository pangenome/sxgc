import Sxgc

/-! # Seam repair: the identity-outside-classes half

Formalization of the seam-repair theorem of
`bit6/sxi_logs/sep-convention/repair2/DERIVATION.md`.

Model.  `suffixAt T i` is the finite suffix of `T` at 0-based position `i`
(the row content of the padded frame after removing the dollar rows: finite
suffix order, shorter-prefix-first, i.e. dollar below every corpus byte).
`rotAt T i` is the cyclic rotation of `T` starting at `i` (the canonical
cyclic order of the 0x1E corpus contract).

The padded frame's finite suffix order and the canonical cyclic rotation
order can disagree ONLY on rows whose suffixes are prefix-comparable
(one is a prefix of the other): everywhere else the first mismatch decides
both orders identically.  These prefix-comparable rows are exactly the
seam rows; the seam classes are their partition by shared prefix.  The
repair reorders rows only inside their classes and leaves every other row
untouched; the theorems below prove that order-stability (the
identity-outside-classes half of the seam-repair theorem).

The reordering-inside-classes half (sorting by exact cyclic LCE, ties by
increasing position, equals the independent cyclic oracle) additionally
needs the r-space sampling model (Phi anchors, run splicing) and is
statement-locked below as an open proposition with its measured warrant.
-/

namespace SxgcSeam

open Sxgc

/-- Lexicographic order on byte lists, in first-difference form:
the lists share the prefix `pre` and first differ at bytes `a < b`. -/
def lexLt (l m : List Nat) : Prop :=
  ∃ (pre : List Nat) (a b : Nat) (l' m' : List Nat),
    l = pre ++ (a :: l') ∧ m = pre ++ (b :: m') ∧ a < b

/-- Finite suffix at 0-based position `i`. -/
def suffixAt (T : Text) (i : Nat) : Text := T.drop i

/-- Cyclic rotation of `T` starting at 0-based position `i`. -/
def rotAt (T : Text) (i : Nat) : Text := T.drop i ++ T.take i

/-- First-difference decomposition of two prefix-incomparable lists. -/
theorem firstDiff (l m : List Nat)
    (h1 : ¬ (l <+: m)) (h2 : ¬ (m <+: l)) :
    ∃ (pre : List Nat) (a b : Nat) (l' m' : List Nat),
      l = pre ++ (a :: l') ∧ m = pre ++ (b :: m') ∧ a ≠ b := by
  induction l generalizing m with
  | nil =>
    exact absurd ⟨m, by simp⟩ h1
  | cons a l' ih =>
    cases m with
    | nil => exact absurd ⟨a :: l', by simp⟩ h2
    | cons b m' =>
      by_cases hab : a = b
      · subst hab
        obtain ⟨pre, x, y, l'', m'', hl, hm, hxy⟩ := ih m'
          (by intro hp
              obtain ⟨t, ht⟩ := hp
              exact h1 ⟨t, by simp [ht]⟩)
          (by intro hp
              obtain ⟨t, ht⟩ := hp
              exact h2 ⟨t, by simp [ht]⟩)
        refine ⟨a :: pre, x, y, l'', m'', ?_, ?_, hxy⟩
        · exact congrArg (List.cons a) hl
        · exact congrArg (List.cons a) hm
      · exact ⟨[], a, b, l', m', by simp, by simp, hab⟩

/-- Uniqueness of the first-difference witness: for lists in
first-difference shape, `lexLt` is decided exactly by the differing bytes. -/
theorem lexLt_shape (pre : List Nat) (a b : Nat) (l' m' : List Nat) (hab : a ≠ b) :
    lexLt (pre ++ (a :: l')) (pre ++ (b :: m')) ↔ a < b := by
  induction pre generalizing a b l' m' with
  | nil =>
    constructor
    · intro ⟨pre', a', b', l'', m'', hl, hm, hlt⟩
      cases pre' with
      | nil =>
        simp only [List.nil_append] at hl hm
        injection hl with hl1 _
        injection hm with hm1 _
        subst hl1; subst hm1
        exact hlt
      | cons x X =>
        simp only [List.nil_append] at hl hm
        injection hl with hl1 _
        injection hm with hm1 _
        omega
    · intro hlt
      exact ⟨[], a, b, l', m', by simp, by simp, hlt⟩
  | cons x pre ih =>
    constructor
    · intro ⟨pre', a', b', l'', m'', hl, hm, hlt⟩
      cases pre' with
      | nil =>
        simp only [List.cons_append] at hl hm
        injection hl with hl1 _
        injection hm with hm1 _
        omega
      | cons y Y =>
        simp only [List.cons_append] at hl hm
        injection hl with _ hl2
        injection hm with _ hm2
        exact (ih a b l' m' hab).mp ⟨Y, a', b', l'', m'', hl2, hm2, hlt⟩
    · intro hlt
      refine ⟨x :: pre, a, b, l', m', ?_, ?_, hlt⟩
      · rfl
      · rfl

/-- **Order agreement on prefix-incomparable rows.**  If neither suffix is a
prefix of the other, the comparison is decided by a mismatch strictly inside
both suffixes, and that same mismatch decides the comparison of the cyclic
rotations.  Hence finite suffix order and cyclic rotation order agree on
every such pair. -/
theorem rot_agree (T : Text) (i j : Nat)
    (h1 : ¬ (suffixAt T i <+: suffixAt T j))
    (h2 : ¬ (suffixAt T j <+: suffixAt T i)) :
    (lexLt (suffixAt T i) (suffixAt T j) ↔ lexLt (rotAt T i) (rotAt T j)) := by
  obtain ⟨pre, a, b, l', m', hl, hm, hab⟩ := firstDiff (suffixAt T i) (suffixAt T j) h1 h2
  have hkey : lexLt (suffixAt T i) (suffixAt T j) ↔ a < b := by
    rw [hl, hm]
    exact lexLt_shape pre a b l' m' hab
  have hrot : ∀ (T : Text) (i : Nat), rotAt T i = suffixAt T i ++ T.take i := fun _ _ => rfl
  have hri : rotAt T i = pre ++ (a :: (l' ++ T.take i)) := by
    rw [hrot, hl]; simp
  have hrj : rotAt T j = pre ++ (b :: (m' ++ T.take j)) := by
    rw [hrot, hm]; simp
  have hkey2 : lexLt (rotAt T i) (rotAt T j) ↔ a < b := by
    rw [hri, hrj]
    exact lexLt_shape pre a b _ _ hab
  rw [hkey, hkey2]

/-- **Seam position**: suffix `i` is prefix-comparable with some other
suffix `j ≠ i`.  These are exactly the rows the repair may touch. -/
def seamPos (T : Text) (i : Nat) : Prop :=
  ∃ j, i ≠ j ∧ ((suffixAt T i <+: suffixAt T j) ∨ (suffixAt T j <+: suffixAt T i))

/-- **Identity outside classes.**  For two non-seam rows the finite suffix
order and the cyclic rotation order agree: the repair leaves their relative
order untouched.  (DERIVATION.md: "Every comparison outside the union
mismatches before either suffix ends and retains its order.") -/
theorem identity_outside_classes (T : Text) (i j : Nat) (hij : i ≠ j)
    (hi : ¬ seamPos T i) (hj : ¬ seamPos T j) :
    (lexLt (suffixAt T i) (suffixAt T j) ↔ lexLt (rotAt T i) (rotAt T j)) := by
  refine rot_agree T i j ?_ ?_
  · intro hp; exact hi ⟨j, hij, Or.inl hp⟩
  · intro hp; exact hj ⟨i, hij.symm, Or.inl hp⟩

/-- **A non-seam row cannot be crossed by any class reordering.**  A
non-seam row `l` is prefix-incomparable with *every* other row, seam or
not; hence its comparison against any row `c` is order-stable.  This is
the formal content of "every rotation in a class still starts with that
class's prefix; therefore it cannot move outside the class": reordering
inside a class can never displace a row across a non-seam row. -/
theorem agreement_with_nonseam (T : Text) (l c : Nat) (hcl : c ≠ l)
    (hl : ¬ seamPos T l) :
    (lexLt (suffixAt T l) (suffixAt T c) ↔ lexLt (rotAt T l) (rotAt T c)) := by
  refine rot_agree T l c ?_ ?_
  · intro hp; exact hl ⟨c, Ne.symm hcl, Or.inl hp⟩
  · intro hp; exact hl ⟨c, Ne.symm hcl, Or.inr hp⟩

/-! ### Totality of the cyclic comparison, and statement-locked open propositions -/

/-- Lexicographic order is total on equal-length byte lists. -/
theorem lexLt_total_same_length : ∀ (l m : List Nat), l.length = m.length →
    lexLt l m ∨ lexLt m l ∨ l = m := by
  intro l m
  induction l generalizing m with
  | nil =>
    cases m with
    | nil => intro _; exact Or.inr (Or.inr rfl)
    | cons c m' => intro h; simp at h
  | cons a l' ih =>
    cases m with
    | nil => intro h; simp at h
    | cons b m' =>
      intro h
      by_cases hab : a = b
      · subst hab
        rcases ih m' (by simp at h; omega) with h1 | h1 | h1
        · obtain ⟨pre, x, y, L, M, hl, hm, hxy⟩ := h1
          exact Or.inl ⟨a :: pre, x, y, L, M, by simp [hl], by simp [hm], hxy⟩
        · obtain ⟨pre, x, y, L, M, hl, hm, hxy⟩ := h1
          exact Or.inr (Or.inl ⟨a :: pre, x, y, L, M, by simp [hl], by simp [hm], hxy⟩)
        · exact Or.inr (Or.inr (by rw [h1]))
      · rcases Nat.lt_or_ge a b with hlt | hge
        · exact Or.inl ⟨[], a, b, l', m', rfl, rfl, hlt⟩
        · have hb : b < a := by omega
          exact Or.inr (Or.inl ⟨[], b, a, m', l', rfl, rfl, hb⟩)

/-- Rotations have the length of the text. -/
theorem rotAt_length (T : Text) (i : Nat) :
    (rotAt T i).length = T.length := by
  simp only [rotAt]
  rw [List.length_append, List.length_drop, List.length_take]
  omega

/-- **Cyclic comparison totality.**  For two distinct positions the cyclic
comparison `cycCmp` (rotation order, complete-rotation ties by increasing
position) decides one way or the other. -/
def cycCmp (T : Text) (i j : Nat) : Prop :=
  lexLt (rotAt T i) (rotAt T j) ∨ (rotAt T i = rotAt T j ∧ i < j)

theorem rotation_cmp_total (T : Text) (i j : Nat) (hij : i ≠ j) :
    cycCmp T i j ∨ cycCmp T j i := by
  have hl : (rotAt T i).length = (rotAt T j).length := by
    rw [rotAt_length, rotAt_length]
  rcases lexLt_total_same_length (rotAt T i) (rotAt T j) hl with h | h | h
  · exact Or.inl (Or.inl h)
  · exact Or.inr (Or.inl h)
  · rcases Nat.lt_or_ge i j with hlt | hge
    · exact Or.inl (Or.inr ⟨h, hlt⟩)
    · exact Or.inr (Or.inr ⟨h.symm, by omega⟩)

/-! ### Statement-locked open propositions -/

/-- **Reordering inside classes (open).**  On the rows of one seam class
(suffixes pairwise prefix-comparable), the repair sorts by exact cyclic
LCE with complete-rotation ties by increasing starting position; the
result must equal the restriction of the cyclic rotation order to the
class.  The non-vacuous core proven above: the cyclic comparison is total
(`rotation_cmp_total`), and no reordering can cross a non-seam row
(`agreement_with_nonseam`).  What remains open: the class-partition
structure (terminal-suffix prefix intervals are nested or disjoint and
their maximal union partitions the ambiguous rows) and the equivalence of
the r-space spliced output (Phi anchors, run splicing, coalescing) with
this order.  Measured warrant: yeast235, seven maximal classes, 13,503
candidate rows, repaired output byte-identical to the dense independent
cyclic oracle, identical endpoint tie order. -/
def seamRepair_reorders_in_classes : Prop :=
  ∀ (T : Text) (i j : Nat), i ≠ j →
    (suffixAt T i <+: suffixAt T j ∨ suffixAt T j <+: suffixAt T i) →
      -- sorting the class by cyclic LCE (ties by increasing position)
      -- places i before j exactly when cycCmp T i j
      (cycCmp T i j ∨ cycCmp T j i)

/-- Insert one reserved separator byte (0x1E = 30) at 0-based position `p`. -/
def insertSep (T : Text) (p : Nat) : Text := T.take p ++ [30] ++ T.drop p

/-- **Boundary-delta identity (open).**  Inserting `k` reserved separator
bytes into a text changes chi by at most a constant times `k`.  This is the
falsifiable bridge between the single-string chi of a concatenation and the
canonical multi-string chi of the 0x1E-joined corpus.  Measured instances:
yeast k=1: single seam class, empty repair, delta handled by the class
mechanism; yeast235 k=9,901: delta +96 (85,404,240 → 85,404,336).
Deriving it from `chi_eq_maxClasses` requires comparing maximal coverage
classes through insertion, which shifts positions and spawns at most O(k)
new boundaries; the per-boundary class count change is the open part. -/
def boundaryDelta_bounded : Prop :=
  ∃ (C : Nat), ∀ (T : Text) (ps : List Nat),
    chi (ps.foldl insertSep T) ≤ chi T + C * ps.length ∧
      chi T ≤ chi (ps.foldl insertSep T) + C * ps.length

end SxgcSeam
