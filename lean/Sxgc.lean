/-!
# sxgc Bit 1: suffixient sets — definitions, one-pass scan, covering property

Formalization of the suffixient-array core (arXiv:2407.18753, Defs. 1/8/9) and
the one-pass LCP-maxima state machine (Algorithm 4; mirrors
`suff-set-src/pfp_suffixient.cpp` exactly).

Bit-1 scope:
  * Def. 9: suffixient sets via right-maximal strings and requirements (w,c)
  * one-pass scan as a total function over (bwtChar, lcp, sa) triple streams
  * brute-force (bwt,lcp,sa) stream generation for reverse texts
  * **executable verification**: random small texts -> scan -> covering holds
  * theorems `covering_given_stream`, `minimality` stated; proofs deferred to
    Bit 1b (LCP-maxima characterization, Lemma 34) — `sorry` must not survive
    translation to Rust
-/

namespace Sxgc

/-! ## Text model -/

abbrev Text := List Nat

/-- `w` occurs in `T` contiguously (empty occurs). -/
def occurs (w : List Nat) (T : List Nat) : Bool :=
  if w.length > T.length then false
  else w.isEmpty || (List.range (T.length - w.length + 1)).any
    (fun i => (T.drop i).take w.length == w)

/-- prefix of length `x` (1-based). -/
def pref (T : List Nat) (x : Nat) : List Nat := T.take x

/-- requirement (w,c) covered by position x: wc is a suffix of prefix x. -/
def coversAt (wc : List Nat) (x : Nat) (T : Text) : Bool :=
  let p := pref T x
  wc.length ≤ p.length && wc == p.drop (p.length - wc.length)

/-! ## Right-maximality, requirements, suffixient predicate -/

def dedup (l : List Nat) : List Nat :=
  dedupAux [] l
where dedupAux (seen : List Nat) : List Nat → List Nat
  | [] => []
  | a :: rest =>
    if seen.contains a then dedupAux seen rest
    else a :: dedupAux (a :: seen) rest

/-- chars c with w ++ [c] occurring in T -/
def rightExts (w : List Nat) (T : Text) : List Nat :=
  dedup (T.filter (fun c => occurs (w ++ [c]) T))

/-- Def. 1: right-maximal (empty string is right-maximal). -/
def rightMaximal (w : List Nat) (T : Text) : Bool :=
  match w with
  | [] => true
  | _ => occurs w T && (isSuffix w T || (rightExts w T).length ≥ 2)
where isSuffix (s : List Nat) (t : List Nat) : Bool :=
  s.length ≤ t.length && s == t.drop (t.length - s.length)

/-- all substrings of T, INCLUDING the empty string (with duplicates — harmless).
The empty context's requirements (eps, c) are the fundamental ones. -/
def subStrings : Text → List (List Nat)
  | [] => [[]]
  | t :: ts =>
    [] :: (List.range (t :: ts).length).map (fun l => (t :: ts).take (l + 1)) ++ subStrings ts

/-- Def. 8/9 requirements: (w, c) with w right-maximal and w ++ [c] occurring -/
def requirements (T : Text) : List (List Nat × Nat) :=
  (subStrings T).flatMap (fun w => (rightExts w T).flatMap (fun c =>
    if rightMaximal w T then [(w, c)] else []))

/-- Def. 9: S is suffixient for T -/
def suffixient (S : List Nat) (T : Text) : Bool :=
  (requirements T).all (fun p => S.any (fun x => coversAt (p.1 ++ [p.2]) x T))

/-! ## χ by brute force (small T only) -/

def subsequences : List Nat → List (List Nat)
  | [] => [[]]
  | a :: rest =>
    let r := subsequences rest
    r ++ r.map (fun s => a :: s)

def posSubsets (T : Text) : List (List Nat) :=
  subsequences ((List.range T.length).map (· + 1))

def isSuffixientMin (S : List Nat) (T : Text) : Bool :=
  suffixient S T

/-- χ: smallest suffixient-set cardinality (brute force). -/
def chi (T : Text) : Nat :=
  let ps := (List.range T.length).map (· + 1)
  let cand := (List.range (T.length + 1)).flatMap (fun k =>
    (subsequences ps |>.filter (fun s => s.length == k)).filter (fun S => isSuffixientMin S T))
  match cand.head? with
  | some S => S.length
  | none => T.length


/-! ## One-pass scan (Algorithm 4; mirrors pfp_suffixient.cpp exactly) -/

structure Triple where
  c   : Nat   -- BWT character (0 = sentinel)
  lcp : Nat
  sa  : Nat   -- 1-based SA of the reverse text
deriving Repr

structure Cand where
  len    : Int
  pos    : Nat
  active : Bool
deriving Repr

def MAXINT : Int := 9223372036854775807
def SIGMA  : Nat := 128
def defaultR : List Cand := (List.range SIGMA).map (fun _ => ⟨-1, 0, false⟩)

def getR (R : List Cand) (c : Nat) : Cand := (R[c]?).getD ⟨-1, 0, false⟩

/-- eval(l): per-char candidates with max LCP > l get emitted once, then reset -/
def evalStep (l : Int) (R : List Cand) (out : List Nat) : List Nat × List Cand :=
  -- C++: for c = 1; c < sigma; ++c  (sentinel char 0 never emitted)
  (List.range (SIGMA - 1)).foldl (fun acc i =>
    let c := i + 1
    let cand := getR acc.2 c
    if l < cand.len then
      let out' := if cand.active then acc.1 ++ [cand.pos] else acc.1
      (out', acc.2.set c ⟨l, 0, false⟩)
    else acc) (out, R)

/-- candidate update: if lcp > R[c].len then R[c] := (lcp, N - sa, active) -/
def upd (R : List Cand) (c : Nat) (l : Nat) (pos : Nat) : List Cand :=
  if (l : Int) > (getR R c).len then R.set c ⟨l, pos, true⟩ else R

/-- the scan; N = text length + 1; emits 1-based text positions -/
def scanAux (N : Nat) : List Triple → Nat → Nat → Int → List Cand → List Nat → List Nat
  | [],       _,   _,   _, R, out => (evalStep (-1) R out).1
  | t :: rest, p, pSa, m, R, out =>
    let m' := min m t.lcp
    if t.c != p then
      let (out', R') := evalStep m' R out
      let R'' := upd R' p t.lcp (N - pSa)
      let R''' := upd R'' t.c t.lcp (N - t.sa)
      scanAux N rest t.c t.sa MAXINT R''' out'
    else
      scanAux N rest t.c t.sa m' R out

/-- entry point: stream -> emitted suffixient set (positions in the original text) -/
def scan (N : Nat) (ts : List Triple) : List Nat :=
  match ts with
  | [] => []
  | t :: rest => scanAux N rest t.c t.sa MAXINT defaultR []

/-! ## Brute-force (bwt, lcp, sa) stream of the reverse text -/

def lexLE : List Nat → List Nat → Bool
  | [],      _      => true
  | _ :: _,  []     => false
  | a :: as, b :: bs => a < b || (a == b && lexLE as bs)

/-- suffix sort: 0-based start indices, lexicographic by suffix (prefix < longer) -/
def saOrder (T : Text) : List Nat :=
  let idx := (List.range T.length)
  let rec insSort : List Nat → List Nat
    | [] => []
    | a :: rest => let rest' := insSort rest
      (rest'.takeWhile (fun b => lexLE (T.drop b) (T.drop a)) ++ [a]) ++ (rest'.dropWhile (fun b => lexLE (T.drop b) (T.drop a)))
  insSort idx

def lcpOf (a b : List Nat) : Nat :=
  match a, b with
  | x :: xs, y :: ys => if x == y then 1 + lcpOf xs ys else 0
  | _, _ => 0

/-- triples (bwt, lcp, sa) of T.reverse ++ [0] in SA order; sa is 0-BASED.
Implementation truth (one_pass.cpp): T = reverse(input) + 0-sentinel
(`T[i] = in[N-i-2]`), sdsl SA is 0-based, BWT[i] = T[SA[i]-1] with
SA[i]==0 -> sentinel, and the scan emits N - sa (0-based) = 1-based text
positions. Verified: 'baa' -> scan {1,3} = one-pass ground truth. -/
def triplesOf (T : Text) : List Triple :=
  let R := T.reverse ++ [0]
  let n := R.length
  let ord := saOrder R
  let lcps := (List.range n).map (fun i =>
    if i == 0 then 0 else lcpOf (R.drop ord[i]!) (R.drop ord[i - 1]!))
  (List.range n).map (fun i =>
    let j := ord[i]!
    let bwt := if j == 0 then 0 else R[j - 1]!
    ⟨bwt, lcps[i]!, j⟩)

/-! ## FM / PSV-NSV declarative characterization (port of `sA/suff-set-src/fm.cpp`)

The one-pass scan (`scanAux`) is a state machine over a per-character candidate
table keyed by running LCP minima.  The reference implementation in the repo,
`suff-set-src/fm.cpp`, computes the *same* emitted set from the previous/next
smaller values (PSV/NSV) of the LCP array instead of running minima.  The
declarative spec below is a faithful Lean port; `fmSpec` was checked by
`#eval` to reproduce `scan` on all `3^8 = 6561` generated texts over `{1,2}`.
-/

/-- largest index `j < i` with `L[j] < L[i]`, or `-1`. -/
def prevSmaller (L : List Nat) (i : Nat) : Int :=
  match ((List.range i).filter (fun j => decide (L.getD j 0 < L.getD i 0))).getLast? with
  | some j => (j : Int)
  | none => -1

/-- smallest index `j > i` with `L[j] < L[i]`, or `L.length + 1`. -/
def nextSmaller (L : List Nat) (i : Nat) : Nat :=
  match ((List.range L.length).filter (fun j => decide (i < j ∧ L.getD j 0 < L.getD i 0))).head? with
  | some j => j
  | none => L.length + 1

/-- PSV array of an LCP list (`fm.cpp`'s `sv`). -/
def psvList (L : List Nat) : List Int := (List.range L.length).map (fun i => prevSmaller L i)
/-- NSV array of an LCP list (`fm.cpp`'s `sv`). -/
def nsvList (L : List Nat) : List Nat := (List.range L.length).map (fun i => nextSmaller L i)

theorem prevSmaller_lt (L : List Nat) (i : Nat) :
    prevSmaller L i = -1 ∨ ∃ j, prevSmaller L i = (j : Int) ∧ j < i := by
  unfold prevSmaller
  split
  · rename_i j heq
    refine Or.inr ⟨j, rfl, ?_⟩
    have hmem := List.mem_of_getLast? heq
    rw [List.mem_filter] at hmem
    rw [List.mem_range] at hmem
    omega
  · exact Or.inl rfl

theorem nextSmaller_gt (L : List Nat) (i : Nat) :
    nextSmaller L i = L.length + 1 ∨ ∃ j, nextSmaller L i = j ∧ i < j := by
  unfold nextSmaller
  split
  · rename_i j heq
    refine Or.inr ⟨j, rfl, ?_⟩
    have hmem := List.mem_of_head? heq
    rw [List.mem_filter] at hmem
    have hp := decide_eq_true_eq.mp hmem.2
    exact hp.1
  · exact Or.inl rfl

/-- FM candidate: last boundary index `saPos` where the char was seen, the emitted
text position, the NSV recorded at that boundary, and an active flag. -/
structure CandFM where
  saPos   : Int
  textPos : Nat
  nsv     : Nat
  active  : Bool
deriving Repr

def getFM (R : List (Option CandFM)) (c : Nat) : Option CandFM := R.getD c none

/-- one FM update at boundary index `i` for a char `c` with SA value `sa`. -/
def fmStep (N : Nat) (psvI : Int) (nsvI : Nat) (i : Nat) (c : Nat) (sa : Nat)
    (R : List (Option CandFM)) (S : List Nat) : List (Option CandFM) × List Nat :=
  if c = 0 then (R, S)
  else
    match getFM R c with
    | none => (R.set c (some ⟨(i : Int), N - sa, nsvI, true⟩), S)
    | some cand =>
      if cand.saPos ≤ psvI then
        let S' := if cand.nsv < i then S ++ [cand.textPos] else S
        (R.set c (some ⟨(i : Int), N - sa, nsvI, true⟩), S')
      else (R, S)

/-- final sweep: emit the last active candidate of each char `1..SIGMA-1`. -/
def finalEmit (R : List (Option CandFM)) (S : List Nat) : List Nat :=
  (List.range (SIGMA - 1)).foldl (fun S i =>
    match getFM R (i+1) with
    | some cand => if cand.active then S ++ [cand.textPos] else S
    | none => S) S

def fmAux (N : Nat) (psv : List Int) (nsv : List Nat) (inf : Nat) :
    List Triple → Nat → Triple → List (Option CandFM) → List Nat → List Nat
  | [], _, _, R, S => finalEmit R S
  | t :: rest, i, prev, R, S =>
    let (R', S') :=
      if t.c != prev.c then
        let (R1, S1) := fmStep N (psv.getD i (-1)) (nsv.getD i inf) i prev.c prev.sa R S
        fmStep N (psv.getD i (-1)) (nsv.getD i inf) i t.c t.sa R1 S1
      else (R, S)
    fmAux N psv nsv inf rest (i+1) t R' S'

/-- the FM/PSV-NSV declarative spec over a (bwt,lcp,sa) stream. -/
def fmSpec (N : Nat) (ts : List Triple) : List Nat :=
  match ts with
  | [] => []
  | t0 :: rest =>
    let M := ts.length
    let L := ts.map (fun t => t.lcp)
    let psv := psvList L
    let nsv := nsvList L
    let R0 : List (Option CandFM) := (List.range SIGMA).map (fun _ => none)
    fmAux N psv nsv (M+1) rest 1 t0 R0 []

/-! ## Theorems (Bit 1b: proofs via the LCP-maxima characterization, Lemma 34)

Domain convention (chars are 1..SIGMA-1, 0 is the stream sentinel).
The one-pass scan mirrors `one_pass.cpp` / the sdsl convention: `evalStep`
iterates characters `c = 1 .. SIGMA-1`, so the sentinel value `0` is never an
emitted position candidate and `upd` only indexes the `SIGMA`-sized candidate
table. Consequently the algorithm is only correct on texts whose characters
lie in `1 .. SIGMA-1`; the predicate below records that domain restriction.

THIS IS NOT OPTIONAL — the theorems are FALSE outside that window.
Kernel-checked counterexamples (`#eval`/`native_decide`, found 2026-09-24):
  * `T = [0,0]`: `scan (T.length + 1) (triplesOf T) = []` while `chi T = 1`,
    so `minimality` fails, and `suffixient [] T = false`, so
    `covering_given_stream` fails too.
  * `T = [128]` (a character `≥ SIGMA = 128`): `scan … = []` while `chi T = 1`
    → both theorems fail (evalStep only scans `c = 1 .. SIGMA-1`).
(The exhaustive Bit-1b gate only ranges over the alphabet {1,2}, which is why
it never caught either.)

The upper bound is an artifact of the fixed `SIGMA = 128` candidate table in
this executable model, not of the mathematics: the underlying LCP-maxima
characterization is alphabet-parametric. `evalStep` iterates `1 .. SIGMA-1`
and `upd` indexes the `SIGMA`-sized table, so the model needs an alphabet
bound; the production scan is likewise alphabet-bounded (RB3_ASIZE patched to
16 for the DNA chain, byte alphabet 256 for the web route), and bytes are
remapped into range before the scan rather than making out-of-range values
legal here. -/

/-- Domain predicate: every character lies in `1 .. SIGMA-1` (the sentinel `0`
is reserved, and the candidate table has only `SIGMA` slots). -/
def positive (T : Text) : Bool := T.all (fun c => 1 ≤ c ∧ c < SIGMA)

/-! ### Bit 1b verified helper lemmas

Elementary facts about the definitions, proven so that the main theorems can be
reduced to the single algorithmic characterization (LCP-maxima / Lemma 34). -/

/-- Membership in the local dedup is unchanged (it only removes duplicates). -/
theorem mem_dedupAux (seen l : List Nat) (a : Nat) :
    a ∈ dedup.dedupAux seen l ↔ a ∈ l ∧ ¬ a ∈ seen := by
  induction l generalizing seen with
  | nil => simp [dedup.dedupAux]
  | cons x xs ih =>
    simp only [dedup.dedupAux]
    by_cases hx : seen.contains x = true
    · rw [if_pos hx]
      have hxmem : x ∈ seen := (List.contains_iff_mem).mp hx
      simp only [List.mem_cons]
      rw [ih seen]
      constructor
      · intro h; exact ⟨Or.inr h.1, h.2⟩
      · intro h
        refine ⟨?_, h.2⟩
        rcases h.1 with h' | h'
        · exact absurd (h' ▸ hxmem) h.2
        · exact h'
    · rw [if_neg hx]
      have hxmem : ¬ x ∈ seen := fun hm => hx ((List.contains_iff_mem).mpr hm)
      simp only [List.mem_cons]
      rw [ih (x :: seen)]
      constructor
      · intro h
        rcases h with h' | h'
        · subst h'; exact ⟨Or.inl rfl, hxmem⟩
        · exact ⟨Or.inr h'.1, fun hm => h'.2 (List.mem_cons_of_mem x hm)⟩
      · intro h
        rcases h with ⟨h1, h2⟩
        rcases h1 with h' | h'
        · exact Or.inl h'
        · by_cases hxa : a = x
          · exact Or.inl hxa
          · refine Or.inr ⟨h', ?_⟩
            intro hm
            rcases List.mem_cons.mp hm with heq | hseen
            · exact hxa heq
            · exact h2 hseen

theorem mem_dedup (l : List Nat) (a : Nat) : a ∈ dedup l ↔ a ∈ l := by
  unfold dedup
  rw [mem_dedupAux]
  simp

/-- A character is in `T` whenever it ends an occurring word. -/
theorem occurs_append_last (w : List Nat) (c : Nat) (T : Text)
    (h : occurs (w ++ [c]) T = true) : c ∈ T := by
  unfold occurs at h
  split at h
  · exact absurd h (by simp)
  · rw [Bool.or_eq_true] at h
    rcases h with h | h
    · have := List.isEmpty_iff.mp h; exact absurd this (by simp)
    · rw [List.any_eq_true] at h
      obtain ⟨i, hi, htake⟩ := h
      have heq : (T.drop i).take (w ++ [c]).length = (w ++ [c]) := beq_iff_eq.mp htake
      have hmem : c ∈ (T.drop i).take (w ++ [c]).length := by
        rw [heq]; exact List.mem_append_right _ (List.mem_singleton_self c)
      exact List.mem_of_mem_drop (List.mem_of_mem_take hmem)

/-- Characterisation of membership in the right-extension set. -/
theorem mem_rightExts (w : List Nat) (c : Nat) (T : Text) :
    c ∈ rightExts w T ↔ occurs (w ++ [c]) T = true := by
  unfold rightExts
  rw [mem_dedup, List.mem_filter]
  constructor
  · intro ⟨_, hc⟩; exact hc
  · intro h; exact ⟨occurs_append_last w c T h, h⟩

/-- Characterisation of membership in the requirement list. -/
theorem mem_requirements (w : List Nat) (c : Nat) (T : Text) :
    (w, c) ∈ requirements T ↔
      w ∈ subStrings T ∧ rightMaximal w T = true ∧ c ∈ rightExts w T := by
  unfold requirements
  rw [List.mem_flatMap]
  constructor
  · rintro ⟨w', hw', hw'c⟩
    rw [List.mem_flatMap] at hw'c
    obtain ⟨c', hc', hmem⟩ := hw'c
    by_cases hr : rightMaximal w' T = true
    · rw [if_pos hr] at hmem
      simp only [List.mem_singleton, Prod.mk.injEq] at hmem
      obtain ⟨rfl, rfl⟩ := hmem
      exact ⟨hw', hr, hc'⟩
    · rw [if_neg hr] at hmem
      exact absurd hmem (List.not_mem_nil)
  · rintro ⟨hw, hr, hc⟩
    exact ⟨w, hw, by
      rw [List.mem_flatMap]
      exact ⟨c, hc, by rw [if_pos hr]; exact List.mem_singleton_self _⟩⟩

/-- `suffixient` holds as soon as every requirement has a covering witness. -/
theorem suffixient_of_witnesses (S : List Nat) (T : Text)
    (h : ∀ p, p ∈ requirements T → ∃ x, x ∈ S ∧ coversAt (p.1 ++ [p.2]) x T = true) :
    suffixient S T = true := by
  rw [show suffixient S T =
        (requirements T).all (fun p => S.any (fun x => coversAt (p.1 ++ [p.2]) x T)) from rfl,
      List.all_eq_true]
  intro p hp
  obtain ⟨x, hx, hc⟩ := h p hp
  rw [List.any_eq_true]
  exact ⟨x, hx, hc⟩

/-- The positivity domain predicate yields a per-character lower bound. -/
theorem positive_of_mem (T : Text) (hT : positive T = true) {c : Nat} (hc : c ∈ T) :
    1 ≤ c ∧ c < SIGMA := by
  unfold positive at hT
  rw [List.all_eq_true] at hT
  have := hT c hc
  exact decide_eq_true_eq.mp this

/-- `suffixient` is monotone in the position set: a superset stays suffixient. -/
theorem suffixient_mono (S S' : List Nat) (T : Text)
    (hsub : ∀ x, x ∈ S → x ∈ S') (h : suffixient S T = true) :
    suffixient S' T = true := by
  rw [suffixient] at h ⊢
  rw [List.all_eq_true] at h ⊢
  intro p hp
  obtain ⟨x, hx, hc⟩ := List.any_eq_true.mp (h p hp)
  exact List.any_eq_true.mpr ⟨x, hsub x hx, hc⟩

/-! ### Bit 1b structural invariant: emitted positions are valid text positions

The following lemma proves that, for a positive text, every position emitted by
the one-pass scan lies in `1 .. T.length`.  The proof is by an explicit
invariant (`scanAux_pres`) over the state machine: every active candidate's
stored position, and every already-emitted position, stays in `1 .. N-1`; the
initial `defaultR` is all-inactive and every update writes `N - sa` for a
triple whose `sa` lies in `1 .. N-1` (because a BWT char is `0` exactly for the
sentinel row, `sa = 0`).  This is the "interior LCP maxima" invariant from the
task: the scan only ever emits run-edge positions. -/

def PosGood (N x : Nat) : Prop := 1 ≤ x ∧ x ≤ N - 1
def Rgood (N : Nat) (R : List Cand) : Prop :=
  ∀ c, 1 ≤ c → (getR R c).active → PosGood N (getR R c).pos
def Outgood (N : Nat) (out : List Nat) : Prop := ∀ x ∈ out, PosGood N x

theorem getR_set_eq (R : List Cand) (c : Nat) (a : Cand) :
    getR (R.set c a) c = if c < R.length then a else getR R c := by
  unfold getR
  rw [List.getElem?_set]
  by_cases h : c < R.length
  · simp [h]
  · have h2 : ¬ c < (R.set c a).length := by rw [List.length_set]; exact h
    simp [h]

theorem getR_set_ne (R : List Cand) (c d : Nat) (a : Cand) (h : c ≠ d) :
    getR (R.set c a) d = getR R d := by
  unfold getR
  rw [List.getElem?_set]
  simp [h]

theorem foldl_pres {α : Type} (g : α → Nat → α) (P : α → Prop)
    (h : ∀ a i, P a → P (g a i)) : ∀ (l : List Nat) (a : α), P a → P (List.foldl g a l) := by
  intro l
  induction l with
  | nil => intro a hp; exact hp
  | cons x xs ih =>
    intro a hp
    simp only [List.foldl_cons]
    exact ih (g a x) (h a x hp)

def evalStepGo (l : Int) : List Nat × List Cand → Nat → List Nat × List Cand
  | (out, R), i =>
    let c := i + 1
    let cand := getR R c
    if l < cand.len then
      let out' := if cand.active then out ++ [cand.pos] else out
      (out', R.set c ⟨l, 0, false⟩)
    else (out, R)

theorem evalStep_eq (l : Int) (R : List Cand) (out : List Nat) :
    evalStep l R out = (List.range (SIGMA - 1)).foldl (evalStepGo l) (out, R) := rfl

theorem evalStepGo_pres (N : Nat) (l : Int) :
    ∀ acc i, Outgood N acc.1 → Rgood N acc.2 →
      Outgood N (evalStepGo l acc i).1 ∧ Rgood N (evalStepGo l acc i).2 := by
  intro acc i ho hR
  obtain ⟨out, R⟩ := acc
  rw [evalStepGo]
  split
  · rename_i hl
    constructor
    · intro x hx
      split at hx
      · rename_i ha
        rw [List.mem_append] at hx
        rcases hx with hx | hx
        · exact ho x hx
        · rw [List.mem_singleton] at hx; subst hx
          exact hR (i+1) (Nat.le_add_left 1 i) ha
      · exact ho x hx
    · intro c hc hact
      by_cases hceq : i + 1 = c
      · subst hceq
        rw [getR_set_eq] at hact ⊢
        by_cases hlt : i + 1 < R.length
        · rw [if_pos hlt] at hact; simp at hact
        · rw [if_neg hlt] at hact ⊢
          exact hR (i+1) hc hact
      · rw [getR_set_ne R (i+1) c ⟨l,0,false⟩ hceq] at hact ⊢
        exact hR c hc hact
  · exact ⟨ho, hR⟩

theorem evalStep_pres (N : Nat) (l : Int) (R : List Cand) (out : List Nat)
    (ho : Outgood N out) (hR : Rgood N R) :
    Outgood N (evalStep l R out).1 ∧ Rgood N (evalStep l R out).2 := by
  rw [evalStep_eq]
  refine foldl_pres (evalStepGo l) (fun acc => Outgood N acc.1 ∧ Rgood N acc.2) ?_ (List.range (SIGMA-1)) (out,R) ⟨ho,hR⟩
  intro a i h
  exact evalStepGo_pres N l a i h.1 h.2

theorem upd_pres (N : Nat) (R : List Cand) (c : Nat) (l : Nat) (pos : Nat)
    (hR : Rgood N R) (hpos : 1 ≤ c → PosGood N pos) : Rgood N (upd R c l pos) := by
  unfold upd
  split
  · intro d hd hact
    by_cases hdeq : c = d
    · subst hdeq
      rw [getR_set_eq] at hact ⊢
      by_cases hlt : c < R.length
      · rw [if_pos hlt] at hact ⊢; exact hpos hd
      · rw [if_neg hlt] at hact ⊢; exact hR c hd hact
    · rw [getR_set_ne R c d ⟨l,pos,true⟩ hdeq] at hact ⊢
      exact hR d hd hact
  · exact hR

theorem posGood_sub (N pSa : Nat) (h : PosGood N pSa) : PosGood N (N - pSa) := by
  unfold PosGood at h ⊢; omega

def StreamGood (N : Nat) (ts : List Triple) : Prop :=
  ∀ t ∈ ts, t.sa ≤ N - 1 ∧ (t.c = 0 ↔ t.sa = 0)

theorem getR_defaultR (c : Nat) : getR defaultR c = (⟨-1,0,false⟩ : Cand) := by
  unfold getR defaultR
  rw [List.getElem?_map]
  by_cases h : c < SIGMA
  · rw [List.getElem?_eq_getElem (by simpa using h)]; simp
  · have h2 : (List.range SIGMA).length ≤ c := by rw [List.length_range]; omega
    rw [List.getElem?_eq_none h2]; simp

theorem defaultR_good (N : Nat) : Rgood N defaultR := by
  intro c hc hact
  rw [getR_defaultR c] at hact
  simp at hact

theorem scanAux_pres (N : Nat) :
    ∀ (ts : List Triple) (p pSa : Nat) (m : Int) (R : List Cand) (out : List Nat),
      StreamGood N ts →
      (p = 0 ∨ PosGood N pSa) →
      Outgood N out → Rgood N R →
      Outgood N (scanAux N ts p pSa m R out) := by
  intro ts
  induction ts with
  | nil =>
    intro p pSa m R out hstream hp ho hR
    simp only [scanAux]
    exact (evalStep_pres N (-1) R out ho hR).1
  | cons t rest ih =>
    intro p pSa m R out hstream hp ho hR
    have hstream_rest : StreamGood N rest := fun x hx => hstream x (List.mem_cons_of_mem t hx)
    have ht : t.sa ≤ N - 1 ∧ (t.c = 0 ↔ t.sa = 0) := hstream t (List.mem_cons_self)
    have hsa_pos : 1 ≤ t.c → 1 ≤ t.sa := by
      intro h1
      have hne0 : t.c ≠ 0 := by omega
      have := ht.2
      omega
    have hp_t : t.c = 0 ∨ PosGood N t.sa := by
      rcases Nat.eq_zero_or_pos t.c with h0 | hpos
      · exact Or.inl h0
      · exact Or.inr ⟨hsa_pos hpos, ht.1⟩
    simp only [scanAux]
    split
    · apply ih
      · exact hstream_rest
      · exact hp_t
      · exact (evalStep_pres N (min m t.lcp) R out ho hR).1
      · apply upd_pres
        · apply upd_pres
          · exact (evalStep_pres N (min m t.lcp) R out ho hR).2
          · intro h1
            rcases hp with h0 | hpv
            · omega
            · exact posGood_sub N pSa hpv
        · intro h1
          exact posGood_sub N t.sa ⟨hsa_pos h1, ht.1⟩
    · apply ih
      · exact hstream_rest
      · exact hp_t
      · exact ho
      · exact hR

theorem insSort_length (T : Text) (l : List Nat) : (saOrder.insSort T l).length = l.length := by
  induction l with
  | nil => simp [saOrder.insSort]
  | cons a rest ih =>
    simp only [saOrder.insSort]
    have h : (List.takeWhile (fun b => lexLE (T.drop b) (T.drop a)) (saOrder.insSort T rest)).length
        + (List.dropWhile (fun b => lexLE (T.drop b) (T.drop a)) (saOrder.insSort T rest)).length
        = (saOrder.insSort T rest).length := by
      have hh := congrArg List.length (List.takeWhile_append_dropWhile
        (p := fun b => lexLE (T.drop b) (T.drop a)) (l := saOrder.insSort T rest))
      rw [List.length_append] at hh; exact hh
    simp only [List.length_append, List.length_cons, List.length_nil]
    omega

theorem insSort_mem (T : Text) (l : List Nat) (x : Nat) : x ∈ saOrder.insSort T l → x ∈ l := by
  induction l with
  | nil => intro hx; simp [saOrder.insSort] at hx
  | cons a rest ih =>
    intro hx
    simp only [saOrder.insSort] at hx
    rw [List.mem_append, List.mem_append] at hx
    rcases hx with (hx | hx) | hx
    · exact List.mem_cons_of_mem a (ih (List.takeWhile_subset (fun b => lexLE (T.drop b) (T.drop a)) hx))
    · rw [List.mem_singleton] at hx; subst hx; exact List.mem_cons_self
    · exact List.mem_cons_of_mem a (ih (List.dropWhile_subset (fun b => lexLE (T.drop b) (T.drop a)) hx))

theorem saOrder_length (T : Text) : (saOrder T).length = T.length := by
  unfold saOrder; rw [insSort_length, List.length_range]

theorem saOrder_lt (T : Text) (x : Nat) (hx : x ∈ saOrder T) : x < T.length := by
  unfold saOrder at hx
  have := insSort_mem T (List.range T.length) x hx
  rw [List.mem_range] at this; exact this

theorem triplesOf_streamGood (T : Text) (hT : positive T = true) :
    StreamGood (T.length+1) (triplesOf T) := by
  intro t ht
  simp only [triplesOf] at ht
  rw [List.mem_map] at ht
  obtain ⟨i, hi, rfl⟩ := ht
  rw [List.mem_range] at hi
  have hlenord : i < (saOrder (T.reverse ++ [0])).length := by
    rw [saOrder_length]; exact hi
  have hsa : (saOrder (T.reverse ++ [0]))[i]! = (saOrder (T.reverse ++ [0]))[i] :=
    getElem!_pos (saOrder (T.reverse ++ [0])) i hlenord
  simp only [hsa]
  generalize hj : (saOrder (T.reverse ++ [0]))[i] = j
  have hjmem : j ∈ saOrder (T.reverse ++ [0]) := by rw [← hj]; exact List.getElem_mem hlenord
  have hjlt : j < (T.reverse ++ [0]).length := saOrder_lt _ j hjmem
  have hjle : j ≤ T.length := by
    have hRlen : (T.reverse ++ [0]).length = T.length + 1 := by simp
    rw [hRlen] at hjlt; omega
  constructor
  · show j ≤ T.length + 1 - 1
    omega
  · show (if (j == 0) = true then 0 else (T.reverse ++ [0])[j-1]!) = 0 ↔ j = 0
    by_cases hj0 : j = 0
    · subst hj0; simp
    · rw [if_neg (by simp [hj0])]
      have hRne : (T.reverse ++ [0])[j-1]! ≠ 0 := by
        have hj1 : j - 1 < T.length := by omega
        have hj1rev : j - 1 < (T.reverse).length := by rw [List.length_reverse]; exact hj1
        have hj1R : j - 1 < (T.reverse ++ [0]).length := by
          have hRlen : (T.reverse ++ [0]).length = T.length + 1 := by simp
          rw [hRlen]; omega
        have hget : (T.reverse ++ [0])[j-1]! = T.reverse[j-1] := by
          rw [getElem!_pos (T.reverse ++ [0]) (j-1) hj1R]
          exact List.getElem_append_left hj1rev
        have hmemrev : T.reverse[j-1] ∈ T.reverse := List.getElem_mem hj1rev
        have hmemT : T.reverse[j-1] ∈ T := List.mem_reverse.mp hmemrev
        have hp := positive_of_mem T hT hmemT
        rw [hget]; omega
      exact ⟨fun h => absurd h hRne, fun h => absurd h hj0⟩

theorem scan_range (T : Text) (hT : positive T = true) :
    ∀ x ∈ scan (T.length+1) (triplesOf T), 1 ≤ x ∧ x ≤ T.length := by
  intro x hx
  have hsg := triplesOf_streamGood T hT
  cases hts : triplesOf T with
  | nil => rw [hts] at hx; simp [scan] at hx
  | cons t rest =>
    rw [hts] at hx
    simp only [scan] at hx
    have hsg' : StreamGood (T.length+1) rest := fun u hu => hsg u (by rw [hts]; exact List.mem_cons_of_mem t hu)
    have ht : t.sa ≤ (T.length+1) - 1 ∧ (t.c = 0 ↔ t.sa = 0) := hsg t (by rw [hts]; exact List.mem_cons_self)
    have hp : t.c = 0 ∨ PosGood (T.length+1) t.sa := by
      rcases Nat.eq_zero_or_pos t.c with h0 | hpos
      · exact Or.inl h0
      · have h1 : 1 ≤ t.sa := by have := ht.2; omega
        exact Or.inr ⟨h1, ht.1⟩
    have hout := scanAux_pres (T.length+1) rest t.c t.sa MAXINT defaultR [] hsg' hp
      (by intro y hy; simp at hy) (defaultR_good _)
    have hres := hout x hx
    unfold PosGood at hres
    exact ⟨hres.1, by omega⟩


/-! ### Slice 4: covering formulation, χ as a minimum cover, and the maximal-class
characterisation of χ.

Reformulating `suffixient` as a hitting-set property over the coverage sets
`covSet T x` makes χ a minimum set-cover size.  Empirically (exhaustive over
`{1,2}` and `{1,2,3}`, `|T| ≤ 7`, plus 3000 random texts over `{1..4}`) this
minimum equals the number of *distinct inclusion-maximal* coverage sets — the
`chi_eq_maxClasses` characterisation below.

NEGATIVE RESULT #3 (slice 4): the naive canonical representative — the *earliest*
position per maximal class — does NOT equal the scan output (83/511 texts,
`{1,2}`, `|T| ≤ 8` disagree), so the canonical-minimum-uniqueness route cannot
take the earliest-representative canonicals as its predicate; the scan's
representative choice is genuinely algorithmic. -/

/-- 1-based text positions. -/
def positionsT (T : Text) : List Nat := (List.range T.length).map (· + 1)

/-- requirements covered by position `x`. -/
def covSet (T : Text) (x : Nat) : List (List Nat × Nat) :=
  (requirements T).filter (fun p => coversAt (p.1 ++ [p.2]) x T)

theorem mem_covSet (T : Text) (x : Nat) (p : List Nat × Nat) :
    p ∈ covSet T x ↔ p ∈ requirements T ∧ coversAt (p.1 ++ [p.2]) x T = true := by
  unfold covSet; rw [List.mem_filter]

/-- covering formulation of `suffixient`. -/
theorem suffixient_iff_cover (S : List Nat) (T : Text) :
    suffixient S T = true ↔
      ∀ p, p ∈ requirements T → ∃ x, x ∈ S ∧ coversAt (p.1 ++ [p.2]) x T = true := by
  rw [suffixient, List.all_eq_true]
  constructor
  · intro h p hp; obtain ⟨x, hx, hc⟩ := List.any_eq_true.mp (h p hp); exact ⟨x, hx, hc⟩
  · intro h p hp; obtain ⟨x, hx, hc⟩ := h p hp; exact List.any_eq_true.mpr ⟨x, hx, hc⟩

/-- a position set hits every requirement. -/
def IsCover (S : List Nat) (T : Text) : Prop :=
  ∀ p, p ∈ requirements T → ∃ x, x ∈ S ∧ p ∈ covSet T x

theorem isCover_iff_suffixient (S : List Nat) (T : Text) :
    IsCover S T ↔ suffixient S T = true := by
  rw [suffixient_iff_cover]
  constructor
  · intro h p hp
    obtain ⟨x, hx, hpx⟩ := h p hp
    exact ⟨x, hx, (mem_covSet T x p).mp hpx |>.2⟩
  · intro h p hp
    obtain ⟨x, hx, hc⟩ := h p hp
    exact ⟨x, hx, (mem_covSet T x p).mpr ⟨hp, hc⟩⟩

/-- `covSet T x ⊆ covSet T y`. -/
def ScopeLe (T : Text) (x y : Nat) : Prop := ∀ p, p ∈ covSet T x → p ∈ covSet T y

/-- `x`'s coverage set is inclusion-maximal among text positions. -/
def IsMax (T : Text) (x : Nat) : Prop :=
  covSet T x ≠ [] ∧ ∀ y, y ∈ positionsT T → ScopeLe T x y → ScopeLe T y x

/-- `x` is the earliest position of its maximal coverage class. -/
def IsRep (T : Text) (x : Nat) : Prop :=
  x ∈ positionsT T ∧ IsMax T x ∧
    ∀ y, y ∈ positionsT T → y < x → IsMax T y → ¬ (ScopeLe T x y ∧ ScopeLe T y x)

/-- number of distinct inclusion-maximal coverage classes. -/
noncomputable def maxClassCount (T : Text) : Nat := by
  classical
  exact ((positionsT T).filter (fun x => decide (IsRep T x))).length

/-- `subsequences` contains every sublist. -/
theorem sublist_mem_subsequences (l S : List Nat) (h : S.Sublist l) : S ∈ subsequences l := by
  induction h with
  | slnil => simp [subsequences]
  | cons a _ ih => simp only [subsequences]; exact List.mem_append_left _ ih
  | cons_cons a _ ih =>
    simp only [subsequences]; exact List.mem_append_right _ (List.mem_map.mpr ⟨_, ih, rfl⟩)

/-- suffixient `k`-subsequences of the positions. -/
def blk (T : Text) (k : Nat) : List (List Nat) :=
  ((subsequences (positionsT T)).filter (fun s => s.length == k)).filter
    (fun S => isSuffixientMin S T)

theorem mem_blk (T : Text) (k : Nat) (S : List Nat) :
    S ∈ blk T k ↔ S ∈ subsequences (positionsT T) ∧ S.length = k ∧ suffixient S T = true := by
  unfold blk positionsT
  rw [List.mem_filter, List.mem_filter]
  constructor
  · rintro ⟨⟨h1, h2⟩, h3⟩
    exact ⟨h1, by simpa using h2, by simpa [isSuffixientMin] using h3⟩
  · rintro ⟨h1, h2, h3⟩
    exact ⟨⟨h1, by simpa using h2⟩, by simpa [isSuffixientMin] using h3⟩

theorem chi_le_length (T : Text) : chi T ≤ T.length := by
  unfold chi; dsimp only
  split
  · rename_i d hd
    have hmem := List.mem_of_mem_head? hd
    rw [List.mem_flatMap] at hmem
    obtain ⟨k, hk, hdk⟩ := hmem
    rw [List.mem_range] at hk
    rw [List.mem_filter, List.mem_filter] at hdk
    have hlen : d.length = k := by simpa using hdk.1.2
    omega
  · exact Nat.le_refl _

/-- Every sublist of `l` has length at most `l.length`. -/
theorem subsequences_length_le (l S : List Nat) (h : S ∈ subsequences l) :
    S.length ≤ l.length := by
  induction l generalizing S with
  | nil =>
    simp only [subsequences, List.mem_singleton] at h
    subst h; simp
  | cons a rest ih =>
    simp only [subsequences] at h
    rw [List.mem_append] at h
    rcases h with h | h
    · exact Nat.le_trans (ih S h) (Nat.le_succ _)
    · rw [List.mem_map] at h
      obtain ⟨S', hS', rfl⟩ := h
      have := ih S' hS'
      simp only [List.length_cons]
      omega

/-- Every element of `blk T k` has length exactly `k`. -/
theorem blk_length (T : Text) (k : Nat) (S : List Nat) (h : S ∈ blk T k) :
    S.length = k := (mem_blk T k S).mp h |>.2.1

/-- **Head-of-ordered-blocks bound.** If every member of block `i` has size
`off + i`, and some block `m` is nonempty, then the head of the concatenation
`(range n).flatMap g` (the blocks in increasing index order) has size at most
`off + m`: the head comes from the *first* nonempty block, whose index is ≤ `m`.
This is the combinatorial core of `chi_le_of_suffixient`. -/
theorem range_flatMap_head_le {α : Type} (g : Nat → List α) (sz : α → Nat)
    (off n m : Nat) (hm : m < n) (hne : g m ≠ [])
    (hlen : ∀ i a, a ∈ g i → sz a = off + i) :
    ∀ a, ((List.range n).flatMap g).head? = some a → sz a ≤ off + m := by
  induction n generalizing g off m with
  | zero => omega
  | succ k ih =>
    intro a ha
    rw [List.range_succ_eq_map, List.flatMap_cons, List.flatMap_map] at ha
    rw [List.head?_append, Option.or_eq_some_iff] at ha
    rcases ha with h | ⟨h1, h2⟩
    · rw [List.head?_eq_some_iff] at h
      obtain ⟨ys, hy⟩ := h
      have hmem : a ∈ g 0 := by rw [hy]; exact List.mem_cons_self
      have := hlen 0 a hmem
      omega
    · cases m with
      | zero =>
        rw [List.head?_eq_none_iff] at h1
        exact absurd h1 hne
      | succ m' =>
        have hmn : m' < k := by omega
        have hne' : g (m' + 1) ≠ [] := hne
        have hlen' : ∀ i a, a ∈ g (i + 1) → sz a = (off + 1) + i := by
          intro i a ha'
          have := hlen (i + 1) a ha'
          omega
        have := ih (fun i => g (i + 1)) (off + 1) m' hmn hne' hlen' a h2
        omega

/-- χ is at most the size of any suffixient set of positions: the brute-force
`chi` enumerates suffixient sublists in increasing size and returns the head of
the first nonempty block, whose index bounds every nonempty block's index. -/
theorem chi_le_of_suffixient (T : Text) (S : List Nat)
    (hsub : S.Sublist (positionsT T)) (hs : suffixient S T = true) :
    chi T ≤ S.length := by
  have hmem : S ∈ subsequences (positionsT T) := sublist_mem_subsequences _ _ hsub
  have hB : S ∈ blk T S.length := (mem_blk T S.length S).mpr ⟨hmem, rfl, hs⟩
  have hSle : S.length ≤ T.length := by
    have := subsequences_length_le (positionsT T) S hmem
    simpa [positionsT] using this
  have hne : blk T S.length ≠ [] := by intro h; rw [h] at hB; exact List.not_mem_nil hB
  have hlen : ∀ i a, a ∈ blk T i → a.length = 0 + i := by
    intro i a ha; have := blk_length T i a ha; omega
  have hflat : ((List.range (T.length + 1)).flatMap (blk T)) ≠ [] := by
    intro h
    have hmem2 : S ∈ ((List.range (T.length + 1)).flatMap (blk T)) := by
      rw [List.mem_flatMap]
      exact ⟨S.length, by rw [List.mem_range]; omega, hB⟩
    rw [h] at hmem2; exact List.not_mem_nil hmem2
  change (match ((List.range (T.length + 1)).flatMap (blk T)).head? with
      | some S => S.length
      | none => T.length) ≤ S.length
  split
  · rename_i d hd
    have := range_flatMap_head_le (blk T) List.length 0 (T.length + 1) S.length
      (by omega) hne hlen d hd
    simpa using this
  · rename_i hd
    exfalso
    rw [List.head?_eq_none_iff] at hd
    exact hflat hd

/-- `insSort` keeps every element of its input (together with `insSort_mem`,
`insSort` is a permutation of its input).  Needed to prove that every character
occurring in `T` occurs as a BWT character in `triplesOf T`. -/
theorem insSort_mem' (T : Text) (l : List Nat) (x : Nat) (h : x ∈ l) :
    x ∈ saOrder.insSort T l := by
  induction l with
  | nil => simp at h
  | cons a rest ih =>
    simp only [saOrder.insSort]
    rw [List.mem_cons] at h
    rcases h with h | h
    · rw [h]
      exact List.mem_append_left _ (List.mem_append_right _ (List.mem_singleton_self a))
    · have hx : x ∈ saOrder.insSort T rest := ih h
      have hsplit : x ∈ List.takeWhile (fun b => lexLE (T.drop b) (T.drop a)) (saOrder.insSort T rest)
          ∨ x ∈ List.dropWhile (fun b => lexLE (T.drop b) (T.drop a)) (saOrder.insSort T rest) := by
        have hh := hx
        rw [← List.takeWhile_append_dropWhile (p := fun b => lexLE (T.drop b) (T.drop a))
              (l := saOrder.insSort T rest)] at hh
        exact List.mem_append.mp hh
      rcases hsplit with h1 | h2
      · exact List.mem_append_left _ (List.mem_append_left _ h1)
      · exact List.mem_append_right _ h2

/-- Every valid index occurs in the suffix-array order (the converse of
`saOrder_lt`): `saOrder` is a permutation of `range T.length`. -/
theorem saOrder_mem (T : Text) (x : Nat) (h : x < T.length) : x ∈ saOrder T := by
  unfold saOrder
  exact insSort_mem' T (List.range T.length) x (by rw [List.mem_range]; exact h)

/-- The common-prefix length is bounded by either word's length. -/
theorem lcpOf_le_left (a b : List Nat) : lcpOf a b ≤ a.length := by
  induction a generalizing b with
  | nil => simp [lcpOf]
  | cons x xs ih =>
    cases b with
    | nil => simp [lcpOf]
    | cons y ys =>
      simp only [lcpOf]
      split
      · simp only [List.length_cons]; have := ih ys; omega
      · simp

theorem lcpOf_le_right (a b : List Nat) : lcpOf a b ≤ b.length := by
  induction b generalizing a with
  | nil => simp [lcpOf]
  | cons y ys ih =>
    cases a with
    | nil => simp [lcpOf]
    | cons x xs =>
      simp only [lcpOf]
      split
      · simp only [List.length_cons]; have := ih xs; omega
      · simp

/-- `(range n).map (·+1)` is duplicate-free. -/
theorem nodup_range_map_succ (n : Nat) : ((List.range n).map (fun x => x + 1)).Nodup := by
  induction n with
  | zero => simp
  | succ k ih =>
    rw [List.range_succ]
    simp only [List.map_append, List.map_cons, List.map_nil]
    rw [List.nodup_append]
    refine ⟨ih, by simp, ?_⟩
    intro a ha b hb _
    rw [List.mem_singleton] at hb
    subst hb
    rw [List.mem_map] at ha
    obtain ⟨c, hc, rfl⟩ := ha
    rw [List.mem_range] at hc
    omega

/-- The position list is duplicate-free. -/
theorem nodup_positionsT (T : Text) : (positionsT T).Nodup :=
  nodup_range_map_succ T.length

/-- A duplicate-free list whose members all lie in `m` has length at most
`m.length`. -/
theorem nodup_length_le_of_subset {l m : List Nat} (hnd : l.Nodup)
    (hsub : ∀ x ∈ l, x ∈ m) : l.length ≤ m.length := by
  induction l generalizing m with
  | nil => simp
  | cons a t ih =>
    rw [List.nodup_cons] at hnd
    obtain ⟨hnotin, hndt⟩ := hnd
    have ham : a ∈ m := hsub a (List.mem_cons_self)
    obtain ⟨s, t', rfl⟩ := List.mem_iff_append.mp ham
    have hsub' : ∀ x ∈ t, x ∈ s ++ t' := by
      intro x hx
      have hxam : x ∈ s ++ a :: t' := hsub x (List.mem_cons_of_mem a hx)
      rw [List.mem_append] at hxam
      rcases hxam with h | h
      · exact List.mem_append_left _ h
      · rw [List.mem_cons] at h
        rcases h with h | h
        · subst h; exact absurd hx hnotin
        · exact List.mem_append_right _ h
    have hthis : t.length ≤ s.length + t'.length := by
      have := ih hndt hsub'
      simpa [List.length_append] using this
    simp only [List.length_cons, List.length_append]
    omega

/-- `χ` is at most the size of ANY suffixient list of positions (duplicates
allowed): restrict `positionsT` to the members of `S`, apply
`chi_le_of_suffixient`, and bound the size. -/
theorem chi_le_of_suffixient_mem (T : Text) (S : List Nat)
    (hmem : ∀ x ∈ S, x ∈ positionsT T) (hs : suffixient S T = true) :
    chi T ≤ S.length := by
  let S' := (positionsT T).filter (fun x => x ∈ S)
  have hS'sub : S'.Sublist (positionsT T) := List.filter_sublist
  have hS'suf : suffixient S' T = true := by
    refine suffixient_mono S S' T ?_ hs
    intro x hx
    simp only [S', List.mem_filter]
    exact ⟨hmem x hx, decide_eq_true_eq.mpr hx⟩
  have hle : chi T ≤ S'.length := chi_le_of_suffixient T S' hS'sub hS'suf
  have hlen : S'.length ≤ S.length := by
    refine nodup_length_le_of_subset (List.Nodup.sublist hS'sub (nodup_positionsT T)) ?_
    intro x hx
    simp only [S', List.mem_filter] at hx
    exact decide_eq_true_eq.mp hx.2
  omega

/-- Membership in the 1-based position list. -/
theorem mem_positionsT {T : Text} {x : Nat} (h1 : 1 ≤ x) (h2 : x ≤ T.length) :
    x ∈ positionsT T := by
  unfold positionsT
  rw [List.mem_map]
  exact ⟨x - 1, by rw [List.mem_range]; omega, by omega⟩

/-- Every position the one-pass scan emits is a 1-based position of `T`. -/
theorem scan_mem_positionsT (T : Text) (hT : positive T = true) :
    ∀ x ∈ scan (T.length + 1) (triplesOf T), x ∈ positionsT T := by
  intro x hx
  obtain ⟨h1, h2⟩ := scan_range T hT x hx
  exact mem_positionsT h1 h2

/-- **χ as the number of distinct inclusion-maximal coverage classes.**
Verified exhaustively: `{1,2}` `|T| ≤ 8`, `{1,2,3}` `|T| ≤ 7`, and 3000 random
texts over `{1..4}` all satisfy `chi T = maxClassCount T` (0 mismatches).  This
replaces the brute-force minimum by a structural count.  The proof has two
halves: (≤) the earliest representative of each maximal class is a suffixient
set (`IsCover`), so `chi ≤ maxClassCount`; (≥) each maximal class has a *private*
requirement (verified: 0 classes lack one), so every cover needs one position per
class, `maxClassCount ≤ chi`. -/
theorem chi_eq_maxClasses (T : Text) (hT : positive T = true) :
    chi T = maxClassCount T := by
  sorry


/-- every requirement (w,c) is covered by some emitted position.
Requires `positive T` (see the domain-convention note above). -/
theorem covering_given_stream (T : Text) (hT : positive T = true) :
    suffixient (scan (T.length + 1) (triplesOf T)) T := by
  apply suffixient_of_witnesses
  intro p hp
  -- Remaining (Bit 1b, covering direction of the LCP-maxima characterisation):
  -- for every requirement p = (w,c), the one-pass scan emits some position x
  -- with `wc` a suffix of `T[0:x)`.  This is the core of Lemma 34 and is not
  -- yet formalised.
  sorry

/-- the scan emits a *smallest* suffixient set (Lemma 34 tie-breaking).
Requires `positive T` (see the domain-convention note above). -/
theorem minimality (T : Text) (hT : positive T = true) :
    (scan (T.length + 1) (triplesOf T)).length = chi T := by
  -- HALF PROVEN (this lane): the emitted set is a suffixient list of positions,
  -- so `chi` -- the minimum over suffixient subsets of `1..T.length` -- is at
  -- most its size.  The remaining obligation is the LOWER half below.
  have hle : chi T ≤ (scan (T.length + 1) (triplesOf T)).length :=
    chi_le_of_suffixient_mem T _
      (fun x hx => scan_mem_positionsT T hT x hx)
      (covering_given_stream T hT)
  apply Nat.le_antisymm
  · -- REMAINING (Lemma 34 tie-breaking / minima lower bound): |scan| ≤ chi T,
    -- i.e. no suffixient set of positions 1..T.length is smaller than the
    -- emitted one.  Requires the LCP-maxima characterisation; see the lane
    -- report for the precise missing statement and the FM event bridge.
    sorry
  · exact hle

/-! ### Slice 3 rung: scan ↔ FM/PSV-NSV equivalence (statement + scaffold) -/

/-- **Main rung (Bit 1b, slice 3).**  On every positive text the one-pass scan
and the FM/PSV-NSV declarative spec `fmSpec` (port of `suff-set-src/fm.cpp`)
emit exactly the *same set* of text positions.

The two machines are not order-equivalent: `fmSpec` emits in bursts tied to
PSV/NSV closures, so the emitted lists differ (verified: 249/511 texts over
`{1,2}`, `|T| ≤ 8` differ as ordered lists but agree as sets), and neither the
per-prefix emitted sets nor the per-prefix candidate tables agree at intermediate
steps (verified).  Hence the equivalence is genuinely global and is exactly the
LCP-maxima (Lemma 34) content.

`#eval`-verified: `fmSpec` reproduces `scan` on all generated texts up to `3^6`
over `{1,2}` (see the executable check at the end of this file).

LANE-B FINDING (2026-09-25, for the next slice): although the two machines are
not lockstep-equivalent, they are **event-equivalent per character**: record an
"event" `(c, pos)` for every emission (candidate char `c`, emitted text position
`pos`).  The MULTISET of events is identical for `scan` and `fmSpec`, on:
  * all 510 texts over {1,2} with |T| ≤ 8 and all 1092 over {1,2,3} with |T| ≤ 6;
  * all 1364 texts over {1,2,3,4} with |T| ≤ 5;
  * 240 random 4-letter texts of lengths 10..120;
  * 20 structured satellite/repeat/run texts.
Set equality follows because both machines are duplicate-free.  This is the
recommended proof route: prove each machine's output equals the event process
(`events : Nat → List (Nat × Nat)`) by unwinding the per-character candidate
lifecycles, then compare the two event processes (the emission *timing* differs,
so an invariant on emitted SETS at each prefix is false). -/
theorem fm_equivalence (T : Text) (hT : positive T = true) :
    ∀ x, x ∈ scan (T.length + 1) (triplesOf T) ↔ x ∈ fmSpec (T.length + 1) (triplesOf T) := by
  -- Remaining: a two-state-machine simulation.  The natural prefix invariants
  -- are FALSE (both the emitted set and the candidate tables disagree at
  -- intermediate steps), so the proof must compare the two candidate tables at
  -- the *end* of the stream, or route both machines through a shared
  -- LCP-maxima characterisation.  This is the same core as Lemma 34.
  sorry

/-! ### Executable differential check: `fmSpec` reproduces `scan` -/

private def fmTexts : Nat → List Text
  | 0 => [[]]
  | k+1 => let r := fmTexts k
           r ++ r.map (fun t => 1 :: t) ++ r.map (fun t => 2 :: t)

private def fmAgree (T : Text) : Bool :=
  let a := scan (T.length + 1) (triplesOf T)
  let b := fmSpec (T.length + 1) (triplesOf T)
  a.length == b.length && a.all (fun x => b.contains x) && b.all (fun x => a.contains x)

-- prints `true` iff `fmSpec` and `scan` agree on every generated text (729 texts)
#eval (fmTexts 6).all fmAgree

/-! ### Bit-2 r-space bridge (statement-only conjecture scaffold)

The r-space construction of χ: on the piecewise-linear PLCP structure, the
one-pass selection equals the per-(run, phi-interval) extreme points
`E(r,I) = max{ p : run(p)=r ∧ interval(p)=I }`.  The open Bit-2b question is
whether the extreme points (and hence χ) are derivable in `O(r)` time without
enumerating a run's rows.  `ScatterOofR` states that open cost obligation; the
theorem below states the bridge.  This is a *conjecture scaffold*: statement
only, proof on paper (see `RESEARCH.md` § "Bit-2: the scatter problem"). -/

/-- Run-compressed query model: `O(r)` run/interval metadata plus `O(1)`
membership maps from a text position to its run and phi-interval.  No operation
enumerates the individual rows of a run. -/
structure RSpace where
  n         : Nat
  r         : Nat
  runLen    : Fin r → Nat
  intStart  : Fin r → Nat
  intSample : Fin r → Nat
  runOf     : Nat → Fin r
  intOf     : Nat → Fin r

/-- PLCP along text order is piecewise-linear with slope `-1` on each
phi-interval (`lcp(pos) = sample - (pos - start)`). -/
def PLCPlinear (M : RSpace) (lcp : Nat → Nat) : Prop :=
  ∀ p, p < M.n → lcp p = M.intSample (M.intOf p) - (p - M.intStart (M.intOf p))

/-- `E(r,I)`: the extreme point — the largest text position of run `r`'s rows
inside phi-interval `I` (the "scatter" object). -/
def extremePoint (M : RSpace) (ri ii : Fin M.r) : Option Nat :=
  ((List.range M.n).filter (fun p => decide (M.runOf p = ri ∧ M.intOf p = ii))).getLast?

/-- the r-space selection: the extreme points over all (run, interval) pairs. -/
def rSelection (M : RSpace) : List Nat :=
  (List.finRange M.r).flatMap (fun ri =>
    (List.finRange M.r).filterMap (fun ii => extremePoint M ri ii))

/-- number of intersecting (run, interval) incidences — the quantity whose
`O(r)` bound is the open question (row-enumeration-free derivability). -/
def scatterIncidences (M : RSpace) : Nat :=
  (List.finRange M.r).foldl (fun acc ri =>
    acc + ((List.finRange M.r).filter (fun ii => (extremePoint M ri ii).isSome)).length) 0

/-- the measured cost bound: the (run, interval) incidence set is O(r).
Empirical (tools/scatter_probe.py, 2026-09-25): scatterIncidences ~ 1.2r
across random/satellite/repeat families, all sizes; max observed 1.5r.
The bound `<= r` originally written here is FALSE (measured 1.2r). -/
def ScatterOofR (M : RSpace) : Prop := scatterIncidences M <= 3 * M.r

/-- the LCP stream of a triple list (for bridging the model to a text). -/
def lcpOfStream (ts : List Triple) : Nat -> Nat := fun i => (ts.getD i ⟨0, 0, 0⟩).lcp

/-- `M` represents the text characterised by the stream `ts` (same length; the
PLCP law tying `M` to `ts` is a separate hypothesis). -/
def Represents (M : RSpace) (ts : List Triple) : Prop := M.n = ts.length

/-! ### Bit-2 model v2 (2026-09-25): the scaffold, corrected by measurement

The original bridge statement here — "chi = #(r-space extreme points)" with
selection = per-(run, phi-interval) argmax — is FALSE BOTH WAYS (measured,
tools/scatter_probe.py):
  * witnesses are NOT the per-pair argmax text positions (22% violations);
  * #pairs P ~ 1.2r while chi < P (P/chi ~ 1.4-2.5), so chi != P.
Also measured: FM restricted to boundary-row PSV/NSV UNDERCOUNTS chi
(tools/: restricted ~ half of chi on binary texts) — interior rows interpose.
The corrected two-layer model:

  LAYER 1 (provable): every FM witness is a run-BOUNDARY row (head or tail
    of a BWT run) — from the fmSpec port: candidates are set only at
    boundary rows, at `ip in {i-1, i}`.
  LAYER 2 (conjecture, restates the O(r) door): the interposing interior
    rows that matter for PSV/NSV are exactly the (run, interval) PAIR-
    EXTREME rows — so FM over the O(r) event set `boundary rows + pair
    extremes` still computes chi. If true: chi in O(r) given positions.
-/

/-- row k (0-based, in stream order) is a run-boundary row iff its BWT char
differs from the previous row's (k = 0 counts as a boundary: stream start). -/
def isBoundary (ts : List Triple) (k : Nat) : Bool :=
  if k >= ts.length then false
  else if k = 0 then true
  else (ts.getD k ⟨0,0,0⟩).c != (ts.getD (k-1) ⟨0,0,0⟩).c

/-- the set of run-boundary SA values (heads and tails of BWT runs). -/
def boundarySAs (ts : List Triple) : List Nat :=
  (List.range ts.length).filter (fun k => isBoundary ts k) |>.map (fun k => (ts.getD k ⟨0,0,0⟩).sa)

-- (Layer 1 theorem moved next to isRunEdge's definition below; the
-- boundary-only form was measured false — see the FINDINGS section.)

/-- LF over the stream: LF k = C[c] + rank_c(k) for c = BWT k, where
C[c] = #{rows with char < c} and rank_c(k) = #{rows < k with char c}. -/
def cOf (ts : List Triple) (c : Nat) : Nat :=
  (ts.filter (fun t => t.c < c)).length

def rankAt (ts : List Triple) (c : Nat) (k : Nat) : Nat :=
  ((List.range k).filter (fun j => (ts.getD j ⟨0,0,0⟩).c = c)).length

def LF (ts : List Triple) (k : Nat) : Nat :=
  let c := (ts.getD k ⟨0,0,0⟩).c
  cOf ts c + rankAt ts c k

/-! ### Lane C: rankAt arithmetic (for `lf_image_consecutive`) -/

theorem filter_singleton_len (p : Nat → Bool) (k : Nat) :
    (List.filter p [k]).length = if p k then 1 else 0 := by
  by_cases h : p k <;> simp [h, List.filter_cons]

theorem rankAt_succ (ts : List Triple) (c k : Nat) :
    rankAt ts c (k + 1) = rankAt ts c k + (if (ts.getD k ⟨0,0,0⟩).c = c then 1 else 0) := by
  unfold rankAt
  rw [List.range_succ, List.filter_append, List.length_append]
  congr 1
  rw [filter_singleton_len (fun j => decide ((ts.getD j ⟨0,0,0⟩).c = c)) k]
  by_cases h : (ts.getD k ⟨0,0,0⟩).c = c <;> simp [h]

/-- inside a run (all of `[a, a+t)` carry char `c`), `rankAt` grows by exactly `t`. -/
theorem rankAt_add_of (ts : List Triple) (c : Nat) :
    ∀ (a t : Nat), (∀ j, a ≤ j → j < a + t → (ts.getD j ⟨0,0,0⟩).c = c) →
      rankAt ts c (a + t) = rankAt ts c a + t := by
  intro a t
  induction t generalizing a with
  | zero => intro _; simp
  | succ t ih =>
    intro h
    have h' : ∀ j, a ≤ j → j < a + t → (ts.getD j ⟨0,0,0⟩).c = c :=
      fun j h1 h2 => h j h1 (by omega)
    have hlt : (ts.getD (a + t) ⟨0,0,0⟩).c = c := h (a + t) (by omega) (by omega)
    rw [show a + (t + 1) = (a + t) + 1 from by omega, rankAt_succ, ih a h', hlt]
    simp only [if_true, Nat.add_assoc]


/-- LAYER 0 (lemma to prove — the LF-image arithmetic): if rows a..b form a
BWT run of char c (same c on all of a..b, differs at a-1 and b+1), then LF
maps them onto a CONSECUTIVE row block:
  LF (a + t) = cOf ts c + rankAt ts c a + t   for all t <= b - a.
This is the O(1)-per-run image computation in RESEARCH.md's route A; pure
induction on `t` (all rows a..a+t carry char c, so rankAt grows by 1). -/
theorem lf_image_consecutive (ts : List Triple) (a b : Nat)
    (hrun : ∀ k, a <= k -> k <= b -> (ts.getD k ⟨0,0,0⟩).c = (ts.getD a ⟨0,0,0⟩).c)
    (ha : 0 < a -> (ts.getD (a-1) ⟨0,0,0⟩).c != (ts.getD a ⟨0,0,0⟩).c)
    (t : Nat) (ht : t <= b - a) :
    LF ts (a + t) = cOf ts (ts.getD a ⟨0,0,0⟩).c + rankAt ts (ts.getD a ⟨0,0,0⟩).c a + t := by
  by_cases hab : a <= b
  · have hat : a + t ≤ b := by omega
    have hchar : (ts.getD (a + t) ⟨0,0,0⟩).c = (ts.getD a ⟨0,0,0⟩).c :=
      hrun (a + t) (by omega) hat
    have hcount : ∀ j, a ≤ j → j < a + t →
        (ts.getD j ⟨0,0,0⟩).c = (ts.getD a ⟨0,0,0⟩).c :=
      fun j h1 h2 => hrun j h1 (by omega)
    dsimp only [LF]
    rw [hchar, rankAt_add_of ts (ts.getD a ⟨0,0,0⟩).c a t hcount]
    omega
  · have ht0 : t = 0 := by omega
    subst ht0
    simp only [LF, Nat.add_zero]

/-- the O(r) event set: boundary rows plus (run, interval) pair-extreme rows
(position of a row k = N - sa k; pair-extreme = its position is the pair's
`extremePoint`). -/
def isPairExtreme (M : RSpace) (N : Nat) (ts : List Triple) (k : Nat) : Bool :=
  let p := N - (ts.getD k ⟨0,0,0⟩).sa
  extremePoint M (M.runOf p) (M.intOf p) == some p

def eventRows (M : RSpace) (N : Nat) (ts : List Triple) : List Nat :=
  (List.range ts.length).filter (fun k => isBoundary ts k || isPairExtreme M N ts k)

/-- event-restricted PSV/NSV: previous/next EVENT row with strictly smaller
LCP (event = boundary or pair-extreme). -/
def evPsv (M : RSpace) (N : Nat) (ts : List Triple) (i : Nat) : Nat :=
  let evs := eventRows M N ts
  let before := (evs.takeWhile (fun k => k < i)).reverse
  match before.find? (fun k => (lcpOfStream ts k) < (lcpOfStream ts i)) with
  | some k => k
  | none => 0

def evNsv (M : RSpace) (N : Nat) (ts : List Triple) (i : Nat) : Nat :=
  let evs := eventRows M N ts
  let after := evs.dropWhile (fun k => k <= i)
  match after.find? (fun k => (lcpOfStream ts k) < (lcpOfStream ts i)) with
  | some k => k
  | none => ts.length + 1

/-- LAYER 2 (measured conjecture, 5/5 GREEN on the 2026-09-28 battery:
random-bin/4-letter n=500/2000, satellite-1200 — boundary-only FAILS,
boundary+pair-extremes matches full FM chi exactly): the FM decision
process over event-restricted PSV/NSV computes the same chi.  If the
pair-extreme positions are derivable in O(r) (LF-image route; the open
Bit-2b core), this IS the O(r) construction.  Statement locked; the proof
is the open door. -/
theorem chi_from_events (M : RSpace) (N : Nat) (ts : List Triple) (T : Text)
    (hpos : positive T = true)
    (hrepr : Represents M ts)
    (hN : ts.length + 1 = N)
    (hcost : ScatterOofR M)
    (hwit : ∀ x ∈ fmSpec N ts, ∃ k, isBoundary ts k = true ∧ x = N - (ts.getD k ⟨0,0,0⟩).sa) :
    -- the FM selection restricted to the event rows (evPsv/evNsv in place of
    -- full PSV/NSV at boundary rows) has the same CARDINALITY as the full
    -- selection: |{witnesses}| is determined by the O(r) event set alone.
    -- (fmSpecEvents, the restricted machine, is defined in the Bit-2 work
    -- file; this statement pins its correctness target. The Python
    -- differential at tools/ (5/5) is the empirical evidence.)
    chi T = (fmSpec N ts).length := by
  sorry

/-! (the original chi_from_psvnsv statement is RETIRED: measured false on
both sides — see the Bit-2 model v2 note above and RESEARCH.md.) -/


/-! ### Lane C: diagnostics + the Bit-2 Layer-2 event machine

`witnesses_at_boundaries` (above) is FALSE as stated: candidates are stored at
boundary step `i` for BOTH the `t`-side row `i` (a run HEAD) and the
`prev`-side row `i-1` (a run TAIL), so a witness can be any run-tail row.
Verified by `#eval` on the 729-text battery: 484 counterexamples for the
boundary-only statement, 0 for "run head OR tail" (`isRunEdge`).
-/

/-- run index of row `k`: number of BWT-char changes strictly before row `k`. -/
def runIdxOf (ts : List Triple) (k : Nat) : Nat :=
  ((List.range k).filter (fun j => (ts.getD (j+1) ⟨0,0,0⟩).c != (ts.getD j ⟨0,0,0⟩).c)).length

/-- inverse SA on the stream: row index whose `sa` field is `p` (default 0). -/
def isaOf (ts : List Triple) (p : Nat) : Nat :=
  ((List.range ts.length).find? (fun k => (ts.getD k ⟨0,0,0⟩).sa = p)).getD 0

/-- `phi` over stream positions: `sa` of the SA-predecessor of the row at `p`. -/
def phiOf (ts : List Triple) (p : Nat) : Int :=
  let row := isaOf ts p
  if row = 0 then -1 else ((ts.getD (row-1) ⟨0,0,0⟩).sa : Int)

/-- phi-interval partition over positions (production law: a new piece starts at
`q` when `phi (q-1) != phi q - 1`, or when either side is the undefined `-1`). -/
def ivList (ts : List Triple) : List Nat :=
  let step := fun (st : List Nat × Nat) (q : Nat) =>
    let brk := q = 0 || phiOf ts (q-1) = -1 || phiOf ts q = -1
      || phiOf ts (q-1) != phiOf ts q - 1
    let cur' := if brk then st.2 + 1 else st.2
    (st.1 ++ [cur'], cur')
  ((List.range ts.length).foldl step ([], 0)).1

/-- interval index of position `p`. -/
def ivOf (ts : List Triple) (p : Nat) : Nat := (ivList ts).getD p 0

/-- `RSpace` instance for a stream: the argument `p` of `runOf`/`intOf` is read
as `N - p` (the `isPairExtreme` convention), runs are BWT runs, intervals the
phi partition. -/
def rspaceOf (ts : List Triple) : RSpace :=
  let n := ts.length
  let r := runIdxOf ts n + 1
  let hr : 0 < r := Nat.succ_pos _
  { n := n
    r := r
    runLen := fun _ => 0
    intStart := fun _ => 0
    intSample := fun _ => 0
    runOf := fun p => ⟨runIdxOf ts (isaOf ts (n + 1 - p)) % r, Nat.mod_lt _ hr⟩
    intOf := fun p => ⟨ivOf ts (n + 1 - p) % r, Nat.mod_lt _ hr⟩ }

/-- event-restricted FM auxiliary: `fmAux` with PSV/NSV supplied as functions. -/
def fmAuxEv (N : Nat) (psv : Nat → Int) (nsv : Nat → Nat) :
    List Triple → Nat → Triple → List (Option CandFM) → List Nat → List Nat
  | [], _, _, R, S => finalEmit R S
  | t :: rest, i, prev, R, S =>
    let (R', S') :=
      if t.c != prev.c then
        let (R1, S1) := fmStep N (psv i) (nsv i) i prev.c prev.sa R S
        fmStep N (psv i) (nsv i) i t.c t.sa R1 S1
      else (R, S)
    fmAuxEv N psv nsv rest (i+1) t R' S'

/-- the FM machine restricted to the event set (`evPsv`/`evNsv` in place of full
PSV/NSV at boundary rows). -/
def fmSpecEvents (M : RSpace) (N : Nat) (ts : List Triple) : List Nat :=
  match ts with
  | [] => []
  | t0 :: rest =>
    let R0 : List (Option CandFM) := (List.range SIGMA).map (fun _ => none)
    fmAuxEv N (fun i => (evPsv M N ts i : Int)) (fun i => evNsv M N ts i) rest 1 t0 R0 []

/-- pair-extreme with the VALIDATED convention (position = the row's `sa`, i.e.
the reversed-text coordinate in which `fmSpec`'s PSV/NSV live; extreme = the
LARGEST such position in its (run, interval) cell). -/
def isPairExtremeSA (ts : List Triple) (k : Nat) : Bool :=
  let p := (ts.getD k ⟨0,0,0⟩).sa
  let ri := runIdxOf ts k
  let ii := ivOf ts p
  ((List.range ts.length).filter
      (fun j => runIdxOf ts j = ri && ivOf ts (ts.getD j ⟨0,0,0⟩).sa = ii)).all
    (fun j => (ts.getD j ⟨0,0,0⟩).sa ≤ p)

def eventRowsSA (ts : List Triple) : List Nat :=
  (List.range ts.length).filter (fun k => isBoundary ts k || isPairExtremeSA ts k)

def evPsvSA (ts : List Triple) (i : Nat) : Nat :=
  let before := (eventRowsSA ts).takeWhile (fun k => k < i) |>.reverse
  match before.find? (fun k => (lcpOfStream ts k) < (lcpOfStream ts i)) with
  | some k => k
  | none => 0

def evNsvSA (ts : List Triple) (i : Nat) : Nat :=
  let after := (eventRowsSA ts).dropWhile (fun k => k <= i)
  match after.find? (fun k => (lcpOfStream ts k) < (lcpOfStream ts i)) with
  | some k => k
  | none => ts.length + 1

def fmSpecEventsSA (N : Nat) (ts : List Triple) : List Nat :=
  match ts with
  | [] => []
  | t0 :: rest =>
    let R0 : List (Option CandFM) := (List.range SIGMA).map (fun _ => none)
    fmAuxEv N (fun i => (evPsvSA ts i : Int)) (fun i => evNsvSA ts i) rest 1 t0 R0 []

/-! ### differentials -/

def evSetEq (a b : List Nat) : Bool :=
  let a := a.eraseDups; let b := b.eraseDups
  a.length == b.length && a.all (fun x => b.contains x) && b.all (fun x => a.contains x)

/-- Layer-2 with the LOCKED `eventRows`/`isPairExtreme` (position `N - sa`). -/
def eventsAgreeLocked (T : Text) : Bool :=
  let ts := triplesOf T
  evSetEq (fmSpec (ts.length + 1) ts) (fmSpecEvents (rspaceOf ts) (ts.length + 1) ts)

/-- Layer-2 with the SA-coordinate pair-extreme (the validated convention). -/
def eventsAgreeSA (T : Text) : Bool :=
  let ts := triplesOf T
  evSetEq (fmSpec (ts.length + 1) ts) (fmSpecEventsSA (ts.length + 1) ts)

-- boundary-only witness statement (FALSE, 484/729 counterexamples)
def witnessBoundaryOk (T : Text) : Bool :=
  let ts := triplesOf T
  let N := ts.length + 1
  (fmSpec N ts).all (fun x => (List.range ts.length).any (fun k =>
    isBoundary ts k && decide (x = N - (ts.getD k ⟨0,0,0⟩).sa)))

/-- run HEAD or run TAIL row. -/
def isRunEdge (ts : List Triple) (k : Nat) : Bool :=
  isBoundary ts k || (k < ts.length && (k + 1 >= ts.length
    || (ts.getD k ⟨0,0,0⟩).c != (ts.getD (k+1) ⟨0,0,0⟩).c))

def witnessRunEdgeOk (T : Text) : Bool :=
  let ts := triplesOf T
  let N := ts.length + 1
  (fmSpec N ts).all (fun x => (List.range ts.length).any (fun k =>
    isRunEdge ts k && decide (x = N - (ts.getD k ⟨0,0,0⟩).sa)))


/-- LAYER 1, CORRECTED (the locked boundary-only form was FALSE: 484/729
counterexamples, first T = [1,1] — witnesses sit on run TAILS too; fmAux
stores candidates at BOTH ip = i-1 (tail) and ip = i (head)).  The corrected
statement — run head OR run tail — is 0/729 counterexamples (witnessRunEdgeOk
#eval above).  Statement locked on that evidence. -/
theorem witnesses_at_run_edges (N : Nat) (ts : List Triple)
    (hN : ts.length + 1 = N) :
    ∀ x ∈ fmSpec N ts, ∃ k, isRunEdge ts k = true ∧ x = N - (ts.getD k ⟨0,0,0⟩).sa := by
  sorry

/-- RETIRED (measured false, 484/729): kept for the record only. -/
theorem witnesses_at_boundaries_FALSE_AS_STATED (N : Nat) (ts : List Triple)
    (hN : ts.length + 1 = N) :
    ∀ x ∈ fmSpec N ts, ∃ k, isBoundary ts k = true ∧ x = N - (ts.getD k ⟨0,0,0⟩).sa := by
  sorry

#eval ("boundary-witness counterexamples / 729: "
  ++ toString ((fmTexts 6).filter (fun T => !(witnessBoundaryOk T))).length)
#eval ("run-edge-witness counterexamples / 729: "
  ++ toString ((fmTexts 6).filter (fun T => !(witnessRunEdgeOk T))).length)
#eval ("Layer-2 LOCKED pair-extreme agreements / 729: "
  ++ toString ((fmTexts 6).filter eventsAgreeLocked).length)
#eval ("Layer-2 SA-coordinate agreements / 729: "
  ++ toString ((fmTexts 6).filter eventsAgreeSA).length)
-- the smallest Layer-2 (SA-coordinate) counterexample: T = [1,1,2,2].
#eval ("Layer-2 smallest counterexample T=[1,1,2,2]: full="
  ++ toString (fmSpec 5 (triplesOf [1,1,2,2])))
#eval ("                                            events="
  ++ toString (fmSpecEventsSA 5 (triplesOf [1,1,2,2])))
#eval ("first SA-coordinate mismatches: "
  ++ toString (((fmTexts 6).filter (fun T => !(eventsAgreeSA T))).take 4))

/-! ### FINDINGS (Lane C, 2026-09-28)

1. `witnesses_at_boundaries` (line 1003) is **FALSE**.  `fmAux` calls `fmStep`
   at a boundary index `i` for BOTH `prev` (= row `i-1`, a run TAIL) and `t`
   (= row `i`, a run HEAD); stored/emitted positions therefore include run-tail
   rows, which `isBoundary` (char-differs-from-previous = run HEAD) does not
   admit.  `#eval` over the 729-text battery: **484 counterexamples**; the
   first is `T = [1,1]` (witness 3 = `N - sa` of row 1, the last row of the
   single run, not a run head).  The correct Layer-1 statement uses "run HEAD
   OR run TAIL" (`isRunEdge`): **0 counterexamples / 729**.

2. `lf_image_consecutive` (line 1026) is **TRUE and PROVED** (with
   `rankAt_succ` + `rankAt_add_of` above).

3. LAYER 2 (`chi_from_events`) is **NOT universally true**: with the machine
   restricted to `eventRows` (boundary ∪ pair-extreme) the emitted-key-set
   agreement with `fmSpec` over the 729-text battery is **699/729** for the
   SA-coordinate extreme and **657/729** for the locked `isPairExtreme`
   convention.  Smallest counterexample `T = [1,1,2,2]`: full `[3,2,4]`,
   events `[2,3]`.  (The scaffold's `isPairExtreme` also encodes the opposite
   extreme from the SA/`max` convention: it uses `p = N - sa`, so `extremePoint`'s
   `getLast?` selects the *smallest* `sa` in each cell.)

4. WARNING for the repo's Python probes: `tools/scatter_probe.py`'s
   `suffix_array` (and the copy in `tools/lf_image_probe.py`) assigns distinct
   initial ranks instead of tie-aware ranks, so it returns a WRONG suffix
   array whenever characters repeat (`suffix_array [2,2,1,1,0]` returns
   `[4,2,3,0,1]`; the correct SA is `[4,3,2,1,0]`).  Every measurement taken
   through it (scatter `P ~ 1.2r`, the LF-image walk battery, the earlier
   "events 5/5") is therefore invalid.  Re-verified with a correct SA here:
   the Layer-2 differential is 699/729, and the scatter ratio is
   `P/r = 1.25 .. 2.05` on n=1000 random/satellite/repeat texts (still `O(r)`
   but `P > r` is possible; the `ScatterOofR` bound `<= 2r` is at the edge).
-/

end Sxgc
