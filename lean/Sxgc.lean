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

/-- one step of `finalEmit` (named so the provenance invariant can induct). -/
def finalStep (R : List (Option CandFM)) (S : List Nat) (i : Nat) : List Nat :=
  match getFM R (i+1) with
  | some cand => if cand.active then S ++ [cand.textPos] else S
  | none => S

/-- final sweep: emit the last active candidate of each char `1..SIGMA-1`. -/
def finalEmit (R : List (Option CandFM)) (S : List Nat) : List Nat :=
  (List.range (SIGMA - 1)).foldl (finalStep R) S

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


/-- `insSort` preserves distinctness (it is a permutation of its input). -/
theorem insSort_nodup (T : Text) : ∀ l : List Nat, l.Nodup → (saOrder.insSort T l).Nodup := by
  intro l
  induction l with
  | nil => intro _; simp [saOrder.insSort]
  | cons a rest ih =>
    intro hnd
    rw [List.nodup_cons] at hnd
    obtain ⟨ha, hrest⟩ := hnd
    have hnd_rest : (saOrder.insSort T rest).Nodup := ih hrest
    have ha' : ¬ a ∈ saOrder.insSort T rest := fun hx => ha (insSort_mem T rest a hx)
    simp only [saOrder.insSort]
    rw [List.nodup_append]
    constructor
    · rw [List.nodup_append]
      refine ⟨List.Nodup.sublist (List.takeWhile_sublist _) hnd_rest, by simp, ?_⟩
      intro b hb c hc hbc
      rw [List.mem_singleton] at hc
      subst hc
      subst hbc
      exact ha' (List.takeWhile_subset _ hb)
    · refine ⟨List.Nodup.sublist (List.dropWhile_sublist _) hnd_rest, ?_⟩
      intro b hb c hc hbc
      rw [List.mem_append, List.mem_singleton] at hb
      rcases hb with hb | hb
      · have hS : (List.takeWhile (fun b => lexLE (T.drop b) (T.drop a)) (saOrder.insSort T rest)
            ++ List.dropWhile (fun b => lexLE (T.drop b) (T.drop a)) (saOrder.insSort T rest)).Nodup := by
          rw [List.takeWhile_append_dropWhile]; exact hnd_rest
        rw [List.nodup_append] at hS
        exact hS.2.2 b hb c hc hbc
      · rw [← hbc] at hc
        rw [hb] at hc
        exact ha' (List.dropWhile_subset _ hc)

/-- `saOrder T` is duplicate-free: each text position occurs exactly once. -/
theorem saOrder_nodup (T : Text) : (saOrder T).Nodup := by
  unfold saOrder
  exact insSort_nodup T (List.range T.length) List.nodup_range

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

/-! ### Lane L: SA/LCP foundation (Task 2b)

`saOrder` is a genuine lexicographic sort of the suffixes (proved: `lexLE` is a
total preorder, and insertion respects it), and `lcpOf` is the longest common
prefix (proved: within both lengths, the length-`k` prefixes agree exactly up to
the lcp).  These are the two facts `covering_given_stream` stood on. -/

/-- `lexLE` is reflexive. -/
theorem lexLE_refl : ∀ a : List Nat, lexLE a a = true := by
  intro a
  induction a with
  | nil => rfl
  | cons x xs ih => simp [lexLE, ih]

/-- `lexLE` is total. -/
theorem lexLE_total (a b : List Nat) : lexLE a b = true ∨ lexLE b a = true := by
  induction a generalizing b with
  | nil => exact Or.inl rfl
  | cons x xs ih =>
    cases b with
    | nil => exact Or.inr (by simp [lexLE])
    | cons y ys =>
      simp only [lexLE]
      rcases Nat.lt_trichotomy x y with h | h | h
      · exact Or.inl (by simp [h])
      · subst h
        rcases ih ys with h' | h'
        · exact Or.inl (by simp [h'])
        · exact Or.inr (by simp [h'])
      · exact Or.inr (by simp [h])

/-- `lexLE` is transitive. -/
theorem lexLE_trans : ∀ a b : List Nat, lexLE a b = true → ∀ c : List Nat,
    lexLE b c = true → lexLE a c = true := by
  intro a
  induction a with
  | nil => intro b _ c _; rfl
  | cons x xs ih =>
    intro b hab c hbc
    cases b with
    | nil => simp [lexLE] at hab
    | cons y ys =>
      cases c with
      | nil => simp [lexLE] at hbc
      | cons z zs =>
        simp only [lexLE] at hab hbc ⊢
        rw [Bool.or_eq_true] at hab hbc
        rcases hab with hab | hab
        · have hxy : x < y := of_decide_eq_true hab
          rw [Bool.or_eq_true]
          rcases hbc with hbc | hbc
          · left
            exact decide_eq_true (Nat.lt_trans hxy (of_decide_eq_true hbc))
          · rw [Bool.and_eq_true] at hbc
            have hyz : y = z := beq_iff_eq.mp hbc.1
            left
            exact decide_eq_true (by omega)
        · rw [Bool.and_eq_true] at hab
          have hxyeq : x = y := beq_iff_eq.mp hab.1
          rw [hxyeq]
          rw [Bool.or_eq_true]
          rcases hbc with hbc | hbc
          · left
            exact hbc
          · rw [Bool.and_eq_true] at hbc
            have hxzeq : y = z := beq_iff_eq.mp hbc.1
            right
            rw [Bool.and_eq_true]
            exact ⟨beq_iff_eq.mpr hxzeq, ih ys hab.2 zs hbc.2⟩

/-- membership in `takeWhile` implies the predicate holds. -/
theorem mem_takeWhile_imp {α : Type} (p : α → Bool) (l : List α) (b : α)
    (h : b ∈ List.takeWhile p l) : p b = true := by
  induction l with
  | nil => simp at h
  | cons a t ih =>
    rw [List.takeWhile_cons] at h
    by_cases ha : p a = true
    · rw [if_pos ha] at h
      rw [List.mem_cons] at h
      rcases h with h | h
      · rw [h]; exact ha
      · exact ih h
    · rw [if_neg ha] at h
      exact absurd h (by simp)

/-- the head of a nonempty `dropWhile` fails the predicate. -/
theorem dropWhile_head_fails {α : Type} (p : α → Bool) : ∀ {l : List α} {c : α} {cs : List α},
    List.dropWhile p l = c :: cs → p c = false := by
  intro l
  induction l with
  | nil => intro c cs h; simp at h
  | cons a t ih =>
    intro c cs h
    rw [List.dropWhile_cons] at h
    by_cases ha : p a = true
    · rw [if_pos ha] at h
      exact ih h
    · rw [if_neg ha] at h
      have hac : a = c := (List.cons.inj h).1
      rw [← hac]
      cases hpa : p a with
      | false => rfl
      | true => exact absurd hpa ha

/-- (ii) **`lcpOf` computes the longest common prefix**: for `k` within both
lengths, the length-`k` prefixes agree exactly up to the lcp. -/
theorem take_eq_iff_le_lcpOf (a b : List Nat) (k : Nat)
    (hk : k ≤ a.length ∧ k ≤ b.length) :
    a.take k = b.take k ↔ k ≤ lcpOf a b := by
  induction a generalizing b k with
  | nil =>
    have hk0 : k = 0 := by simpa using hk.1
    subst hk0
    simp [lcpOf]
  | cons x xs ih =>
    cases b with
    | nil =>
      have hk0 : k = 0 := by simpa using hk.2
      subst hk0
      simp [lcpOf]
    | cons y ys =>
      cases k with
      | zero => simp [lcpOf]
      | succ k' =>
        have hk1 : k' ≤ xs.length := by simpa using hk.1
        have hk2 : k' ≤ ys.length := by simpa using hk.2
        rw [List.take_succ_cons, List.take_succ_cons]
        simp only [List.cons.injEq]
        by_cases hxy : x = y
        · subst hxy
          simp only [lcpOf, beq_self_eq_true, if_true]
          rw [ih ys k' ⟨hk1, hk2⟩]
          simp only [true_and]
          omega
        · have hbeq : (x == y) = false := by
            rw [beq_eq_false_iff_ne]; exact hxy
          simp only [lcpOf, hbeq, Bool.false_eq_true, if_false]
          constructor
          · rintro ⟨h1, _⟩; exact absurd h1 hxy
          · intro h; omega

/-- the "suffix order" relation: suffix `a` is lexicographically ≤ suffix `b`. -/
def SAle (T : Text) : Nat → Nat → Prop := fun a b => lexLE (T.drop a) (T.drop b) = true

theorem SAle_trans (T : Text) {x y z : Nat} (h1 : SAle T x y) (h2 : SAle T y z) :
    SAle T x z :=
  lexLE_trans (T.drop x) (T.drop y) h1 (T.drop z) h2

/-- (i) `insSort` produces a lexicographically sorted list (for ANY input). -/
theorem insSort_pairwise (T : Text) : ∀ l : List Nat, (saOrder.insSort T l).Pairwise (SAle T) := by
  intro l
  induction l with
  | nil => exact List.Pairwise.nil
  | cons a rest ih =>
    simp only [saOrder.insSort]
    let p : Nat → Bool := fun b => lexLE (T.drop b) (T.drop a)
    change ((List.takeWhile p (saOrder.insSort T rest) ++ [a]) ++
              List.dropWhile p (saOrder.insSort T rest)).Pairwise (SAle T)
    have htw : List.Pairwise (SAle T) (List.takeWhile p (saOrder.insSort T rest)) :=
      List.Pairwise.sublist (List.takeWhile_sublist p) ih
    have hdw : List.Pairwise (SAle T) (List.dropWhile p (saOrder.insSort T rest)) :=
      List.Pairwise.sublist (List.dropWhile_sublist p) ih
    have hcross1 : ∀ b, b ∈ List.takeWhile p (saOrder.insSort T rest) → SAle T b a := by
      intro b hb
      exact mem_takeWhile_imp p _ b hb
    rw [List.pairwise_append]
    refine ⟨?_, hdw, ?_⟩
    · rw [List.pairwise_append]
      refine ⟨htw, List.pairwise_singleton (SAle T) a, ?_⟩
      intro b hb c hc
      rw [List.mem_singleton] at hc
      rw [hc]
      exact hcross1 b hb
    · intro b hb c hc
      rw [List.mem_append] at hb
      rcases hb with hb | hb
      · have hrest'' := ih
        rw [← List.takeWhile_append_dropWhile (p := p) (l := saOrder.insSort T rest)] at hrest''
        rw [List.pairwise_append] at hrest''
        exact hrest''.2.2 b hb c hc
      · rw [List.mem_singleton] at hb
        rw [hb]
        by_cases hdw0 : List.dropWhile p (saOrder.insSort T rest) = []
        · rw [hdw0] at hc; simp at hc
        · obtain ⟨c0, cs, hcs⟩ : ∃ c0 cs, List.dropWhile p (saOrder.insSort T rest) = c0 :: cs := by
            cases hd : List.dropWhile p (saOrder.insSort T rest) with
            | nil => exact absurd hd hdw0
            | cons c0 cs => exact ⟨c0, cs, rfl⟩
          have hdw_pw : (List.dropWhile p (saOrder.insSort T rest)).Pairwise (SAle T) :=
            List.Pairwise.sublist (List.dropWhile_sublist p) ih
          rw [hcs] at hdw_pw
          have htail : ∀ c, c ∈ cs → SAle T c0 c := (List.pairwise_cons.mp hdw_pw).1
          have hfail : p c0 = false := dropWhile_head_fails p hcs
          have hR : SAle T a c0 := by
            rcases lexLE_total (T.drop a) (T.drop c0) with h | h
            · exact h
            · exfalso
              have hpc : p c0 = true := h
              rw [hpc] at hfail
              exact absurd hfail (by simp)
          rw [hcs] at hc
          rw [List.mem_cons] at hc
          rcases hc with hc | hc
          · rw [hc]; exact hR
          · exact SAle_trans T hR (htail c hc)

/-- (i) **`saOrder` is sorted lexicographically.** -/
theorem saOrder_pairwise (T : Text) : (saOrder T).Pairwise (SAle T) := by
  unfold saOrder
  exact insSort_pairwise T (List.range T.length)

/-- (i) getElem form: earlier entries in `saOrder` have lexicographically smaller
suffixes. -/
theorem saOrder_sorted_getElem (T : Text) (i j : Nat)
    (hi : i < (saOrder T).length) (hj : j < (saOrder T).length) (hij : i < j) :
    lexLE (T.drop ((saOrder T)[i])) (T.drop ((saOrder T)[j])) = true := by
  have h := (List.pairwise_iff_getElem.mp (saOrder_pairwise T)) i j hi hj hij
  simpa [SAle] using h

/-- the lcp is at most the shorter word's length. -/
theorem lcpOf_le_min (a b : List Nat) : lcpOf a b ≤ min a.length b.length :=
  Nat.le_min.mpr ⟨lcpOf_le_left a b, lcpOf_le_right a b⟩

/-- the length-`lcpOf` prefixes of any two words agree. -/
theorem take_lcpOf_eq (a b : List Nat) : a.take (lcpOf a b) = b.take (lcpOf a b) :=
  (take_eq_iff_le_lcpOf a b (lcpOf a b) ⟨lcpOf_le_left a b, lcpOf_le_right a b⟩).mpr
    (Nat.le_refl _)

/-- a word is its own longest common prefix. -/
theorem lcpOf_self (a : List Nat) : lcpOf a a = a.length := by
  induction a with
  | nil => rfl
  | cons x xs ih =>
    simp only [lcpOf, beq_self_eq_true, if_true, ih, List.length_cons]
    omega

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

theorem le_foldl_max {α : Type} (f : α → Nat) (l : List α) (v : Nat) :
    v ≤ l.foldl (fun acc a => max acc (f a)) v := by
  induction l generalizing v with
  | nil => simp
  | cons a t ih =>
    simp only [List.foldl_cons]
    exact Nat.le_trans (by omega) (ih (max v (f a)))

theorem elem_le_foldl_max {α : Type} (f : α → Nat) (l : List α) (v : Nat) (a : α) (ha : a ∈ l) :
    f a ≤ l.foldl (fun acc a => max acc (f a)) v := by
  induction l generalizing v with
  | nil => exact absurd ha (List.not_mem_nil)
  | cons b t ih =>
    rw [List.mem_cons] at ha
    rcases ha with heq | ha
    · rw [heq]
      simp only [List.foldl_cons]
      exact Nat.le_trans (by omega) (le_foldl_max f t (max v (f b)))
    · simp only [List.foldl_cons]
      exact ih (max v (f b)) ha

theorem exists_ge_foldl_max {α : Type} (f : α → Nat) (l : List α) (h : l ≠ []) (v : Nat) :
    ∃ a ∈ l, l.foldl (fun acc a => max acc (f a)) v ≤ max v (f a) := by
  induction l generalizing v with
  | nil => exact absurd rfl h
  | cons b t ih =>
    by_cases ht : t = []
    · subst ht
      exact ⟨b, by simp, by simp⟩
    · obtain ⟨c, hc, hle⟩ := ih ht (max v (f b))
      rcases Nat.le_total (f c) (f b) with hcb | hbc
      · refine ⟨b, by simp, ?_⟩
        simp only [List.foldl_cons] at hle ⊢
        omega
      · refine ⟨c, by simp [hc], ?_⟩
        simp only [List.foldl_cons] at hle ⊢
        omega

theorem exists_argmax_f (l : List Nat) (h : l ≠ []) (f : Nat → Nat) :
    ∃ m ∈ l, ∀ y ∈ l, f y ≤ f m := by
  induction l with
  | nil => exact absurd rfl h
  | cons a t ih =>
    by_cases ht : t = []
    · subst ht
      refine ⟨a, by simp, ?_⟩
      intro y hy
      rw [List.mem_cons] at hy
      rcases hy with rfl | hy
      · omega
      · exact absurd hy (List.not_mem_nil)
    · obtain ⟨m, hm, hmax⟩ := ih ht
      rcases Nat.le_total (f a) (f m) with ham | hma
      · refine ⟨m, by simp [hm], ?_⟩
        intro y hy
        rw [List.mem_cons] at hy
        rcases hy with rfl | hy
        · exact ham
        · exact hmax y hy
      · refine ⟨a, by simp, ?_⟩
        intro y hy
        rw [List.mem_cons] at hy
        rcases hy with rfl | hy
        · omega
        · exact Nat.le_trans (hmax y hy) hma

theorem exists_min_le (l : List Nat) (h : l ≠ []) : ∃ m ∈ l, ∀ y ∈ l, m ≤ y := by
  induction l with
  | nil => exact absurd rfl h
  | cons a t ih =>
    by_cases ht : t = []
    · subst ht
      refine ⟨a, by simp, ?_⟩
      intro y hy
      rw [List.mem_cons] at hy
      rcases hy with rfl | hy
      · omega
      · exact absurd hy (List.not_mem_nil)
    · obtain ⟨m, hm, hmin⟩ := ih ht
      rcases Nat.le_total a m with ham | hma
      · refine ⟨a, by simp, ?_⟩
        intro y hy
        rw [List.mem_cons] at hy
        rcases hy with rfl | hy
        · omega
        · exact Nat.le_trans ham (hmin y hy)
      · refine ⟨m, by simp [hm], ?_⟩
        intro y hy
        rw [List.mem_cons] at hy
        rcases hy with rfl | hy
        · omega
        · exact hmin y hy

theorem nodup_map_of_inj {α β : Type} (f : α → β) :
    ∀ (l : List α), l.Nodup → (∀ a ∈ l, ∀ b ∈ l, f a = f b → a = b) → (l.map f).Nodup := by
  intro l
  induction l with
  | nil => intro _ _; exact List.nodup_nil
  | cons a t ih =>
    intro hnd hinj
    simp only [List.map_cons, List.nodup_cons] at hnd ⊢
    obtain ⟨hat, ht⟩ := hnd
    refine ⟨?_, ih ht (fun x hx y hy => hinj x (by simp [hx]) y (by simp [hy]))⟩
    intro hmem
    rw [List.mem_map] at hmem
    obtain ⟨b, hb, hfb⟩ := hmem
    exact hat ((hinj a (by simp) b (by simp [hb]) hfb.symm) ▸ hb)

theorem count_le_of_disjoint_witnesses :
    ∀ (S L : List Nat) (D : Nat → Nat → Prop),
      L.Nodup → (∀ r ∈ L, ∃ x ∈ S, D r x) →
      (∀ r1 ∈ L, ∀ r2 ∈ L, r1 ≠ r2 → ∀ x, D r1 x → D r2 x → False) →
      L.length ≤ S.length := by
  intro S L D
  induction L generalizing S with
  | nil => intro _ _ _; simp
  | cons r t ih =>
    intro hnd hw hd
    rw [List.nodup_cons] at hnd
    obtain ⟨hrt, htnd⟩ := hnd
    obtain ⟨x, hxS, hxD⟩ := hw r (by simp)
    have hle : t.length ≤ (S.erase x).length := by
      refine ih (S.erase x) htnd ?_ ?_
      · intro r' hr'
        obtain ⟨y, hyS, hyD⟩ := hw r' (by simp [hr'])
        have hyx : y ≠ x := by
          intro hyx
          exact hd r (by simp) r' (by simp [hr'])
            (fun h => hrt (h ▸ hr')) x hxD (hyx ▸ hyD)
        refine ⟨y, ?_, hyD⟩
        rw [List.mem_erase_of_ne hyx]
        exact hyS
      · intro r1 hr1 r2 hr2 hne y h1 h2
        exact hd r1 (by simp [hr1]) r2 (by simp [hr2]) hne y h1 h2
    have hpos : 0 < S.length := List.length_pos_of_mem hxS
    have hlen : (S.erase x).length = S.length - 1 := List.length_erase_of_mem hxS
    simp only [List.length_cons]
    omega



theorem pref_length (T : Text) (x : Nat) : (pref T x).length = min x T.length := by
  simp [pref, List.length_take]

theorem coversAt_eq_true (wc : List Nat) (x : Nat) (T : Text) :
    coversAt wc x T = true ↔
      wc.length ≤ (pref T x).length ∧ (pref T x).drop ((pref T x).length - wc.length) = wc := by
  unfold coversAt
  rw [Bool.and_eq_true]
  constructor
  · rintro ⟨h1, h2⟩
    exact ⟨of_decide_eq_true h1, (beq_iff_eq.mp h2).symm⟩
  · rintro ⟨h1, h2⟩
    exact ⟨decide_eq_true h1, beq_iff_eq.mpr h2.symm⟩

theorem coversAt_iff_suffix (wc : List Nat) (x : Nat) (T : Text) :
    coversAt wc x T = true ↔ ∃ u, pref T x = u ++ wc := by
  constructor
  · intro h
    obtain ⟨hlen, hdrop⟩ := (coversAt_eq_true wc x T).mp h
    refine ⟨(pref T x).take ((pref T x).length - wc.length), ?_⟩
    have htad := List.take_append_drop ((pref T x).length - wc.length) (pref T x)
    rw [hdrop] at htad
    exact htad.symm
  · rintro ⟨u, hu⟩
    rw [coversAt_eq_true]
    constructor
    · rw [hu, List.length_append]; omega
    · rw [hu, List.length_append]
      have : u.length + wc.length - wc.length = u.length := by omega
      rw [this, List.drop_left]

theorem drop_suffix_suffix {p a b : List Nat} (ha : p.drop (p.length - a.length) = a)
    (hb : p.drop (p.length - b.length) = b) (hlen : a.length ≤ b.length) :
    b.drop (b.length - a.length) = a := by
  have hlb : b.length ≤ p.length := by
    have := congrArg List.length hb
    rw [List.length_drop] at this
    omega
  have h2 : (p.length - b.length) + (b.length - a.length) = p.length - a.length := by omega
  calc b.drop (b.length - a.length)
      = (p.drop (p.length - b.length)).drop (b.length - a.length) :=
          congrArg (fun z => z.drop (b.length - a.length)) hb.symm
    _ = p.drop ((p.length - b.length) + (b.length - a.length)) := List.drop_drop
    _ = p.drop (p.length - a.length) := by rw [h2]
    _ = a := ha

theorem coversAt_suffix_of_coversAt {T : Text} {x : Nat} {a b : List Nat}
    (ha : coversAt a x T = true) (hb : coversAt b x T = true) (hlen : a.length ≤ b.length) :
    b.drop (b.length - a.length) = a := by
  obtain ⟨_, hda⟩ := (coversAt_eq_true a x T).mp ha
  obtain ⟨_, hdb⟩ := (coversAt_eq_true b x T).mp hb
  exact drop_suffix_suffix hda hdb hlen

theorem coversAt_of_suffix {T : Text} {x : Nat} {a b : List Nat}
    (hb : coversAt b x T = true) (h : b.drop (b.length - a.length) = a) :
    coversAt a x T = true := by
  obtain ⟨u, hu⟩ := (coversAt_iff_suffix b x T).mp hb
  rw [coversAt_iff_suffix]
  have hb_decomp : b = b.take (b.length - a.length) ++ a := by
    have htad := List.take_append_drop (b.length - a.length) b
    rw [h] at htad
    exact htad.symm
  refine ⟨u ++ b.take (b.length - a.length), ?_⟩
  rw [hu]
  conv =>
    lhs
    rw [hb_decomp]
  exact (List.append_assoc u (b.take (b.length - a.length)) a).symm

def wlen (p : List Nat × Nat) : Nat := p.1.length + 1

def maxW (T : Text) (x : Nat) : Nat := (covSet T x).foldl (fun acc p => max acc (wlen p)) 0

theorem wlen_congr (p : List Nat × Nat) : wlen p = (p.1 ++ [p.2]).length := by
  simp [wlen, List.length_append]

theorem wlen_le_maxW (T : Text) (x : Nat) (p : List Nat × Nat) (hp : p ∈ covSet T x) :
    wlen p ≤ maxW T x := elem_le_foldl_max wlen (covSet T x) 0 p hp

theorem exists_maxW {T : Text} {x : Nat} (hne : covSet T x ≠ []) :
    ∃ p ∈ covSet T x, maxW T x ≤ wlen p := by
  obtain ⟨p, hp, hle⟩ := exists_ge_foldl_max wlen (covSet T x) hne 0
  exact ⟨p, hp, by simpa [maxW, Nat.zero_max] using hle⟩

theorem maxW_pos {T : Text} {x : Nat} (hne : covSet T x ≠ []) : 1 ≤ maxW T x := by
  obtain ⟨p, hp, _⟩ := exists_maxW (T := T) (x := x) hne
  have h1 : 1 ≤ wlen p := by simp [wlen]
  exact Nat.le_trans h1 (wlen_le_maxW T x p hp)

theorem maxW_eq_zero {T : Text} {x : Nat} (h : covSet T x = []) : maxW T x = 0 := by
  simp [maxW, h]

theorem ScopeLe_refl (T : Text) (x : Nat) : ScopeLe T x x := fun _ hp => hp

theorem ScopeLe_trans (T : Text) {x y z : Nat} (hxy : ScopeLe T x y) (hyz : ScopeLe T y z) :
    ScopeLe T x z := fun p hp => hyz p (hxy p hp)

theorem maxW_le_of_ScopeLe {T : Text} {x y : Nat} (hne : covSet T x ≠ [])
    (hxy : ScopeLe T x y) : maxW T x ≤ maxW T y := by
  obtain ⟨p, hp, hle⟩ := exists_maxW (T := T) (x := x) hne
  exact Nat.le_trans hle (wlen_le_maxW T y p (hxy p hp))

theorem covSet_subset_of_maxW_witness {T : Text} {x y : Nat} {p : List Nat × Nat}
    (hmax : maxW T x ≤ wlen p)
    (hp_cov : coversAt (p.1 ++ [p.2]) x T = true)
    (hpy : coversAt (p.1 ++ [p.2]) y T = true) (hy : y ∈ positionsT T) (hx : IsMax T x) :
    ScopeLe T y x := by
  have hkey : ∀ q ∈ covSet T y, wlen q ≤ wlen p := by
    intro q hq
    by_cases hle : wlen q ≤ wlen p
    · exact hle
    · have hcon' : wlen p < wlen q := by omega
      have h1 : ScopeLe T x y := by
        intro s hs
        have hs_req : s ∈ requirements T := ((mem_covSet T x s).mp hs).1
        have hs_cov : coversAt (s.1 ++ [s.2]) x T = true := ((mem_covSet T x s).mp hs).2
        have hlen_s : (s.1 ++ [s.2]).length ≤ (p.1 ++ [p.2]).length := by
          have hle' : wlen s ≤ wlen p := Nat.le_trans (wlen_le_maxW T x s hs) hmax
          simpa [wlen] using hle'
        have hdrop := coversAt_suffix_of_coversAt hs_cov hp_cov hlen_s
        exact (mem_covSet T y s).mpr ⟨hs_req, coversAt_of_suffix hpy hdrop⟩
      have h2 : ScopeLe T y x := hx.2 y hy h1
      have hun : wlen q ≤ maxW T x := wlen_le_maxW T x q (h2 q hq)
      omega
  intro q hq
  have hq_req : q ∈ requirements T := ((mem_covSet T y q).mp hq).1
  have hq_cov : coversAt (q.1 ++ [q.2]) y T = true := ((mem_covSet T y q).mp hq).2
  have hlen_q : (q.1 ++ [q.2]).length ≤ (p.1 ++ [p.2]).length := by
    have h := hkey q hq
    simpa [wlen] using h
  have hdrop := coversAt_suffix_of_coversAt hq_cov hpy hlen_q
  exact (mem_covSet T x q).mpr ⟨hq_req, coversAt_of_suffix hp_cov hdrop⟩

theorem privacy_of_rep {T : Text} {r y : Nat} (hr : IsRep T r) (hy : y ∈ positionsT T)
    {p : List Nat × Nat} (hp : p ∈ covSet T r) (hmax : maxW T r ≤ wlen p) :
    p ∈ covSet T y → ScopeLe T y r := by
  intro hpy
  have hp_cov : coversAt (p.1 ++ [p.2]) r T = true := ((mem_covSet T r p).mp hp).2
  have hpy_cov : coversAt (p.1 ++ [p.2]) y T = true := ((mem_covSet T y p).mp hpy).2
  exact covSet_subset_of_maxW_witness hmax hp_cov hpy_cov hy hr.2.1


theorem occurs_eq_true (w : List Nat) (T : Text) (hw : w ≠ []) :
    occurs w T = true ↔ ∃ i, i + w.length ≤ T.length ∧ (T.drop i).take w.length = w := by
  unfold occurs
  by_cases hlen : w.length > T.length
  · rw [if_pos hlen]
    constructor
    · intro h; exact absurd h (by simp)
    · rintro ⟨i, hi, _⟩; omega
  · rw [if_neg hlen, Bool.or_eq_true, List.any_eq_true]
    constructor
    · rintro (h | ⟨i, hi, htake⟩)
      · exact absurd (List.isEmpty_iff.mp h) hw
      · rw [List.mem_range] at hi
        exact ⟨i, by omega, beq_iff_eq.mp htake⟩
    · rintro ⟨i, hi, htake⟩
      exact Or.inr ⟨i, by rw [List.mem_range]; omega, beq_iff_eq.mpr htake⟩

theorem exists_covers_of_occurs (w : List Nat) (T : Text) (hw : occurs w T = true)
    (hne : w ≠ []) : ∃ x, x ∈ positionsT T ∧ coversAt w x T = true := by
  obtain ⟨i, hi, htake⟩ := (occurs_eq_true w T hne).mp hw
  have hwlen : 1 ≤ w.length := by
    cases w with
    | nil => exact absurd rfl hne
    | cons a t => simp
  refine ⟨i + w.length, ?_, ?_⟩
  · rw [positionsT, List.mem_map]
    refine ⟨i + w.length - 1, ?_, by omega⟩
    rw [List.mem_range]
    omega
  · rw [coversAt_iff_suffix]
    refine ⟨T.take i, ?_⟩
    show pref T (i + w.length) = T.take i ++ w
    rw [pref, List.take_add, htake]


theorem exists_max_above (T : Text) (x : Nat) (hx : x ∈ positionsT T) (hne : covSet T x ≠ []) :
    ∃ m, m ∈ positionsT T ∧ ScopeLe T x m ∧ IsMax T m := by
  classical
  let d := (positionsT T).filter (fun y => decide (ScopeLe T x y))
  have hxd : x ∈ d := by
    refine List.mem_filter.mpr ⟨hx, ?_⟩
    exact decide_eq_true (ScopeLe_refl T x)
  obtain ⟨m, hmd, hmax⟩ := exists_argmax_f d (List.ne_nil_of_mem hxd) (maxW T)
  have hmP : m ∈ positionsT T := (List.mem_filter.mp hmd).1
  have hxm : ScopeLe T x m := of_decide_eq_true (List.mem_filter.mp hmd).2
  have hmne : covSet T m ≠ [] := by
    intro hemp
    have h0 : maxW T m = 0 := maxW_eq_zero hemp
    have hpos : 1 ≤ maxW T x := maxW_pos hne
    have hle : maxW T x ≤ maxW T m := hmax x hxd
    omega
  refine ⟨m, hmP, hxm, hmne, ?_⟩
  intro y hyP hym
  have hyd : y ∈ d := by
    refine List.mem_filter.mpr ⟨hyP, ?_⟩
    exact decide_eq_true (ScopeLe_trans T hxm hym)
  have hle : maxW T y ≤ maxW T m := hmax y hyd
  obtain ⟨pm, hpm, hmaxpm⟩ := exists_maxW (T := T) (x := m) hmne
  intro q hq
  have hq_req : q ∈ requirements T := ((mem_covSet T y q).mp hq).1
  have hq_cov : coversAt (q.1 ++ [q.2]) y T = true := ((mem_covSet T y q).mp hq).2
  have hpm_cov_y : coversAt (pm.1 ++ [pm.2]) y T = true :=
    ((mem_covSet T y pm).mp (hym pm hpm)).2
  have hlen : (q.1 ++ [q.2]).length ≤ (pm.1 ++ [pm.2]).length := by
    have h1 : wlen q ≤ maxW T y := wlen_le_maxW T y q hq
    have h2 : wlen q ≤ wlen pm := Nat.le_trans (Nat.le_trans h1 hle) hmaxpm
    simpa [wlen] using h2
  have hdrop := coversAt_suffix_of_coversAt hq_cov hpm_cov_y hlen
  have hpm_cov_m : coversAt (pm.1 ++ [pm.2]) m T = true := ((mem_covSet T m pm).mp hpm).2
  exact (mem_covSet T m q).mpr ⟨hq_req, coversAt_of_suffix hpm_cov_m hdrop⟩

theorem exists_rep_of_max (T : Text) (m : Nat) (hm : m ∈ positionsT T) (hmax : IsMax T m) :
    ∃ r, r ∈ positionsT T ∧ IsRep T r ∧ ScopeLe T r m ∧ ScopeLe T m r := by
  classical
  let c := (positionsT T).filter
    (fun y => decide (IsMax T y ∧ ScopeLe T m y ∧ ScopeLe T y m))
  have hmc : m ∈ c := by
    refine List.mem_filter.mpr ⟨hm, ?_⟩
    exact decide_eq_true ⟨hmax, ScopeLe_refl T m, ScopeLe_refl T m⟩
  obtain ⟨r, hrc, hmin⟩ := exists_min_le c (List.ne_nil_of_mem hmc)
  have hrP : r ∈ positionsT T := (List.mem_filter.mp hrc).1
  have hrc' : IsMax T r ∧ ScopeLe T m r ∧ ScopeLe T r m :=
    of_decide_eq_true (List.mem_filter.mp hrc).2
  obtain ⟨hrmax, hmr, hrm⟩ := hrc'
  refine ⟨r, hrP, ⟨hrP, hrmax, ?_⟩, hrm, hmr⟩
  intro y hyP hylt hymax hsc
  have hyc : y ∈ c := by
    refine List.mem_filter.mpr ⟨hyP, ?_⟩
    exact decide_eq_true ⟨hymax, ScopeLe_trans T hmr hsc.1, ScopeLe_trans T hsc.2 hrm⟩
  have := hmin y hyc
  omega

theorem sublist_of_mem_subsequences : ∀ (l S : List Nat), S ∈ subsequences l → S.Sublist l := by
  intro l
  induction l with
  | nil =>
    intro S h
    simp only [subsequences, List.mem_singleton] at h
    rw [h]
    exact List.Sublist.slnil
  | cons a t ih =>
    intro S h
    simp only [subsequences, List.mem_append, List.mem_map] at h
    rcases h with h | ⟨S', hS', hS'eq⟩
    · exact List.Sublist.cons a (ih S h)
    · rw [← hS'eq]
      exact List.Sublist.cons_cons a (ih S' hS')


noncomputable def reps (T : Text) : List Nat := by
  classical
  exact (positionsT T).filter (fun x => decide (IsRep T x))

theorem reps_suffixient (T : Text) : suffixient (reps T) T = true := by
  classical
  apply suffixient_of_witnesses
  intro p hp
  obtain ⟨_hsub, _hmaxw, hcext⟩ := (mem_requirements p.1 p.2 T).mp hp
  have hocc : occurs (p.1 ++ [p.2]) T = true := (mem_rightExts p.1 p.2 T).mp hcext
  have hne : p.1 ++ [p.2] ≠ [] := by simp
  obtain ⟨x0, hx0P, hx0cov⟩ := exists_covers_of_occurs (p.1 ++ [p.2]) T hocc hne
  have hcov0 : p ∈ covSet T x0 := (mem_covSet T x0 p).mpr ⟨hp, hx0cov⟩
  obtain ⟨m, hmP, hx0m, hmaxm⟩ :=
    exists_max_above T x0 hx0P (List.ne_nil_of_mem hcov0)
  obtain ⟨r, hrP, hrep, _hrm, hmr⟩ := exists_rep_of_max T m hmP hmaxm
  refine ⟨r, ?_, ?_⟩
  · exact List.mem_filter.mpr ⟨hrP, decide_eq_true hrep⟩
  · have hx0r : ScopeLe T x0 r := ScopeLe_trans T hx0m hmr
    exact ((mem_covSet T r p).mp (hx0r p hcov0)).2

theorem maxClassCount_eq_reps (T : Text) : maxClassCount T = (reps T).length := by
  unfold maxClassCount reps
  rfl

theorem maxClassCount_le_of_suffixient (T : Text) (S : List Nat)
    (hsub : S.Sublist (positionsT T)) (hs : suffixient S T = true) :
    maxClassCount T ≤ S.length := by
  classical
  have hposT : (positionsT T).Nodup := by
    unfold positionsT
    refine nodup_map_of_inj (fun x => x + 1) (List.range T.length) List.nodup_range ?_
    intro a _ b _ hab
    omega
  have hrepnd : (reps T).Nodup := by
    unfold reps
    exact hposT.filter _
  let D : Nat → Nat → Prop := fun r x =>
    x ∈ S ∧ ∃ p ∈ covSet T r, maxW T r ≤ wlen p ∧ p ∈ covSet T x
  have hw : ∀ r ∈ reps T, ∃ x ∈ S, D r x := by
    intro r hr
    have hrep : IsRep T r := of_decide_eq_true (List.mem_filter.mp hr).2
    have hne : covSet T r ≠ [] := hrep.2.1.1
    obtain ⟨p, hp, hmaxp⟩ := exists_maxW (T := T) (x := r) hne
    have hp_req : p ∈ requirements T := ((mem_covSet T r p).mp hp).1
    have hcover : IsCover S T := (isCover_iff_suffixient S T).mpr hs
    obtain ⟨x, hxS, hpx⟩ := hcover p hp_req
    exact ⟨x, hxS, hxS, p, hp, hmaxp, hpx⟩
  have hd : ∀ r1 ∈ reps T, ∀ r2 ∈ reps T, r1 ≠ r2 → ∀ x, D r1 x → D r2 x → False := by
    intro r1 hr1 r2 hr2 hne x h1 h2
    obtain ⟨hxS, p1, hp1, hmax1, hpx1⟩ := h1
    obtain ⟨_hxS2, p2, hp2, hmax2, hpx2⟩ := h2
    have hrep1 : IsRep T r1 := of_decide_eq_true (List.mem_filter.mp hr1).2
    have hrep2 : IsRep T r2 := of_decide_eq_true (List.mem_filter.mp hr2).2
    have hP1 : r1 ∈ positionsT T := (List.mem_filter.mp hr1).1
    have hP2 : r2 ∈ positionsT T := (List.mem_filter.mp hr2).1
    have hxP : x ∈ positionsT T := hsub.subset hxS
    have hs1 : ScopeLe T x r1 := privacy_of_rep hrep1 hxP hp1 hmax1 hpx1
    have hs2 : ScopeLe T x r2 := privacy_of_rep hrep2 hxP hp2 hmax2 hpx2
    have h12 : ScopeLe T r1 r2 := privacy_of_rep hrep2 hP1 hp2 hmax2 (hs1 p2 hpx2)
    have h21 : ScopeLe T r2 r1 := privacy_of_rep hrep1 hP2 hp1 hmax1 (hs2 p1 hpx1)
    rcases Nat.lt_trichotomy r1 r2 with hlt | heq | hgt
    · exact hrep2.2.2 r1 hP1 hlt hrep1.2.1 ⟨h21, h12⟩
    · exact hne heq
    · exact hrep1.2.2 r2 hP2 hgt hrep2.2.1 ⟨h12, h21⟩
  have hlen := count_le_of_disjoint_witnesses S (reps T) D hrepnd hw hd
  rw [maxClassCount_eq_reps]
  exact hlen




private theorem head_flatMap_le_aux (g : Nat → List (List Nat)) :
    ∀ (n : Nat), (∀ k, k < n → ∀ y ∈ g k, y.length = k) →
    ∀ (j : Nat), j < n → ∀ (x : List Nat), x ∈ g j →
      List.flatMap g (List.range n) ≠ [] ∧
      ∀ y, (List.flatMap g (List.range n)).head? = some y → y.length ≤ j := by
  intro n
  induction n with
  | zero =>
    intro hlen j hj
    omega
  | succ n ih =>
    intro hlen j hj x hx
    rw [List.range_succ, List.flatMap_append]
    simp only [List.flatMap_cons, List.flatMap_nil, List.append_nil]
    have hlen' : ∀ k, k < n → ∀ y ∈ g k, y.length = k := fun k hk => hlen k (by omega)
    by_cases hj' : j < n
    · have hIH := ih hlen' j hj' x hx
      constructor
      · intro h
        exact hIH.1 (List.append_eq_nil_iff.mp h).1
      · intro y hy
        rw [List.head?_append] at hy
        cases h1 : (List.flatMap g (List.range n)).head? with
        | none =>
          rw [h1, Option.none_or] at hy
          exact absurd (List.head?_eq_none_iff.mp h1) hIH.1
        | some y1 =>
          rw [h1, Option.some_or] at hy
          injection hy with hyy
          subst hyy
          exact hIH.2 y1 h1
    · have hjn : j = n := by omega
      constructor
      · intro h
        have hmem : x ∈ List.flatMap g (List.range n) ++ g n :=
          List.mem_append.mpr (Or.inr (hjn ▸ hx))
        rw [h] at hmem
        exact List.not_mem_nil hmem
      · intro y hy
        rw [List.head?_append] at hy
        cases h1 : (List.flatMap g (List.range n)).head? with
        | none =>
          rw [h1, Option.none_or] at hy
          have hy' : y ∈ g n := List.mem_of_mem_head? hy
          have := hlen n (by omega) y hy'
          omega
        | some y1 =>
          rw [h1, Option.some_or] at hy
          injection hy with hyy
          subst hyy
          have hy1 : y1 ∈ List.flatMap g (List.range n) := List.mem_of_mem_head? h1
          rw [List.mem_flatMap] at hy1
          obtain ⟨k, hk, hyk⟩ := hy1
          rw [List.mem_range] at hk
          have := hlen k (by omega) y1 hyk
          omega

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
  apply Nat.le_antisymm
  · exact chi_le_of_suffixient T (reps T) List.filter_sublist (reps_suffixient T)
  · unfold chi
    dsimp only
    split
    · rename_i d hd
      have hdmem : d ∈ List.flatMap (blk T) (List.range (T.length + 1)) :=
        List.mem_of_mem_head? hd
      rw [List.mem_flatMap] at hdmem
      obtain ⟨k, _hk, hdk⟩ := hdmem
      obtain ⟨hsub_seq, _hlen_d, hsuff_d⟩ := (mem_blk T k d).mp hdk
      have hsub : d.Sublist (positionsT T) := sublist_of_mem_subsequences _ _ hsub_seq
      exact maxClassCount_le_of_suffixient T d hsub hsuff_d
    · rename_i _hd
      have hthis : maxClassCount T ≤ (positionsT T).length := by
        rw [maxClassCount_eq_reps]
        unfold reps
        exact List.length_filter_le _ _
      simpa [positionsT] using hthis


/-! ### Reductions isolating the remaining obligations (statement-locked)

Each of the three open theorems above reduces to a single stream-level
characterization of the one-pass scan.  The reductions below are PROVED; the
obligations they isolate are empirically verified (0 counterexamples) by the
in-file `#eval`s below (exhaustive `{1,2}`, `|T| ≤ 6`: 729 texts) and by scratch
probes (320 texts: random 4-letter, random binary, single-symbol `[1]^k`,
periodic/repeated-pattern; `|T|` up to 100).

  (O1) DOMINATION — for `covering_given_stream`: every text position is
       `ScopeLe`-dominated by some emitted position,
       `∀ x ∈ positionsT T, ∃ y ∈ scan …, ScopeLe T x y`;
  (O2) NO DUPLICATES — `(scan …).Nodup`;
  (O3) MAXIMALITY — every emitted position is coverage-maximal (`IsMax T`);
  (O4) DISTINCT CLASSES — distinct emitted positions are never mutually
       `ScopeLe` (they sit in distinct maximal classes).

Intuitively (O1) says the scan emits a witness for every right-extension
context, and (O2)–(O4) say the emitted positions are one per maximal coverage
class.  Both are statements about the LCP-maxima semantics of `scanAux`; see
the `fmAux_good` provenance pattern for the style of invariant that proves
`isRunEdge`-style facts, and `scan_emits_run_edges` for the scan-side analogue. -/

/-- Counting: a duplicate-free list whose members carry pairwise-disjoint
witnesses in `m` is no longer than `m`. -/
theorem length_le_of_rel_inj :
    ∀ (l m : List Nat) (R : Nat → Nat → Prop),
      l.Nodup → (∀ x ∈ l, ∃ r ∈ m, R x r) →
      (∀ x ∈ l, ∀ y ∈ l, x ≠ y → ∀ r, R x r → R y r → False) →
      l.length ≤ m.length := by
  intro l
  induction l with
  | nil => intro m R _ _ _; simp
  | cons x t ih =>
    intro m R hnd hw hd
    rw [List.nodup_cons] at hnd
    obtain ⟨hxt, htnd⟩ := hnd
    obtain ⟨r, hrm, hxr⟩ := hw x (by simp)
    have hle : t.length ≤ (m.erase r).length := by
      refine ih (m.erase r) R htnd ?_ ?_
      · intro y hy
        obtain ⟨r', hr'm, hyr'⟩ := hw y (by simp [hy])
        have hne : r' ≠ r := by
          intro heq
          exact hd x (by simp) y (by simp [hy]) (fun hxy => hxt (hxy ▸ hy)) r hxr (heq ▸ hyr')
        exact ⟨r', (List.mem_erase_of_ne hne).mpr hr'm, hyr'⟩
      · intro y hy z hz hyz r' hyr' hzr'
        exact hd y (by simp [hy]) z (by simp [hz]) hyz r' hyr' hzr'
    have hpos : 0 < m.length := List.length_pos_of_mem hrm
    have hlen : (m.erase r).length = m.length - 1 := List.length_erase_of_mem hrm
    simp only [List.length_cons]
    omega

/-- **(O1) Domination implies covering.** -/
theorem covering_of_domination (T : Text) (hT : positive T = true)
    (hdom : ∀ x ∈ positionsT T, ∃ y, y ∈ scan (T.length + 1) (triplesOf T) ∧ ScopeLe T x y) :
    suffixient (scan (T.length + 1) (triplesOf T)) T = true := by
  apply suffixient_of_witnesses
  intro p hp
  obtain ⟨_hw, _hr, hcext⟩ := (mem_requirements p.1 p.2 T).mp hp
  have hocc : occurs (p.1 ++ [p.2]) T = true := (mem_rightExts p.1 p.2 T).mp hcext
  have hne : p.1 ++ [p.2] ≠ [] := by simp
  obtain ⟨x0, hx0, hcov0⟩ := exists_covers_of_occurs (p.1 ++ [p.2]) T hocc hne
  obtain ⟨y, hy, hle⟩ := hdom x0 hx0
  exact ⟨y, hy, ((mem_covSet T y p).mp (hle p ((mem_covSet T x0 p).mpr ⟨hp, hcov0⟩))).2⟩

/-- **(O2)+(O3)+(O4) imply the minimality lower bound** `|scan| ≤ χ` (the half
that Lemma 34 supplies).  The upper half `χ ≤ |scan|` is `chi_le_of_suffixient_mem`
instantiated at the scan, i.e. requires only covering. -/
theorem minimality_lower_of_scan_classes (T : Text) (hT : positive T = true)
    (hnd : (scan (T.length + 1) (triplesOf T)).Nodup)
    (hmax : ∀ x ∈ scan (T.length + 1) (triplesOf T), IsMax T x)
    (hdisj : ∀ x ∈ scan (T.length + 1) (triplesOf T),
      ∀ y ∈ scan (T.length + 1) (triplesOf T), ScopeLe T x y → ScopeLe T y x → x = y) :
    (scan (T.length + 1) (triplesOf T)).length ≤ chi T := by
  classical
  rw [chi_eq_maxClasses T hT, maxClassCount_eq_reps]
  refine length_le_of_rel_inj (scan (T.length + 1) (triplesOf T)) (reps T)
    (fun x r => r ∈ positionsT T ∧ IsRep T r ∧ ScopeLe T r x ∧ ScopeLe T x r) hnd ?_ ?_
  · intro x hx
    obtain ⟨r, hrP, hrep, hrx, hxr⟩ :=
      exists_rep_of_max T x (scan_mem_positionsT T hT x hx) (hmax x hx)
    exact ⟨r, List.mem_filter.mpr ⟨hrP, decide_eq_true hrep⟩, hrP, hrep, hrx, hxr⟩
  · intro x hx y hy hxy r hxr hyr
    obtain ⟨_hrP, _hrep, hrx, hxrle⟩ := hxr
    obtain ⟨_hrP2, _hrep2, hry, hyrle⟩ := hyr
    have hxy' : ScopeLe T x y := ScopeLe_trans T hxrle hry
    have hyx' : ScopeLe T y x := ScopeLe_trans T hyrle hrx
    exact hxy (hdisj x hx y hy hxy' hyx')

/-- Consolidated: covering plus the three scan-side class facts give
`minimality` (χ = |scan|). -/
theorem minimality_of_scan_classes (T : Text) (hT : positive T = true)
    (hnd : (scan (T.length + 1) (triplesOf T)).Nodup)
    (hmax : ∀ x ∈ scan (T.length + 1) (triplesOf T), IsMax T x)
    (hdisj : ∀ x ∈ scan (T.length + 1) (triplesOf T),
      ∀ y ∈ scan (T.length + 1) (triplesOf T), ScopeLe T x y → ScopeLe T y x → x = y)
    (hcover : suffixient (scan (T.length + 1) (triplesOf T)) T = true) :
    (scan (T.length + 1) (triplesOf T)).length = chi T := by
  apply Nat.le_antisymm
  · exact minimality_lower_of_scan_classes T hT hnd hmax hdisj
  · exact chi_le_of_suffixient_mem T _ (fun x hx => scan_mem_positionsT T hT x hx) hcover

/-- Reduction for `fm_equivalence` via the Lane-B **event bridge**: if `scan`
and `fmSpec` emit the same multiset of `(char, position)` events and the
per-character candidate tables agree at the end of the stream, their output
sets agree.  Stated so the next lane can attack the two machines' event
processes (the 3,226-text differential in the file header) instead of the
machines themselves. -/
theorem fm_equivalence_of_event_bridge (T : Text)
    (hperm : (scan (T.length + 1) (triplesOf T)).mergeSort (· ≤ ·)
      = (fmSpec (T.length + 1) (triplesOf T)).mergeSort (· ≤ ·)) :
    ∀ x, x ∈ scan (T.length + 1) (triplesOf T) ↔ x ∈ fmSpec (T.length + 1) (triplesOf T) := by
  intro x
  have hm : (scan (T.length + 1) (triplesOf T)).mergeSort (· ≤ ·)
      = (fmSpec (T.length + 1) (triplesOf T)).mergeSort (· ≤ ·) := hperm
  constructor
  · intro hx
    have : x ∈ (scan (T.length + 1) (triplesOf T)).mergeSort (· ≤ ·) :=
      List.mem_mergeSort.mpr hx
    rw [hm] at this
    exact List.mem_mergeSort.mp this
  · intro hx
    have : x ∈ (fmSpec (T.length + 1) (triplesOf T)).mergeSort (· ≤ ·) :=
      List.mem_mergeSort.mpr hx
    rw [← hm] at this
    exact List.mem_mergeSort.mp this

/-! ## Joint scan/FM bisimulation machinery (merged from FmJoint.lean,
2026-09-26): the coupling invariant `Coupled`/`JointInv`, the per-char
boundary step lemmas `coupled_step_notinv`/`coupled_step_inv`, and the
window/PSV/NSV infrastructure they rest on.  Merged here because the
assembly for `fm_equivalence_bounded` (below) consumes it and Sxgc.lean
cannot import a downstream file. -/

def dT : Triple := ⟨0, 0, 0⟩

def Ls (ts : List Triple) : List Nat := ts.map (fun t => t.lcp)

def isBnd (ts : List Triple) (i : Nat) : Bool :=
  decide (1 ≤ i ∧ i < ts.length ∧ (ts.getD i dT).c ≠ (ts.getD (i-1) dT).c)

/-- last boundary index strictly below i (0 = none) -/
def prevBnd (ts : List Triple) : Nat → Nat
  | 0 => 0
  | i+1 => if isBnd ts i then i else prevBnd ts i

/-- min lcp over the OPEN window (pb, i); MAXINT if the window is empty -/
def winOpen (L : List Nat) (pb i : Nat) : Int :=
  if i ≤ pb then MAXINT
  else ((List.range (i - pb - 1)).map (fun k => (L.getD (pb + 1 + k) 0 : Int))).foldl min MAXINT

/-! ### List helpers -/

theorem getD_append {α : Type} : ∀ (l₁ l₂ : List α) (i : Nat) (d : α),
    (l₁ ++ l₂).getD i d = if i < l₁.length then l₁.getD i d else l₂.getD (i - l₁.length) d := by
  intro l₁
  induction l₁ with
  | nil => intro l₂ i d; simp
  | cons a as ih =>
    intro l₂ i d
    match i with
    | 0 => simp
    | i'+1 =>
      have h1 : (a :: as ++ l₂).getD (i'+1) d = (as ++ l₂).getD i' d := List.getD_cons_succ
      rw [h1, ih l₂ i' d]
      by_cases h : i' < as.length
      · have h2 : i' + 1 < (a :: as).length := by simp; omega
        have h3 : (a :: as).getD (i'+1) d = as.getD i' d := List.getD_cons_succ
        rw [if_pos h2, if_pos h, h3]
      · have h2 : ¬ (i' + 1 < (a :: as).length) := by simp; omega
        rw [if_neg h2, if_neg h]
        have h4 : i' + 1 - (a :: as).length = i' - as.length := by
          have : (a :: as).length = as.length + 1 := rfl
          omega
        rw [h4]

theorem getD_map_range {β : Type} (f : Nat → β) (n i : Nat) (d : β) (h : i < n) :
    ((List.range n).map f).getD i d = f i := by
  induction n with
  | zero => omega
  | succ n ih =>
    have hr : List.range (n+1) = List.range n ++ [n] := List.range_succ
    rw [hr, List.map_append, getD_append]
    simp only [List.length_map, List.length_range, List.length_singleton, Nat.add_zero]
    by_cases hlt : i < n
    · rw [if_pos hlt]
      exact ih hlt
    · rw [if_neg hlt]
      have : i = n := by omega
      subst this
      simp

theorem getD_mem {α : Type} : ∀ (l : List α) (i : Nat) (d : α), i < l.length → l.getD i d ∈ l := by
  intro l
  induction l with
  | nil => intro i d h; simp at h
  | cons a as ih =>
    intro i
    match i with
    | 0 => intro d h; exact List.mem_cons_self
    | i'+1 =>
      intro d h
      simp only [List.length_cons] at h
      exact List.mem_cons_of_mem _ (ih i' d (by omega))

theorem foldl_min_le_all : ∀ (l : List Int) (a : Int),
    (∀ x ∈ l, l.foldl min a ≤ x) ∧ l.foldl min a ≤ a := by
  intro l
  induction l with
  | nil =>
    intro a
    refine ⟨fun x hx => absurd hx (by simp), ?_⟩
    show a ≤ a
    omega
  | cons b bs ih =>
    intro a
    obtain ⟨h1, h2⟩ := ih (min a b)
    refine ⟨?_, ?_⟩
    · intro x hx
      rcases List.mem_cons.mp hx with rfl | hm
      · have : (x :: bs).foldl min a ≤ min a x := h2
        omega
      · exact h1 x hm
    · have : (b :: bs).foldl min a ≤ min a b := h2
      omega

theorem foldl_min_lt : ∀ (l : List Int) (a q : Int),
    l.foldl min a < q → (∃ x ∈ l, x < q) ∨ a < q := by
  intro l
  induction l with
  | nil => intro a q h; exact Or.inr h
  | cons b bs ih =>
    intro a q h
    rcases ih (min a b) q h with ⟨x, hx, hxq⟩ | hq
    · exact Or.inl ⟨x, List.mem_cons_of_mem _ hx, hxq⟩
    · by_cases h1 : b < q
      · exact Or.inl ⟨b, List.mem_cons_self, h1⟩
      · exact Or.inr (by have : min a b < q := hq; omega)

theorem foldl_min_ge0 : ∀ (l : List Int) (a : Int), 0 ≤ a →
    (∀ x ∈ l, 0 ≤ x) → 0 ≤ l.foldl min a := by
  intro l
  induction l with
  | nil => intro a ha _; exact ha
  | cons b bs ih =>
    intro a ha hall
    have hb : 0 ≤ b := hall b List.mem_cons_self
    have hall' : ∀ x ∈ bs, 0 ≤ x := fun x hx => hall x (List.mem_cons_of_mem _ hx)
    exact ih (min a b) (by omega) hall'

theorem foldl_min_mem : ∀ (l : List Int) (a : Int),
    (∃ x ∈ l, l.foldl min a = x) ∨ l.foldl min a = a := by
  intro l
  induction l with
  | nil => intro a; exact Or.inr rfl
  | cons b bs ih =>
    intro a
    rw [List.foldl_cons]
    rcases ih (min a b) with ⟨x, hx, hxe⟩ | h
    · exact Or.inl ⟨x, List.mem_cons_of_mem _ hx, hxe⟩
    · by_cases hab : min a b = a
      · exact Or.inr (by omega)
      · exact Or.inl ⟨b, List.mem_cons_self, by omega⟩

/-! ### winOpen lemmas -/

theorem winOpen_ge {L : List Nat} {pb i q : Nat} (h : (q : Int) ≤ winOpen L pb i) :
    ∀ j, pb < j → j < i → (q : Int) ≤ L.getD j 0 := by
  intro j h1 h2
  unfold winOpen at h
  split at h
  · omega
  · have hk : j - pb - 1 < i - pb - 1 := by omega
    have helem : ((List.range (i - pb - 1)).map (fun k => (L.getD (pb + 1 + k) 0 : Int))).getD (j - pb - 1) (0 : Int)
        = (L.getD j 0 : Int) := by
      rw [getD_map_range _ _ _ _ hk]
      have hidx : pb + 1 + (j - pb - 1) = j := by omega
      rw [hidx]
    have hmem := getD_mem ((List.range (i - pb - 1)).map (fun k => (L.getD (pb + 1 + k) 0 : Int)))
      (j - pb - 1) (0 : Int) (by rw [List.length_map, List.length_range]; omega)
    rw [helem] at hmem
    have hfold := (foldl_min_le_all
      (List.map (fun k => (L.getD (pb + 1 + k) 0 : Int)) (List.range (i - pb - 1))) MAXINT).1 _ hmem
    omega

theorem winOpen_lt {L : List Nat} {pb i : Nat} {q : Int} (hqb : q ≤ MAXINT)
    (h : winOpen L pb i < q) : ∃ j, pb < j ∧ j < i ∧ (L.getD j 0 : Int) < q := by
  unfold winOpen at h
  split at h
  · omega
  · rcases foldl_min_lt _ _ _ h with ⟨x, hx, hxq⟩ | hmax
    · rw [List.mem_map] at hx
      obtain ⟨k, hk, hxk⟩ := hx
      rw [List.mem_range] at hk
      refine ⟨pb + 1 + k, by omega, by omega, ?_⟩
      omega
    · omega

theorem winOpen_ach {L : List Nat} {pb i : Nat} (h : winOpen L pb i < MAXINT) :
    ∃ j, pb < j ∧ j < i ∧ (L.getD j 0 : Int) = winOpen L pb i := by
  unfold winOpen at h ⊢
  split at h
  · omega
  · rename_i hnp
    have hpi : pb < i := by omega
    rcases foldl_min_mem
      ((List.range (i - pb - 1)).map (fun k => (L.getD (pb + 1 + k) 0 : Int))) MAXINT with ⟨x, hx, hxe⟩ | hmax
    · rw [List.mem_map] at hx
      obtain ⟨k, hk, hxk⟩ := hx
      rw [List.mem_range] at hk
      have h1 : pb < pb + 1 + k := by omega
      have h2 : pb + 1 + k < i := by omega
      refine ⟨pb + 1 + k, h1, h2, ?_⟩
      rw [if_neg hnp, hxe, hxk]
    · omega

theorem winOpen_geI {L : List Nat} {pb i : Nat} {q : Int} (h : q ≤ winOpen L pb i) :
    ∀ j, pb < j → j < i → q ≤ L.getD j 0 := by
  intro j h1 h2
  by_cases hq : (0 : Int) ≤ q
  · have hqle : (q.toNat : Int) ≤ winOpen L pb i := by omega
    have := winOpen_ge (L := L) (pb := pb) (i := i) (q := q.toNat) hqle j h1 h2
    have hqc : (q.toNat : Int) = q := by omega
    omega
  · have h0 : (0 : Int) ≤ L.getD j 0 := by omega
    omega

theorem winOpen_extend {L : List Nat} {pb i : Nat} (hpb : pb < i) :
    winOpen L pb (i+1) = min (winOpen L pb i) (L.getD i 0) := by
  have h1 : ¬ (i + 1 ≤ pb) := by omega
  have h2 : ¬ (i ≤ pb) := by omega
  unfold winOpen
  rw [if_neg h1, if_neg h2]
  have hr : List.range (i + 1 - pb - 1) = List.range (i - pb - 1) ++ [i - pb - 1] := by
    have hnn : i + 1 - pb - 1 = (i - pb - 1) + 1 := by omega
    rw [hnn, List.range_succ]
  have hmap : ((List.range (i + 1 - pb - 1)).map (fun k => (L.getD (pb + 1 + k) 0 : Int)))
      = ((List.range (i - pb - 1)).map (fun k => (L.getD (pb + 1 + k) 0 : Int))) ++ [(L.getD (pb + 1 + (i - pb - 1)) 0 : Int)] := by
    rw [hr, List.map_append]
    simp
  rw [hmap, List.foldl_append]
  have hlast : (L.getD (pb + 1 + (i - pb - 1)) 0 : Int) = (L.getD i 0 : Int) := by
    have hidx : pb + 1 + (i - pb - 1) = i := by omega
    rw [hidx]
  rw [hlast]
  simp [List.foldl]

theorem winOpen_fresh (L : List Nat) (i : Nat) : winOpen L i (i+1) = MAXINT := by
  have h : ¬ (i + 1 ≤ i) := by omega
  unfold winOpen
  rw [if_neg h]
  have : i + 1 - i - 1 = 0 := by omega
  rw [this]
  simp

theorem winOpen_nonneg (L : List Nat) (pb i : Nat) : 0 ≤ winOpen L pb i := by
  unfold winOpen
  split
  · have : (0 : Int) ≤ MAXINT := by decide
    omega
  · exact foldl_min_ge0 _ _ (by decide)
      (fun x hx => by
        rcases List.mem_map.mp hx with ⟨k, _, hk⟩
        rw [← hk]
        omega)

/-! ### prevBnd lemmas -/

theorem prevBnd_bnd (ts : List Triple) (i : Nat) (hb : isBnd ts i = true) :
    prevBnd ts (i+1) = i := by simp [prevBnd, hb]

theorem prevBnd_nonbnd (ts : List Triple) (i : Nat) (hb : isBnd ts i = false) :
    prevBnd ts (i+1) = prevBnd ts i := by simp [prevBnd, hb]

theorem prevBnd_lt (ts : List Triple) : ∀ i, prevBnd ts i < i ∨ i = 0 := by
  intro i
  induction i with
  | zero => exact Or.inr rfl
  | succ i ih =>
    by_cases hb : isBnd ts i = true
    · simp [prevBnd, hb]
    · have hbf : isBnd ts i = false := by
        have : ¬ (isBnd ts i = true) := hb
        simp [Bool.not_eq_true] at this
        exact this
      rcases ih with h | h
      · exact Or.inl (by
          have heq : prevBnd ts (i+1) = prevBnd ts i := prevBnd_nonbnd ts i hbf
          omega)
      · exact Or.inl (by
          have heq : prevBnd ts (i+1) = prevBnd ts i := prevBnd_nonbnd ts i hbf
          rw [h] at heq ⊢
          simp [prevBnd] at heq ⊢)

theorem prevBnd_ge_of_bnd (ts : List Triple) : ∀ i b, isBnd ts b = true → b < i → b ≤ prevBnd ts i := by
  intro i
  induction i with
  | zero => intro b _ hb; omega
  | succ i ih =>
    intro b hbt hbl
    by_cases h : isBnd ts i = true
    · rw [prevBnd_bnd ts i h]
      by_cases h1 : b < i
      · exact Nat.le_of_lt h1
      · have hbeq : b = i := by omega
        omega
    · have hbf : isBnd ts i = false := by
        have : ¬ (isBnd ts i = true) := h
        simp [Bool.not_eq_true] at this
        exact this
      have hbl' : b < i := by
        have hne : b ≠ i := by
          intro hbe
          rw [hbe] at hbt
          rw [hbt] at hbf
          exact absurd hbf (by simp)
        omega
      rw [prevBnd_nonbnd ts i hbf]
      exact ih b hbt hbl'

/-! ### PSV/NSV characterizations -/

def psvFilter (L : List Nat) (i : Nat) : List Nat :=
  (List.range i).filter (fun j => decide (L.getD j 0 < L.getD i 0))

theorem range_pairwise (n : Nat) : (List.range n).Pairwise (fun a b => a < b) := by
  induction n with
  | zero => simp
  | succ n ih =>
    rw [List.range_succ, List.pairwise_append]
    exact ⟨ih, by simp, fun x hx y hy => by
      rw [List.mem_range] at hx
      simp only [List.mem_singleton] at hy
      rcases hy with rfl
      omega⟩

theorem psvFilter_pairwise (L : List Nat) (i : Nat) : (psvFilter L i).Pairwise (fun a b => a < b) :=
  (range_pairwise i).filter _

theorem pairwise_lt_last : ∀ {l : List Nat}, l.Pairwise (fun a b => a < b) → ∀ x : Nat,
    x ∈ l → ∃ y, l.getLast? = some y ∧ x ≤ y := by
  intro l
  induction l with
  | nil => intro h x hx; cases hx
  | cons a as ih =>
    intro h x hx
    cases h with
    | cons hhead hp =>
      rcases List.mem_cons.mp hx with hxa | hxa
      · rw [hxa]
        cases as with
        | nil => exact ⟨a, by rw [List.getLast?_cons]; simp, Nat.le_refl _⟩
        | cons b bs =>
          obtain ⟨y, hy, hby⟩ := ih hp b List.mem_cons_self
          refine ⟨y, ?_, ?_⟩
          · rw [List.getLast?_cons_cons]; exact hy
          · have hab : a < b := hhead b List.mem_cons_self
            omega
      · obtain ⟨y, hy, hby⟩ := ih hp x hxa
        cases as with
        | nil => cases hxa
        | cons b bs =>
          refine ⟨y, ?_, hby⟩
          rw [List.getLast?_cons_cons]; exact hy

theorem prevSmaller_mem (L : List Nat) (i : Nat) (h : prevSmaller L i ≠ -1) :
    ∃ j : Nat, prevSmaller L i = (j : Int) ∧ j < i ∧ L.getD j 0 < L.getD i 0 := by
  unfold prevSmaller at h ⊢
  match hget : ((List.range i).filter (fun j => decide (L.getD j 0 < L.getD i 0))).getLast? with
  | none =>
    simp only [hget] at h
    exact absurd rfl h
  | some j =>
    have hmem : j ∈ (List.range i).filter (fun j => decide (L.getD j 0 < L.getD i 0)) :=
      List.mem_of_getLast? hget
    rw [List.mem_filter, List.mem_range] at hmem
    exact ⟨j, rfl, hmem.1, of_decide_eq_true hmem.2⟩

theorem prevSmaller_ge (L : List Nat) (i j : Nat) (hj : j < i)
    (hl : L.getD j 0 < L.getD i 0) : (j : Int) ≤ prevSmaller L i := by
  have hmem : j ∈ psvFilter L i := by
    rw [psvFilter, List.mem_filter, List.mem_range]
    exact ⟨hj, decide_eq_true hl⟩
  obtain ⟨y, hy, hby⟩ := pairwise_lt_last (psvFilter_pairwise L i) j hmem
  simp only [prevSmaller]
  simp only [psvFilter] at hy
  rw [hy]
  simp only
  omega

theorem prevSmaller_none (L : List Nat) (i : Nat) (h : prevSmaller L i = -1) :
    ∀ j, j < i → L.getD i 0 ≤ L.getD j 0 := by
  intro j hj
  by_cases hle : L.getD i 0 ≤ L.getD j 0
  · exact hle
  · have hl : L.getD j 0 < L.getD i 0 := by omega
    have hmem : j ∈ psvFilter L i := by
      rw [psvFilter, List.mem_filter, List.mem_range]
      exact ⟨hj, decide_eq_true hl⟩
    obtain ⟨y, hy, hby⟩ := pairwise_lt_last (psvFilter_pairwise L i) j hmem
    simp only [psvFilter] at hy
    simp only [prevSmaller] at h
    rw [hy] at h
    simp only at h
    omega

theorem prevSmaller_eq_neg1 (L : List Nat) (i : Nat) (h : ∀ j, j < i → L.getD i 0 ≤ L.getD j 0) :
    prevSmaller L i = -1 := by
  by_cases hcon : prevSmaller L i = -1
  · exact hcon
  · obtain ⟨j, hj1, hj2, hj3⟩ := prevSmaller_mem L i hcon
    have := h j hj2
    omega

def nsvFilter (L : List Nat) (i : Nat) : List Nat :=
  (List.range L.length).filter (fun j => decide (i < j ∧ L.getD j 0 < L.getD i 0))

theorem nsvFilter_pairwise (L : List Nat) (i : Nat) : (nsvFilter L i).Pairwise (fun a b => a < b) :=
  (range_pairwise L.length).filter _

theorem pairwise_lt_head : ∀ {l : List Nat}, l.Pairwise (fun a b => a < b) → ∀ x : Nat,
    x ∈ l → ∃ y, l.head? = some y ∧ y ≤ x := by
  intro l
  induction l with
  | nil => intro h x hx; cases hx
  | cons a as ih =>
    intro h x hx
    cases h with
    | cons hhead hp =>
      rcases List.mem_cons.mp hx with hxa | hxa
      · rw [hxa]
        exact ⟨a, List.head?_cons, Nat.le_refl _⟩
      · obtain ⟨y, hy, hby⟩ := ih hp x hxa
        have hymem : y ∈ as := List.mem_of_head? hy
        have hay : a < y := hhead y hymem
        exact ⟨a, List.head?_cons, by omega⟩

theorem nextSmaller_mem (L : List Nat) (i : Nat) (h : nextSmaller L i ≠ L.length + 1) :
    ∃ k : Nat, nextSmaller L i = k ∧ i < k ∧ L.getD k 0 < L.getD i 0 := by
  unfold nextSmaller at h ⊢
  match hget : ((List.range L.length).filter
      (fun j => decide (i < j ∧ L.getD j 0 < L.getD i 0))).head? with
  | none =>
    simp only [hget] at h
    exact absurd rfl h
  | some k =>
    have hmem : k ∈ (List.range L.length).filter
        (fun j => decide (i < j ∧ L.getD j 0 < L.getD i 0)) :=
      List.mem_of_head? hget
    rw [List.mem_filter, List.mem_range] at hmem
    obtain ⟨hk1, hk2⟩ := of_decide_eq_true hmem.2
    exact ⟨k, rfl, hk1, hk2⟩

theorem nextSmaller_le (L : List Nat) (i k : Nat) (hik : i < k) (hkl : k < L.length)
    (hl : L.getD k 0 < L.getD i 0) : nextSmaller L i ≤ k := by
  have hmem : k ∈ nsvFilter L i := by
    rw [nsvFilter, List.mem_filter, List.mem_range]
    exact ⟨by omega, decide_eq_true ⟨hik, hl⟩⟩
  obtain ⟨y, hy, hby⟩ := pairwise_lt_head (nsvFilter_pairwise L i) k hmem
  simp only [nextSmaller]
  simp only [nsvFilter] at hy
  rw [hy]
  simp only
  omega

theorem nextSmaller_none (L : List Nat) (i : Nat) (h : nextSmaller L i = L.length + 1) :
    ∀ k, i < k → k < L.length → L.getD i 0 ≤ L.getD k 0 := by
  intro k hk hkl
  by_cases hle : L.getD i 0 ≤ L.getD k 0
  · exact hle
  · have hl : L.getD k 0 < L.getD i 0 := by omega
    have hmem : k ∈ nsvFilter L i := by
      rw [nsvFilter, List.mem_filter, List.mem_range]
      exact ⟨by omega, decide_eq_true ⟨hk, hl⟩⟩
    obtain ⟨y, hy, hby⟩ := pairwise_lt_head (nsvFilter_pairwise L i) k hmem
    have hymem : y ∈ nsvFilter L i := List.mem_of_head? hy
    simp only [nsvFilter] at hy
    simp only [nextSmaller] at h
    rw [hy] at h
    simp only at h
    simp only [nsvFilter, List.mem_filter, List.mem_range] at hymem
    omega

/-! ### psvList/nsvList indexing -/

theorem psvList_getD (L : List Nat) (i : Nat) (h : i < L.length) :
    (psvList L).getD i (-1) = prevSmaller L i := by
  have : (psvList L).getD i (-1) = ((List.range L.length).map (fun j => prevSmaller L j)).getD i (-1) := rfl
  rw [this, getD_map_range _ _ _ _ h]

theorem nsvList_getD (L : List Nat) (i : Nat) (h : i < L.length) :
    (nsvList L).getD i (L.length + 1) = nextSmaller L i := by
  have : (nsvList L).getD i (L.length + 1) = ((List.range L.length).map (fun j => nextSmaller L j)).getD i (L.length+1) := rfl
  rw [this, getD_map_range _ _ _ _ h]

/-! ### per-char machine lemmas -/

theorem foldl_evalStepGo_len (l : Int) : ∀ (n : Nat) (out : List Nat) (R : List Cand),
    ((List.range n).foldl (evalStepGo l) (out, R)).2.length = R.length := by
  intro n
  induction n with
  | zero => intro out R; simp only [List.range_zero, List.foldl_nil, Prod.snd]
  | succ n ih =>
    intro out R
    rw [List.range_succ, List.foldl_append]
    simp only [List.foldl_cons, List.foldl_nil]
    rcases hf : ((List.range n).foldl (evalStepGo l) (out, R)) with ⟨out₁, R₁⟩
    simp only [evalStepGo]
    have hlen : R₁.length = R.length := by
      have := ih out R
      rw [hf] at this
      exact this
    by_cases hlt : l < (getR R₁ (n+1)).len
    · rw [if_pos hlt]
      simp only [Prod.snd]
      rw [List.length_set]
      exact hlen
    · rw [if_neg hlt]
      exact hlen

theorem foldl_evalStepGo_getR_gt (l : Int) : ∀ (n : Nat) (out : List Nat) (R : List Cand) (c : Nat),
    n < c → getR ((List.range n).foldl (evalStepGo l) (out, R)).2 c = getR R c := by
  intro n
  induction n with
  | zero =>
    intro out R c hgt
    simp only [List.range_zero, List.foldl_nil]
  | succ n ih =>
    intro out R c hgt
    rw [List.range_succ, List.foldl_append]
    simp only [List.foldl_cons, List.foldl_nil]
    rcases hf : ((List.range n).foldl (evalStepGo l) (out, R)) with ⟨out₁, R₁⟩
    have hne : c ≠ n + 1 := by omega
    simp only [evalStepGo]
    have hfold : getR R₁ c = getR R c := by
      have := ih out R c (by omega)
      rw [hf] at this
      exact this
    by_cases hlt : l < (getR R₁ (n+1)).len
    · rw [if_pos hlt]
      simp only [Prod.snd]
      rw [getR_set_ne R₁ (n+1) c ⟨l, 0, false⟩ (Ne.symm hne)]
      exact hfold
    · rw [if_neg hlt]
      exact hfold

theorem foldl_evalStepGo_getR (l : Int) : ∀ (n : Nat) (out : List Nat) (R : List Cand) (c : Nat),
    1 ≤ c → c ≤ n → n + 1 ≤ R.length →
    getR ((List.range n).foldl (evalStepGo l) (out, R)).2 c =
      (if l < (getR R c).len then ⟨l, 0, false⟩ else getR R c) := by
  intro n
  induction n with
  | zero => intro out R c hc1 hcn _; omega
  | succ n ih =>
    intro out R c hc1 hcn hlen
    rw [List.range_succ, List.foldl_append]
    simp only [List.foldl_cons, List.foldl_nil]
    rcases hf : ((List.range n).foldl (evalStepGo l) (out, R)) with ⟨out₁, R₁⟩
    simp only [evalStepGo]
    have hfoldc : ∀ (hc : c ≤ n), getR R₁ c
        = (if l < (getR R c).len then ⟨l, 0, false⟩ else getR R c) := by
      intro hc
      have := ih out R c hc1 hc (by omega)
      rw [hf] at this
      exact this
    have hgt1 : getR R₁ (n+1) = getR R (n+1) := by
      have := foldl_evalStepGo_getR_gt l n out R (n+1) (Nat.lt_succ_self n)
      rw [hf] at this
      exact this
    have hlen1 : R₁.length = R.length := by
      have := foldl_evalStepGo_len l n out R
      rw [hf] at this
      exact this
    by_cases hcl : c ≤ n
    · have hne : c ≠ n + 1 := by omega
      by_cases hlt : l < (getR R₁ (n+1)).len
      · rw [if_pos hlt]
        simp only [Prod.snd]
        rw [getR_set_ne R₁ (n+1) c ⟨l,0,false⟩ (Ne.symm hne)]
        exact hfoldc hcl
      · rw [if_neg hlt]
        exact hfoldc hcl
    · have hceq : c = n + 1 := by omega
      subst hceq
      rw [hgt1]
      by_cases hlt2 : l < (getR R (n+1)).len
      · rw [if_pos hlt2]
        simp only [Prod.snd]
        rw [getR_set_eq]
        rw [if_pos (by omega)]
        simp only [hlt2, if_true]
      · rw [if_neg hlt2]
        simp only [Prod.snd]
        rw [hgt1]
        rw [if_neg hlt2]

theorem evalStep_getR (l : Int) (R : List Cand) (out : List Nat) (c : Nat)
    (hc1 : 1 ≤ c) (hc2 : c < SIGMA) (hR : SIGMA ≤ R.length) :
    getR (evalStep l R out).2 c = if l < (getR R c).len then ⟨l, 0, false⟩ else getR R c := by
  rw [evalStep_eq]
  exact foldl_evalStepGo_getR l (SIGMA-1) out R c hc1 (by omega) (by omega)

theorem foldl_evalStepGo_mem (l : Int) : ∀ (n : Nat) (out : List Nat) (R : List Cand) (x : Nat),
    x ∈ ((List.range n).foldl (evalStepGo l) (out, R)).1 ↔
      x ∈ out ∨ ∃ c, 1 ≤ c ∧ c ≤ n ∧ l < (getR R c).len ∧ (getR R c).active ∧ x = (getR R c).pos := by
  intro n
  induction n with
  | zero =>
    intro out R x
    simp only [List.range_zero, List.foldl_nil, Prod.fst]
    constructor
    · intro h; exact Or.inl h
    · intro h
      rcases h with h | ⟨c, hc1, hcn, _⟩
      · exact h
      · omega
  | succ n ih =>
    intro out R x
    rw [List.range_succ, List.foldl_append]
    simp only [List.foldl_cons, List.foldl_nil]
    rcases hf : ((List.range n).foldl (evalStepGo l) (out, R)) with ⟨out₁, R₁⟩
    simp only [evalStepGo]
    have ihx := ih out R x
    rw [hf] at ihx
    simp only [Prod.fst] at ihx
    have hgt1 : getR R₁ (n+1) = getR R (n+1) := by
      have := foldl_evalStepGo_getR_gt l n out R (n+1) (Nat.lt_succ_self n)
      rw [hf] at this
      exact this
    by_cases hlt : l < (getR R₁ (n+1)).len
    · rw [if_pos hlt]
      by_cases hact : (getR R₁ (n+1)).active
      · rw [if_pos hact]
        rw [List.mem_append]
        constructor
        · intro h
          rcases h with h | h
          · rcases ihx.mp h with h | ⟨c, hc1, hcn, hcl, hcact, hcx⟩
            · exact Or.inl h
            · exact Or.inr ⟨c, hc1, by omega, hcl, hcact, hcx⟩
          · rw [List.mem_singleton] at h
            rw [hgt1] at h hlt hact
            refine Or.inr ⟨n+1, by omega, by omega, hlt, hact, h⟩
        · intro h
          rcases h with h | ⟨c, hc1, hcn, hcl, hcact, hcx⟩
          · exact Or.inl (ihx.mpr (Or.inl h))
          · by_cases hceq : c = n + 1
            · subst hceq
              refine Or.inr ?_
              rw [List.mem_singleton]
              rw [← hgt1] at hcx
              exact hcx
            · refine Or.inl (ihx.mpr (Or.inr ⟨c, hc1, by omega, hcl, hcact, hcx⟩))
      · rw [if_neg hact]
        show x ∈ out₁ ↔ _
        constructor
        · intro h
          rcases ihx.mp h with h | ⟨c, hc1, hcn, hcl, hcact, hcx⟩
          · exact Or.inl h
          · exact Or.inr ⟨c, hc1, by omega, hcl, hcact, hcx⟩
        · intro h
          rcases h with h | ⟨c, hc1, hcn, hcl, hcact, hcx⟩
          · exact ihx.mpr (Or.inl h)
          · by_cases hceq : c = n + 1
            · subst hceq
              rw [hgt1] at hact
              exact absurd hcact hact
            · exact ihx.mpr (Or.inr ⟨c, hc1, by omega, hcl, hcact, hcx⟩)
    · rw [if_neg hlt]
      show x ∈ out₁ ↔ _
      constructor
      · intro h
        rcases ihx.mp h with h | ⟨c, hc1, hcn, hcl, hcact, hcx⟩
        · exact Or.inl h
        · exact Or.inr ⟨c, hc1, by omega, hcl, hcact, hcx⟩
      · intro h
        rcases h with h | ⟨c, hc1, hcn, hcl, hcact, hcx⟩
        · exact ihx.mpr (Or.inl h)
        · by_cases hceq : c = n + 1
          · subst hceq
            rw [hgt1] at hlt
            exact absurd hcl hlt
          · exact ihx.mpr (Or.inr ⟨c, hc1, by omega, hcl, hcact, hcx⟩)

theorem evalStep_mem (l : Int) (R : List Cand) (out : List Nat) (x : Nat) :
    x ∈ (evalStep l R out).1 ↔
      x ∈ out ∨ ∃ c, 1 ≤ c ∧ c < SIGMA ∧ l < (getR R c).len ∧ (getR R c).active ∧ x = (getR R c).pos := by
  rw [evalStep_eq]
  rw [foldl_evalStepGo_mem l (SIGMA-1) out R x]
  constructor
  · intro h
    rcases h with h | ⟨c, hc1, hcn, hcl, hcact, hcx⟩
    · exact Or.inl h
    · exact Or.inr ⟨c, hc1, by omega, hcl, hcact, hcx⟩
  · intro h
    rcases h with h | ⟨c, hc1, hc2, hcl, hcact, hcx⟩
    · exact Or.inl h
    · exact Or.inr ⟨c, hc1, by omega, hcl, hcact, hcx⟩

/-! ### fmStep per-char lemmas -/

theorem getFM_set_eq (R : List (Option CandFM)) (c : Nat) (a : CandFM) :
    getFM (R.set c a) c = if c < R.length then some a else getFM R c := by
  simp only [getFM, List.getD, List.getElem?_set]
  by_cases h : c < R.length
  · simp [h]
  · have h2 : ¬ c < (R.set c a).length := by rw [List.length_set]; exact h
    simp [h]

/-! ### The coupling invariant -/

/-- per-char three-case coupling at index i (before processing triple i).
  (c) never-involved: FM none, scan slot untouched (len = -1).
  (a) coupled-active: both machines hold the same pending position; scan len = L[s];
      no LCP below L[s] in any completed window since the arming boundary s.
  (b) delayed: scan has already emitted the position (it is in the scan output);
      FM still holds it; the arming-time NSV is already strictly below i, so any
      future FM overwrite is guaranteed to emit it. -/
def Coupled (ts : List Triple) (i : Nat) (R : List Cand) (R' : List (Option CandFM)) (c : Nat) : Prop :=
  match getFM R' c with
  | none => getR R c = ⟨-1, 0, false⟩
  | some q =>
      q.active = true ∧
      if (getR R c).active then
        ∃ s : Nat, isBnd ts s = true ∧ q.saPos = (s : Int) ∧ s < i
          ∧ (getR R c).len = (Ls ts).getD s 0
          ∧ (getR R c).pos = q.textPos
          ∧ q.nsv = nextSmaller (Ls ts) s
          ∧ ∀ j, s < j → j ≤ prevBnd ts i → (Ls ts).getD s 0 ≤ (Ls ts).getD j 0
      else
        ∃ s : Nat, isBnd ts s = true ∧ q.saPos = (s : Int) ∧ s < i
          ∧ q.nsv = nextSmaller (Ls ts) s
          ∧ nextSmaller (Ls ts) s < i
          ∧ (getR R c).len < (Ls ts).getD s 0
          ∧ (∃ j, s < j ∧ j < i ∧ ((Ls ts).getD j 0 : Int) ≤ (getR R c).len)
          ∧ (∀ j, s < j → j ≤ prevBnd ts i → (getR R c).len ≤ ((Ls ts).getD j 0 : Int))

/-- x is the pending (delayed) textPos of some (b)-character -/
def Delayed (R : List Cand) (R' : List (Option CandFM)) (x : Nat) : Prop :=
  ∃ c, 1 ≤ c ∧ c < SIGMA ∧ (getR R c).active = false ∧
    ∃ s p n, getFM R' c = some ⟨s, p, n, true⟩ ∧ p = x

/-- the joint invariant at index i -/
def JointInv (ts : List Triple) (i : Nat) (R : List Cand) (R' : List (Option CandFM))
    (out : List Nat) (S : List Nat) : Prop :=
  (∀ c, 1 ≤ c → c < SIGMA → Coupled ts i R R' c)
  ∧ (∀ x, x ∈ out ↔ x ∈ S ∨ Delayed R R' x)

/-! ### not-involved per-char step lemma -/

/-- if the boundary-window min w = min m0 L[i] drops below L[v] (v ≤ prevBnd ts i,
then there is a witness index in (v, i] with LCP at or below w. -/
theorem winOpen_witness (ts : List Triple) (i : Nat) (w : Int)
    (hw : w = min (winOpen (Ls ts) (prevBnd ts i) i) ((Ls ts).getD i 0)) :
    ∀ v : Nat, v ≤ prevBnd ts i → prevBnd ts i < i → (w < ((Ls ts).getD v 0 : Int)) →
      ((Ls ts).getD v 0 : Int) ≤ MAXINT →
      ∃ j, v < j ∧ j ≤ i ∧ ((Ls ts).getD j 0 : Int) ≤ w := by
  intro v hv hpb hlt hvmax
  have hwsplit : w = winOpen (Ls ts) (prevBnd ts i) i ∨ w = ((Ls ts).getD i 0 : Int) := by
    rw [hw]; omega
  rcases hwsplit with hwm | hwi
  · have hach : ∃ j, prevBnd ts i < j ∧ j < i ∧ ((Ls ts).getD j 0 : Int) = winOpen (Ls ts) (prevBnd ts i) i := by
      apply winOpen_ach
      rw [← hwm]
      omega
    obtain ⟨j, hj1, hj2, hj3⟩ := hach
    refine ⟨j, by omega, by omega, by omega⟩
  · refine ⟨i, by omega, by omega, by omega⟩

/-- STRICT window witness: when the boundary window-min w drops strictly below
L[i] (so w = m0 < vi; the vi-path w = vi is excluded), the achiever lies
strictly inside the open window: prevBnd < j < i with L[j] <= w. -/
theorem winOpen_witness_lt (ts : List Triple) (i : Nat) (w : Int)
    (hw : w = min (winOpen (Ls ts) (prevBnd ts i) i) ((Ls ts).getD i 0))
    (hmax : ((Ls ts).getD i 0 : Int) ≤ MAXINT)
    (hlt : w < ((Ls ts).getD i 0 : Int)) :
    ∃ j, prevBnd ts i < j ∧ j < i ∧ ((Ls ts).getD j 0 : Int) ≤ w := by
  have hwsplit : w = winOpen (Ls ts) (prevBnd ts i) i ∨ w = ((Ls ts).getD i 0 : Int) := by
    rw [hw]; omega
  rcases hwsplit with hwm | hwi
  · have hach : ∃ j, prevBnd ts i < j ∧ j < i ∧ ((Ls ts).getD j 0 : Int) = winOpen (Ls ts) (prevBnd ts i) i := by
      apply winOpen_ach
      rw [← hwm]
      have hmax : (0 : Int) ≤ MAXINT := by decide
      omega
    obtain ⟨j, hj1, hj2, hj3⟩ := hach
    refine ⟨j, hj1, hj2, ?_⟩
    omega
  · omega

/-- A character not involved in boundary i: only evalStep touches it. -/
theorem coupled_step_notinv (ts : List Triple) (i : Nat) (hi1 : 1 ≤ i) (hi : i < ts.length)
    (hb : isBnd ts i = true)
    (hsat : ∀ j, j < ts.length → (Ls ts).getD j 0 ≤ MAXINT.toNat)
    (R : List Cand) (R' : List (Option CandFM)) (c : Nat)
    (hc1 : 1 ≤ c) (hc2 : c < SIGMA)
    (hC : Coupled ts i R R' c)
    (R₃ : List Cand) (R₂' : List (Option CandFM))
    (w : Int)
    (hw : w = min (winOpen (Ls ts) (prevBnd ts i) i) ((Ls ts).getD i 0))
    (hR₃ : getR R₃ c = (if w < (getR R c).len then ⟨w, 0, false⟩ else getR R c))
    (hR₂' : getFM R₂' c = getFM R' c) :
    Coupled ts (i+1) R₃ R₂' c
    ∧ (w < (getR R c).len → (getR R c).active = true →
        ∃ s p n, getFM R₂' c = some ⟨(s : Int), p, n, true⟩ ∧ p = (getR R c).pos
          ∧ (getR R₃ c).active = false) := by
  have hpb : prevBnd ts (i+1) = i := prevBnd_bnd ts i hb
  have hpbi : prevBnd ts i < i := by
    rcases prevBnd_lt ts i with h | h
    · exact h
    · omega
  have hm : winOpen (Ls ts) (prevBnd ts i) i ≤ MAXINT := by
    unfold winOpen
    split
    · omega
    · exact foldl_min_le_all _ _ |>.2
  have hwle1 : w ≤ winOpen (Ls ts) (prevBnd ts i) i := by rw [hw]; omega
  have hwle2 : w ≤ ((Ls ts).getD i 0 : Int) := by rw [hw]; omega
  have hwE : (0 : Int) ≤ w := by
    have h1 := winOpen_nonneg (Ls ts) (prevBnd ts i) i
    rw [hw]
    omega
  have hwsplit : w = winOpen (Ls ts) (prevBnd ts i) i ∨ w = ((Ls ts).getD i 0 : Int) := by
    rw [hw]; omega
  -- main case analysis
  cases hq : getFM R' c with
  | none =>
    simp only [Coupled, hq] at hC
    have hR : getR R c = ⟨-1, 0, false⟩ := hC
    constructor
    · show Coupled ts (i+1) R₃ R₂' c
      simp only [Coupled, hR₂', hq]
      rw [hR₃, hR]
      have : ¬ (w < (-1 : Int)) := by omega
      simp only [this, if_false]
    · intro hlt hact
      simp only [hR] at hlt
      omega
  | some q =>
    simp only [Coupled, hq] at hC
    obtain ⟨hqact, hbody⟩ := hC
    obtain ⟨qsa, qtx, qns, qac⟩ := q
    have hLlen : (Ls ts).length = ts.length := by rw [Ls, List.length_map]
    by_cases hqa : (getR R c).active = true
    · -- case (a): coupled active
      rw [if_pos hqa] at hbody
      obtain ⟨s, hbnd, hsa, hsi, hlen, hpos, hnsv, hpast⟩ := hbody
      have hsL : s < ts.length := by omega
      have hLs : ((Ls ts).getD s 0 : Int) ≤ MAXINT := by
        have := hsat s hsL
        omega
      have hpbge : s ≤ prevBnd ts i := prevBnd_ge_of_bnd ts i s hbnd (by omega)
      by_cases hwlt : w < ((Ls ts).getD s 0 : Int)
      · -- drop: scan emits pos, post-(b)
        have hemit : getR R₃ c = ⟨w, 0, false⟩ := by
          rw [hR₃, hlen]
          rw [if_pos hwlt]
        have hulen : (getR R₃ c).len = w := by rw [hemit]
        have hact3 : (getR R₃ c).active = false := by rw [hemit]
        obtain ⟨jw, hjw1, hjw2, hjw3⟩ := winOpen_witness ts i w hw s hpbge hpbi (by exact hwlt) hLs
        have hnsvi : nextSmaller (Ls ts) s ≤ i := by
          have hjwL : jw < (Ls ts).length := by omega
          have := nextSmaller_le (Ls ts) s jw (by omega) hjwL (by omega)
          omega
        constructor
        · show Coupled ts (i+1) R₃ R₂' c
          simp only [Coupled, hR₂', hq]
          refine ⟨hqact, ?_⟩
          rw [if_neg (by simp [hact3])]
          refine ⟨s, hbnd, hsa, by omega, hnsv, by omega, ?_, ⟨jw, by omega, by omega, by omega⟩, ?_⟩
          · omega
          · intro j hj1 hj2
            rw [hpb] at hj2
            by_cases hjp : j ≤ prevBnd ts i
            · have := hpast j hj1 hjp
              omega
            · by_cases hji : j = i
              · rw [hji]
                omega
              · have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i) (q := w)
                  (by omega) j (by omega) (by omega)
                omega
        · intro hlt hact
          have hsa' : qsa = (s : Int) := hsa
          have hqact' : qac = true := hqact
          rw [hsa', hqact'] at hq
          refine ⟨s, qtx, qns, ?_, ?_, ?_⟩
          · rw [hR₂']
            exact hq
          · rw [hpos]
          · rw [hemit]
      · -- no drop: stays (a)
        have hnoemit : getR R₃ c = getR R c := by
          rw [hR₃, hlen]
          rw [if_neg (by rw [← hlen]; omega)]
        have hact3 : (getR R₃ c).active = true := by rw [hnoemit]; simp [hqa]
        constructor
        · show Coupled ts (i+1) R₃ R₂' c
          simp only [Coupled, hR₂', hq]
          refine ⟨hqact, ?_⟩
          rw [if_pos hact3]
          refine ⟨s, hbnd, hsa, by omega, ?_, ?_, hnsv, ?_⟩
          · rw [hnoemit, hlen]
          · rw [hnoemit]
            exact hpos
          · intro j hj1 hj2
            rw [hpb] at hj2
            by_cases hjp : j ≤ prevBnd ts i
            · exact hpast j hj1 hjp
            · by_cases hji : j = i
              · rw [hji]
                have hwge : ((Ls ts).getD s 0 : Int) ≤ w := by omega
                have hwle2' : w ≤ ((Ls ts).getD i 0 : Int) := hwle2
                omega
              · have hwge : ((Ls ts).getD s 0 : Int) ≤ w := by omega
                have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i)
                  (q := ((Ls ts).getD s 0 : Int)) (by
                    have := hwle1
                    omega) j (by omega) (by omega)
                omega
        · intro hlt hact
          have : ¬ (w < ((Ls ts).getD s 0 : Int)) := by omega
          rw [hlen] at hlt
          omega
    · -- case (b): delayed
      rw [if_neg hqa] at hbody
      obtain ⟨s, hbnd, hsa, hsi, hnsv, hnsvi, hult, hach, hpast⟩ := hbody
      have hsL : s < ts.length := by omega
      have hLs : ((Ls ts).getD s 0 : Int) ≤ MAXINT := by
        have := hsat s hsL
        omega
      have hpbge : s ≤ prevBnd ts i := prevBnd_ge_of_bnd ts i s hbnd (by omega)
      by_cases hwlt : w < (getR R c).len
      · -- decay: u' := w
        have hemit : getR R₃ c = ⟨w, 0, false⟩ := by
          rw [hR₃]
          rw [if_pos hwlt]
        have hulen : (getR R₃ c).len = w := by rw [hemit]
        have hact3 : (getR R₃ c).active = false := by rw [hemit]
        obtain ⟨jw, hjw1, hjw2, hjw3⟩ := winOpen_witness ts i w hw s hpbge hpbi
          (by omega) hLs
        constructor
        · show Coupled ts (i+1) R₃ R₂' c
          simp only [Coupled, hR₂', hq]
          refine ⟨hqact, ?_⟩
          rw [if_neg (by simp [hact3])]
          refine ⟨s, hbnd, hsa, by omega, hnsv, by omega, ?_, ⟨jw, by omega, by omega, by omega⟩, ?_⟩
          · omega
          · intro j hj1 hj2
            rw [hpb] at hj2
            by_cases hjp : j ≤ prevBnd ts i
            · have := hpast j hj1 hjp
              omega
            · by_cases hji : j = i
              · rw [hji]
                omega
              · have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i) (q := w)
                  (by omega) j (by omega) (by omega)
                omega
        · intro hlt hact
          exact absurd hact hqa
      · -- no decay: unchanged (b), past extends
        have hnoemit : getR R₃ c = getR R c := by
          rw [hR₃]
          rw [if_neg hwlt]
        have hact3 : (getR R₃ c).active = false := by simp [hqa, hnoemit]
        have hulen3 : (getR R₃ c).len = (getR R c).len := by rw [hnoemit]
        constructor
        · show Coupled ts (i+1) R₃ R₂' c
          simp only [Coupled, hR₂', hq]
          refine ⟨hqact, ?_⟩
          rw [if_neg (by simp [hact3])]
          obtain ⟨s', hs1, hs2, hs3⟩ := hach
          refine ⟨s, hbnd, hsa, by omega, hnsv, by omega,
            (by rw [hnoemit]; exact hult), (by rw [hnoemit]; exact ⟨s', hs1, by omega, hs3⟩), ?_⟩
          intro j hj1 hj2
          rw [hpb] at hj2
          by_cases hjp : j ≤ prevBnd ts i
          · have hpp := hpast j hj1 hjp
            omega
          · by_cases hji : j = i
            · rw [hji]
              have hwge : w ≥ (getR R c).len := by omega
              omega
            · have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i)
                (q := (getR R c).len) (by
                  have := hwle1
                  omega) j (by omega) (by omega)
              omega
        · intro hlt hact
          exact absurd hact hqa

/-! ### involved per-char step lemma -/

/-- A character involved in boundary i (as ending or starting side).  The scan
composite is evalStep + upd (arm with L[i] at pos); the FM composite is one
fmStep (set/overwrite-hold, maybe emitting the old textPos). -/
theorem coupled_step_inv (ts : List Triple) (i : Nat) (hi1 : 1 ≤ i) (hi : i < ts.length)
    (hb : isBnd ts i = true)
    (hsat : ∀ j, j < ts.length → (Ls ts).getD j 0 ≤ MAXINT.toNat)
    (R : List Cand) (R' : List (Option CandFM)) (c : Nat)
    (hc1 : 1 ≤ c) (hc2 : c < SIGMA)
    (hC : Coupled ts i R R' c)
    (w : Int)
    (hw : w = min (winOpen (Ls ts) (prevBnd ts i) i) ((Ls ts).getD i 0))
    (pos_c : Nat) (psvI : Int) (nsvI : Nat)
    (hpsv : psvI = prevSmaller (Ls ts) i) (hnsvI : nsvI = nextSmaller (Ls ts) i)
    (R₃ : List Cand) (R₂' : List (Option CandFM))
    (hR₃ : getR R₃ c = (if ((Ls ts).getD i 0 : Int) > (if w < (getR R c).len then w else (getR R c).len)
          then ⟨(Ls ts).getD i 0, pos_c, true⟩
          else (if w < (getR R c).len then ⟨w, 0, false⟩ else getR R c)))
    (hR₂' : getFM R₂' c = (match getFM R' c with
              | none => some ⟨(i : Int), pos_c, nsvI, true⟩
              | some q => if q.saPos ≤ psvI then some ⟨(i : Int), pos_c, nsvI, true⟩ else some q))
    (hem : Option Nat)
    (hHem : hem = (match getFM R' c with
              | none => none
              | some q => if q.saPos ≤ psvI ∧ (q.nsv < i) then some q.textPos else none)) :
    Coupled ts (i+1) R₃ R₂' c
    ∧ (∀ p, hem = some p →
        ((getR R c).active = false ∧ ∃ sa sn, getFM R' c = some ⟨sa, p, sn, true⟩)
        ∨ (w < (getR R c).len ∧ (getR R c).active = true ∧ p = (getR R c).pos))
    ∧ ((getR R₃ c).active = true → w < (getR R c).len → (getR R c).active = true →
        hem = some (getR R c).pos)
    ∧ (∀ x, (getR R₃ c).active = false → (∃ sa sn, getFM R₂' c = some ⟨sa, x, sn, true⟩) →
        ((getR R c).active = false ∧ (∃ sa sn, getFM R' c = some ⟨sa, x, sn, true⟩))
        ∨ (w < (getR R c).len ∧ (getR R c).active = true ∧ x = (getR R c).pos))
    ∧ (∀ x, (getR R c).active = false → (∃ sa sn, getFM R' c = some ⟨sa, x, sn, true⟩) →
        hem = none → (getR R₃ c).active = false ∧ (∃ sa sn, getFM R₂' c = some ⟨sa, x, sn, true⟩)) := by
  have hpb : prevBnd ts (i+1) = i := prevBnd_bnd ts i hb
  have hpbi : prevBnd ts i < i := by
    rcases prevBnd_lt ts i with h | h
    · exact h
    · omega
  have hwle1 : w ≤ winOpen (Ls ts) (prevBnd ts i) i := by rw [hw]; omega
  have hwle2 : w ≤ ((Ls ts).getD i 0 : Int) := by rw [hw]; omega
  have hwE : (0 : Int) ≤ w := by
    have h1 := winOpen_nonneg (Ls ts) (prevBnd ts i) i
    rw [hw]
    omega
  have hM : ((MAXINT.toNat : Int)) = MAXINT := rfl
  have hviMax : ((Ls ts).getD i 0 : Int) ≤ MAXINT := by
    have := hsat i hi
    omega
  have hLlen : (Ls ts).length = ts.length := by rw [Ls, List.length_map]
  -- the m₀ < vi witness: some window element below vi
  have hwt : winOpen (Ls ts) (prevBnd ts i) i < ((Ls ts).getD i 0 : Int) →
      ∃ j, prevBnd ts i < j ∧ j < i ∧ ((Ls ts).getD j 0 : Int) < ((Ls ts).getD i 0 : Int) := by
    intro hlt
    have := winOpen_lt (L := (Ls ts)) (pb := prevBnd ts i) (i := i) (q := ((Ls ts).getD i 0 : Int))
      (by
        have := winOpen_nonneg (Ls ts) (prevBnd ts i) i
        omega) hlt
    obtain ⟨j, hj1, hj2, hj3⟩ := this
    exact ⟨j, hj1, hj2, by omega⟩
  have hwsplit : w = winOpen (Ls ts) (prevBnd ts i) i ∨ w = ((Ls ts).getD i 0 : Int) := by
    rw [hw]; omega
  cases hq : getFM R' c with
  | none =>
    -- case (c): first involvement; both machines arm
    have hR : getR R c = ⟨-1, 0, false⟩ := by
      simp only [Coupled, hq] at hC
      exact hC
    rw [hq] at hR₂' hHem
    simp only at hR₂' hHem
    -- hR₂' : getFM R₂' c = some ⟨i, pos_c, nsvI, true⟩ ; hHem : hem = none
    have hemv : hem = none := hHem
    -- scan: no drop (w >= 0 > -1), then arms
    have hpost : getR R₃ c = ⟨(Ls ts).getD i 0, pos_c, true⟩ := by
      rw [hR₃, hR]
      have h1 : ¬ (w < (-1 : Int)) := by omega
      simp only [h1, if_false]
      have h2 : ((Ls ts).getD i 0 : Int) > (-1 : Int) := by omega
      simp only [h2, if_true]
    constructor
    · show Coupled ts (i+1) R₃ R₂' c
      simp only [Coupled, hR₂']
      refine ⟨trivial, ?_⟩
      rw [if_pos (by rw [hpost])]
      refine ⟨i, hb, rfl, by omega, ?_, ?_, hnsvI, ?_⟩
      · rw [hpost]
      · rw [hpost]
      · intro j hj1 hj2
        rw [hpb] at hj2
        omega
    constructor
    · intro p hp
      rw [hemv] at hp
      exact absurd hp (by simp)
    constructor
    · intro h1 h2 h3
      simp only [hR] at h2
      omega
    constructor
    · intro x h1 h2
      rw [hpost] at h1
      exact absurd h1 (by simp)
    · intro x h1 h2 h3
      exact absurd h2 (by simp)
  | some q =>
    simp only [Coupled, hq] at hC
    obtain ⟨hqact, hbody⟩ := hC
    obtain ⟨qsa, qtx, qns, qac⟩ := q
    rw [hq] at hR₂' hHem
    simp only at hR₂' hHem
    -- hR₂' : getFM R₂' c = if qsa ≤ psvI then some ⟨i, pos_c, nsvI, true⟩ else some ⟨qsa, qtx, qns, qac⟩
    -- hHem : hem = if qsa ≤ psvI ∧ (qns < i) then some qtx else none
    by_cases hqa : (getR R c).active = true
    · -- case (a): coupled active
      rw [if_pos hqa] at hbody
      obtain ⟨s, hbnd, hsa, hsi, hlen, hpos, hnsv, hpast⟩ := hbody
      have hsL : s < ts.length := by omega
      have hLs : ((Ls ts).getD s 0 : Int) ≤ MAXINT := by
        have := hsat s hsL
        omega
      have hpbge : s ≤ prevBnd ts i := prevBnd_ge_of_bnd ts i s hbnd (by omega)
      subst hsa
      subst hnsv
      by_cases hvilt : ((Ls ts).getD i 0 : Int) < ((Ls ts).getD s 0 : Int)
      · -- vi < v: scan drops; arm iff m0 < vi
        have hwlt : w < ((Ls ts).getD s 0 : Int) := by omega
        by_cases hmvi : winOpen (Ls ts) (prevBnd ts i) i < ((Ls ts).getD i 0 : Int)
        · -- m0 < vi: emit + arm + overwrite + FM-emit; post-(a) with s := i
          obtain ⟨jw, hjw1, hjw2, hjw3⟩ := hwt hmvi
          have hpsvge : ((s : Int) ≤ psvI) := by
            rw [hpsv]
            have hjwL : jw < (Ls ts).length := by omega
            have := prevSmaller_ge (Ls ts) i jw hjw2 (by omega)
            have hjws : s < jw := by omega
            omega
          have hnsvlt : nextSmaller (Ls ts) s < i := by
            have hjwL : jw < (Ls ts).length := by omega
            have := nextSmaller_le (Ls ts) s jw (by omega) hjwL (by omega)
            omega
          have hemv : hem = some qtx := by
            rw [hHem]
            simp [hpsvge, hnsvlt]
          have hpost : getR R₃ c = ⟨(Ls ts).getD i 0, pos_c, true⟩ := by
            rw [hR₃, hlen]
            rw [if_pos hwlt]
            rw [if_pos (by omega)]
          have hact3 : (getR R₃ c).active = true := by rw [hpost]
          constructor
          · show Coupled ts (i+1) R₃ R₂' c
            simp only [Coupled, hR₂', hpsvge, if_true]
            refine ⟨trivial, ?_⟩
            rw [if_pos hact3]
            refine ⟨i, hb, rfl, by omega, ?_, ?_, hnsvI, ?_⟩
            · rw [hpost]
            · rw [hpost]
            · intro j hj1 hj2
              rw [hpb] at hj2
              omega
          constructor
          · intro p hp
            refine Or.inr ⟨by rw [hlen]; omega, hqa, ?_⟩
            rw [hemv] at hp
            have hqp : qtx = p := Option.some.inj hp
            rw [← hqp, hpos]
          constructor
          · intro h1 h2 h3
            rw [hpos]
            exact hemv
          constructor
          · intro x h1 h2
            rw [hact3] at h1
            exact absurd h1 (by simp)
          · intro x h1 h2 h3
            rw [hqa] at h1
            exact absurd h1 (by simp)
        · -- m0 >= vi: scan drops, no arm; FM holds; post-(b)
          have hnow : ∀ j, s ≤ j → j < i → ((Ls ts).getD i 0 : Int) ≤ ((Ls ts).getD j 0 : Int) := by
            intro j hj1 hj2
            by_cases hjp : j ≤ prevBnd ts i
            · by_cases hsj : s < j
              · have hpp := hpast j hsj hjp
                omega
              · have hjs : j = s := by omega
                rw [hjs]
                omega
            · have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i)
                (q := ((Ls ts).getD i 0 : Int)) (by
                  have := winOpen_nonneg (Ls ts) (prevBnd ts i) i
                  omega) j (by omega) (by omega)
              omega
          have hpsvno : ¬ ((s : Int) ≤ psvI) := by
            intro hge
            rw [hpsv] at hge
            have hne : prevSmaller (Ls ts) i ≠ -1 := by
              intro h0
              rw [h0] at hge
              omega
            obtain ⟨j, hj1, hj2, hj3⟩ := prevSmaller_mem (Ls ts) i hne
            have := hnow j (by omega) hj2
            omega
          have hemv : hem = none := by
            rw [hHem]
            simp [hpsvno]
          have hpost : getR R₃ c = ⟨w, 0, false⟩ := by
            rw [hR₃, hlen]
            rw [if_neg (by rw [← hlen]; omega)]
            rw [if_pos (by rw [← hlen]; omega)]
          have hact3 : (getR R₃ c).active = false := by rw [hpost]
          have hulen : (getR R₃ c).len = w := by rw [hpost]
          -- nsv[s] <= i via the drop witness (w < v)
          obtain ⟨jw, hjw1, hjw2, hjw3⟩ := winOpen_witness ts i w hw s hpbge hpbi (by
            rw [← hlen]; omega) hLs
          have hnsvi : nextSmaller (Ls ts) s ≤ i := by
            have hjwL : jw < (Ls ts).length := by omega
            have := nextSmaller_le (Ls ts) s jw (by omega) hjwL (by omega)
            omega
          constructor
          · show Coupled ts (i+1) R₃ R₂' c
            simp only [Coupled, hR₂', hpsvno, if_false]
            refine ⟨hqact, ?_⟩
            rw [if_neg (by rw [hact3]; simp)]
            refine ⟨s, hbnd, rfl, by omega, rfl, by omega, ?_, ⟨jw, by omega, by omega, by omega⟩, ?_⟩
            · rw [hpost]
              omega
            · intro j hj1 hj2
              rw [hpb] at hj2
              by_cases hjp : j ≤ prevBnd ts i
              · have hpp := hpast j (by omega) hjp
                omega
              · by_cases hji : j = i
                · rw [hji]
                  omega
                · have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i) (q := w)
                    (by omega) j (by omega) (by omega)
                  omega
          constructor
          · intro p hp
            rw [hemv] at hp
            exact absurd hp (by simp)
          constructor
          · intro h1 h2 h3
            rw [hact3] at h1
            exact absurd h1 (by simp)
          constructor
          · intro x h1 h2
            refine Or.inr ⟨by rw [hlen]; omega, hqa, ?_⟩
            obtain ⟨sa, sn, hq2⟩ := h2
            rw [hR₂', if_neg hpsvno] at hq2
            have hx : qtx = x := by
              have e1 := (Option.some.inj hq2 :
                (({ saPos := (s : Int), textPos := qtx, nsv := nextSmaller (Ls ts) s, active := qac } : CandFM)
                  = ⟨sa, x, sn, true⟩))
              exact congrArg CandFM.textPos e1
            exact hx.symm.trans hpos.symm
          · intro x h1 h2 h3
            rw [hqa] at h1
            exact absurd h1 (by simp)
      · by_cases hvieq : ((Ls ts).getD i 0 : Int) = ((Ls ts).getD s 0 : Int)
        · -- vi = v
          by_cases hmv : winOpen (Ls ts) (prevBnd ts i) i < ((Ls ts).getD s 0 : Int)
          · -- m0 < v: both emit + both arm; post-(a) s := i
            obtain ⟨jw, hjw1, hjw2, hjw3⟩ := winOpen_witness_lt ts i w hw hviMax (by omega)
            have hpsvge : ((s : Int) ≤ psvI) := by
              rw [hpsv]
              have hjwL : jw < (Ls ts).length := by omega
              have := prevSmaller_ge (Ls ts) i jw hjw2 (by omega)
              have hjws : s < jw := by omega
              omega
            have hnsvlt : nextSmaller (Ls ts) s < i := by
              have hjwL : jw < (Ls ts).length := by omega
              have := nextSmaller_le (Ls ts) s jw (by omega) hjwL (by omega)
              omega
            have hemv : hem = some qtx := by
              rw [hHem]
              simp [hpsvge, hnsvlt]
            have hpost : getR R₃ c = ⟨(Ls ts).getD i 0, pos_c, true⟩ := by
              rw [hR₃, hlen]
              have hwv : w < ((Ls ts).getD s 0 : Int) := by omega
              rw [if_pos (by rw [← hlen]; omega)]
            have hact3 : (getR R₃ c).active = true := by rw [hpost]
            constructor
            · show Coupled ts (i+1) R₃ R₂' c
              simp only [Coupled, hR₂', hpsvge, if_true]
              refine ⟨trivial, ?_⟩
              rw [if_pos hact3]
              refine ⟨i, hb, rfl, by omega, ?_, ?_, hnsvI, ?_⟩
              · rw [hpost]
              · rw [hpost]
              · intro j hj1 hj2
                rw [hpb] at hj2
                omega
            constructor
            · intro p hp
              refine Or.inr ⟨by rw [hlen]; omega, hqa, ?_⟩
              rw [hemv] at hp
              have hqp : qtx = p := Option.some.inj hp
              rw [← hqp, hpos]
            constructor
            · intro h1 h2 h3
              rw [hpos]
              exact hemv
            constructor
            · intro x h1 h2
              rw [hact3] at h1
              exact absurd h1 (by simp)
            · intro x h1 h2 h3
              rw [hqa] at h1
              exact absurd h1 (by simp)
          · -- m0 >= v: silent; both hold; past extends
            have hnow : ∀ j, s ≤ j → j < i → ((Ls ts).getD s 0 : Int) ≤ ((Ls ts).getD j 0 : Int) := by
              intro j hj1 hj2
              by_cases hjp : j ≤ prevBnd ts i
              · by_cases hsj : s < j
                · have hpp := hpast j hsj hjp
                  omega
                · have hjs : j = s := by omega
                  rw [hjs]
                  omega
              · have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i)
                  (q := ((Ls ts).getD s 0 : Int)) (by
                    have := winOpen_nonneg (Ls ts) (prevBnd ts i) i
                    omega) j (by omega) (by omega)
                omega
            have hpsvno : ¬ ((s : Int) ≤ psvI) := by
              intro hge
              rw [hpsv] at hge
              have hne : prevSmaller (Ls ts) i ≠ -1 := by
                intro h0
                rw [h0] at hge
                omega
              obtain ⟨j, hj1, hj2, hj3⟩ := prevSmaller_mem (Ls ts) i hne
              have := hnow j (by omega) hj2
              omega
            have hemv : hem = none := by
              rw [hHem]
              simp [hpsvno]
            have hpost : getR R₃ c = getR R c := by
              rw [hR₃, hlen]
              rw [if_neg (by rw [← hlen]; omega)]
              rw [if_neg (by rw [← hlen]; omega)]
            have hact3 : (getR R₃ c).active = true := by rw [hpost]; exact hqa
            constructor
            · show Coupled ts (i+1) R₃ R₂' c
              simp only [Coupled, hR₂', hpsvno, if_false]
              refine ⟨hqact, ?_⟩
              rw [if_pos hact3]
              refine ⟨s, hbnd, rfl, by omega, ?_, ?_, rfl, ?_⟩
              · rw [hpost, hlen]
              · rw [hpost]
                exact hpos
              · intro j hj1 hj2
                rw [hpb] at hj2
                by_cases hjp : j ≤ prevBnd ts i
                · have hpp := hpast j (by omega) hjp
                  omega
                · by_cases hji : j = i
                  · rw [hji]
                    omega
                  · have := hnow j (by omega) (by omega)
                    omega
            constructor
            · intro p hp
              rw [hemv] at hp
              exact absurd hp (by simp)
            constructor
            · intro h1 h2 h3
              rw [hlen] at h2
              omega
            constructor
            · intro x h1 h2
              rw [hact3] at h1
              exact absurd h1 (by simp)
            · intro x h1 h2 h3
              rw [hqa] at h1
              exact absurd h1 (by simp)
        · -- vi > v: arm always; emit iff m0 < v
          by_cases hmv : winOpen (Ls ts) (prevBnd ts i) i < ((Ls ts).getD s 0 : Int)
          · -- m0 < v: scan emits; FM overwrites and emits; both arm; post-(a) s := i
            obtain ⟨jw, hjw1, hjw2, hjw3⟩ := winOpen_witness_lt ts i w hw hviMax (by omega)
            have hpsvge : ((s : Int) ≤ psvI) := by
              rw [hpsv]
              have hjwL : jw < (Ls ts).length := by omega
              have := prevSmaller_ge (Ls ts) i jw hjw2 (by omega)
              have hjws : s < jw := by omega
              omega
            have hnsvlt : nextSmaller (Ls ts) s < i := by
              have hjwL : jw < (Ls ts).length := by omega
              have := nextSmaller_le (Ls ts) s jw (by omega) hjwL (by omega)
              omega
            have hemv : hem = some qtx := by
              rw [hHem]
              simp [hpsvge, hnsvlt]
            have hpost : getR R₃ c = ⟨(Ls ts).getD i 0, pos_c, true⟩ := by
              rw [hR₃, hlen]
              rw [if_pos (by rw [← hlen]; omega)]
            have hact3 : (getR R₃ c).active = true := by rw [hpost]
            constructor
            · show Coupled ts (i+1) R₃ R₂' c
              simp only [Coupled, hR₂', hpsvge, if_true]
              refine ⟨trivial, ?_⟩
              rw [if_pos hact3]
              refine ⟨i, hb, rfl, by omega, ?_, ?_, hnsvI, ?_⟩
              · rw [hpost]
              · rw [hpost]
              · intro j hj1 hj2
                rw [hpb] at hj2
                omega
            constructor
            · intro p hp
              refine Or.inr ⟨by rw [hlen]; omega, hqa, ?_⟩
              rw [hemv] at hp
              have hqp : qtx = p := Option.some.inj hp
              rw [← hqp, hpos]
            constructor
            · intro h1 h2 h3
              rw [hpos]
              exact hemv
            constructor
            · intro x h1 h2
              rw [hact3] at h1
              exact absurd h1 (by simp)
            · intro x h1 h2 h3
              rw [hqa] at h1
              exact absurd h1 (by simp)
          · -- m0 >= v: silent arm; post-(a) s := i
            have hnow : ∀ j, s ≤ j → j < i → ((Ls ts).getD s 0 : Int) ≤ ((Ls ts).getD j 0 : Int) := by
              intro j hj1 hj2
              by_cases hjp : j ≤ prevBnd ts i
              · by_cases hsj : s < j
                · have hpp := hpast j hsj hjp
                  omega
                · have hjs : j = s := by omega
                  rw [hjs]
                  omega
              · have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i)
                  (q := ((Ls ts).getD s 0 : Int)) (by
                    have := winOpen_nonneg (Ls ts) (prevBnd ts i) i
                    omega) j (by omega) (by omega)
                omega
            -- FM ALWAYS overwrites here: j := s witnesses s <= psvI since L[s] = v < vi
            have hpsvge : ((s : Int) ≤ psvI) := by
              rw [hpsv]
              have := prevSmaller_ge (Ls ts) i s hsi (by omega)
              omega
            -- but emits nothing: no element of (s, i) is below v, so nsv[s] >= i
            have hnsvge : ¬ (nextSmaller (Ls ts) s < i) := by
              intro hlt
              obtain ⟨k, hk1, hk2, hk3⟩ := nextSmaller_mem (Ls ts) s (by omega)
              have := hnow k (by omega) (by omega)
              omega
            have hemv : hem = none := by
              rw [hHem]
              simp [hpsvge, hnsvge]
            have hpost : getR R₃ c = ⟨(Ls ts).getD i 0, pos_c, true⟩ := by
              rw [hR₃, hlen]
              rw [if_pos (by rw [← hlen]; omega)]
            have hact3 : (getR R₃ c).active = true := by rw [hpost]
            constructor
            · show Coupled ts (i+1) R₃ R₂' c
              simp only [Coupled, hR₂', hpsvge, if_true]
              refine ⟨trivial, ?_⟩
              rw [if_pos hact3]
              refine ⟨i, hb, rfl, by omega, ?_, ?_, hnsvI, ?_⟩
              · rw [hpost]
              · rw [hpost]
              · intro j hj1 hj2
                rw [hpb] at hj2
                omega
            constructor
            · intro p hp
              rw [hemv] at hp
              exact absurd hp (by simp)
            constructor
            · intro h1 h2 h3
              rw [hlen] at h2
              omega
            constructor
            · intro x h1 h2
              rw [hact3] at h1
              exact absurd h1 (by simp)
            · intro x h1 h2 h3
              rw [hqa] at h1
              exact absurd h1 (by simp)
    · -- case (b): delayed
      rw [if_neg hqa] at hbody
      obtain ⟨s, hbnd, hsa, hsi, hnsv, hnsvi, hult, hach, hpast⟩ := hbody
      have hsL : s < ts.length := by omega
      have hLs : ((Ls ts).getD s 0 : Int) ≤ MAXINT := by
        have := hsat s hsL
        omega
      have hpbge : s ≤ prevBnd ts i := prevBnd_ge_of_bnd ts i s hbnd (by omega)
      subst hsa
      subst hnsv
      -- arm/overwrite equivalence
      by_cases hpsv' : ((s : Int) ≤ psvI)
      · -- FM overwrites and emits (nsv[s] < i); scan arms; post-(a) s := i
        have hemv : hem = some qtx := by
          rw [hHem]
          simp [hpsv', hnsvi]
        -- scan arms too: the FM witness forces vi > post-eval len
        have harm : ((Ls ts).getD i 0 : Int) > (if w < (getR R c).len then w else (getR R c).len) := by
          by_cases hvi : ((Ls ts).getD i 0 : Int) ≤ (getR R c).len
          · -- vi <= len: the FM witness lies in the open window, below vi
            rw [hpsv] at hpsv'
            have hne : prevSmaller (Ls ts) i ≠ -1 := by
              intro h0
              rw [h0] at hpsv'
              omega
            obtain ⟨j, hj1, hj2, hj3⟩ := prevSmaller_mem (Ls ts) i hne
            -- s <= j, j < i, L[j] < vi <= len
            by_cases hjp : j ≤ prevBnd ts i
            · -- past region: (b)-past gives len <= L[j], but L[j] < vi <= len: absurd
              by_cases hjs : j = s
              · rw [hjs] at hj3
                omega
              · have hpp := hpast j (by omega) hjp
                exact absurd hj3 (by omega)
            · -- open window: w <= winOpen <= L[j] < vi <= len
              have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i)
                (q := winOpen (Ls ts) (prevBnd ts i) i) (by omega) j (by omega) (by omega)
              split <;> omega
          · -- vi > len: both branch values are below vi
            split
            · omega
            · omega
        have hpost : getR R₃ c = ⟨(Ls ts).getD i 0, pos_c, true⟩ := by
          rw [hR₃]
          rw [if_pos harm]
        have hact3 : (getR R₃ c).active = true := by rw [hpost]
        have hR₂v : getFM R₂' c = some ⟨(i : Int), pos_c, nsvI, true⟩ := by
          rw [hR₂']
          simp [hpsv']
        constructor
        · show Coupled ts (i+1) R₃ R₂' c
          simp only [Coupled, hR₂v]
          refine ⟨trivial, ?_⟩
          rw [if_pos hact3]
          refine ⟨i, hb, rfl, by omega, ?_, ?_, hnsvI, ?_⟩
          · rw [hpost]
          · rw [hpost]
          · intro j hj1 hj2
            rw [hpb] at hj2
            omega
        constructor
        · intro p hp
          rw [hemv] at hp
          have hqp : qtx = p := Option.some.inj hp
          have hqac : qac = true := hqact
          refine Or.inl ⟨by simpa using hqa, s, nextSmaller (Ls ts) s, ?_⟩
          rw [hqac, hqp]
        constructor
        · intro h1 h2 h3
          exact absurd h3 hqa
        constructor
        · intro x h1 h2
          rw [hact3] at h1
          exact absurd h1 (by simp)
        · intro x h1 h2 h3
          rw [hemv] at h3
          exact absurd h3 (by simp)
      · -- FM holds: no witness below vi in [s, i); scan cannot arm
        have hemv : hem = none := by
          rw [hHem]
          simp [hpsv']
        have hqac : qac = true := hqact
        have hR₂v : getFM R₂' c = some ⟨(s : Int), qtx, nextSmaller (Ls ts) s, true⟩ := by
          rw [hR₂']
          simp [hpsv', hqac]
        -- m0 >= vi (else the window min would be an FM witness)
        have hm0 : ((Ls ts).getD i 0 : Int) ≤ winOpen (Ls ts) (prevBnd ts i) i := by
          by_cases h : ((Ls ts).getD i 0 : Int) ≤ winOpen (Ls ts) (prevBnd ts i) i
          · exact h
          · -- winOpen < vi: a window element below vi is an FM witness: contradiction
            obtain ⟨j, hj1, hj2, hj3⟩ := winOpen_lt (L := (Ls ts)) (pb := prevBnd ts i) (i := i)
              (q := ((Ls ts).getD i 0 : Int)) (by omega) (by omega)
            have hwit := prevSmaller_ge (Ls ts) i j hj2 (by omega)
            have hs' : ((s : Int) ≤ psvI) := by
              rw [hpsv]
              omega
            omega
        -- vi <= len (else the achiever ja would be an FM witness)
        obtain ⟨ja, hja1, hja2, hja3⟩ := hach
        have hvile : ((Ls ts).getD i 0 : Int) ≤ (getR R c).len := by
          by_cases h : ((Ls ts).getD i 0 : Int) ≤ (getR R c).len
          · exact h
          · -- vi > len: the achiever ja (L[ja] <= len < vi) is an FM witness: contradiction
            have hwit := prevSmaller_ge (Ls ts) i ja hja2 (by omega)
            have hs' : ((s : Int) ≤ psvI) := by
              rw [hpsv]
              omega
            omega
        have hwv : w = ((Ls ts).getD i 0 : Int) := by rw [hw]; omega
        have hnarm : ¬ ((Ls ts).getD i 0 : Int) > (if w < (getR R c).len then w else (getR R c).len) := by
          split
          · omega
          · omega
        have hpost : getR R₃ c = (if w < (getR R c).len then ⟨w, 0, false⟩ else getR R c) := by
          rw [hR₃]
          rw [if_neg hnarm]
        have hact3 : (getR R₃ c).active = false := by
          rw [hpost]
          split
          · rfl
          · simp at hqa
            exact hqa
        have hlen3 : (getR R₃ c).len = (if w < (getR R c).len then w else (getR R c).len) := by
          rw [hpost]
          split
          · rfl
          · rfl
        constructor
        · show Coupled ts (i+1) R₃ R₂' c
          simp only [Coupled, hR₂v]
          refine ⟨trivial, ?_⟩
          rw [if_neg (by simp [hact3])]
          refine ⟨s, hbnd, rfl, by omega, rfl, by omega, ?_, ?_, ?_⟩
          · -- len' < u
            by_cases hdrop : w < (getR R c).len
            · have hl3 : (getR R₃ c).len = w := by rw [hlen3]; rw [if_pos hdrop]
              rw [hl3, hwv]
              omega
            · have hl3 : (getR R₃ c).len = (getR R c).len := by rw [hlen3]; rw [if_neg hdrop]
              rw [hl3]
              exact hult
          · -- achiever for the post len'
            by_cases hdrop : w < (getR R c).len
            · -- reset case: w = vi; the boundary row i itself achieves it
              have hl3 : (getR R₃ c).len = w := by rw [hlen3]; rw [if_pos hdrop]
              refine ⟨i, hsi, Nat.lt_succ_self i, ?_⟩
              have hvi : ((Ls ts).getD i 0 : Int) ≤ w := by omega
              rw [hl3]
              exact hvi
            · -- keep case: the old achiever ja still works
              have hl3 : (getR R₃ c).len = (getR R c).len := by rw [hlen3]; rw [if_neg hdrop]
              exact ⟨ja, hja1, Nat.lt_succ_of_lt hja2, by rw [hl3]; exact hja3⟩
          · -- past condition at i+1: range (s, i]
            intro j hj1 hj2
            rw [hpb] at hj2
            by_cases hjp : j ≤ prevBnd ts i
            · have hpp := hpast j (by omega) hjp
              by_cases hdrop : w < (getR R c).len
              · have hl3 : (getR R₃ c).len = w := by rw [hlen3]; rw [if_pos hdrop]
                rw [hl3]
                omega
              · have hl3 : (getR R₃ c).len = (getR R c).len := by rw [hlen3]; rw [if_neg hdrop]
                rw [hl3]
                omega
            · by_cases hji : j = i
              · -- the boundary row itself: L[i] = vi covers both branch lens
                by_cases hdrop : w < (getR R c).len
                · have hl3 : (getR R₃ c).len = w := by rw [hlen3]; rw [if_pos hdrop]
                  rw [hji, hl3]
                  omega
                · have hl3 : (getR R₃ c).len = (getR R c).len := by rw [hlen3]; rw [if_neg hdrop]
                  rw [hji, hl3]
                  omega
              · -- strict open window: elements >= m0 >= vi
                have hge := winOpen_geI (L := (Ls ts)) (pb := prevBnd ts i) (i := i)
                  (q := ((Ls ts).getD i 0 : Int)) (by omega) j (by omega) (by omega)
                by_cases hdrop : w < (getR R c).len
                · have hl3 : (getR R₃ c).len = w := by rw [hlen3]; rw [if_pos hdrop]
                  rw [hl3, hwv]
                  omega
                · have hl3 : (getR R₃ c).len = (getR R c).len := by rw [hlen3]; rw [if_neg hdrop]
                  rw [hl3]
                  omega
        constructor
        · intro p hp
          rw [hemv] at hp
          exact absurd hp (by simp)
        constructor
        · intro h1 h2 h3
          rw [hact3] at h1
          exact absurd h1 (by simp)
        constructor
        · intro x h1 h2
          rw [hR₂v] at h2
          obtain ⟨sa, sn, hq2⟩ := h2
          injection hq2 with e0
          injection e0 with r1 r2 r3 r4
          refine Or.inl ⟨by simp [hqa], s, nextSmaller (Ls ts) s, ?_⟩
          rw [r2, hqac]
        · intro x h1 h2 h3
          obtain ⟨sa, sn, hq2⟩ := h2
          injection hq2 with e0
          injection e0 with r1 r2 r3 r4
          refine ⟨hact3, s, nextSmaller (Ls ts) s, ?_⟩
          rw [hR₂v, r2]

theorem getFM_set_ne (R : List (Option CandFM)) (c d : Nat) (x : Option CandFM)
    (h : c ≠ d) : getFM (R.set c x) d = getFM R d := by
  unfold getFM
  rw [List.getD_eq_getElem?_getD, List.getElem?_set, List.getD_eq_getElem?_getD]
  simp [h]


theorem getFM_range_none (c : Nat) :
    getFM ((List.range SIGMA).map (fun _ => (none : Option CandFM))) c = none := by
  unfold getFM
  rw [List.getD_eq_getElem?_getD, List.getElem?_map]
  cases h : (List.range SIGMA)[c]? <;> rfl


/-! ### Slice 3 rung: scan ↔ FM/PSV-NSV equivalence (statement + scaffold) -/

/-! **Main rung (Bit 1b, slice 3).**  On every positive text the one-pass scan
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
/-! **RETIRED-FALSE RECORD (2026-09-26, statement-lock).** The unbounded
form below is REFUTED AS STATED: saturation counterexample (supervisor-
verified eval, lean/counterexamples/saturation_refutation.lean): the triple stream
  [⟨1,M+4,6⟩, ⟨2,M+3,5⟩, ⟨1,M+2,4⟩, ⟨2,M+1,3⟩, ⟨1,M,2⟩, ⟨2,M-1,1⟩]
with M = MAXINT.toNat gives scan = [94, 95, 95, 96, 96, 97] vs
fmSpec = [94, 95] — the machines GENUINELY DIVERGE strictly above M
(scan's running-min saturates at M and emits spurious DUPLICATE
positions — O2/Nodup also fails in that regime; fmSpec's PSV/NSV
arithmetic stays exact). At M exactly and below, both machines agree
(the boundary is sharp).  Every physically realizable text satisfies
`lcp ≤ |T| < 2^63`, so the divergence is a model-arifact regime only.
SUCCESSOR: `fm_equivalence_bounded` (the operative form). -/

-- theorem fm_equivalence (T : Text) (hT : positive T = true) :
--   ∀ x, x ∈ scan (T.length + 1) (triplesOf T) ↔
--     x ∈ fmSpec (T.length + 1) (triplesOf T)

/-! ### ASSEMBLY-INSERT -/

/-- drop-plumbing: peeling the head of `ts.drop k` gives row `k` and the
remaining suffix drop. -/
theorem drop_cons_getD : ∀ (ts : List Triple) (k : Nat) (t : Triple) (rest : List Triple),
    ts.drop k = t :: rest → ts.getD k dT = t ∧ ts.drop (k+1) = rest := by
  intro ts
  induction ts with
  | nil => intro k t rest h; simp at h
  | cons a as ih =>
    intro k t rest h
    match k with
    | 0 =>
      have h2 : a :: as = t :: rest := by simpa using h
      obtain ⟨rfl, rfl⟩ := List.cons_eq_cons.mp h2
      exact ⟨rfl, rfl⟩
    | k'+1 =>
      have h2 : as.drop k' = t :: rest := by simpa using h
      obtain ⟨hg, hd⟩ := ih k' t rest h2
      exact ⟨by simpa using hg, by simpa using hd⟩

theorem map_range_getD_oob {β : Type} (f : Nat → β) (n c : Nat) (d : β) (h : ¬ c < n) :
    ((List.range n).map f).getD c d = d := by
  have h2 : ¬ c < ((List.range n).map f).length := by
    rw [List.length_map, List.length_range]; exact h
  rw [List.getD_eq_getElem?_getD]
  have h3 : ((List.range n).map f)[c]? = none := by
    rw [List.getElem?_eq_none_iff]; omega
  rw [h3]; rfl

/-- table lengths are stable across the scan machine's steps -/
theorem evalStep_len (l : Int) (R : List Cand) (out : List Nat) :
    (evalStep l R out).2.length = R.length := by
  rw [evalStep_eq]; exact foldl_evalStepGo_len l (SIGMA-1) out R

theorem upd_len (R : List Cand) (c : Nat) (l : Nat) (pos : Nat) :
    (upd R c l pos).length = R.length := by
  unfold upd
  by_cases h : (l : Int) > (getR R c).len
  · rw [if_pos h, List.length_set]
  · rw [if_neg h]

theorem defaultR_len : defaultR.length = SIGMA := by
  rw [defaultR, List.length_map, List.length_range]

theorem defaultR_getR (c : Nat) : getR defaultR c = ⟨-1, 0, false⟩ := by
  have h1 : getR defaultR c = defaultR.getD c ⟨-1, 0, false⟩ := List.getD_eq_getElem?_getD
  rw [h1, defaultR]
  by_cases h : c < SIGMA
  · exact getD_map_range _ _ _ _ h
  · exact map_range_getD_oob _ _ _ _ h

/-- the FM machine's initial table -/
def fmR0 : List (Option CandFM) := (List.range SIGMA).map (fun _ => none)

theorem fmR0_len : fmR0.length = SIGMA := by
  rw [fmR0, List.length_map, List.length_range]

theorem fmR0_getFM (c : Nat) : getFM fmR0 c = none := by
  unfold getFM fmR0
  by_cases h : c < SIGMA
  · exact getD_map_range _ _ _ _ h
  · exact map_range_getD_oob _ _ _ _ h

theorem fm_step_len (N : Nat) (psvI : Int) (nsvI : Nat) (i : Nat) (c : Nat) (sa : Nat)
    (R : List (Option CandFM)) (S : List Nat) :
    (fmStep N psvI nsvI i c sa R S).1.length = R.length := by
  unfold fmStep
  split
  · rfl
  · split
    · exact List.length_set
    · split
      · exact List.length_set
      · rfl

/-- Coupled is stable across a non-boundary row: the index grows, the past
window is unchanged (same `prevBnd`), and the NSV bound only weakens. -/
theorem coupled_nonbnd (ts : List Triple) (k : Nat)
    (hb : isBnd ts k = false) (R : List Cand) (R' : List (Option CandFM)) (c : Nat) :
    Coupled ts k R R' c → Coupled ts (k+1) R R' c := by
  have hpb : prevBnd ts (k+1) = prevBnd ts k := prevBnd_nonbnd ts k hb
  intro hC
  cases hq : getFM R' c with
  | none =>
    simp only [Coupled, hq] at hC ⊢
    exact hC
  | some q =>
    simp only [Coupled, hq] at hC ⊢
    obtain ⟨hqact, hbody⟩ := hC
    by_cases hqa : (getR R c).active = true
    · rw [if_pos hqa] at hbody ⊢
      obtain ⟨s, hbnd, hsa, hsi, hlen, hpos, hnsv, hpast⟩ := hbody
      refine ⟨hqact, s, hbnd, hsa, by omega, hlen, hpos, hnsv, ?_⟩
      intro j hj1 hj2
      rw [hpb] at hj2
      exact hpast j hj1 hj2
    · rw [if_neg hqa] at hbody ⊢
      obtain ⟨s, hbnd, hsa, hsi, hnsv, hnlt, hlen, ⟨j, hj1, hj2, hj3⟩, hpast⟩ := hbody
      refine ⟨hqact, s, hbnd, hsa, by omega, hnsv, by omega, hlen, ⟨j, hj1, by omega, hj3⟩, ?_⟩
      intro j hj1 hj2
      rw [hpb] at hj2
      exact hpast j hj1 hj2

/-- base invariant: both machines start empty-handed -/
theorem jointInv_base (ts : List Triple) : JointInv ts 1 defaultR fmR0 [] [] := by
  refine ⟨fun c _ _ => ?_, fun x => ?_⟩
  · unfold Coupled
    cases hq : getFM fmR0 c with
    | none => exact defaultR_getR c
    | some q =>
      rw [fmR0_getFM] at hq
      exact absurd hq (by simp)
  · show x ∈ [] ↔ x ∈ [] ∨ Delayed defaultR fmR0 x
    constructor
    · intro h; cases h
    · intro h
      rcases h with h | ⟨c, hc1, hc2, hact, s, p, n, hf⟩
      · cases h
      · rw [fmR0_getFM] at hf
        exact absurd hf (by simp)

theorem fm_set_slot (R : List (Option CandFM)) (c : Nat) (a : CandFM) (h : c < R.length) :
    getFM (R.set c (some a)) c = some a := by
  unfold getFM
  rw [List.getD_eq_getElem?_getD, List.getElem?_set, if_pos h, if_pos rfl]
  rfl

theorem fmStep_S_mem (N : Nat) (psvI : Int) (nsvI : Nat) (i : Nat) (c : Nat) (sa : Nat)
    (R : List (Option CandFM)) (S : List Nat) (x : Nat) (hc : c ≠ 0) :
    x ∈ (fmStep N psvI nsvI i c sa R S).2 ↔
      x ∈ S ∨ ∃ q, getFM R c = some q ∧ q.saPos ≤ psvI ∧ q.nsv < i ∧ x = q.textPos := by
  unfold fmStep
  rw [if_neg hc]
  cases h : getFM R c with
  | none =>
    show x ∈ S ↔ _
    constructor
    · intro hmem; exact Or.inl hmem
    · intro hmem
      rcases hmem with h | ⟨q, hq, _⟩
      · exact h
      · exact absurd hq (by simp)
  | some cand =>
    simp only [h]
    by_cases hle : cand.saPos ≤ psvI
    · rw [if_pos hle]
      show x ∈ (if cand.nsv < i then S ++ [cand.textPos] else S) ↔ _
      by_cases hn : cand.nsv < i
      · rw [if_pos hn, List.mem_append, List.mem_singleton]
        constructor
        · intro h
          rcases h with h | h
          · exact Or.inl h
          · exact Or.inr ⟨cand, rfl, hle, hn, h⟩
        · intro h
          rcases h with h | ⟨q, hq, hq1, hq2, hq3⟩
          · exact Or.inl h
          · injection hq with hq'
            rw [← hq'] at hq1 hq2 hq3
            rw [hq3]; exact Or.inr rfl
      · rw [if_neg hn]
        show x ∈ S ↔ _
        constructor
        · intro hmem; exact Or.inl hmem
        · intro hmem
          rcases hmem with h | ⟨q, hq, hq1, hq2, _⟩
          · exact h
          · injection hq with hq'
            rw [← hq'] at hq2
            omega
    · rw [if_neg hle]
      show x ∈ S ↔ _
      constructor
      · intro hmem; exact Or.inl hmem
      · intro hmem
        rcases hmem with h | ⟨q, hq, hq1, _, _⟩
        · exact h
        · injection hq with hq'
          rw [← hq'] at hq1
          omega

theorem fmStep_getFM_self (N : Nat) (psvI : Int) (nsvI : Nat) (i : Nat) (c : Nat) (sa : Nat)
    (R : List (Option CandFM)) (S : List Nat) (hc : c ≠ 0) (hlen : c < R.length) :
    getFM (fmStep N psvI nsvI i c sa R S).1 c =
      (match getFM R c with
       | none => some ⟨(i : Int), N - sa, nsvI, true⟩
       | some q => if q.saPos ≤ psvI then some ⟨(i : Int), N - sa, nsvI, true⟩ else some q) := by
  unfold fmStep
  rw [if_neg hc]
  cases h : getFM R c with
  | none =>
    simp only [h]
    exact fm_set_slot R c ⟨(i : Int), N - sa, nsvI, true⟩ hlen
  | some cand =>
    simp only [h]
    by_cases hle : cand.saPos ≤ psvI
    · rw [if_pos hle, if_pos hle]
      exact fm_set_slot R c ⟨(i : Int), N - sa, nsvI, true⟩ hlen
    · rw [if_neg hle, if_neg hle]
      show getFM R c = some cand
      exact h

theorem fmStep_getFM_ne (N : Nat) (psvI : Int) (nsvI : Nat) (i : Nat) (c : Nat) (sa : Nat)
    (R : List (Option CandFM)) (S : List Nat) (d : Nat) (hd : d ≠ c) :
    getFM (fmStep N psvI nsvI i c sa R S).1 d = getFM R d := by
  unfold fmStep
  by_cases hc : c = 0
  · rw [if_pos hc]
  · rw [if_neg hc]
    cases h : getFM R c with
    | none =>
      simp only [h]
      exact getFM_set_ne R c d (some ⟨(i : Int), N - sa, nsvI, true⟩) (Ne.symm hd)
    | some q =>
      simp only [h]
      by_cases hle : q.saPos ≤ psvI
      · rw [if_pos hle]
        exact getFM_set_ne R c d (some ⟨(i : Int), N - sa, nsvI, true⟩) (Ne.symm hd)
      · rw [if_neg hle]

theorem finalStep_unfold (R : List (Option CandFM)) (S : List Nat) (i : Nat) :
    finalStep R S i =
      (match getFM R (i+1) with
       | some cand => if cand.active = true then S ++ [cand.textPos] else S
       | none => S) := rfl

theorem foldl_finalStep_mem (R : List (Option CandFM)) : ∀ (n : Nat) (S : List Nat) (x : Nat),
    x ∈ ((List.range n).foldl (finalStep R) S) ↔
      x ∈ S ∨ ∃ c, 1 ≤ c ∧ c ≤ n ∧ ∃ q, getFM R c = some q ∧ q.active = true ∧ x = q.textPos := by
  intro n
  induction n with
  | zero =>
    intro S x
    simp only [List.range_zero, List.foldl_nil]
    constructor
    · intro h; exact Or.inl h
    · intro h
      rcases h with h | ⟨c, _, hcn, _⟩
      · exact h
      · omega
  | succ n ih =>
    intro S x
    rw [List.range_succ, List.foldl_append]
    simp only [List.foldl_cons, List.foldl_nil]
    have key := ih S x
    rw [finalStep_unfold R ((List.range n).foldl (finalStep R) S) n]
    match hq : getFM R (n+1) with
    | none =>
      simp only [hq]
      rw [key]
      constructor
      · intro h
        rcases h with h | ⟨c, hc1, hcn, hc⟩
        · exact Or.inl h
        · exact Or.inr ⟨c, hc1, by omega, hc⟩
      · intro h
        rcases h with h | ⟨c, hc1, hcn, q, hq2, hq3, hx⟩
        · exact Or.inl h
        · by_cases hce : c = n+1
          · rw [hce] at hq2
            rw [hq] at hq2
            exact absurd hq2 (by simp)
          · exact Or.inr ⟨c, hc1, by omega, q, hq2, hq3, hx⟩
    | some cand =>
      simp only [hq]
      by_cases hact : cand.active = true
      · rw [if_pos hact, List.mem_append, List.mem_singleton, key]
        constructor
        · intro h
          rcases h with (h | ⟨c, hc1, hcn, hc⟩) | hx
          · exact Or.inl h
          · exact Or.inr ⟨c, hc1, by omega, hc⟩
          · exact Or.inr ⟨n+1, by omega, by omega, cand, hq, hact, hx⟩
        · intro h
          rcases h with h | ⟨c, hc1, hcn, q, hq2, hq3, hx⟩
          · exact Or.inl (Or.inl h)
          · by_cases hce : c = n+1
            · rw [hce] at hq2
              rw [hq] at hq2
              injection hq2 with hq5
              rw [← hq5] at hx
              exact Or.inr hx
            · exact Or.inl (Or.inr ⟨c, hc1, by omega, q, hq2, hq3, hx⟩)
      · rw [if_neg hact, key]
        constructor
        · intro h
          rcases h with h | ⟨c, hc1, hcn, hc⟩
          · exact Or.inl h
          · exact Or.inr ⟨c, hc1, by omega, hc⟩
        · intro h
          rcases h with h | ⟨c, hc1, hcn, q, hq2, hq3, hx⟩
          · exact Or.inl h
          · by_cases hce : c = n+1
            · rw [hce] at hq2
              rw [hq] at hq2
              injection hq2 with hq5
              rw [← hq5] at hq3
              exact absurd hq3 hact
            · exact Or.inr ⟨c, hc1, by omega, q, hq2, hq3, hx⟩


theorem finalEmit_mem (R : List (Option CandFM)) (S : List Nat) (x : Nat) :
    x ∈ finalEmit R S ↔
      x ∈ S ∨ ∃ c, 1 ≤ c ∧ c < SIGMA ∧ ∃ q, getFM R c = some q ∧ q.active = true ∧ x = q.textPos := by
  have key := foldl_finalStep_mem R (SIGMA-1) S x
  show x ∈ (List.range (SIGMA-1)).foldl (finalStep R) S ↔ _
  rw [key]
  constructor
  · intro h
    rcases h with h | ⟨c, hc1, hcn, hc⟩
    · exact Or.inl h
    · exact Or.inr ⟨c, hc1, by omega, hc⟩
  · intro h
    rcases h with h | ⟨c, hc1, hc2, hc⟩
    · exact Or.inl h
    · exact Or.inr ⟨c, hc1, by omega, hc⟩

theorem upd_getR_ne (R : List Cand) (c : Nat) (l : Nat) (pos : Nat) (d : Nat) (hd : d ≠ c) :
    getR (upd R c l pos) d = getR R d := by
  unfold upd
  by_cases h : (l : Int) > (getR R c).len
  · rw [if_pos h]
    exact getR_set_ne R c d ⟨l, pos, true⟩ (Ne.symm hd)
  · rw [if_neg h]

theorem upd_getR_self (R : List Cand) (c : Nat) (l : Nat) (pos : Nat) (hlen : c < R.length) :
    getR (upd R c l pos) c =
      (if (l : Int) > (getR R c).len then ⟨l, pos, true⟩ else getR R c) := by
  unfold upd
  by_cases h : (l : Int) > (getR R c).len
  · rw [if_pos h, getR_set_eq, if_pos hlen, if_pos h]
  · rw [if_neg h, if_neg h]

theorem Ls_getD : ∀ (ts : List Triple) (k : Nat), k < ts.length →
    (Ls ts).getD k 0 = (ts.getD k dT).lcp := by
  intro ts
  induction ts with
  | nil => intro k hk; simp at hk
  | cons a as ih =>
    intro k hk
    match k with
    | 0 => rfl
    | k'+1 =>
      have h' : k' < as.length := by
        simp only [List.length_cons] at hk; omega
      show (as.map (fun t => t.lcp)).getD k' 0 = (as.getD k' dT).lcp
      exact ih k' h'

theorem slot_composite (w : Int) (lk : Nat) (pos_c : Nat) (R R₁ R₃ : List Cand) (c : Nat)
    (hlen : c < R₁.length)
    (hE : getR R₁ c = if w < (getR R c).len then ⟨w, 0, false⟩ else getR R c)
    (hU : getR R₃ c = getR (upd R₁ c lk pos_c) c) :
    getR R₃ c =
      (if ((lk : Int) > (if w < (getR R c).len then w else (getR R c).len))
       then ⟨lk, pos_c, true⟩
       else (if w < (getR R c).len then ⟨w, 0, false⟩ else getR R c)) := by
  rw [hU, upd_getR_self R₁ c lk pos_c hlen]
  by_cases hw : w < (getR R c).len
  · rw [if_pos hw] at hE
    rw [hE, if_pos hw, if_pos hw]
  · rw [if_neg hw] at hE
    rw [hE, if_neg hw, if_neg hw]

theorem coupled_unpack (ts : List Triple) (k : Nat) (R : List Cand)
    (R' : List (Option CandFM)) (c : Nat) (hC : Coupled ts k R R' c) :
    (getFM R' c = none ∧ getR R c = ⟨-1, 0, false⟩)
    ∨ (∃ q, getFM R' c = some q ∧ q.active = true ∧ (getR R c).active = true ∧
         q.textPos = (getR R c).pos)
    ∨ ((getR R c).active = false ∧ ∃ q, getFM R' c = some q ∧ q.active = true ∧
         q.nsv < k) := by
  unfold Coupled at hC
  cases hq : getFM R' c with
  | none =>
    rw [hq] at hC
    exact Or.inl ⟨rfl, hC⟩
  | some q =>
    rw [hq] at hC
    obtain ⟨hqact, hbody⟩ := hC
    by_cases hqa : (getR R c).active = true
    · rw [if_pos hqa] at hbody
      obtain ⟨s, hbnd, hsa, hsi, hlen, hpos, hnsv, hpast⟩ := hbody
      exact Or.inr (Or.inl ⟨q, rfl, hqact, hqa, hpos.symm⟩)
    · rw [if_neg hqa] at hbody
      obtain ⟨s, hbnd, hsa, hsi, hnsv, hnlt, hlen, hwit, hpast⟩ := hbody
      refine Or.inr (Or.inr ⟨by simp [hqa], q, rfl, hqact, ?_⟩)
      rw [hnsv]
      exact hnlt

def hemOf (R' : List (Option CandFM)) (c : Nat) (psvI : Int) (i : Nat) : Option Nat :=
  match getFM R' c with
  | none => none
  | some q => if q.saPos ≤ psvI ∧ (q.nsv : Nat) < i then some q.textPos else none

theorem hemOf_cases (R' : List (Option CandFM)) (c : Nat) (psvI : Int) (i : Nat) (y : Nat)
    (h : hemOf R' c psvI i = some y) :
    ∃ q, getFM R' c = some q ∧ q.saPos ≤ psvI ∧ q.nsv < i ∧ q.textPos = y := by
  unfold hemOf at h
  cases hq : getFM R' c with
  | none =>
    simp only [hq] at h
    exact absurd h (by simp)
  | some q =>
    simp only [hq] at h
    by_cases hif : q.saPos ≤ psvI ∧ q.nsv < i
    · rw [if_pos hif] at h
      injection h with h'
      exact ⟨q, rfl, hif.1, hif.2, h'⟩
    · rw [if_neg hif] at h
      exact absurd h (by simp)

theorem hemOf_mem_S (N : Nat) (psvI : Int) (nsvI : Nat) (i : Nat) (c : Nat) (sa : Nat)
    (R' : List (Option CandFM)) (S : List Nat) (y : Nat) (hc : c ≠ 0)
    (h : hemOf R' c psvI i = some y) :
    y ∈ (fmStep N psvI nsvI i c sa R' S).2 := by
  obtain ⟨q, hq, h1, h2, hy⟩ := hemOf_cases R' c psvI i y h
  exact (fmStep_S_mem N psvI nsvI i c sa R' S y hc).mpr
    (Or.inr ⟨q, hq, h1, h2, by rw [hy]⟩)

theorem fmStep_S_zero (N : Nat) (psvI : Int) (nsvI : Nat) (i : Nat) (sa : Nat)
    (R : List (Option CandFM)) (S : List Nat) :
    (fmStep N psvI nsvI i 0 sa R S).2 = S := by
  unfold fmStep
  rw [if_pos rfl]

theorem getFM_lt (R : List (Option CandFM)) (c : Nat) (q : CandFM)
    (h : getFM R c = some q) : c < R.length := by
  by_cases hlt : c < R.length
  · exact hlt
  · unfold getFM at h
    rw [List.getD_eq_getElem?_getD] at h
    have hn : R[c]? = none := by rw [List.getElem?_eq_none_iff]; omega
    rw [hn] at h
    exact absurd h (by simp)

theorem bound_mem (N : Nat) (ts : List Triple) (k : Nat) (hk1 : 1 ≤ k) (hk : k < ts.length)
    (hb : isBnd ts k = true)
    (hsat : ∀ j, j < ts.length → (Ls ts).getD j 0 ≤ MAXINT.toNat)
    (R : List Cand) (R' : List (Option CandFM))
    (hJ1 : ∀ c, 1 ≤ c → c < SIGMA → Coupled ts k R R' c)
    (hRlen : SIGMA ≤ R.length) (hR'len : R'.length = SIGMA)
    (t : Triple) (p : Nat) (pSa : Nat)
    (hp : p = (ts.getD (k-1) dT).c) (hpSa : pSa = (ts.getD (k-1) dT).sa)
    (ht : t = ts.getD k dT)
    (out out₁ S S₁ S' : List Nat) (R₁ R₂ R₃ : List Cand) (R₁' R₂' : List (Option CandFM))
    (hJ2 : ∀ x, x ∈ out ↔ x ∈ S ∨ Delayed R R' x)
    (w : Int) (hw : w = min (winOpen (Ls ts) (prevBnd ts k) k) ((Ls ts).getD k 0))
    (hE : (out₁, R₁) = evalStep w R out)
    (hU1 : R₂ = upd R₁ p t.lcp (N - pSa))
    (hU2 : R₃ = upd R₂ t.c t.lcp (N - t.sa))
    (hF1 : (R₁', S₁) = fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S)
    (hF2 : (R₂', S') = fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa R₁' S₁)
    : ∀ x, x ∈ out₁ ↔ x ∈ S' ∨ Delayed R₃ R₂' x := by
  have hE1 : out₁ = (evalStep w R out).1 := congrArg Prod.fst hE
  have hE2 : R₁ = (evalStep w R out).2 := congrArg Prod.snd hE
  have hF1a : R₁' = (fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S).1 := congrArg Prod.fst hF1
  have hF1b : S₁ = (fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S).2 := congrArg Prod.snd hF1
  have hF2a : R₂' = (fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa R₁' S₁).1 := congrArg Prod.fst hF2
  have hF2b : S' = (fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa R₁' S₁).2 := congrArg Prod.snd hF2
  have hLlen : (Ls ts).length = ts.length := by rw [Ls, List.length_map]
  have hlk : (Ls ts).getD k 0 = t.lcp := by rw [Ls_getD ts k hk, ← ht]
  have hne : t.c ≠ p := by
    unfold isBnd at hb
    obtain ⟨_, _, hcp⟩ := of_decide_eq_true hb
    rw [ht, hp]
    exact hcp
  have hR1len : SIGMA ≤ R₁.length := by rw [hE2, evalStep_len]; exact hRlen
  have hR2len : SIGMA ≤ R₂.length := by rw [hU1, upd_len]; exact hR1len
  have hR1'len : R₁'.length = SIGMA := by rw [hF1a, fm_step_len]; exact hR'len
  have hwE : (0 : Int) ≤ w := by
    have h1 : (0 : Int) ≤ winOpen (Ls ts) (prevBnd ts k) k := winOpen_nonneg _ _ _
    rw [hw]
    have h2 : (0 : Int) ≤ ((Ls ts).getD k 0 : Int) := by
      have := Nat.zero_le ((Ls ts).getD k 0)
      omega
    omega
  have hS1 : ∀ x, x ∈ S → x ∈ S₁ := by
    intro x hSx
    by_cases hp0 : p = 0
    · rw [hF1b, hp0, fmStep_S_zero]
      exact hSx
    · rw [hF1b]
      exact (fmStep_S_mem N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S x hp0).mpr
        (Or.inl hSx)
  have hS2 : ∀ x, x ∈ S₁ → x ∈ S' := by
    intro x hSx
    by_cases ht0 : t.c = 0
    · rw [hF2b, ht0, fmStep_S_zero]
      exact hSx
    · rw [hF2b]
      exact (fmStep_S_mem N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa R₁' S₁ x ht0).mpr
        (Or.inl hSx)
  have hout1 : ∀ x, x ∈ out → x ∈ out₁ := by
    intro x hx
    rw [hE1]
    exact (evalStep_mem w R out x).mpr (Or.inl hx)
  have hshape_p : 1 ≤ p → p < SIGMA → getFM R₂' p =
      (match getFM R' p with
       | none => some ⟨(k : Int), N - pSa, nextSmaller (Ls ts) k, true⟩
       | some q => if q.saPos ≤ prevSmaller (Ls ts) k then
                    some ⟨(k : Int), N - pSa, nextSmaller (Ls ts) k, true⟩ else some q) := by
    intro hp1 hpS
    rw [hF2a, fmStep_getFM_ne _ _ _ _ _ _ _ _ p (Ne.symm hne), hF1a]
    exact fmStep_getFM_self N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S
      (by omega) (by omega)
  have hshape_c : 1 ≤ t.c → t.c < SIGMA → getFM R₂' t.c =
      (match getFM R' t.c with
       | none => some ⟨(k : Int), N - t.sa, nextSmaller (Ls ts) k, true⟩
       | some q => if q.saPos ≤ prevSmaller (Ls ts) k then
                    some ⟨(k : Int), N - t.sa, nextSmaller (Ls ts) k, true⟩ else some q) := by
    intro ht1 htS
    rw [hF2a]
    rw [fmStep_getFM_self N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa R₁' S₁
      (by omega) (by omega)]
    rw [hF1a, fmStep_getFM_ne _ _ _ _ _ _ _ _ t.c hne]
  have hslot_p : 1 ≤ p → p < SIGMA → getR R₃ p =
      (if ((Ls ts).getD k 0 : Int) > (if w < (getR R p).len then w else (getR R p).len)
       then ⟨(Ls ts).getD k 0, N - pSa, true⟩
       else (if w < (getR R p).len then ⟨w, 0, false⟩ else getR R p)) := by
    intro hp1 hpS
    have hslotE : getR R₁ p = if w < (getR R p).len then ⟨w, 0, false⟩ else getR R p := by
      rw [hE2]; exact evalStep_getR w R out p hp1 hpS hRlen
    have hslotU : getR R₃ p = getR (upd R₁ p t.lcp (N - pSa)) p := by
      rw [hU2, upd_getR_ne _ _ _ _ p (Ne.symm hne), hU1]
    have hcomp := slot_composite w t.lcp (N - pSa) R R₁ R₃ p (by omega) hslotE hslotU
    rw [← hlk] at hcomp
    exact hcomp
  have hslot_c : 1 ≤ t.c → t.c < SIGMA → getR R₃ t.c =
      (if ((Ls ts).getD k 0 : Int) > (if w < (getR R t.c).len then w else (getR R t.c).len)
       then ⟨(Ls ts).getD k 0, N - t.sa, true⟩
       else (if w < (getR R t.c).len then ⟨w, 0, false⟩ else getR R t.c)) := by
    intro ht1 htS
    have hslotE : getR R₁ t.c = if w < (getR R t.c).len then ⟨w, 0, false⟩ else getR R t.c := by
      rw [hE2]; exact evalStep_getR w R out t.c ht1 htS hRlen
    have hslotE' : getR R₂ t.c = if w < (getR R t.c).len then ⟨w, 0, false⟩ else getR R t.c := by
      rw [hU1, upd_getR_ne _ _ _ _ t.c hne, hslotE]
    have hslotU : getR R₃ t.c = getR (upd R₂ t.c t.lcp (N - t.sa)) t.c := by rw [hU2]
    have hcomp := slot_composite w t.lcp (N - t.sa) R R₂ R₃ t.c (by omega) hslotE' hslotU
    rw [← hlk] at hcomp
    exact hcomp
  have hslot_u : ∀ c, 1 ≤ c → c < SIGMA → c ≠ p → c ≠ t.c →
      getR R₃ c = (if w < (getR R c).len then ⟨w, 0, false⟩ else getR R c) := by
    intro c hc1 hc2 hcp hct
    have hslotE : getR R₁ c = if w < (getR R c).len then ⟨w, 0, false⟩ else getR R c := by
      rw [hE2]; exact evalStep_getR w R out c hc1 hc2 hRlen
    rw [hU2, upd_getR_ne _ _ _ _ c hct, hU1, upd_getR_ne _ _ _ _ c hcp, hslotE]
  have hFM_u : ∀ c, c ≠ p → c ≠ t.c → getFM R₂' c = getFM R' c := by
    intro c hcp hct
    rw [hF2a, fmStep_getFM_ne _ _ _ _ _ _ _ _ c hct, hF1a, fmStep_getFM_ne _ _ _ _ _ _ _ _ c hcp]
  -- per-char step-lemma applications
  have app_p : 1 ≤ p → p < SIGMA →
      (∀ p', hemOf R' p (prevSmaller (Ls ts) k) k = some p' →
        ((getR R p).active = false ∧ ∃ sa sn, getFM R' p = some ⟨sa, p', sn, true⟩)
        ∨ (w < (getR R p).len ∧ (getR R p).active = true ∧ p' = (getR R p).pos)) ∧
      ((getR R₃ p).active = true → w < (getR R p).len → (getR R p).active = true →
        hemOf R' p (prevSmaller (Ls ts) k) k = some (getR R p).pos) ∧
      (∀ x', (getR R₃ p).active = false → (∃ sa sn, getFM R₂' p = some ⟨sa, x', sn, true⟩) →
        ((getR R p).active = false ∧ (∃ sa sn, getFM R' p = some ⟨sa, x', sn, true⟩))
        ∨ (w < (getR R p).len ∧ (getR R p).active = true ∧ x' = (getR R p).pos)) ∧
      (∀ x', (getR R p).active = false → (∃ sa sn, getFM R' p = some ⟨sa, x', sn, true⟩) →
        hemOf R' p (prevSmaller (Ls ts) k) k = none →
        (getR R₃ p).active = false ∧ (∃ sa sn, getFM R₂' p = some ⟨sa, x', sn, true⟩)) := by
    intro hp1 hpS
    exact (coupled_step_inv ts k hk1 hk hb hsat R R' p hp1 hpS (hJ1 p hp1 hpS) w hw (N - pSa)
      (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) rfl rfl R₃ R₂' (hslot_p hp1 hpS)
      (hshape_p hp1 hpS) (hemOf R' p (prevSmaller (Ls ts) k) k) rfl).2
  have app_c : 1 ≤ t.c → t.c < SIGMA →
      (∀ p', hemOf R' t.c (prevSmaller (Ls ts) k) k = some p' →
        ((getR R t.c).active = false ∧ ∃ sa sn, getFM R' t.c = some ⟨sa, p', sn, true⟩)
        ∨ (w < (getR R t.c).len ∧ (getR R t.c).active = true ∧ p' = (getR R t.c).pos)) ∧
      ((getR R₃ t.c).active = true → w < (getR R t.c).len → (getR R t.c).active = true →
        hemOf R' t.c (prevSmaller (Ls ts) k) k = some (getR R t.c).pos) ∧
      (∀ x', (getR R₃ t.c).active = false → (∃ sa sn, getFM R₂' t.c = some ⟨sa, x', sn, true⟩) →
        ((getR R t.c).active = false ∧ (∃ sa sn, getFM R' t.c = some ⟨sa, x', sn, true⟩))
        ∨ (w < (getR R t.c).len ∧ (getR R t.c).active = true ∧ x' = (getR R t.c).pos)) ∧
      (∀ x', (getR R t.c).active = false → (∃ sa sn, getFM R' t.c = some ⟨sa, x', sn, true⟩) →
        hemOf R' t.c (prevSmaller (Ls ts) k) k = none →
        (getR R₃ t.c).active = false ∧ (∃ sa sn, getFM R₂' t.c = some ⟨sa, x', sn, true⟩)) := by
    intro ht1 htS
    exact (coupled_step_inv ts k hk1 hk hb hsat R R' t.c ht1 htS (hJ1 t.c ht1 htS) w hw (N - t.sa)
      (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) rfl rfl R₃ R₂' (hslot_c ht1 htS)
      (hshape_c ht1 htS) (hemOf R' t.c (prevSmaller (Ls ts) k) k) rfl).2
  have app_notinv : ∀ c, 1 ≤ c → c < SIGMA → c ≠ p → c ≠ t.c →
      ((w < (getR R c).len → (getR R c).active = true →
        ∃ s p n, getFM R₂' c = some ⟨s, p, n, true⟩ ∧ p = (getR R c).pos
          ∧ (getR R₃ c).active = false)) := by
    intro c hc1 hc2 hcp hct
    exact (coupled_step_notinv ts k hk1 hk hb hsat R R' c hc1 hc2 (hJ1 c hc1 hc2) R₃ R₂' w hw
      (hslot_u c hc1 hc2 hcp hct) (hFM_u c hcp hct)).2
  -- MAIN PROOF
  intro x
  constructor
  · -- → direction: out₁ ⊆ S' ∪ Delayed-post
    intro hx
    have hx' : x ∈ (evalStep w R out).1 := by rw [← hE1]; exact hx
    rcases (evalStep_mem w R out x).mp hx' with hout | hdrop
    · -- x already in out
      rcases (hJ2 x).mp hout with hS | hdelay
      · exact Or.inl (hS2 x (hS1 x hS))
      · obtain ⟨c, hc1, hc2, hinact, s0, x0, n0, hf0, hx0⟩ := hdelay
        rw [hx0] at hf0
        by_cases hcp : c = p
        · -- involved p, pre-(b)
          rw [hcp] at hc1 hc2 hinact hf0
          rcases coupled_unpack ts k R R' p (hJ1 p hc1 hc2) with h1 | h2 | h3
          · exact absurd hf0 (by rw [h1.1]; simp)
          · obtain ⟨_, _, _, hactp, _⟩ := h2
            rw [hinact] at hactp
            exact absurd hactp (by simp)
          · obtain ⟨_, q, hq', _, hn0⟩ := h3
            have hq3 : some ⟨s0, x, n0, true⟩ = some q := hf0.symm.trans hq'
            injection hq3 with hq4
            have hn0' : n0 < k := by have h := hn0; rw [← hq4] at h; exact h
            by_cases hs0 : s0 ≤ prevSmaller (Ls ts) k
            · -- FM overwrites and emits x
              have hh : hemOf R' p (prevSmaller (Ls ts) k) k = some x := by
                unfold hemOf
                simp only [hf0]
                rw [if_pos ⟨hs0, hn0'⟩]
              have hmem : x ∈ (fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S).2 :=
                hemOf_mem_S N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S x
                  (by omega) hh
              rw [← hF1b] at hmem
              exact Or.inl (hS2 x hmem)
            · -- FM holds: x stays delayed
              have hh : hemOf R' p (prevSmaller (Ls ts) k) k = none := by
                unfold hemOf
                simp only [hf0]
                rw [if_neg (by omega)]
              obtain ⟨_, _, _, cl5⟩ := app_p hc1 hc2
              obtain ⟨hact3, sa2, sn2, hhold'⟩ := cl5 x hinact ⟨s0, n0, hf0⟩ hh
              exact Or.inr ⟨p, hc1, hc2, hact3, sa2, x, sn2, hhold', rfl⟩
        · by_cases hct : c = t.c
          · -- involved t.c, pre-(b)
            rw [hct] at hc1 hc2 hinact hf0
            rcases coupled_unpack ts k R R' t.c (hJ1 t.c hc1 hc2) with h1 | h2 | h3
            · exact absurd hf0 (by rw [h1.1]; simp)
            · obtain ⟨_, _, _, hactp, _⟩ := h2
              rw [hinact] at hactp
              exact absurd hactp (by simp)
            · obtain ⟨_, q, hq', _, hn0⟩ := h3
              have hq3 : some ⟨s0, x, n0, true⟩ = some q := hf0.symm.trans hq'
              injection hq3 with hq4
              have hn0' : n0 < k := by have h := hn0; rw [← hq4] at h; exact h
              by_cases hs0 : s0 ≤ prevSmaller (Ls ts) k
              · have hh : hemOf R' t.c (prevSmaller (Ls ts) k) k = some x := by
                  unfold hemOf
                  simp only [hf0]
                  rw [if_pos ⟨hs0, hn0'⟩]
                have hscr : getFM R₁' t.c = getFM R' t.c := by
                  rw [hF1a]
                  exact fmStep_getFM_ne N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S t.c hne
                have hmem : x ∈ (fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa R₁' S₁).2 := by
                  obtain ⟨q, hq, h1, h2, hy⟩ := hemOf_cases R' t.c (prevSmaller (Ls ts) k) k x hh
                  exact (fmStep_S_mem N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa R₁' S₁ x
                    (by omega)).mpr (Or.inr ⟨q, by rw [hscr]; exact hq, h1, h2, by rw [hy]⟩)
                rw [← hF2b] at hmem
                exact Or.inl hmem
              · have hh : hemOf R' t.c (prevSmaller (Ls ts) k) k = none := by
                  unfold hemOf
                  simp only [hf0]
                  rw [if_neg (by omega)]
                obtain ⟨_, _, _, cl5⟩ := app_c hc1 hc2
                obtain ⟨hact3, sa2, sn2, hhold'⟩ := cl5 x hinact ⟨s0, n0, hf0⟩ hh
                exact Or.inr ⟨t.c, hc1, hc2, hact3, sa2, x, sn2, hhold', rfl⟩
          · -- uninvolved c: FM untouched, slot stays inactive
            have hhold : getFM R₂' c = some ⟨s0, x, n0, true⟩ := by
              rw [hFM_u c hcp hct]
              exact hf0
            by_cases hwlt : w < (getR R c).len
            · have hs := hslot_u c hc1 hc2 hcp hct
              rw [if_pos hwlt] at hs
              exact Or.inr ⟨c, hc1, hc2, by rw [hs], s0, x, n0, hhold, rfl⟩
            · have hs := hslot_u c hc1 hc2 hcp hct
              rw [if_neg hwlt] at hs
              exact Or.inr ⟨c, hc1, hc2, by rw [hs]; exact hinact, s0, x, n0, hhold, rfl⟩
    · -- scan drops x at char c
      obtain ⟨c, hc1, hc2, hcw, hcact, hcx⟩ := hdrop
      by_cases hcp : c = p
      · -- involved p
        rw [hcp] at hc1 hc2 hcw hcact hcx
        have hs := hslot_p hc1 hc2
        rw [if_pos hcw, if_pos hcw] at hs
        by_cases hla : ((Ls ts).getD k 0 : Int) > w
        · -- re-armed: FM emits the same pos
          rw [if_pos hla] at hs
          obtain ⟨_, cl3, _, _⟩ := app_p hc1 hc2
          have hh := cl3 (by rw [hs]) hcw hcact
          rw [← hcx] at hh
          have hmem : x ∈ (fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S).2 :=
            hemOf_mem_S N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S x
              (by omega) hh
          rw [← hF1b] at hmem
          exact Or.inl (hS2 x hmem)
        · -- no re-arm: FM either emits (overwrite) or holds (no overwrite)
          rw [if_neg hla] at hs
          have hCa := hJ1 p hc1 hc2
          unfold Coupled at hCa
          cases hq : getFM R' p with
          | none =>
            rw [hq] at hCa
            rw [hCa] at hcact
            exact absurd hcact (by simp)
          | some q =>
            rw [hq] at hCa
            obtain ⟨hqact, hbody⟩ := hCa
            rw [if_pos hcact] at hbody
            obtain ⟨sa0, tp0, ns0, ac0⟩ := q
            obtain ⟨s, hbnd, hsa, hsi, hlen, hpos, hnsv, hpast⟩ := hbody
            have hsa' : sa0 = (s : Int) := hsa
            have hnsv' : ns0 = nextSmaller (Ls ts) s := hnsv
            have hpos' : (getR R p).pos = tp0 := hpos
            have hqact' : ac0 = true := hqact
            by_cases hov : (s : Int) ≤ prevSmaller (Ls ts) k
            · by_cases hns2 : (nextSmaller (Ls ts) s : Nat) < k
              · -- overwrite + emit
                have hh : hemOf R' p (prevSmaller (Ls ts) k) k = some tp0 := by
                  unfold hemOf
                  simp only [hq]
                  rw [if_pos ⟨show sa0 ≤ prevSmaller (Ls ts) k by rw [hsa']; omega,
                    show ns0 < k by rw [hnsv']; exact hns2⟩]
                have hmem : tp0 ∈ (fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S).2 :=
                  hemOf_mem_S N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S tp0
                    (by omega) hh
                rw [← hF1b] at hmem
                rw [hcx, hpos]
                exact Or.inl (hS2 tp0 hmem)
              · -- overwrite without emit: IMPOSSIBLE
                exfalso
                have hlk_w : ((Ls ts).getD k 0 : Int) ≤ w := by omega
                have hLs : (Ls ts).getD s 0 = (getR R p).len := by rw [← hlen]
                have hLt : ((Ls ts).getD k 0 : Int) < ((Ls ts).getD s 0 : Int) := by omega
                have hne1 : prevSmaller (Ls ts) k ≠ -1 := by omega
                obtain ⟨j0, hj0e, hj0k, hj0lt⟩ := prevSmaller_mem (Ls ts) k hne1
                have hslej : s ≤ j0 := by
                  have h1 : (s : Int) ≤ prevSmaller (Ls ts) k := hov
                  rw [hj0e] at h1
                  omega
                have hj0L : j0 < (Ls ts).length := by omega
                by_cases hjs : j0 = s
                · rw [hjs] at hj0lt
                  omega
                · have hns : nextSmaller (Ls ts) s ≤ j0 :=
                    nextSmaller_le (Ls ts) s j0 (by omega) hj0L (by omega)
                  omega
            · -- FM holds: x becomes delayed
              have hhold : getFM R₂' p = some ⟨sa0, tp0, ns0, true⟩ := by
                rw [hshape_p hc1 hc2]
                simp only [hq]
                rw [if_neg (by
                  show ¬(sa0 ≤ prevSmaller (Ls ts) k)
                  rw [hsa']
                  exact hov)]
                rw [hqact']
              exact Or.inr ⟨p, hc1, hc2, by rw [hs], sa0, tp0, ns0, hhold,
                by rw [← hpos', ← hcx]⟩
      · by_cases hct : c = t.c
        · -- starting char t.c (symmetric to p)
          rw [hct] at hc1 hc2 hcw hcact hcx
          have hs := hslot_c hc1 hc2
          rw [if_pos hcw, if_pos hcw] at hs
          by_cases hla : ((Ls ts).getD k 0 : Int) > w
          · rw [if_pos hla] at hs
            obtain ⟨_, cl3, _, _⟩ := app_c hc1 hc2
            have hh := cl3 (by rw [hs]) hcw hcact
            rw [← hcx] at hh
            -- hem for the second fmStep needs the R₁'-scrutinee conversion
            have hmem : x ∈ (fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa R₁' S₁).2 := by
              obtain ⟨q, hq1, hcond1, hcond2, hy⟩ := hemOf_cases R' t.c (prevSmaller (Ls ts) k) k x hh
              have hscr : getFM R₁' t.c = getFM R' t.c := by
                rw [hF1a]
                exact fmStep_getFM_ne N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S t.c hne
              exact (fmStep_S_mem N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa R₁' S₁ x
                (by omega)).mpr (Or.inr ⟨q, by rw [hscr]; exact hq1, hcond1, hcond2, by rw [hy]⟩)
            rw [← hF2b] at hmem
            exact Or.inl hmem
          · rw [if_neg hla] at hs
            have hCa := hJ1 t.c hc1 hc2
            unfold Coupled at hCa
            cases hq : getFM R' t.c with
            | none =>
              rw [hq] at hCa
              rw [hCa] at hcact
              exact absurd hcact (by simp)
            | some q =>
              rw [hq] at hCa
              obtain ⟨hqact, hbody⟩ := hCa
              rw [if_pos hcact] at hbody
              obtain ⟨sa0, tp0, ns0, ac0⟩ := q
              obtain ⟨s, hbnd, hsa, hsi, hlen, hpos, hnsv, hpast⟩ := hbody
              have hsa' : sa0 = (s : Int) := hsa
              have hnsv' : ns0 = nextSmaller (Ls ts) s := hnsv
              have hpos' : (getR R t.c).pos = tp0 := hpos
              have hqact' : ac0 = true := hqact
              by_cases hov : (s : Int) ≤ prevSmaller (Ls ts) k
              · by_cases hns2 : (nextSmaller (Ls ts) s : Nat) < k
                · have hh : hemOf R' t.c (prevSmaller (Ls ts) k) k = some tp0 := by
                    unfold hemOf
                    simp only [hq]
                    rw [if_pos ⟨show sa0 ≤ prevSmaller (Ls ts) k by rw [hsa']; omega,
                      show ns0 < k by rw [hnsv']; exact hns2⟩]
                  have hscr : getFM R₁' t.c = getFM R' t.c := by
                    rw [hF1a]
                    exact fmStep_getFM_ne N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S t.c hne
                  have hmem : tp0 ∈ (fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa R₁' S₁).2 := by
                    obtain ⟨q1, hq1, hcc1, hcc2, hy⟩ := hemOf_cases R' t.c (prevSmaller (Ls ts) k) k tp0 hh
                    exact (fmStep_S_mem N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa R₁' S₁ tp0
                      (by omega)).mpr (Or.inr ⟨q1, by rw [hscr]; exact hq1, hcc1, hcc2, by rw [hy]⟩)
                  rw [← hF2b] at hmem
                  rw [hcx, hpos]
                  exact Or.inl hmem
                · exfalso
                  have hlk_w : ((Ls ts).getD k 0 : Int) ≤ w := by omega
                  have hLs : (Ls ts).getD s 0 = (getR R t.c).len := by rw [← hlen]
                  have hLt : ((Ls ts).getD k 0 : Int) < ((Ls ts).getD s 0 : Int) := by omega
                  have hne1 : prevSmaller (Ls ts) k ≠ -1 := by omega
                  obtain ⟨j0, hj0e, hj0k, hj0lt⟩ := prevSmaller_mem (Ls ts) k hne1
                  have hslej : s ≤ j0 := by
                    have h1 : (s : Int) ≤ prevSmaller (Ls ts) k := hov
                    rw [hj0e] at h1
                    omega
                  have hj0L : j0 < (Ls ts).length := by omega
                  by_cases hjs : j0 = s
                  · rw [hjs] at hj0lt
                    omega
                  · have hns : nextSmaller (Ls ts) s ≤ j0 :=
                      nextSmaller_le (Ls ts) s j0 (by omega) hj0L (by omega)
                    omega
              · have hhold : getFM R₂' t.c = some ⟨sa0, tp0, ns0, true⟩ := by
                  rw [hshape_c hc1 hc2]
                  simp only [hq]
                  rw [if_neg (by
                    show ¬(sa0 ≤ prevSmaller (Ls ts) k)
                    rw [hsa']
                    exact hov)]
                  rw [hqact']
                exact Or.inr ⟨t.c, hc1, hc2, by rw [hs], sa0, tp0, ns0, hhold,
                  by rw [← hpos', ← hcx]⟩
        · -- uninvolved
          obtain ⟨s1, p1, n1, hhold, hp1eq, hact3⟩ :=
            app_notinv c hc1 hc2 hcp hct hcw hcact
          rw [← hcx] at hp1eq
          rw [hp1eq] at hhold
          exact Or.inr ⟨c, hc1, hc2, hact3, s1, x, n1, hhold, rfl⟩
  · -- ← direction: S' ∪ Delayed-post ⊆ out₁
    intro h
    rcases h with hS' | hdelay3
    · -- x ∈ S'
      by_cases ht0 : t.c = 0
      · -- t.c-step is a no-op: S' = S₁; peel the p-step
        rw [hF2b, ht0, fmStep_S_zero] at hS'
        by_cases hp0 : p = 0
        · rw [hF1b, hp0, fmStep_S_zero] at hS'
          exact hout1 x ((hJ2 x).mpr (Or.inl hS'))
        · rw [hF1b] at hS'
          rcases (fmStep_S_mem N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S x hp0).mp hS'
            with hS | ⟨q, hq, h1, h2, hy⟩
          · exact hout1 x ((hJ2 x).mpr (Or.inl hS))
          · have hh : hemOf R' p (prevSmaller (Ls ts) k) k = some x := by
              unfold hemOf
              simp only [hq]
              rw [if_pos ⟨h1, h2⟩, ← hy]
            have hp1 : 1 ≤ p := by omega
            have hpS : p < SIGMA := by
              have := getFM_lt R' p q hq
              omega
            obtain ⟨cl2, _, _, _⟩ := app_p hp1 hpS
            rcases cl2 x hh with ⟨hinact, sa, sn, hf⟩ | ⟨hcw, hcact, hcx⟩
            · exact hout1 x ((hJ2 x).mpr (Or.inr ⟨p, hp1, hpS, hinact, sa, x, sn, hf, rfl⟩))
            · rw [hE1]
              exact (evalStep_mem w R out x).mpr (Or.inr ⟨p, hp1, hpS, hcw, hcact, hcx⟩)
      · -- t.c ≠ 0
        rw [hF2b] at hS'
        rcases (fmStep_S_mem N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa R₁' S₁ x ht0).mp hS'
          with hS1 | ⟨q, hq, h1, h2, hy⟩
        · -- x ∈ S₁: peel the p-step
          by_cases hp0 : p = 0
          · rw [hF1b, hp0, fmStep_S_zero] at hS1
            exact hout1 x ((hJ2 x).mpr (Or.inl hS1))
          · rw [hF1b] at hS1
            rcases (fmStep_S_mem N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S x hp0).mp hS1
              with hS | ⟨q2, hq2, h12, h22, hy2⟩
            · exact hout1 x ((hJ2 x).mpr (Or.inl hS))
            · have hh : hemOf R' p (prevSmaller (Ls ts) k) k = some x := by
                unfold hemOf
                simp only [hq2]
                rw [if_pos ⟨h12, h22⟩, ← hy2]
              have hp1 : 1 ≤ p := by omega
              have hpS : p < SIGMA := by
                have := getFM_lt R' p q2 hq2
                omega
              obtain ⟨cl2, _, _, _⟩ := app_p hp1 hpS
              rcases cl2 x hh with ⟨hinact, sa, sn, hf⟩ | ⟨hcw, hcact, hcx⟩
              · exact hout1 x ((hJ2 x).mpr (Or.inr ⟨p, hp1, hpS, hinact, sa, x, sn, hf, rfl⟩))
              · rw [hE1]
                exact (evalStep_mem w R out x).mpr (Or.inr ⟨p, hp1, hpS, hcw, hcact, hcx⟩)
        · -- emitted at t.c: clause 2 of app_c (convert R₁'-scrutinee to R')
          have hscr : getFM R₁' t.c = getFM R' t.c := by
            rw [hF1a]
            exact fmStep_getFM_ne N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S t.c hne
          have hq' : getFM R' t.c = some q := by rw [← hscr]; exact hq
          have hh : hemOf R' t.c (prevSmaller (Ls ts) k) k = some x := by
            unfold hemOf
            simp only [hq']
            rw [if_pos ⟨h1, h2⟩, ← hy]
          have ht1 : 1 ≤ t.c := by omega
          have htS : t.c < SIGMA := by
            have := getFM_lt R' t.c q hq'
            omega
          obtain ⟨cl2, _, _, _⟩ := app_c ht1 htS
          rcases cl2 x hh with ⟨hinact, sa, sn, hf⟩ | ⟨hcw, hcact, hcx⟩
          · exact hout1 x ((hJ2 x).mpr (Or.inr ⟨t.c, ht1, htS, hinact, sa, x, sn, hf, rfl⟩))
          · rw [hE1]
            exact (evalStep_mem w R out x).mpr (Or.inr ⟨t.c, ht1, htS, hcw, hcact, hcx⟩)
    · -- Delayed R₃ R₂' x
      obtain ⟨c, hc1, hc2, hinact3, s0, x0, n0, hf3, hx0⟩ := hdelay3
      rw [hx0] at hf3
      by_cases hcp : c = p
      · rw [hcp] at hc1 hc2 hinact3 hf3
        obtain ⟨_, _, cl4, _⟩ := app_p hc1 hc2
        rcases cl4 x hinact3 ⟨s0, n0, hf3⟩ with ⟨hinact, sa, sn, hf⟩ | ⟨hcw, hcact, hcx⟩
        · exact hout1 x ((hJ2 x).mpr (Or.inr ⟨p, hc1, hc2, hinact, sa, x, sn, hf, rfl⟩))
        · rw [hE1]
          exact (evalStep_mem w R out x).mpr (Or.inr ⟨p, hc1, hc2, hcw, hcact, hcx⟩)
      · by_cases hct : c = t.c
        · rw [hct] at hc1 hc2 hinact3 hf3
          obtain ⟨_, _, cl4, _⟩ := app_c hc1 hc2
          rcases cl4 x hinact3 ⟨s0, n0, hf3⟩ with ⟨hinact, sa, sn, hf⟩ | ⟨hcw, hcact, hcx⟩
          · exact hout1 x ((hJ2 x).mpr (Or.inr ⟨t.c, hc1, hc2, hinact, sa, x, sn, hf, rfl⟩))
          · rw [hE1]
            exact (evalStep_mem w R out x).mpr (Or.inr ⟨t.c, hc1, hc2, hcw, hcact, hcx⟩)
        · -- uninvolved: FM R' c = FM R₂' c; case on Coupled-pre
          have hf0 : getFM R' c = some ⟨s0, x, n0, true⟩ := by
            rw [← hFM_u c hcp hct]
            exact hf3
          rcases coupled_unpack ts k R R' c (hJ1 c hc1 hc2) with h1 | h2 | h3
          · exact absurd hf0 (by rw [h1.1]; simp)
          · -- (a): pre-active; post-inactive forces the reset; drop with pos = x
            obtain ⟨q, hq', hqact', hactp, htp⟩ := h2
            have hq3 : some ⟨s0, x, n0, true⟩ = some q := hf0.symm.trans hq'
            injection hq3 with hq4
            rw [← hq4] at htp
            have hs := hslot_u c hc1 hc2 hcp hct
            by_cases hwlt : w < (getR R c).len
            · rw [if_pos hwlt] at hs
              rw [hE1]
              exact (evalStep_mem w R out x).mpr (Or.inr ⟨c, hc1, hc2, hwlt, hactp, htp⟩)
            · rw [if_neg hwlt] at hs
              rw [hs] at hinact3
              rw [hinact3] at hactp
              exact absurd hactp (by simp)
          · -- (b): pre-delayed: x was already emitted
            obtain ⟨hinact, _, _, _, _⟩ := h3
            exact hout1 x ((hJ2 x).mpr (Or.inr ⟨c, hc1, hc2, hinact, s0, x, n0, hf0, rfl⟩))


-- one-step machine unfoldings
theorem scanAux_cons_bnd (N : Nat) (t : Triple) (rest' : List Triple) (p : Nat) (pSa : Nat)
    (m : Int) (R : List Cand) (out : List Nat) (hne : (t.c != p) = true) :
    scanAux N (t :: rest') p pSa m R out =
      scanAux N rest' t.c t.sa MAXINT
        (upd (upd (evalStep (min m t.lcp) R out).2 p t.lcp (N - pSa)) t.c t.lcp (N - t.sa))
        (evalStep (min m t.lcp) R out).1 := by
  simp only [scanAux]
  rw [if_pos hne]

theorem scanAux_cons_nonbnd (N : Nat) (t : Triple) (rest' : List Triple) (p : Nat) (pSa : Nat)
    (m : Int) (R : List Cand) (out : List Nat) (hne : (t.c != p) = false) :
    scanAux N (t :: rest') p pSa m R out =
      scanAux N rest' t.c t.sa (min m t.lcp) R out := by
  simp only [scanAux]
  rw [if_neg (by simp [hne])]

theorem fmAux_cons_bnd (N : Nat) (psv : List Int) (nsv : List Nat) (inf : Nat) (t : Triple)
    (rest' : List Triple) (k : Nat) (prev : Triple) (R' : List (Option CandFM)) (S : List Nat)
    (hne : (t.c != prev.c) = true) :
    fmAux N psv nsv inf (t :: rest') k prev R' S =
      fmAux N psv nsv inf rest' (k+1) t
        (fmStep N (psv.getD k (-1)) (nsv.getD k inf) k t.c t.sa
          (fmStep N (psv.getD k (-1)) (nsv.getD k inf) k prev.c prev.sa R' S).1
          (fmStep N (psv.getD k (-1)) (nsv.getD k inf) k prev.c prev.sa R' S).2).1
        (fmStep N (psv.getD k (-1)) (nsv.getD k inf) k t.c t.sa
          (fmStep N (psv.getD k (-1)) (nsv.getD k inf) k prev.c prev.sa R' S).1
          (fmStep N (psv.getD k (-1)) (nsv.getD k inf) k prev.c prev.sa R' S).2).2 := by
  simp only [fmAux]
  rw [if_pos hne]

theorem fmAux_cons_nonbnd (N : Nat) (psv : List Int) (nsv : List Nat) (inf : Nat) (t : Triple)
    (rest' : List Triple) (k : Nat) (prev : Triple) (R' : List (Option CandFM)) (S : List Nat)
    (hne : (t.c != prev.c) = false) :
    fmAux N psv nsv inf (t :: rest') k prev R' S =
      fmAux N psv nsv inf rest' (k+1) t R' S := by
  simp only [fmAux]
  rw [if_neg (by simp [hne])]

theorem bound_coupled (N : Nat) (ts : List Triple) (k : Nat) (hk1 : 1 ≤ k) (hk : k < ts.length)
    (hb : isBnd ts k = true)
    (hsat : ∀ j, j < ts.length → (Ls ts).getD j 0 ≤ MAXINT.toNat)
    (R : List Cand) (R' : List (Option CandFM))
    (hJ1 : ∀ c, 1 ≤ c → c < SIGMA → Coupled ts k R R' c)
    (hRlen : SIGMA ≤ R.length) (hR'len : SIGMA ≤ R'.length)
    (t : Triple) (p : Nat) (pSa : Nat)
    (hp : p = (ts.getD (k-1) dT).c) (hpSa : pSa = (ts.getD (k-1) dT).sa)
    (ht : t = ts.getD k dT)
    (out out₁ S S₁ S' : List Nat) (R₁ R₂ R₃ : List Cand) (R₁' R₂' : List (Option CandFM))
    (w : Int) (hw : w = min (winOpen (Ls ts) (prevBnd ts k) k) ((Ls ts).getD k 0))
    (hE : (out₁, R₁) = evalStep w R out)
    (hU1 : R₂ = upd R₁ p t.lcp (N - pSa))
    (hU2 : R₃ = upd R₂ t.c t.lcp (N - t.sa))
    (hF1 : (R₁', S₁) = fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S)
    (hF2 : (R₂', S') = fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa R₁' S₁)
    : ∀ c, 1 ≤ c → c < SIGMA → Coupled ts (k+1) R₃ R₂' c := by
  have hE2 : R₁ = (evalStep w R out).2 := congrArg Prod.snd hE
  have hF1a : R₁' = (fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S).1 := congrArg Prod.fst hF1
  have hF2a : R₂' = (fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa R₁' S₁).1 := congrArg Prod.fst hF2
  have hlk : (Ls ts).getD k 0 = t.lcp := by rw [Ls_getD ts k hk, ← ht]
  have hne : t.c ≠ p := by
    unfold isBnd at hb
    obtain ⟨_, _, hcp⟩ := of_decide_eq_true hb
    rw [ht, hp]
    exact hcp
  have hR1len : SIGMA ≤ R₁.length := by rw [hE2, evalStep_len]; exact hRlen
  have hR2len : SIGMA ≤ R₂.length := by rw [hU1, upd_len]; exact hR1len
  have hR1'len : SIGMA ≤ R₁'.length := by rw [hF1a, fm_step_len]; exact hR'len
  intro c hc1 hc2
  by_cases hcp : c = p
  · subst hcp
    have hC := hJ1 c hc1 hc2
    have hslotE : getR R₁ c = if w < (getR R c).len then ⟨w, 0, false⟩ else getR R c := by
      rw [hE2]; exact evalStep_getR w R out c hc1 hc2 hRlen
    have hslotU : getR R₃ c = getR (upd R₁ c t.lcp (N - pSa)) c := by
      rw [hU2, upd_getR_ne _ _ _ _ c (Ne.symm hne), hU1]
    have hR₃ := slot_composite w t.lcp (N - pSa) R R₁ R₃ c (by omega) hslotE hslotU
    rw [← hlk] at hR₃
    have hR₂' : getFM R₂' c =
        (match getFM R' c with
         | none => some ⟨(k : Int), N - pSa, nextSmaller (Ls ts) k, true⟩
         | some q => if q.saPos ≤ prevSmaller (Ls ts) k then
                      some ⟨(k : Int), N - pSa, nextSmaller (Ls ts) k, true⟩ else some q) := by
      rw [hF2a, fmStep_getFM_ne _ _ _ _ _ _ _ _ c (Ne.symm hne), hF1a]
      exact fmStep_getFM_self N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k c pSa R' S
        (by omega) (by omega)
    exact (coupled_step_inv ts k hk1 hk hb hsat R R' c hc1 hc2 hC w hw (N - pSa)
      (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) rfl rfl R₃ R₂' hR₃ hR₂'
      (match getFM R' c with
       | none => none
       | some q => if q.saPos ≤ prevSmaller (Ls ts) k ∧ (q.nsv < k) then some q.textPos else none)
      rfl).1
  · by_cases hct : c = t.c
    · subst hct
      have hC := hJ1 t.c hc1 hc2
      have hslotE : getR R₁ t.c = if w < (getR R t.c).len then ⟨w, 0, false⟩ else getR R t.c := by
        rw [hE2]; exact evalStep_getR w R out t.c hc1 hc2 hRlen
      have hslotE' : getR R₂ t.c = if w < (getR R t.c).len then ⟨w, 0, false⟩ else getR R t.c := by
        rw [hU1, upd_getR_ne _ _ _ _ t.c hne, hslotE]
      have hslotU : getR R₃ t.c = getR (upd R₂ t.c t.lcp (N - t.sa)) t.c := by rw [hU2]
      have hR₃ := slot_composite w t.lcp (N - t.sa) R R₂ R₃ t.c (by omega) hslotE' hslotU
      rw [← hlk] at hR₃
      have hR₂' : getFM R₂' t.c =
          (match getFM R' t.c with
           | none => some ⟨(k : Int), N - t.sa, nextSmaller (Ls ts) k, true⟩
           | some q => if q.saPos ≤ prevSmaller (Ls ts) k then
                        some ⟨(k : Int), N - t.sa, nextSmaller (Ls ts) k, true⟩ else some q) := by
        rw [hF2a]
        rw [fmStep_getFM_self N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa R₁' S₁
          (by omega) (by omega)]
        rw [hF1a, fmStep_getFM_ne _ _ _ _ _ _ _ _ t.c hne]
      exact (coupled_step_inv ts k hk1 hk hb hsat R R' t.c hc1 hc2 hC w hw (N - t.sa)
        (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) rfl rfl R₃ R₂' hR₃ hR₂'
        (match getFM R' t.c with
         | none => none
         | some q => if q.saPos ≤ prevSmaller (Ls ts) k ∧ (q.nsv < k) then some q.textPos else none)
        rfl).1
    · have hC := hJ1 c hc1 hc2
      have hslotE : getR R₁ c = if w < (getR R c).len then ⟨w, 0, false⟩ else getR R c := by
        rw [hE2]; exact evalStep_getR w R out c hc1 hc2 hRlen
      have hR₃ : getR R₃ c = (if w < (getR R c).len then ⟨w, 0, false⟩ else getR R c) := by
        rw [hU2, upd_getR_ne _ _ _ _ c hct, hU1, upd_getR_ne _ _ _ _ c hcp, hslotE]
      have hR₂' : getFM R₂' c = getFM R' c := by
        rw [hF2a, fmStep_getFM_ne _ _ _ _ _ _ _ _ c hct,
            hF1a, fmStep_getFM_ne _ _ _ _ _ _ _ _ c hcp]
      exact (coupled_step_notinv ts k hk1 hk hb hsat R R' c hc1 hc2 hC R₃ R₂' w hw hR₃ hR₂').1

set_option maxRecDepth 20000 in
theorem joint_stream (N : Nat) (ts : List Triple)
    (hsat : ∀ j, j < ts.length → (Ls ts).getD j 0 ≤ MAXINT.toNat) :
    ∀ (rest : List Triple) (k : Nat) (prev : Triple) (p : Nat) (pSa : Nat) (m : Int)
      (R : List Cand) (R' : List (Option CandFM)) (out S : List Nat),
    rest = ts.drop k → 1 ≤ k → k ≤ ts.length →
    prev = ts.getD (k-1) dT → p = prev.c → pSa = prev.sa →
    m = winOpen (Ls ts) (prevBnd ts k) k →
    SIGMA ≤ R.length → R'.length = SIGMA →
    JointInv ts k R R' out S →
    ∀ x, x ∈ scanAux N rest p pSa m R out ↔
         x ∈ fmAux N (psvList (Ls ts)) (nsvList (Ls ts)) (ts.length + 1) rest k prev R' S := by
  intro rest k
  induction rest generalizing k with
  | nil =>
    intro prev p pSa m R R' out S hrest hk1 hklen hprev hp hpSa hm hRlen hR'len hJ x
    -- flush both machines (the nil-arms fire regardless of k)
    show x ∈ (evalStep (-1) R out).1 ↔ x ∈ finalEmit R' S
    obtain ⟨hJ1, hJ2⟩ := hJ
    constructor
    · intro hx
      rcases (evalStep_mem (-1) R out x).mp hx with hout | ⟨c, hc1, hc2, hcw, hcact, hcx⟩
      · rcases (hJ2 x).mp hout with hS | ⟨c, hc1, hc2, hinact, s0, x0, n0, hf0, hx0⟩
        · exact (finalEmit_mem R' S x).mpr (Or.inl hS)
        · rw [hx0] at hf0
          exact (finalEmit_mem R' S x).mpr
            (Or.inr ⟨c, hc1, hc2, ⟨s0, x, n0, true⟩, hf0, rfl, rfl⟩)
      · have hC := hJ1 c hc1 hc2
        rcases coupled_unpack ts k R R' c hC with h1 | h2 | h3
        · obtain ⟨_, hslot⟩ := h1
          rw [hslot] at hcact
          exact absurd hcact (by simp)
        · obtain ⟨q, hq', hqact', hactp, htp⟩ := h2
          rw [← hcx] at htp
          exact (finalEmit_mem R' S x).mpr (Or.inr ⟨c, hc1, hc2, q, hq', hqact', htp.symm⟩)
        · obtain ⟨hinact, _, _, _, _⟩ := h3
          rw [hinact] at hcact
          exact absurd hcact (by simp)
    · intro hx
      rcases (finalEmit_mem R' S x).mp hx with hS | ⟨c, hc1, hc2, q, hq', hqact', htx⟩
      · exact (evalStep_mem (-1) R out x).mpr (Or.inl ((hJ2 x).mpr (Or.inl hS)))
      · have hC := hJ1 c hc1 hc2
        unfold Coupled at hC
        cases hq2 : getFM R' c with
        | none =>
          rw [hq2] at hq'
          exact absurd hq' (by simp)
        | some q2 =>
          rw [hq2] at hC
          obtain ⟨hqact2, hbody⟩ := hC
          by_cases hqa : (getR R c).active = true
          · -- (a): scan slot active with pos = q2.textPos = x; flush emits it
            rw [if_pos hqa] at hbody
            obtain ⟨s, hbnd, hsa, hsi, hlen, hpos, hnsv, hpast⟩ := hbody
            -- -1 < len from len = L[s] (Nat)
            have hlt : (-1 : Int) < (getR R c).len := by
              rw [hlen]
              have := Nat.zero_le ((Ls ts).getD s 0)
              omega
            -- q2.textPos = x
            have hq4 : q = q2 := Option.some.inj (hq'.symm.trans hq2)
            rw [hq4] at htx
            rw [← htx] at hpos
            exact (evalStep_mem (-1) R out x).mpr
              (Or.inr ⟨c, hc1, hc2, hlt, hqa, hpos.symm⟩)
          · -- (b): pre-delayed: x already in out
            rw [if_neg hqa] at hbody
            obtain ⟨s, hbnd, hsa, hsi, hnsv, hnlt, hlen, hwit, hpast⟩ := hbody
            have hq5 : getFM R' c = some ⟨q.saPos, q.textPos, q.nsv, q.active⟩ := hq'
            rw [hqact'] at hq5
            exact (evalStep_mem (-1) R out x).mpr
              (Or.inl ((hJ2 x).mpr (Or.inr ⟨c, hc1, hc2, by simp [hqa], q.saPos, q.textPos, q.nsv, hq5, htx.symm⟩)))
  | cons t rest' ih =>
    intro prev p pSa m R R' out S hrest hk1 hklen hprev hp hpSa hm hRlen hR'len hJ x
    obtain ⟨ht, hrest'⟩ := drop_cons_getD ts k t rest' hrest.symm
    have hklt : k < ts.length := by
      have h1 : (ts.drop k).length = ts.length - k := List.length_drop
      rw [hrest.symm] at h1
      simp only [List.length_cons] at h1
      omega
    have hlk : (Ls ts).getD k 0 = t.lcp := by rw [Ls_getD ts k hklt, ← ht]
    by_cases hcp : t.c = p
    · -- NON-boundary row
      have hb : isBnd ts k = false := by
        have h3 : (ts.getD k dT).c = (ts.getD (k-1) dT).c := by rw [ht, ← hprev, ← hp]; exact hcp
        unfold isBnd
        rw [decide_eq_false]
        intro hcon
        obtain ⟨_, _, h3'⟩ := hcon
        exact absurd h3 h3'
      have hbne : (t.c != p) = false := by simp [hcp]
      have hbne2 : (t.c != prev.c) = false := by simp [hcp, hp]
      rw [scanAux_cons_nonbnd N t rest' p pSa m R out hbne]
      rw [fmAux_cons_nonbnd N (psvList (Ls ts)) (nsvList (Ls ts)) (ts.length+1) t rest' k prev R' S hbne2]
      -- new state: k+1, prev := t, p := t.c, pSa := t.sa, m := min m t.lcp; tables unchanged
      have hbf : isBnd ts k = false := hb
      have hJ' : JointInv ts (k+1) R R' out S :=
        ⟨fun c hc1 hc2 => coupled_nonbnd ts k hbf R R' c (hJ.1 c hc1 hc2), hJ.2⟩
      have hm' : min m t.lcp = winOpen (Ls ts) (prevBnd ts (k+1)) (k+1) := by
        have hpb : prevBnd ts (k+1) = prevBnd ts k := prevBnd_nonbnd ts k hbf
        have hpbk : prevBnd ts k < k := by
          rcases prevBnd_lt ts k with h | h
          · exact h
          · omega
        rw [hpb, winOpen_extend (L := (Ls ts)) (pb := prevBnd ts k) (i := k) hpbk, ← hlk, hm]
      exact ih (k+1) t t.c t.sa (min m t.lcp) R R' out S hrest'.symm (by omega) (by omega)
        (by show t = ts.getD k dT; exact ht.symm) rfl rfl hm' hRlen hR'len hJ' x
    · -- BOUNDARY row
      have hne : t.c ≠ p := by
        have h1 : ¬(t.c = p) := hcp
        exact fun h => h1 h
      have hb : isBnd ts k = true := by
        have h3 : (ts.getD k dT).c ≠ (ts.getD (k-1) dT).c := by
          rw [ht, ← hprev, ← hp]; exact hne
        unfold isBnd
        rw [decide_eq_true]
        exact ⟨hk1, hklt, h3⟩
      have hbne : (t.c != p) = true := by simp [hne]
      have hne2 : t.c ≠ prev.c := by rw [← hp]; exact hne
      have hbne2 : (t.c != prev.c) = true := by simp [hne2]
      rw [scanAux_cons_bnd N t rest' p pSa m R out hbne]
      rw [fmAux_cons_bnd N (psvList (Ls ts)) (nsvList (Ls ts)) (ts.length+1) t rest' k prev R' S hbne2]
      -- convert psv/nsv lookups and prev-fields
      have hLlen : (Ls ts).length = ts.length := by rw [Ls, List.length_map]
      have hnsvconv : (nsvList (Ls ts)).getD k (ts.length + 1) = nextSmaller (Ls ts) k := by
        rw [← hLlen]
        exact nsvList_getD (Ls ts) k (by omega)
      rw [psvList_getD (Ls ts) k (by omega), hnsvconv, ← hp, ← hpSa]
      -- name intermediates
      generalize hE : (evalStep (min m t.lcp) R out) = EV
      generalize hU1 : (upd EV.2 p t.lcp (N - pSa)) = R2
      generalize hU2 : (upd R2 t.c t.lcp (N - t.sa)) = R3
      generalize hF1 : (fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S) = F1
      generalize hF2 : (fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa F1.1 F1.2) = F2
      -- w alignment
      have hw : min m t.lcp = min (winOpen (Ls ts) (prevBnd ts k) k) ((Ls ts).getD k 0) := by
        rw [← hlk, hm]
      have hE' : (EV.1, EV.2) = evalStep (min m t.lcp) R out := by rw [← hE]
      have hU1' : R2 = upd EV.2 p t.lcp (N - pSa) := hU1.symm
      have hU2' : R3 = upd R2 t.c t.lcp (N - t.sa) := hU2.symm
      have hF1' : (F1.1, F1.2) = fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S := by rw [← hF1]
      have hF2' : (F2.1, F2.2) = fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa F1.1 F1.2 := by rw [← hF2]
      -- table lengths
      have hE2' : EV.2 = (evalStep (min m t.lcp) R out).2 := (congrArg Prod.snd hE).symm
      have hR2len : SIGMA ≤ R2.length := by rw [hU1', upd_len, hE2', evalStep_len]; exact hRlen
      have hR3len : SIGMA ≤ R3.length := by rw [hU2', upd_len]; exact hR2len
      have hF1a : F1.1 = (fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k p pSa R' S).1 := by rw [hF1]
      have hF1len : F1.1.length = SIGMA := by rw [hF1a, fm_step_len]; exact hR'len
      have hF2a : F2.1 = (fmStep N (prevSmaller (Ls ts) k) (nextSmaller (Ls ts) k) k t.c t.sa F1.1 F1.2).1 := by rw [hF2]
      have hF2len : F2.1.length = SIGMA := by rw [hF2a, fm_step_len]; exact hF1len
      have hR'len' : SIGMA ≤ R'.length := by omega
      have hprevRow : prev = ts.getD (k-1) dT := hprev
      have hpRow : p = (ts.getD (k-1) dT).c := by rw [← hprev]; exact hp
      have hpSaRow : pSa = (ts.getD (k-1) dT).sa := by rw [← hprev]; exact hpSa
      -- the step theorems
      have hC' := bound_coupled N ts k hk1 hklt hb hsat R R' hJ.1 hRlen hR'len' t p pSa hpRow hpSaRow ht.symm
        out EV.1 S F1.2 F2.2 EV.2 R2 R3 F1.1 F2.1 (min m t.lcp) hw hE' hU1' hU2' hF1' hF2'
      have hM' := bound_mem N ts k hk1 hklt hb hsat R R' hJ.1 hRlen hR'len t p pSa hpRow hpSaRow ht.symm
        out EV.1 S F1.2 F2.2 EV.2 R2 R3 F1.1 F2.1 hJ.2 (min m t.lcp) hw hE' hU1' hU2' hF1' hF2'
      have hJ' : JointInv ts (k+1) R3 F2.1 EV.1 F2.2 := ⟨hC', hM'⟩
      have hm' : MAXINT = winOpen (Ls ts) (prevBnd ts (k+1)) (k+1) := by
        rw [prevBnd_bnd ts k hb, winOpen_fresh]
      exact ih (k+1) t t.c t.sa MAXINT R3 F2.1 EV.1 F2.2 hrest'.symm (by omega) (by omega)
        (by show t = ts.getD k dT; exact ht.symm) rfl rfl hm' hR3len hF2len hJ' x


theorem fm_equiv_stream (N : Nat) (ts : List Triple) (t0 : Triple) (rest : List Triple)
    (hts : ts = t0 :: rest)
    (hsat : ∀ j, j < ts.length → (Ls ts).getD j 0 ≤ MAXINT.toNat) :
    ∀ x, x ∈ scanAux N rest t0.c t0.sa MAXINT defaultR [] ↔
         x ∈ fmAux N (psvList (Ls ts)) (nsvList (Ls ts)) (ts.length + 1) rest 1 t0 fmR0 [] := by
  intro x
  have hrow : t0 = ts.getD 0 dT := by rw [hts]; rfl
  have hrest : rest = ts.drop 1 := by rw [hts]; rfl
  have hprev : t0 = ts.getD (1-1) dT := hrow
  have hpb0 : prevBnd ts 1 = 0 := by
    have h0 : isBnd ts 0 = false := by
      unfold isBnd
      rw [decide_eq_false]
      intro hcon
      obtain ⟨h1, _, _⟩ := hcon
      omega
    rw [prevBnd, h0]
    simp [prevBnd]
  have hmbase : MAXINT = winOpen (Ls ts) (prevBnd ts 1) 1 := by
    rw [hpb0]
    unfold winOpen
    rw [if_neg (by omega)]
    simp
  have hklen : 1 ≤ ts.length := by rw [hts]; simp [List.length_cons]
  exact joint_stream N ts hsat rest 1 t0 t0.c t0.sa MAXINT defaultR fmR0 [] [] hrest
    (by omega) hklen hprev rfl rfl hmbase (by rw [defaultR_len]; omega) (by rw [fmR0_len])
    (jointInv_base ts) x

/-! ### END ASSEMBLY-INSERT -/

/-- The operative (bounded) form of the FM-equivalence pillar: the scan
machine and the fmSpec machine emit the same suffixient set for every
text whose LCP values respect the model's Int arithmetic (side-condition
`hsat` is vacuous for real texts: lcp ≤ |T| ≤ 2^63).  Proof route: the
proven `fm_equivalence_of_event_bridge` reduction + the FmJoint joint
induction (`coupled_step_inv`, sorry-free) + the HANDOFF assembly steps
(boundary-step interleave, non-boundary row, final flush, main
induction over JointInv). -/
theorem fm_equivalence_bounded (T : Text) (hT : positive T = true)
    (hsat : ∀ t ∈ triplesOf T, t.lcp ≤ MAXINT.toNat) :
    ∀ x, x ∈ scan (T.length + 1) (triplesOf T) ↔
      x ∈ fmSpec (T.length + 1) (triplesOf T) := by
  -- REDUCED (2026-10-02 lane): `fm_equivalence_of_event_bridge` proves this
  -- from the sorted-outputs (multiset) equality
  --   (scan …).mergeSort ≤ = (fmSpec …).mergeSort ≤,
  -- i.e. from the Lane-B event bridge: both machines emit the same multiset of
  -- (char, position) events.  The open obligation is exhibiting that common
  -- per-character event process for the two state machines.
  --
  -- Remaining: a two-state-machine simulation.  The natural prefix invariants
  -- are FALSE (both the emitted set and the candidate tables disagree at
  -- intermediate steps), so the proof must compare the two candidate tables at
  -- the *end* of the stream, or route both machines through a shared
  -- LCP-maxima characterisation.  This is the same core as Lemma 34.
  intro x
  have hsat' : ∀ j, j < (triplesOf T).length → (Ls (triplesOf T)).getD j 0 ≤ MAXINT.toNat := by
    intro j hj
    have hmem : (triplesOf T).getD j dT ∈ triplesOf T := getD_mem _ _ _ hj
    have hle := hsat _ hmem
    rw [Ls_getD (triplesOf T) j hj]
    exact hle
  cases h : triplesOf T with
  | nil =>
    show x ∈ ([] : List Nat) ↔ x ∈ ([] : List Nat)
    exact Iff.rfl
  | cons t0 rest =>
    show x ∈ scanAux (T.length + 1) rest t0.c t0.sa MAXINT defaultR [] ↔
      x ∈ fmAux (T.length + 1) (psvList (Ls (t0 :: rest))) (nsvList (Ls (t0 :: rest)))
        ((t0 :: rest).length + 1) rest 1 t0
        ((List.range SIGMA).map (fun _ => (none : Option CandFM))) []
    have hsat'' : ∀ j, j < (t0 :: rest).length → (Ls (t0 :: rest)).getD j 0 ≤ MAXINT.toNat := by
      intro j hj
      have hj' : j < (triplesOf T).length := by rw [h]; exact hj
      rw [← h]
      exact hsat' j hj'
    exact fm_equiv_stream (T.length + 1) (t0 :: rest) t0 rest rfl hsat'' x


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

/-! ### Statement-lock evals for the isolated obligations (O1)–(O4)

Computable (polynomial) restatements of the obligations, evaluated on the
729-text battery so the evidence travels with the code. -/

private def covSL (T : Text) (x : Nat) : List (List Nat × Nat) :=
  (requirements T).filter (fun p => coversAt (p.1 ++ [p.2]) x T)

private def subSL (T : Text) (x y : Nat) : Bool := (covSL T x).all (fun p => (covSL T y).contains p)

private def maxSL (T : Text) (x : Nat) : Bool :=
  !(covSL T x).isEmpty && (positionsT T).all (fun y => subSL T x y → subSL T y x)

private def obls (T : Text) : Bool × Bool × Bool × Bool :=
  let S := scan (T.length + 1) (triplesOf T)
  ((positionsT T).all (fun x => S.any (fun y => subSL T x y)),
   S.Nodup,
   S.all (maxSL T),
   S.all (fun x => S.all (fun y => x == y || !(subSL T x y && subSL T y x))))

#eval ("O1 domination counterexamples / 729: "
  ++ toString ((fmTexts 6).filter (fun T => !(obls T).1)).length)
#eval ("O2 nodup counterexamples / 729: "
  ++ toString ((fmTexts 6).filter (fun T => !(obls T).2.1)).length)
#eval ("O3 maximality counterexamples / 729: "
  ++ toString ((fmTexts 6).filter (fun T => !(obls T).2.2.1)).length)
#eval ("O4 distinct-class counterexamples / 729: "
  ++ toString ((fmTexts 6).filter (fun T => !(obls T).2.2.2)).length)

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

/-- row index of row `k`: number of BWT-char changes strictly before row `k`. -/
def runIdxOf (ts : List Triple) (k : Nat) : Nat :=
  ((List.range k).filter (fun j => (ts.getD (j+1) ⟨0,0,0⟩).c != (ts.getD j ⟨0,0,0⟩).c)).length

/-- inverse SA on the stream: row index whose `sa` field is `p` (default 0). -/
def isaOf (ts : List Triple) (p : Nat) : Nat :=
  ((List.range ts.length).find? (fun k => (ts.getD k ⟨0,0,0⟩).sa = p)).getD 0

/-- PLCP at text position `p`: the LCP field of the row whose `sa` is `p`. -/
def plcpAt (ts : List Triple) (p : Nat) : Nat := lcpOfStream ts (isaOf ts p)

/-- RETIRED piece condition (measured WRONG on ascending-chain texts): a new
piece started when `phi (q-1) != phi q - 1`.  The phi chain can ascend
(`T = [2,2,2,1,1]` has `phi p = p+1`), in which case the parallel-shift rule
glues wrong pieces and breaks the slope law.  Kept for the record only;
`ivList` below uses the direct law. -/
def phiOf (ts : List Triple) (p : Nat) : Int :=
  let row := isaOf ts p
  if row = 0 then -1 else ((ts.getD (row-1) ⟨0,0,0⟩).sa : Int)

/-- MODEL v3 phi-interval partition over positions, defined DIRECTLY by the
piecewise-linear law: a piece CONTINUES at position `p` (`p ≠ 0`) iff
`PLCP (p-1) = PLCP p + 1`; otherwise a new piece starts.  Measured correct
(2026-10-01): the slope law holds by construction and the piece count stays
`≤ 2r+2` on all battery texts.  See RESEARCH.md "MODEL v3 EXACT". -/
def ivList (ts : List Triple) : List Nat :=
  let step := fun (st : List Nat × Nat) (p : Nat) =>
    let brk := p != 0 && plcpAt ts (p-1) != plcpAt ts p + 1
    let cur' := if brk then st.2 + 1 else st.2
    (st.1 ++ [cur'], cur')
  ((List.range ts.length).foldl step ([], 0)).1

/-- interval index of position `p`. -/
def ivOf (ts : List Triple) (p : Nat) : Nat := (ivList ts).getD p 0

/-- run HEAD or run TAIL row: a boundary row, the last row of the stream, or a
row whose successor carries a different BWT char. -/
def isRunEdge (ts : List Triple) (k : Nat) : Bool :=
  isBoundary ts k || (k < ts.length && (k + 1 >= ts.length
    || (ts.getD k ⟨0,0,0⟩).c != (ts.getD (k+1) ⟨0,0,0⟩).c))

/-- MODEL v3 pair-extreme test (SA coordinate).  Row `k`'s cell is
(run of `k`, direct-law piece of its `sa` position); the row is an event iff it
is the cell's argmax-`sa` row OR its argmin-`sa` row.  BOTH extremes are
needed: witnesses sit at the LCP-max side, PSV/NSV interposers enter at the
LCP-min side (one extreme measured 121/124). -/
def isPairExtreme (ts : List Triple) (k : Nat) : Bool :=
  let p := (ts.getD k ⟨0,0,0⟩).sa
  let ri := runIdxOf ts k
  let ii := ivOf ts p
  let cell := (List.range ts.length).filter
    (fun j => runIdxOf ts j = ri && ivOf ts (ts.getD j ⟨0,0,0⟩).sa = ii)
  cell.all (fun j => (ts.getD j ⟨0,0,0⟩).sa ≤ p)
    || cell.all (fun j => p ≤ (ts.getD j ⟨0,0,0⟩).sa)

/-- the O(r) v3 event set: run-boundary rows plus per-(run,piece) extreme rows. -/
def eventRows (ts : List Triple) : List Nat :=
  (List.range ts.length).filter (fun k => isBoundary ts k || isPairExtreme ts k)

/-- event-restricted PSV: last EVENT row `k < i` with strictly smaller LCP. -/
def evPsv (ts : List Triple) (i : Nat) : Nat :=
  let before := (eventRows ts).takeWhile (fun k => k < i) |>.reverse
  match before.find? (fun k => lcpOfStream ts k < lcpOfStream ts i) with
  | some k => k
  | none => 0

/-- event-restricted NSV: first EVENT row `k > i` with strictly smaller LCP. -/
def evNsv (ts : List Triple) (i : Nat) : Nat :=
  let after := (eventRows ts).dropWhile (fun k => k <= i)
  match after.find? (fun k => lcpOfStream ts k < lcpOfStream ts i) with
  | some k => k
  | none => ts.length + 1

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

/-- the FM machine restricted to the v3 event set. -/
def fmSpecEvents (N : Nat) (ts : List Triple) : List Nat :=
  match ts with
  | [] => []
  | t0 :: rest =>
    let R0 : List (Option CandFM) := (List.range SIGMA).map (fun _ => none)
    fmAuxEv N (fun i => (evPsv ts i : Int)) (fun i => evNsv ts i) rest 1 t0 R0 []

/-- **Bit-2 Layer 2, MODEL v3 — REFUTED AT SCALE (2026-09-25), statement
RETAINED for the record, NOT provable as stated.**  The in-file differential
(`eventsAgreeV3`, 729/729) is green ONLY because small texts have size-1
cells and cannot exercise the refutation: on the 600K copy-heavy text
duplicates-600k, the restricted machine UNDERCOUNTS (13,003 vs 13,004 —
one true interposer row is an interior row of a size-3 cell, neither a
run edge nor a cell extreme; see RESEARCH.md "ASSEMBLY + C-LANE RESULTS"
and tools/chi_rspace_proto.py's gate log).  The CORRECT statement is the
successor model: restricted-FM with exact range-min semantics (the
class-internal RMQ / endpoint rule) — pending measurement.  Do not
attempt to prove this statement; do not cite its #eval as evidence at
scale. -/
theorem chi_from_events_REFUTED_AT_SCALE (M : RSpace) (N : Nat) (ts : List Triple) (T : Text)
    (hpos : positive T = true)
    (hrepr : Represents M ts)
    (hN : ts.length + 1 = N)
    (hcost : ScatterOofR M)
    (hwit : ∀ x ∈ fmSpec N ts, ∃ k, isRunEdge ts k = true ∧ x = N - (ts.getD k ⟨0,0,0⟩).sa) :
    chi T = (fmSpecEvents N ts).eraseDups.length := by
  sorry

/-! ### executable differentials -/

def evSetEq (a b : List Nat) : Bool :=
  let a := a.eraseDups; let b := b.eraseDups
  a.length == b.length && a.all (fun x => b.contains x) && b.all (fun x => a.contains x)

/-- MODEL v3 differential: restricted-FM emitted set == full-FM emitted set. -/
def eventsAgreeV3 (T : Text) : Bool :=
  let ts := triplesOf T
  evSetEq (fmSpec (ts.length + 1) ts) (fmSpecEvents (ts.length + 1) ts)

-- must print `true`: model v3 on the whole 729-text battery
#eval (fmTexts 6).all eventsAgreeV3
#eval ("MODEL v3 restricted==full agreements / 729: "
  ++ toString ((fmTexts 6).filter eventsAgreeV3).length)
-- the former smallest counterexample class (one extreme / phi pieces): T = [1,1,2,2]
#eval ("v3 check T=[1,1,2,2]: full=" ++ toString (fmSpec 5 (triplesOf [1,1,2,2]))
  ++ "  events=" ++ toString (fmSpecEvents 5 (triplesOf [1,1,2,2])))

-- boundary-only witness statement (FALSE, 484/729 counterexamples)
def witnessBoundaryOk (T : Text) : Bool :=
  let ts := triplesOf T
  let N := ts.length + 1
  (fmSpec N ts).all (fun x => (List.range ts.length).any (fun k =>
    isBoundary ts k && decide (x = N - (ts.getD k ⟨0,0,0⟩).sa)))

def witnessRunEdgeOk (T : Text) : Bool :=
  let ts := triplesOf T
  let N := ts.length + 1
  (fmSpec N ts).all (fun x => (List.range ts.length).any (fun k =>
    isRunEdge ts k && decide (x = N - (ts.getD k ⟨0,0,0⟩).sa)))


/-! ### Lane L: provenance invariant for `fmAux` — proof of Layer 1

`fmAux` only ever stores or emits `N - sa` for the row it is currently
processing (`t`) or the previous row (`prev`), and only when their BWT chars
differ — i.e. at run HEADS and run TAILS (`isRunEdge`).  The invariant
`GoodR`/`GoodS` below tracks this through every step; `fmAux_good` is the
induction, and `witnesses_at_run_edges` reads it off at the top level. -/

def GoodCand (ts : List Triple) (N : Nat) (cand : CandFM) : Prop :=
  ∃ k, isRunEdge ts k = true ∧ cand.textPos = N - (ts.getD k ⟨0,0,0⟩).sa

def GoodR (ts : List Triple) (N : Nat) (R : List (Option CandFM)) : Prop :=
  ∀ c cand, getFM R c = some cand → GoodCand ts N cand

def GoodS (ts : List Triple) (N : Nat) (S : List Nat) : Prop :=
  ∀ x ∈ S, ∃ k, isRunEdge ts k = true ∧ x = N - (ts.getD k ⟨0,0,0⟩).sa

theorem getFM_set_self_lt (R : List (Option CandFM)) (c : Nat) (x : Option CandFM)
    (h : c < R.length) : getFM (R.set c x) c = x := by
  unfold getFM
  rw [List.getD_eq_getElem?_getD, List.getElem?_set]
  simp [h]

theorem getFM_set_self_ge (R : List (Option CandFM)) (c : Nat) (x : Option CandFM)
    (h : ¬ c < R.length) : getFM (R.set c x) c = none := by
  unfold getFM
  rw [List.getD_eq_getElem?_getD, List.getElem?_set]
  simp [h]

theorem goodS_append {ts : List Triple} {N : Nat} {S T : List Nat}
    (hS : GoodS ts N S) (hT : GoodS ts N T) : GoodS ts N (S ++ T) := by
  intro x hx
  rw [List.mem_append] at hx
  rcases hx with hx | hx
  · exact hS x hx
  · exact hT x hx

theorem foldl_good (ts : List Triple) (N : Nat) (l : List Nat) (f : List Nat → Nat → List Nat)
    (hf : ∀ S i, GoodS ts N S → GoodS ts N (f S i)) :
    ∀ S, GoodS ts N S → GoodS ts N (l.foldl f S) := by
  induction l with
  | nil => intro S h; exact h
  | cons a t ih => intro S h; exact ih (f S a) (hf S a h)

theorem finalStep_good (ts : List Triple) (N : Nat) (R : List (Option CandFM))
    (hR : GoodR ts N R) : ∀ S i, GoodS ts N S → GoodS ts N (finalStep R S i) := by
  intro S i hS
  unfold finalStep
  split
  · rename_i cand hcand
    by_cases ha : cand.active = true
    · rw [if_pos ha]
      refine goodS_append hS ?_
      intro x hx
      rw [List.mem_singleton] at hx
      subst hx
      exact hR (i+1) cand hcand
    · rw [if_neg ha]; exact hS
  · exact hS

theorem finalEmit_good (ts : List Triple) (N : Nat) (R : List (Option CandFM)) (S : List Nat)
    (hR : GoodR ts N R) (hS : GoodS ts N S) : GoodS ts N (finalEmit R S) := by
  unfold finalEmit
  exact foldl_good ts N (List.range (SIGMA - 1)) (finalStep R)
    (finalStep_good ts N R hR) S hS

theorem getD_of_drop_eq_cons {l : List Triple} {i : Nat} {t : Triple} {r : List Triple}
    (h : l.drop i = t :: r) : l.getD i ⟨0,0,0⟩ = t := by
  have h0 : (l.drop i)[0]? = some t := by rw [h]; rfl
  rw [List.getElem?_drop, Nat.add_zero] at h0
  rw [List.getD_eq_getElem?_getD, h0]
  rfl

theorem drop_succ_of_drop_eq_cons {l : List Triple} {i : Nat} {t : Triple} {r : List Triple}
    (h : l.drop i = t :: r) : l.drop (i+1) = r := by
  have hlen : i < l.length := by
    have h2 : (l.drop i).length ≠ 0 := by rw [h]; simp
    rw [List.length_drop] at h2
    omega
  rw [List.drop_eq_getElem_cons hlen] at h
  exact (List.cons.inj h).2

theorem fmStep_good (N : Nat) (psvI : Int) (nsvI : Nat) (i : Nat) (c : Nat) (sa : Nat)
    (R : List (Option CandFM)) (S : List Nat) (ts : List Triple)
    (hR : GoodR ts N R) (hS : GoodS ts N S)
    (hsa : ∃ k, isRunEdge ts k = true ∧ sa = (ts.getD k ⟨0,0,0⟩).sa) :
    GoodR ts N (fmStep N psvI nsvI i c sa R S).1 ∧
      GoodS ts N (fmStep N psvI nsvI i c sa R S).2 := by
  unfold fmStep
  by_cases hc0 : c = 0
  · rw [if_pos hc0]; exact ⟨hR, hS⟩
  · rw [if_neg hc0]
    cases hsome : getFM R c with
    | none =>
      simp only [hsome]
      refine ⟨?_, hS⟩
      intro d cand hd
      by_cases hdc : c = d
      · subst hdc
        by_cases hlt : c < R.length
        · rw [getFM_set_self_lt R c _ hlt] at hd
          obtain ⟨k, hk, hksa⟩ := hsa
          injection hd with hcand
          rw [← hcand]
          exact ⟨k, hk, by rw [hksa]⟩
        · rw [getFM_set_self_ge R c _ hlt] at hd
          exact absurd hd (by simp)
      · rw [getFM_set_ne R c d _ hdc] at hd
        exact hR d cand hd
    | some cand0 =>
      simp only [hsome]
      by_cases hcond : cand0.saPos ≤ psvI
      · rw [if_pos hcond]
        by_cases hnsv : cand0.nsv < i
        · rw [if_pos hnsv]
          refine ⟨?_, ?_⟩
          · intro d cand hd
            by_cases hdc : c = d
            · subst hdc
              by_cases hlt : c < R.length
              · rw [getFM_set_self_lt R c _ hlt] at hd
                obtain ⟨k, hk, hksa⟩ := hsa
                injection hd with hcand
                rw [← hcand]
                exact ⟨k, hk, by rw [hksa]⟩
              · rw [getFM_set_self_ge R c _ hlt] at hd
                exact absurd hd (by simp)
            · rw [getFM_set_ne R c d _ hdc] at hd
              exact hR d cand hd
          · intro x hx
            rw [List.mem_append, List.mem_singleton] at hx
            rcases hx with hx | rfl
            · exact hS x hx
            · exact hR c cand0 hsome
        · rw [if_neg hnsv]
          refine ⟨?_, hS⟩
          intro d cand hd
          by_cases hdc : c = d
          · subst hdc
            by_cases hlt : c < R.length
            · rw [getFM_set_self_lt R c _ hlt] at hd
              obtain ⟨k, hk, hksa⟩ := hsa
              injection hd with hcand
              rw [← hcand]
              exact ⟨k, hk, by rw [hksa]⟩
            · rw [getFM_set_self_ge R c _ hlt] at hd
              exact absurd hd (by simp)
          · rw [getFM_set_ne R c d _ hdc] at hd
            exact hR d cand hd
      · rw [if_neg hcond]; exact ⟨hR, hS⟩

theorem fmAux_good (N : Nat) (ts : List Triple) (psv : List Int) (nsv : List Nat) (inf : Nat) :
    ∀ (i : Nat) (rest : List Triple) (prev : Triple) (R : List (Option CandFM)) (S : List Nat),
      1 ≤ i → rest = ts.drop i → prev = ts.getD (i-1) ⟨0,0,0⟩ →
      GoodR ts N R → GoodS ts N S →
      GoodS ts N (fmAux N psv nsv inf rest i prev R S) := by
  intro i rest
  induction rest generalizing i with
  | nil =>
    intro prev R S hi hdrop hprev hR hS
    simp only [fmAux]
    exact finalEmit_good ts N R S hR hS
  | cons t rest' ih =>
    intro prev R S hi hdrop hprev hR hS
    have hdrop' : ts.drop i = t :: rest' := hdrop.symm
    have hlen : i < ts.length := by
      have h2 : (ts.drop i).length ≠ 0 := by rw [hdrop']; simp
      rw [List.length_drop] at h2
      omega
    have ht : ts.getD i ⟨0,0,0⟩ = t := getD_of_drop_eq_cons hdrop'
    have hrest' : rest' = ts.drop (i+1) := (drop_succ_of_drop_eq_cons hdrop').symm
    have hprev' : t = ts.getD (i+1-1) ⟨0,0,0⟩ := by
      rw [show i + 1 - 1 = i from by omega]; exact ht.symm
    simp only [fmAux]
    by_cases hbc : (t.c != prev.c) = true
    · rw [if_pos hbc]
      have hne : t.c ≠ prev.c := bne_iff_ne.mp hbc
      have hbnd_i : isBoundary ts i = true := by
        unfold isBoundary
        rw [if_neg (show ¬ ts.length ≤ i by omega), if_neg (show ¬ i = 0 by omega)]
        rw [ht, ← hprev]
        exact hbc
      have hedge_i : isRunEdge ts i = true := by
        unfold isRunEdge
        rw [Bool.or_eq_true]
        exact Or.inl hbnd_i
      have hedge_prev : isRunEdge ts (i-1) = true := by
        unfold isRunEdge
        rw [Bool.or_eq_true]
        right
        rw [Bool.and_eq_true]
        constructor
        · exact decide_eq_true (by omega)
        · rw [Bool.or_eq_true]
          right
          rw [show i - 1 + 1 = i from by omega, ht, ← hprev]
          exact bne_iff_ne.mpr (Ne.symm hne)
      have hsa_prev : ∃ k, isRunEdge ts k = true ∧ prev.sa = (ts.getD k ⟨0,0,0⟩).sa :=
        ⟨i-1, hedge_prev, by rw [hprev]⟩
      have hsa_t : ∃ k, isRunEdge ts k = true ∧ t.sa = (ts.getD k ⟨0,0,0⟩).sa :=
        ⟨i, hedge_i, by rw [ht]⟩
      obtain ⟨hR1, hS1⟩ := fmStep_good N (psv.getD i (-1)) (nsv.getD i inf) i prev.c prev.sa R S ts hR hS hsa_prev
      obtain ⟨hR2, hS2⟩ := fmStep_good N (psv.getD i (-1)) (nsv.getD i inf) i t.c t.sa
        (fmStep N (psv.getD i (-1)) (nsv.getD i inf) i prev.c prev.sa R S).1
        (fmStep N (psv.getD i (-1)) (nsv.getD i inf) i prev.c prev.sa R S).2 ts hR1 hS1 hsa_t
      exact ih (i+1) t
        (fmStep N (psv.getD i (-1)) (nsv.getD i inf) i t.c t.sa
          (fmStep N (psv.getD i (-1)) (nsv.getD i inf) i prev.c prev.sa R S).1
          (fmStep N (psv.getD i (-1)) (nsv.getD i inf) i prev.c prev.sa R S).2).1
        (fmStep N (psv.getD i (-1)) (nsv.getD i inf) i t.c t.sa
          (fmStep N (psv.getD i (-1)) (nsv.getD i inf) i prev.c prev.sa R S).1
          (fmStep N (psv.getD i (-1)) (nsv.getD i inf) i prev.c prev.sa R S).2).2
        (by omega) hrest' hprev' hR2 hS2
    · rw [if_neg hbc]
      exact ih (i+1) t R S (by omega) hrest' hprev' hR hS

theorem goodR_range_none (ts : List Triple) (N : Nat) :
    GoodR ts N ((List.range SIGMA).map (fun _ => (none : Option CandFM))) := by
  intro c cand h
  rw [getFM_range_none c] at h
  exact absurd h (by simp)

/-- LAYER 1, CORRECTED (the locked boundary-only form was FALSE: 484/729
counterexamples, first `T = [1,1]` — witnesses sit on run TAILS too; `fmAux`
stores candidates at BOTH `ip = i-1` (tail) and `ip = i` (head)).  The
corrected statement — run head OR run tail — is 0/729 counterexamples
(`witnessRunEdgeOk` `#eval` below).  Statement locked on that evidence. -/
theorem witnesses_at_run_edges (N : Nat) (ts : List Triple)
    (hN : ts.length + 1 = N) :
    ∀ x ∈ fmSpec N ts, ∃ k, isRunEdge ts k = true ∧ x = N - (ts.getD k ⟨0,0,0⟩).sa := by
  intro x hx
  cases ts with
  | nil => simp [fmSpec] at hx
  | cons t0 rest =>
    simp only [fmSpec] at hx ⊢
    have hgood := fmAux_good N (t0 :: rest)
      (psvList ((t0 :: rest).map (fun t => t.lcp)))
      (nsvList ((t0 :: rest).map (fun t => t.lcp)))
      ((t0 :: rest).length + 1) 1 rest t0
      ((List.range SIGMA).map (fun _ => (none : Option CandFM))) []
      (by omega) rfl rfl (goodR_range_none (t0 :: rest) N)
      (by intro y hy; simp at hy)
    exact hgood x hx

/-- RETIRED (measured false, 484/729): kept for the record only. -/
theorem witnesses_at_boundaries_FALSE_AS_STATED (N : Nat) (ts : List Triple)
    (hN : ts.length + 1 = N) :
    ∀ x ∈ fmSpec N ts, ∃ k, isBoundary ts k = true ∧ x = N - (ts.getD k ⟨0,0,0⟩).sa := by
  sorry

#eval ("boundary-witness counterexamples / 729: "
  ++ toString ((fmTexts 6).filter (fun T => !(witnessBoundaryOk T))).length)
#eval ("run-edge-witness counterexamples / 729: "
  ++ toString ((fmTexts 6).filter (fun T => !(witnessRunEdgeOk T))).length)

/-! ### scan-side run-edge provenance (mirror of `witnesses_at_run_edges`)

`scanAux` stores a position only at a char change (`t.c != p`): for the row
just processed (`t`, a run HEAD) and its predecessor (`prev`, a run TAIL), with
value `N - sa` in both cases; emitted positions are stored positions.  Hence
every position emitted by `scan` is `N - sa` of a run-edge row.  Statement
locked by `#eval` over the 729-text battery (0 counterexamples). -/

def EdgeGood (ts : List Triple) (N : Nat) (x : Nat) : Prop :=
  ∃ k, isRunEdge ts k = true ∧ x = N - (ts.getD k ⟨0,0,0⟩).sa

def Redge (ts : List Triple) (N : Nat) (R : List Cand) : Prop :=
  ∀ c, 1 ≤ c → (getR R c).active → EdgeGood ts N (getR R c).pos

def Oedge (ts : List Triple) (N : Nat) (out : List Nat) : Prop :=
  ∀ x ∈ out, EdgeGood ts N x

theorem redge_defaultR (ts : List Triple) (N : Nat) : Redge ts N defaultR := by
  intro c _ hact
  rw [getR_defaultR c] at hact
  exact absurd hact (by simp)

theorem upd_edge (ts : List Triple) (N : Nat) (R : List Cand) (c l pos : Nat)
    (hR : Redge ts N R) (hpos : EdgeGood ts N pos) : Redge ts N (upd R c l pos) := by
  unfold upd
  split
  · intro d hd hact
    by_cases hdeq : c = d
    · subst hdeq
      rw [getR_set_eq] at hact ⊢
      by_cases hlt : c < R.length
      · rw [if_pos hlt] at hact ⊢; exact hpos
      · rw [if_neg hlt] at hact ⊢; exact hR c hd hact
    · rw [getR_set_ne R c d ⟨l,pos,true⟩ hdeq] at hact ⊢
      exact hR d hd hact
  · exact hR

theorem evalStepGo_edge (ts : List Triple) (N : Nat) (l : Int) :
    ∀ acc i, Oedge ts N acc.1 → Redge ts N acc.2 →
      Oedge ts N (evalStepGo l acc i).1 ∧ Redge ts N (evalStepGo l acc i).2 := by
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

theorem evalStep_edge (ts : List Triple) (N : Nat) (l : Int) (R : List Cand) (out : List Nat)
    (ho : Oedge ts N out) (hR : Redge ts N R) :
    Oedge ts N (evalStep l R out).1 ∧ Redge ts N (evalStep l R out).2 := by
  rw [evalStep_eq]
  refine foldl_pres (evalStepGo l) (fun acc => Oedge ts N acc.1 ∧ Redge ts N acc.2) ?_
    (List.range (SIGMA-1)) (out,R) ⟨ho,hR⟩
  intro a i h
  exact evalStepGo_edge ts N l a i h.1 h.2

theorem scanAux_edge (N : Nat) (ts : List Triple) :
    ∀ (i : Nat) (rest : List Triple) (prev : Triple) (m : Int) (R : List Cand) (out : List Nat),
      1 ≤ i → rest = ts.drop i → prev = ts.getD (i-1) ⟨0,0,0⟩ →
      Redge ts N R → Oedge ts N out →
      Oedge ts N (scanAux N rest prev.c prev.sa m R out) := by
  intro i rest
  induction rest generalizing i with
  | nil =>
    intro prev m R out hi hdrop hprev hR ho
    simp only [scanAux]
    exact (evalStep_edge ts N (-1) R out ho hR).1
  | cons t rest' ih =>
    intro prev m R out hi hdrop hprev hR ho
    have hdrop' : ts.drop i = t :: rest' := hdrop.symm
    have hlen : i < ts.length := by
      have h2 : (ts.drop i).length ≠ 0 := by rw [hdrop']; simp
      rw [List.length_drop] at h2
      omega
    have ht : ts.getD i ⟨0,0,0⟩ = t := getD_of_drop_eq_cons hdrop'
    have hrest' : rest' = ts.drop (i+1) := (drop_succ_of_drop_eq_cons hdrop').symm
    have hprev' : t = ts.getD (i+1-1) ⟨0,0,0⟩ := by
      rw [show i + 1 - 1 = i from by omega]; exact ht.symm
    simp only [scanAux]
    by_cases hbc : (t.c != prev.c) = true
    · rw [if_pos hbc]
      have hne : t.c ≠ prev.c := bne_iff_ne.mp hbc
      have hbnd_i : isBoundary ts i = true := by
        unfold isBoundary
        rw [if_neg (show ¬ ts.length ≤ i by omega), if_neg (show ¬ i = 0 by omega)]
        rw [ht, ← hprev]
        exact bne_iff_ne.mpr hne
      have hedge_i : isRunEdge ts i = true := by
        unfold isRunEdge; rw [Bool.or_eq_true]; exact Or.inl hbnd_i
      have hedge_prev : isRunEdge ts (i-1) = true := by
        unfold isRunEdge
        rw [Bool.or_eq_true]; right
        rw [Bool.and_eq_true]
        constructor
        · exact decide_eq_true (by omega)
        · rw [Bool.or_eq_true]; right
          rw [show i - 1 + 1 = i from by omega, ht, ← hprev]
          exact bne_iff_ne.mpr (Ne.symm hne)
      have hpos_prev : EdgeGood ts N (N - prev.sa) := ⟨i-1, hedge_prev, by rw [hprev]⟩
      have hpos_t : EdgeGood ts N (N - t.sa) := ⟨i, hedge_i, by rw [ht]⟩
      obtain ⟨ho1, hR1⟩ := evalStep_edge ts N (min m t.lcp) R out ho hR
      have hR2 := upd_edge ts N (evalStep (min m t.lcp) R out).2 prev.c t.lcp (N - prev.sa) hR1 hpos_prev
      have hR3 := upd_edge ts N (upd (evalStep (min m t.lcp) R out).2 prev.c t.lcp (N - prev.sa)) t.c t.lcp (N - t.sa) hR2 hpos_t
      exact ih (i+1) t MAXINT
        (upd (upd (evalStep (min m t.lcp) R out).2 prev.c t.lcp (N - prev.sa)) t.c t.lcp (N - t.sa))
        (evalStep (min m t.lcp) R out).1 (by omega) hrest' hprev' hR3 ho1
    · rw [if_neg hbc]
      exact ih (i+1) t (min m t.lcp) R out (by omega) hrest' hprev' hR ho

/-- every position emitted by the one-pass scan is `N - sa` of a run-edge row
(run HEAD or run TAIL) — the scan-side Layer 1, mirror of
`witnesses_at_run_edges` for `fmSpec`. -/
theorem scan_emits_run_edges (N : Nat) (ts : List Triple) :
    ∀ x ∈ scan N ts, EdgeGood ts N x := by
  intro x hx
  cases ts with
  | nil => simp [scan] at hx
  | cons t0 rest =>
    simp only [scan] at hx
    have hgood := scanAux_edge N (t0 :: rest) 1 rest t0 MAXINT defaultR []
      (by omega) rfl rfl (redge_defaultR (t0 :: rest) N)
      (by intro y hy; simp at hy)
    exact hgood x hx

/-! ### FINDINGS

1. `witnesses_at_boundaries` is **FALSE** (484/729): `fmAux` calls `fmStep` at a
   boundary index `i` for BOTH `prev` (= row `i-1`, a run TAIL) and `t` (=
   row `i`, a run HEAD).  The corrected statement "run HEAD OR run TAIL"
   (`isRunEdge`) is 0/729 counterexamples and is **PROVED above**
   (`witnesses_at_run_edges`), via the provenance invariant
   `GoodR`/`GoodS` (`fmAux_good`): every stored/emitted position is
   `N - sa` for the row being processed or its predecessor, and `fmStep` only
   stores when their BWT chars differ — i.e. at run heads and run tails.

2. `lf_image_consecutive` is TRUE and PROVED (rankAt arithmetic above).

3. MODEL v3 (2026-10-01): after correcting the piece partition to the direct
   law (`ivList`) and admitting BOTH per-cell extremes (`isPairExtreme`), the
   restricted-FM / full-FM set differential is TRUE on the whole 729-text
   battery (was 699/729 with the SA-coordinate argmax only, and 657/729 with
   the retired `N - sa` / `extremePoint` convention).  Smallest former
   counterexample `T = [1,1,2,2]`: full `[3,2,4]`, events `[2,3]` (now equal).

4. WARNING (historical): the Python probes' `suffix_array` used tie-unaware
   initial ranks before 2026-09-29; it now uses dense tie-aware ranks and is
   brute-verified.  The measurements quoted in RESEARCH.md were re-derived.

5. SA/LCP FOUNDATION (Lane L, Task 2b) — proved:
   * `lexLE_refl`, `lexLE_total`, `lexLE_trans` (lexicographic order on lists);
   * `take_eq_iff_le_lcpOf`: for `k` within both lengths,
     `a.take k = b.take k ↔ k ≤ lcpOf a b` — `lcpOf` is the longest common
     prefix;
   * `insSort_pairwise`, `saOrder_pairwise`, `saOrder_sorted_getElem`:
     `saOrder` is lexicographically sorted.
   These discharge the two facts the covering skeleton needed; the remaining
   obligation is the scan-covers-requirements invariant (see the skeleton at
   `covering_given_stream`).
-/

/-! ### O1 outright attempt (2026-09-27 lane): reduction to class-hitting

`covering_given_stream` needs (O1): every text position is `ScopeLe`-dominated
by some emitted position.  This section reduces (O1) by pure coverage-set
algebra to a single scan-side statement — `O1_maxHit`: every
inclusion-maximal coverage class contains an emitted position — and then
factors that residual through run edges, the provenance class of every
emitted witness (`scan_emits_run_edges`).  The reductions are exact on
positive texts (`O1_iff_maxHit`, `maxHit_of_runEdge` +
`runEdgeHit_of_maxHit`); no direction weakens the obligation, and the
residual statements are given at statement-lock strength. -/

/-- Every text position covers at least the empty-context requirement
`([], c)` with `c` its own last character: the empty word is right-maximal
by definition, and `[c]` occurs at the position itself.  (Discharges the
`covSet ≠ []` side condition of `exists_max_above` for text positions.) -/
theorem covSet_ne_of_mem_positionsT (T : Text) {x : Nat} (hx : x ∈ positionsT T) :
    covSet T x ≠ [] := by
  classical
  intro hemp
  have hxb : 1 ≤ x ∧ x ≤ T.length := by
    rcases List.mem_map.mp hx with ⟨i, hi, rfl⟩
    simp only [List.mem_range] at hi
    omega
  obtain ⟨hx1, hxle⟩ := hxb
  have hlt : x - 1 < T.length := by omega
  have hgc : T.getD (x - 1) 0 = T[x - 1] := by
    rw [List.getD_eq_getElem?_getD]
    have hs : T[x - 1]? = some (T[x - 1]) := (List.getElem?_eq_some_iff).mpr ⟨hlt, rfl⟩
    rw [hs]
    rfl
  have hdrop : T.drop (x - 1) = T.getD (x - 1) 0 :: T.drop x := by
    rw [List.drop_eq_getElem_cons hlt, hgc]
    have hxs : x - 1 + 1 = x := by omega
    rw [hxs]
  have hdt : (T.drop (x - 1)).take 1 = [T.getD (x - 1) 0] := by
    rw [hdrop]; rfl
  have hsplit : T.take x = T.take (x - 1) ++ (T.drop (x - 1)).take 1 := by
    have h := List.take_add (i := x - 1) (j := 1) (l := T)
    rw [show x - 1 + 1 = x from by omega] at h
    exact h
  have hlast : T.take x = T.take (x - 1) ++ [T.getD (x - 1) 0] := by
    rw [hsplit, hdt]
  have hcocc : occurs ([] ++ [T.getD (x - 1) 0]) T = true := by
    refine (occurs_eq_true ([] ++ [T.getD (x - 1) 0]) T
      (by rw [List.nil_append]; exact fun h => nomatch h)).mpr
      ⟨x - 1, by simp only [List.nil_append, List.length_cons, List.length_nil]; omega, hdt⟩
  have hsub : [] ∈ subStrings T := by
    cases T with
    | nil => exact List.Mem.head _
    | cons t ts => exact List.Mem.head _
  have hreq : ([], T.getD (x - 1) 0) ∈ requirements T := by
    refine (mem_requirements [] (T.getD (x - 1) 0) T).mpr
      ⟨hsub, rfl, (mem_rightExts [] (T.getD (x - 1) 0) T).mpr hcocc⟩
  have hccov : coversAt ([] ++ [T.getD (x - 1) 0]) x T = true := by
    rw [coversAt_iff_suffix]
    exact ⟨T.take (x - 1), hlast⟩
  have hmem : ([], T.getD (x - 1) 0) ∈ covSet T x :=
    (mem_covSet T x ([], T.getD (x - 1) 0)).mpr ⟨hreq, hccov⟩
  exact (List.ne_nil_of_mem hmem) hemp

/-- **(O1-core) MaxHit** — every inclusion-maximal coverage class contains a
position emitted by the scan.  By maximality the emitted witness is in the
same class as `m` (`maxHit_in_class`); via `domination_of_maxHit` and
`covering_of_domination` this single statement completes the covering half
of `minimality` (`covering_given_stream_of_maxHit`). -/
def O1_maxHit (T : Text) : Prop :=
  ∀ m, m ∈ positionsT T → IsMax T m →
    ∃ e, e ∈ scan (T.length + 1) (triplesOf T) ∧ ScopeLe T m e

/-- A maximal position dominated by a position is in the same class. -/
theorem maxHit_in_class (T : Text) {m e : Nat} (hmax : IsMax T m)
    (he : e ∈ positionsT T) (hme : ScopeLe T m e) : ScopeLe T e m :=
  hmax.2 e he hme

/-- **O1 reduction (main).**  MaxHit implies full domination: every text
position is `ScopeLe`-dominated by an emitted position. -/
theorem domination_of_maxHit (T : Text) (hhit : O1_maxHit T) :
    ∀ x, x ∈ positionsT T →
      ∃ y, y ∈ scan (T.length + 1) (triplesOf T) ∧ ScopeLe T x y := by
  intro x hx
  obtain ⟨m, hmP, hxm, hmax⟩ :=
    exists_max_above T x hx (covSet_ne_of_mem_positionsT T hx)
  obtain ⟨e, he, hme⟩ := hhit m hmP hmax
  exact ⟨e, he, ScopeLe_trans T hxm hme⟩

/-- MaxHit implies the scan's emitted set is suffixient — the covering half
of `minimality`, now isolated to `O1_maxHit`. -/
theorem covering_given_stream_of_maxHit (T : Text) (hT : positive T = true)
    (hhit : O1_maxHit T) :
    suffixient (scan (T.length + 1) (triplesOf T)) T = true :=
  covering_of_domination T hT (domination_of_maxHit T hhit)

/-- The reduction is exact on positive texts: domination (O1) and MaxHit are
equivalent, since MaxHit is domination restricted to maximal positions. -/
theorem O1_iff_maxHit (T : Text) :
    (∀ x, x ∈ positionsT T →
      ∃ y, y ∈ scan (T.length + 1) (triplesOf T) ∧ ScopeLe T x y) ↔
      O1_maxHit T := by
  constructor
  · intro h m hmP _
    exact h m hmP
  · intro h
    exact domination_of_maxHit T h

/-- MaxHit equivalently requires only the class *representatives* to be hit
(the first position of each maximal class); every other class member is
dominated by its rep.  This connects the scan obligation to the semantic
`reps` machinery (`reps_suffixient`, `count_le_of_disjoint_witnesses`). -/
theorem maxHit_iff_reps (T : Text) :
    O1_maxHit T ↔
      ∀ r, r ∈ reps T →
        ∃ e, e ∈ scan (T.length + 1) (triplesOf T) ∧ ScopeLe T r e := by
  classical
  constructor
  · intro h r hr
    exact h r (List.mem_filter.mp hr).1
      ((of_decide_eq_true (List.mem_filter.mp hr).2).2.1)
  · intro h m hmP hmax
    obtain ⟨r, hrP, hrep, _hrm, hmr⟩ := exists_rep_of_max T m hmP hmax
    obtain ⟨e, he, hre⟩ :=
      h r (List.mem_filter.mpr ⟨hrP, decide_eq_true hrep⟩)
    exact ⟨e, he, ScopeLe_trans T hmr hre⟩

/-! #### Run-edge factorization of MaxHit

Every emitted witness is the position of a run-edge row
(`scan_emits_run_edges`).  Consequently MaxHit *requires* that every maximal
class contain a run-edge position (`RunEdgeHit`, `runEdgeHit_of_maxHit`),
and MaxHit *follows* from that fact together with every run-edge position
being dominated by an emitted position (`RunEdgeDominate`,
`maxHit_of_runEdge`). -/

/-- Text positions of the run-edge rows (heads and tails of BWT runs of the
reverse text), restricted to genuine text positions. -/
def runEdgePositions (T : Text) : List Nat :=
  (((List.range (triplesOf T).length).filter (fun k => isRunEdge (triplesOf T) k)).map
    (fun k => T.length + 1 - ((triplesOf T).getD k ⟨0,0,0⟩).sa)).filter
    (fun w => decide (1 ≤ w ∧ w ≤ T.length))

theorem isRunEdge_lt (ts : List Triple) {k : Nat} (h : isRunEdge ts k = true) :
    k < ts.length := by
  unfold isRunEdge at h
  rw [Bool.or_eq_true] at h
  rcases h with hb | hand
  · unfold isBoundary at hb
    split at hb
    · exact absurd hb (by simp)
    · omega
  · rw [Bool.and_eq_true] at hand
    exact of_decide_eq_true hand.1

/-- Necessary semantic factor: every maximal class contains a run-edge
position.  Necessary because emitted witnesses are run-edge positions
(`scan_emits_run_edges`). -/
def RunEdgeHit (T : Text) : Prop :=
  ∀ m, m ∈ positionsT T → IsMax T m →
    ∃ w, w ∈ runEdgePositions T ∧ ScopeLe T m w

/-- Every run-edge position is dominated by an emitted position. -/
def RunEdgeDominate (T : Text) : Prop :=
  ∀ w ∈ runEdgePositions T,
    ∃ e, e ∈ scan (T.length + 1) (triplesOf T) ∧ ScopeLe T w e

/-- Sufficiency of the factorization: run-edge class-hitting plus per-run-edge
domination give MaxHit. -/
theorem maxHit_of_runEdge (T : Text) (hhit : RunEdgeHit T) (hdom : RunEdgeDominate T) :
    O1_maxHit T := by
  intro m hmP hmax
  obtain ⟨w, hw, hmw⟩ := hhit m hmP hmax
  obtain ⟨e, he, hwe⟩ := hdom w hw
  exact ⟨e, he, ScopeLe_trans T hmw hwe⟩

/-- Necessity of the run-edge factor (positive texts): MaxHit implies every
maximal class contains a run-edge position. -/
theorem runEdgeHit_of_maxHit (T : Text) (hT : positive T = true) (hhit : O1_maxHit T) :
    RunEdgeHit T := by
  intro m hmP hmax
  obtain ⟨e, he, hme⟩ := hhit m hmP hmax
  refine ⟨e, ?_, hme⟩
  obtain ⟨k, hk, hek⟩ := scan_emits_run_edges (T.length + 1) (triplesOf T) e he
  have hklt := isRunEdge_lt (triplesOf T) hk
  have herange := scan_range T hT e he
  unfold runEdgePositions
  refine List.mem_filter.mpr ⟨?_, decide_eq_true herange⟩
  refine List.mem_map.mpr ⟨k, ?_, hek.symm⟩
  exact List.mem_filter.mpr ⟨by rw [List.mem_range]; exact hklt, hk⟩

/-! #### Executable differentials (statement-lock evals for the residual) -/

private def maxHitOk (T : Text) : Bool :=
  let S := scan (T.length + 1) (triplesOf T)
  ((positionsT T).filter (maxSL T)).all (fun m => S.any (fun e => subSL T m e))

private def runEdgeHitOk (T : Text) : Bool :=
  let W := runEdgePositions T
  ((positionsT T).filter (maxSL T)).all (fun m => W.any (fun w => subSL T m w))

private def runEdgeDominateOk (T : Text) : Bool :=
  let S := scan (T.length + 1) (triplesOf T)
  (runEdgePositions T).all (fun w => S.any (fun e => subSL T w e))

#eval ("O1 maxHit counterexamples / 729: "
  ++ toString ((fmTexts 6).filter (fun T => !maxHitOk T)).length)
#eval ("runEdgeHit counterexamples / 729: "
  ++ toString ((fmTexts 6).filter (fun T => !runEdgeHitOk T)).length)
#eval ("runEdgeDominate counterexamples / 729: "
  ++ toString ((fmTexts 6).filter (fun T => !runEdgeDominateOk T)).length)

/-- 3-letter battery: class/run-edge structure differs from the {1,2} battery. -/
private def triTexts : Nat → List Text
  | 0 => [[]]
  | k + 1 =>
    let r := triTexts k
    r ++ r.map (fun t => 1 :: t) ++ r.map (fun t => 2 :: t) ++ r.map (fun t => 3 :: t)

#eval ("O1 maxHit counterexamples / 243 ternary: "
  ++ toString ((triTexts 5).filter (fun T => !maxHitOk T)).length)
#eval ("runEdgeHit counterexamples / 243 ternary: "
  ++ toString ((triTexts 5).filter (fun T => !runEdgeHitOk T)).length)
#eval ("runEdgeDominate counterexamples / 243 ternary: "
  ++ toString ((triTexts 5).filter (fun T => !runEdgeDominateOk T)).length)

/-- structured repetitive probes (periodic, constant, run-heavy).  Kept
short: the covSet/requirements machinery is recomputed per pairwise check,
so eval cost is superlinear in text length (the battery texts are ≤ 6). -/
private def structTexts : List Text :=
  [List.replicate 10 1,
   List.replicate 10 2,
   (List.range 10).map (fun i => i % 3 + 1),
   (List.range 10).map (fun i => i % 2 + 1),
   List.replicate 5 1 ++ List.replicate 5 2,
   List.replicate 3 1 ++ List.replicate 3 2 ++ List.replicate 3 1 ++ List.replicate 1 3]

#eval ("O1 maxHit counterexamples / structured: "
  ++ toString (structTexts.filter (fun T => !maxHitOk T)).length)
#eval ("runEdgeHit counterexamples / structured: "
  ++ toString (structTexts.filter (fun T => !runEdgeHitOk T)).length)
#eval ("runEdgeDominate counterexamples / structured: "
  ++ toString (structTexts.filter (fun T => !runEdgeDominateOk T)).length)


/-! ## Shared reverse-row geometry (from SxgcRunEdge)

Kept before the covering theorem to avoid a circular module dependency. -/

/-! ## Layer 0: small list lemmas -/

/-- take beyond the end of the list -/
theorem take_full (l : List Nat) (n : Nat) (h : l.length ≤ n) : l.take n = l := by
  have hd : l.drop n = [] := (List.drop_eq_nil_iff).mpr h
  have hsplit : l = l.take n ++ l.drop n := (List.take_append_drop n l).symm
  rw [hd, List.append_nil] at hsplit
  exact hsplit.symm

/-- `append` with a single-element suffix is injective in both arguments. -/
theorem append_last_inj (a b : List Nat) (x y : Nat) (h : a ++ [x] = b ++ [y]) :
    x = y ∧ a = b := by
  have hrev : [x] ++ a.reverse = [y] ++ b.reverse := by
    have h2 : (a ++ [x]).reverse = (b ++ [y]).reverse := by rw [h]
    simpa [List.reverse_append, List.reverse_singleton] using h2
  simp only [List.singleton_append, List.cons.injEq] at hrev
  obtain ⟨rfl, htail⟩ := hrev
  exact ⟨rfl, List.reverse_inj.mp htail⟩

/-- a boolean walk that changes value somewhere has an adjacent change -/
theorem exists_bool_change (f : Nat → Bool) : ∀ d a, f a ≠ f (a + d) →
    ∃ k, a ≤ k ∧ k < a + d ∧ f k ≠ f (k + 1) := by
  intro d
  induction d with
  | zero => intro a h; rw [Nat.add_zero] at h; exact absurd rfl h
  | succ d' ih =>
    intro a h
    by_cases hmid : f a = f (a + d')
    · refine ⟨a + d', by omega, by omega, ?_⟩
      rw [← hmid]; exact h
    · obtain ⟨k, hk1, hk2, hk3⟩ := ih a hmid
      exact ⟨k, hk1, by omega, hk3⟩

/-- all-skip case of `dedupAux`: everything is in `seen` -/
theorem dedupAux_all_skip (seen : List Nat) : ∀ l : List Nat,
    (∀ x ∈ l, seen.contains x = true) → dedup.dedupAux seen l = [] := by
  intro l
  induction l with
  | nil => intro _; rfl
  | cons a rest ih =>
    intro hall
    simp only [dedup.dedupAux]
    rw [if_pos (hall a (by simp))]
    exact ih (fun x hx => hall x (List.mem_cons_of_mem _ hx))

/-- dedup of an all-equal list has length at most 1 -/
theorem dedup_all_eq_le_one (c : Nat) : ∀ l : List Nat, (∀ x ∈ l, x = c) → (dedup l).length ≤ 1 := by
  intro l hall
  cases l with
  | nil => exact Nat.zero_le 1
  | cons a rest =>
    have ha : a = c := hall a List.mem_cons_self
    rw [ha]
    unfold dedup
    simp only [dedup.dedupAux]
    by_cases h0 : List.contains [] c = true
    · rw [if_pos h0]; exact absurd h0 (by simp)
    · rw [if_neg h0]
      have hskip : dedup.dedupAux [c] rest = [] :=
        dedupAux_all_skip [c] rest (fun x hx => by
          rw [hall x (List.mem_cons_of_mem _ hx)]; simp)
      rw [hskip]; simp

/-! ## Layer 1: the reverse-text alignment -/

/-- The R-suffix at 0-based R-position `j` reverses the prefix of length
`T.length - j` and appends the sentinel. -/
theorem R_drop (T : Text) (j : Nat) (hj : j ≤ T.length) :
    (T.reverse ++ [0]).drop j = (T.take (T.length - j)).reverse ++ [0] := by
  have h3 : T = T.take (T.length - j) ++ T.drop (T.length - j) :=
    (List.take_append_drop _ _).symm
  have h4r : T.reverse = (T.take (T.length - j) ++ T.drop (T.length - j)).reverse :=
    congrArg List.reverse h3
  rw [List.reverse_append] at h4r
  rw [List.drop_append, show j - T.reverse.length = 0 from by rw [List.length_reverse]; omega,
      List.drop_zero, h4r, List.drop_append,
      show j - (T.drop (T.length - j)).reverse.length = 0 from by
        rw [List.length_reverse, List.length_drop]; omega,
      List.drop_zero,
      show (T.drop (T.length - j)).reverse.drop j = [] from
        List.drop_eq_nil_iff.mpr (by rw [List.length_reverse, List.length_drop]; omega)]
  simp

/-! ## Layer 2: rows, positions, and characters -/

instance : Inhabited Triple := ⟨⟨0,0,0⟩⟩

/-- sa field of a stream row -/
def saRow (T : Text) (k : Nat) : Nat := (triplesOf T)[k]!.sa

/-- char field of a stream row -/
def charRow (T : Text) (k : Nat) : Nat := (triplesOf T)[k]!.c

/-- getElem! and getD agree in range -/
theorem bang_getD : ∀ (l : List Triple) (i : Nat), i < l.length →
    l[i]! = l.getD i ⟨0,0,0⟩ := by
  intro l i h
  rw [getElem!_pos l i h]
  simp only [List.getD]
  rw [show l[i]? = some l[i] from (List.getElem?_eq_some_iff).mpr ⟨h, rfl⟩]
  simp

theorem getD_bang (T : Text) (k : Nat) (hk : k < (triplesOf T).length) :
    (triplesOf T).getD k ⟨0,0,0⟩ = (triplesOf T)[k]! :=
  (bang_getD (triplesOf T) k hk).symm

/-- row of a map over range: the index function -/
theorem map_range_bang (f : Nat → Triple) (n i : Nat) (h : i < n) :
    ((List.range n).map f)[i]! = f i := by
  rw [bang_getD _ _ (by rw [List.length_map, List.length_range]; exact h),
    getD_map_range f n i ⟨0,0,0⟩ h]

/-- stream length -/
theorem triplesOf_length (T : Text) : (triplesOf T).length = T.length + 1 := by
  simp only [triplesOf]
  rw [List.length_map, List.length_range]
  simp

/-- the sa field of row k is the k-th saOrder entry of R -/
theorem saRow_eq (T : Text) (k : Nat) (hk : k < (triplesOf T).length) :
    saRow T k = (saOrder (T.reverse ++ [0]))[k]! := by
  have hk' : k < T.length + 1 := by rwa [triplesOf_length] at hk
  simp only [saRow, triplesOf, List.length_append, List.length_reverse,
    List.length_singleton] at ⊢
  rw [map_range_bang _ (T.length + 1) k hk']

/-- sa = 0 rows carry the sentinel char -/
theorem charRow_zero (T : Text) (k : Nat) (hk : k < (triplesOf T).length)
    (h0 : (saOrder (T.reverse ++ [0]))[k]! = 0) :
    charRow T k = 0 := by
  have hk' : k < T.length + 1 := by rwa [triplesOf_length] at hk
  simp only [charRow, triplesOf, List.length_append, List.length_reverse,
    List.length_singleton] at ⊢
  rw [map_range_bang _ (T.length + 1) k hk']
  show (if ((saOrder (T.reverse ++ [0]))[k]! == 0) = true then 0
      else (T.reverse ++ [0])[(saOrder (T.reverse ++ [0]))[k]! - 1]!) = 0
  rw [h0]
  simp

/-- sa ≥ 1 rows carry the R-char before their suffix -/
theorem charRow_pos (T : Text) (k : Nat) (hk : k < (triplesOf T).length)
    (h1 : 1 ≤ (saOrder (T.reverse ++ [0]))[k]!) :
    charRow T k = (T.reverse ++ [0])[(saOrder (T.reverse ++ [0]))[k]! - 1]! := by
  have hk' : k < T.length + 1 := by rwa [triplesOf_length] at hk
  simp only [charRow, triplesOf, List.length_append, List.length_reverse,
    List.length_singleton] at ⊢
  rw [map_range_bang _ (T.length + 1) k hk']
  show (if ((saOrder (T.reverse ++ [0]))[k]! == 0) = true then 0
      else (T.reverse ++ [0])[(saOrder (T.reverse ++ [0]))[k]! - 1]!) =
    (T.reverse ++ [0])[(saOrder (T.reverse ++ [0]))[k]! - 1]!
  have hb : ((saOrder (T.reverse ++ [0]))[k]! == 0) = false := by
    cases hbb : ((saOrder (T.reverse ++ [0]))[k]! == 0) with
    | false => rfl
    | true => rw [beq_iff_eq] at hbb; omega
  rw [hb]
  simp

/-- every row's sa is an R-position -/
theorem saRow_lt (T : Text) (k : Nat) (hk : k < (triplesOf T).length) :
    saRow T k < T.length + 1 := by
  rw [saRow_eq T k hk]
  have hlen : (saOrder (T.reverse ++ [0])).length = T.length + 1 := by
    rw [saOrder_length]; simp
  have hklt : k < (saOrder (T.reverse ++ [0])).length := by
    rw [hlen]; rwa [triplesOf_length] at hk
  have hmem : (saOrder (T.reverse ++ [0]))[k]! ∈ saOrder (T.reverse ++ [0]) := by
    rw [getElem!_pos _ k hklt]; exact List.getElem_mem hklt
  have := saOrder_lt (T.reverse ++ [0]) _ hmem
  simpa using this

/-- sa determines the row -/
theorem saRow_inj (T : Text) {k k' : Nat} (hk : k < (triplesOf T).length)
    (hk' : k' < (triplesOf T).length) (h : saRow T k = saRow T k') : k = k' := by
  classical
  by_cases hne : k = k'
  · exact hne
  have hlen : (saOrder (T.reverse ++ [0])).length = T.length + 1 := by
    rw [saOrder_length]; simp
  have hpair : ∀ i j, i < (saOrder (T.reverse ++ [0])).length →
      j < (saOrder (T.reverse ++ [0])).length → i < j →
      (saOrder (T.reverse ++ [0]))[i]! ≠ (saOrder (T.reverse ++ [0]))[j]! := by
    intro i j hi hj hij
    have h := (List.pairwise_iff_getElem.mp (saOrder_nodup _)) i j hi hj hij
    rw [getElem!_pos _ i hi, getElem!_pos _ j hj]
    exact h
  have hklt : k < (saOrder (T.reverse ++ [0])).length := by
    rw [hlen]; rwa [triplesOf_length] at hk
  have hklt' : k' < (saOrder (T.reverse ++ [0])).length := by
    rw [hlen]; rwa [triplesOf_length] at hk'
  rw [saRow_eq T k hk, saRow_eq T k' hk'] at h
  rcases Nat.lt_or_ge k k' with hlt | hge
  · exact absurd h (hpair k k' hklt hklt' hlt)
  · have hlt' : k' < k := by omega
    exact absurd h.symm (hpair k' k hklt' hklt hlt')

/-- every R-position is some row's sa -/
theorem saRow_surj (T : Text) (p : Nat) (hp : p < T.length + 1) :
    ∃ k, k < (triplesOf T).length ∧ saRow T k = p := by
  have hmem : p ∈ saOrder (T.reverse ++ [0]) := by
    refine saOrder_mem _ p ?_
    simp
    omega
  obtain ⟨k, hk, hget⟩ := List.mem_iff_getElem.mp hmem
  have hlen : (saOrder (T.reverse ++ [0])).length = T.length + 1 := by
    rw [saOrder_length, List.length_append, List.length_reverse, List.length_singleton]
  have hkt : k < (triplesOf T).length := by
    rw [triplesOf_length]
    rw [hlen] at hk
    exact hk
  refine ⟨k, hkt, ?_⟩
  rw [saRow_eq T k hkt]
  rw [getElem!_pos _ k hk]
  exact hget

/-- the R-char at position i is the text char at `T.length - 1 - i` -/
theorem R_get (T : Text) (i : Nat) (hi : i < T.length) :
    (T.reverse ++ [0])[i]! = T[T.length - 1 - i]! := by
  have h1 : i < (T.reverse ++ [0]).length := by simp; omega
  have h2 : i < T.reverse.length := by rw [List.length_reverse]; exact hi
  have h3 : T.length - 1 - i < T.length := by omega
  rw [getElem!_pos (T.reverse ++ [0]) i h1, getElem!_pos T (T.length - 1 - i) h3,
    List.getElem_append_left h2, List.getElem_reverse h2]

/-- text position of row k -/
def rowPos (T : Text) (k : Nat) : Nat := T.length + 1 - saRow T k

/-- the BWT char of a row is the text char AT its position -/
theorem charRow_at_pos (T : Text) (k : Nat) (hk : k < (triplesOf T).length)
    (hsa : 1 ≤ saRow T k) : charRow T k = T[rowPos T k - 1]! := by
  have hlt := saRow_lt T k hk
  have heq := saRow_eq T k hk
  have ho : 1 ≤ (saOrder (T.reverse ++ [0]))[k]! := by rw [← heq]; exact hsa
  have h1 : (saOrder (T.reverse ++ [0]))[k]! - 1 < T.length := by omega
  rw [charRow_pos T k hk ho, R_get T _ h1]
  have hfix : T.length - 1 - ((saOrder (T.reverse ++ [0]))[k]! - 1) = rowPos T k - 1 := by
    rw [← heq]
    unfold rowPos
    omega
  rw [hfix]

/-! ## Layer 3: the w-interval (rows whose R-suffix starts with reverse w) -/

/-- a suffix relation on plain lists -/
def SuffixOf (w A : List Nat) : Prop := ∃ q, A = q ++ w

/-- suffix gives reverse-prefix (with the trailing sentinel) -/
theorem suffix_rev_prefix (A w : List Nat) (h : SuffixOf w A) :
    ∃ rest, A.reverse ++ [0] = w.reverse ++ rest := by
  obtain ⟨q, rfl⟩ := h
  exact ⟨q.reverse ++ [0], by rw [List.reverse_append, List.append_assoc]⟩

/-- reverse-prefix (with the trailing sentinel) gives suffix, for positive w -/
theorem rev_prefix_suffix (A w : List Nat) (h : ∃ rest, A.reverse ++ [0] = w.reverse ++ rest)
    (hw : ∀ x ∈ w, 1 ≤ x) : SuffixOf w A := by
  obtain ⟨rest, hrest⟩ := h
  by_cases hlen : w.length ≤ A.length
  · have hlen' : w.length ≤ A.reverse.length := by rw [List.length_reverse]; exact hlen
    have hsub1 : w.length - A.reverse.length = 0 := by omega
    have hlenw : w.reverse.length = w.length := List.length_reverse
    have htake : (A.reverse ++ [0]).take w.length = (w.reverse ++ rest).take w.length := by
      rw [hrest]
    rw [List.take_append, List.take_append, hsub1, hlenw, Nat.sub_self, List.take_zero,
      List.take_zero, List.append_nil, List.append_nil] at htake
    have hwr : w.reverse.take w.length = w.reverse :=
      take_full w.reverse w.length (by rw [List.length_reverse]; omega)
    rw [hwr] at htake
    have hsplit : A.reverse = w.reverse ++ A.reverse.drop w.length := by
      have h1 : A.reverse.take w.length ++ A.reverse.drop w.length = A.reverse :=
        List.take_append_drop _ _
      rw [htake] at h1
      exact h1.symm
    refine ⟨A.reverse.drop w.length |>.reverse, ?_⟩
    calc A = A.reverse.reverse := (List.reverse_reverse A).symm
      _ = (w.reverse ++ A.reverse.drop w.length).reverse := congrArg List.reverse hsplit
      _ = (A.reverse.drop w.length).reverse ++ w.reverse.reverse := List.reverse_append
      _ = (A.reverse.drop w.length).reverse ++ w := by rw [List.reverse_reverse]
  · -- w.length ≥ A.length + 1
    have hlenA : A.reverse.length = A.length := List.length_reverse
    have hlenr : w.reverse.length = w.length := List.length_reverse
    have hlenEq : A.reverse.length + 1 = w.length + rest.length := by
      have := congrArg List.length hrest
      simp only [List.length_append, List.length_singleton] at this
      omega
    have hrest0 : rest.length = 0 := by omega
    have hw0 : rest = [] := List.length_eq_zero_iff.mp hrest0
    have hwfull : w.reverse = A.reverse ++ [0] := by
      rw [hw0, List.append_nil] at hrest; exact hrest.symm
    have hwA : w = 0 :: A := by
      have hw1 : w = (A.reverse ++ [0]).reverse := by
        rw [← hwfull, List.reverse_reverse]
      rw [hw1, List.reverse_append, List.reverse_singleton, List.reverse_reverse,
        List.singleton_append]
    have hhead : 0 ∈ w := by rw [hwA]; exact List.mem_cons_self
    have := hw 0 hhead
    omega

/-- row k is a w-row: its R-suffix starts with reverse w -/
def wRow (T : Text) (w : List Nat) (k : Nat) : Prop :=
  ∃ rest, (T.reverse ++ [0]).drop (saRow T k) = w.reverse ++ rest

/-- w-rows are exactly the rows whose position ends an occurrence of w -/
theorem wRow_iff_suffix (T : Text) (w : List Nat) (k : Nat)
    (hkw : k < (triplesOf T).length) (hw : ∀ x ∈ w, 1 ≤ x) :
    wRow T w k ↔ SuffixOf w (T.take (rowPos T k - 1)) := by
  have hlt := saRow_lt T k hkw
  constructor
  · intro h
    rcases Nat.eq_zero_or_pos (saRow T k) with h0 | h1
    · -- sa = 0: the full R is the suffix; rowPos - 1 = T.length
      have hrp : rowPos T k - 1 = T.length := by
        unfold rowPos; rw [h0]; omega
      have hR : (T.reverse ++ [0]).drop 0 = (T.reverse ++ [0]) := by
        rw [List.drop_zero]
      obtain ⟨rest, hrest⟩ := h
      rw [h0, hR] at hrest
      have : SuffixOf w T := rev_prefix_suffix T w ⟨rest, hrest⟩ hw
      obtain ⟨q, hq⟩ := this
      refine ⟨q, ?_⟩
      rw [hrp, take_full T T.length (by omega)]
      exact hq
    · -- sa ≥ 1
      have hsa : saRow T k ≤ T.length := by omega
      have hrp : rowPos T k - 1 = T.length - saRow T k := by
        unfold rowPos; omega
      obtain ⟨rest, hrest⟩ := h
      have hdrop : (T.reverse ++ [0]).drop (saRow T k)
          = (T.take (T.length - saRow T k)).reverse ++ [0] := R_drop T _ hsa
      rw [hdrop] at hrest
      have hA : SuffixOf w (T.take (T.length - saRow T k)) :=
        rev_prefix_suffix _ w ⟨rest, hrest⟩ hw
      obtain ⟨q, hq⟩ := hA
      refine ⟨q, ?_⟩
      rw [hrp]
      exact hq
  · intro hsuffix
    rcases Nat.eq_zero_or_pos (saRow T k) with h0 | h1
    · have hrp : rowPos T k - 1 = T.length := by
        unfold rowPos; rw [h0]; omega
      have hR : (T.reverse ++ [0]).drop 0 = (T.reverse ++ [0]) := by
        rw [List.drop_zero]
      have hT : T.take (rowPos T k - 1) = T := by
        rw [hrp]; exact take_full T T.length (by omega)
      obtain ⟨q, hq⟩ := hsuffix
      have hfull : SuffixOf w T := ⟨q, by rw [← hT]; exact hq⟩
      obtain ⟨rest, hrest⟩ := suffix_rev_prefix T w hfull
      refine ⟨rest, ?_⟩
      rw [h0, hR]
      exact hrest
    · have hsa : saRow T k ≤ T.length := by omega
      have hrp : rowPos T k - 1 = T.length - saRow T k := by
        unfold rowPos; omega
      have hdrop : (T.reverse ++ [0]).drop (saRow T k)
          = (T.take (T.length - saRow T k)).reverse ++ [0] := R_drop T _ hsa
      obtain ⟨rest, hrest⟩ := suffix_rev_prefix (T.take (rowPos T k - 1)) w hsuffix
      rw [hrp] at hrest
      refine ⟨rest, ?_⟩
      rw [hdrop]
      exact hrest

/-! ## Layer 3b: lex-convexity of the w-interval -/

/-- two lex-comparable prefix-sharing lists force the middle to share the prefix -/
theorem lexLE_between_prefix : ∀ (u a x b : List Nat),
    (∃ ra, a = u ++ ra) → (∃ rb, b = u ++ rb) →
    lexLE a x = true → lexLE x b = true → ∃ rx, x = u ++ rx := by
  intro u
  induction u with
  | nil => intro a x b _ _ _ _; exact ⟨x, rfl⟩
  | cons un ur ih =>
    intro a x b ha hb h1 h2
    obtain ⟨ra, rfl⟩ := ha
    obtain ⟨rb, rfl⟩ := hb
    cases x with
    | nil => simp [lexLE] at h1
    | cons xn xr =>
      simp only [List.cons_append, lexLE, Bool.or_eq_true, Bool.and_eq_true] at h1 h2
      rcases h1 with hlt | ⟨heq, hrest⟩
      · -- un < xn: h2 impossible
        have hun : un < xn := of_decide_eq_true hlt
        have h2' : (decide (xn < un) = true) ∨ ((xn == un) = true ∧ lexLE xr (ur ++ rb) = true) := h2
        rcases h2' with hlt2 | ⟨heq2, _⟩
        · have : xn < un := of_decide_eq_true hlt2
          omega
        · have : xn = un := beq_iff_eq.mp heq2
          omega
      · -- un = xn
        have hun : un = xn := beq_iff_eq.mp heq
        have hrest2 : lexLE xr (ur ++ rb) = true := by
          rcases h2 with hlt2 | ⟨heq2, hrest2'⟩
          · have : xn < un := of_decide_eq_true hlt2
            omega
          · exact hrest2'
        obtain ⟨rx, hrx⟩ := ih (ur ++ ra) xr (ur ++ rb) ⟨_, rfl⟩ ⟨_, rfl⟩ hrest hrest2
        refine ⟨rx, ?_⟩
        rw [hrx, hun, List.cons_append]

/-! ## Layer 4: convexity of the w-interval and the block argument -/

/-- the w-rows are convex in SA order -/
theorem wRow_convex (T : Text) (w : List Nat) {k k' k'' : Nat}
    (hk : k < (triplesOf T).length) (hk'' : k'' < (triplesOf T).length)
    (hlt : k < k') (hlt' : k' < k'') (hWk : wRow T w k) (hWk'' : wRow T w k'') :
    wRow T w k' := by
  have hlen : (saOrder (T.reverse ++ [0])).length = T.length + 1 := by
    rw [saOrder_length, List.length_append, List.length_reverse, List.length_singleton]
  have hklt : k < (saOrder (T.reverse ++ [0])).length := by
    rw [hlen]; rwa [triplesOf_length] at hk
  have hklt'' : k'' < (saOrder (T.reverse ++ [0])).length := by
    rw [hlen]; rwa [triplesOf_length] at hk''
  have hmid : k' < (saOrder (T.reverse ++ [0])).length := by omega
  have hk'len : k' < (triplesOf T).length := by
    have := triplesOf_length T; omega
  have hs1 : lexLE ((T.reverse ++ [0]).drop (saOrder (T.reverse ++ [0]))[k])
      ((T.reverse ++ [0]).drop (saOrder (T.reverse ++ [0]))[k']) :=
    saOrder_sorted_getElem (T.reverse ++ [0]) k k' hklt hmid hlt
  have hs2 : lexLE ((T.reverse ++ [0]).drop (saOrder (T.reverse ++ [0]))[k'])
      ((T.reverse ++ [0]).drop (saOrder (T.reverse ++ [0]))[k'']) :=
    saOrder_sorted_getElem (T.reverse ++ [0]) k' k'' hmid hklt'' hlt'
  obtain ⟨rest, hrest⟩ := hWk
  obtain ⟨rest'', hrest''⟩ := hWk''
  rw [saRow_eq T k hk, getElem!_pos _ k hklt] at hrest
  rw [saRow_eq T k'' hk'', getElem!_pos _ k'' hklt''] at hrest''
  obtain ⟨rx, hrx⟩ := lexLE_between_prefix w.reverse _ _ _ ⟨rest, hrest⟩ ⟨rest'', hrest''⟩ hs1 hs2
  refine ⟨rx, ?_⟩
  rw [saRow_eq T k' hk'len, getElem!_pos _ k' hmid]
  exact hrx


/-- charRow is the getD-char -/
theorem charRow_getD (T : Text) (k : Nat) (hk : k < (triplesOf T).length) :
    ((triplesOf T).getD k ⟨0,0,0⟩).c = charRow T k := by
  rw [getD_bang T k hk]; rfl

theorem saRow_getD (T : Text) (k : Nat) (hk : k < (triplesOf T).length) :
    ((triplesOf T).getD k ⟨0,0,0⟩).sa = saRow T k := by
  rw [getD_bang T k hk]; rfl

/-- char differs from the next row's: run edge -/
theorem isRunEdge_of_diff_next (ts : List Triple) (k : Nat) (hk : k < ts.length)
    (hdiff : (ts.getD k ⟨0,0,0⟩).c ≠ (ts.getD (k+1) ⟨0,0,0⟩).c) : isRunEdge ts k = true := by
  unfold isRunEdge
  rw [Bool.or_eq_true]
  refine Or.inr ?_
  rw [Bool.and_eq_true]
  refine ⟨decide_eq_true hk, ?_⟩
  rw [Bool.or_eq_true]
  refine Or.inr ?_
  exact bne_iff_ne.mpr hdiff

/-- char differs from the previous row's: run edge -/
theorem isRunEdge_of_diff_prev (ts : List Triple) (k : Nat) (hk : k < ts.length) (hk1 : 1 ≤ k)
    (hdiff : (ts.getD k ⟨0,0,0⟩).c ≠ (ts.getD (k-1) ⟨0,0,0⟩).c) : isRunEdge ts k = true := by
  unfold isRunEdge isBoundary
  rw [if_neg (by omega), if_neg (by omega), Bool.or_eq_true]
  exact Or.inl (bne_iff_ne.mpr hdiff)

/-- take m splits at position m -/
theorem take_succ_last (T : Text) {m : Nat} (hm : 1 ≤ m) (hm2 : m ≤ T.length) :
    T.take m = T.take (m - 1) ++ [T[m - 1]!] := by
  have hm1 : m - 1 < T.length := by omega
  have hopt : T[m - 1]? = some T[m - 1]! :=
    (List.getElem?_eq_some_iff).mpr ⟨hm1, (getElem!_pos T (m - 1) hm1).symm⟩
  rw [show m = (m - 1) + 1 from by omega, List.take_add_one, hopt]
  rfl

/-- if a w-row of char ≠ c exists, a char-c w-row is a run edge -/
theorem exists_edge_wRow (T : Text) (w : List Nat) (c : Nat) {a b : Nat}
    (hla : a < (triplesOf T).length) (hlb : b < (triplesOf T).length)
    (hWa : wRow T w a) (hca : charRow T a = c)
    (hWb : wRow T w b) (hcb : charRow T b ≠ c) :
    ∃ k, k < (triplesOf T).length ∧ wRow T w k ∧ charRow T k = c ∧
      isRunEdge (triplesOf T) k = true := by
  classical
  have key : ∀ x y : Nat, x < y → x < (triplesOf T).length → y < (triplesOf T).length →
      wRow T w x → wRow T w y →
      decide (charRow T x = c) ≠ decide (charRow T y = c) →
      ∃ k, k < (triplesOf T).length ∧ wRow T w k ∧ charRow T k = c ∧
        isRunEdge (triplesOf T) k = true := by
    intro x y hxy hlx hly hWx hWy hne
    obtain ⟨k, hk1, hk2, hk3⟩ :=
      exists_bool_change (fun k => decide (charRow T k = c)) (y - x) x (by
        have hxy' : x + (y - x) = y := by omega
        intro hcon
        apply hne
        rw [hxy'] at hcon
        exact hcon)
    have hkl1 : x ≤ k := hk1
    have hkl2 : k + 1 ≤ y := by omega
    have hkl : k < (triplesOf T).length := by
      have := triplesOf_length T; omega
    have hkl1' : k + 1 < (triplesOf T).length := by
      have := triplesOf_length T; omega
    -- both are w-rows (convexity)
    have hWk : wRow T w k := by
      rcases Nat.eq_or_lt_of_le hkl1 with heq | hlt
      · rw [← heq]; exact hWx
      · exact wRow_convex T w hlx hly hlt (by omega) hWx hWy
    have hWk1 : wRow T w (k + 1) := by
      rcases Nat.eq_or_lt_of_le hkl2 with heq | hlt
      · rw [heq]; exact hWy
      · exact wRow_convex T w hlx hly (by omega) hlt hWx hWy
    -- the status change gives a char difference
    have hcdiff : charRow T k ≠ charRow T (k + 1) := by
      intro heqchar
      have hbeta : decide (charRow T k = c) ≠ decide (charRow T (k + 1) = c) := hk3
      rw [heqchar] at hbeta
      exact absurd hbeta (by simp)
    by_cases hck : charRow T k = c
    · refine ⟨k, hkl, hWk, hck, ?_⟩
      exact isRunEdge_of_diff_next (triplesOf T) k hkl
        (by rw [charRow_getD T k hkl, charRow_getD T (k+1) hkl1']; omega)
    · have hck1 : charRow T (k + 1) = c := by
        have hbeta : decide (charRow T k = c) ≠ decide (charRow T (k + 1) = c) := hk3
        have hf : decide (charRow T k = c) = false := by simp [hck]
        rw [hf] at hbeta
        cases hbb : decide (charRow T (k + 1) = c) with
        | true => exact of_decide_eq_true hbb
        | false => rw [hbb] at hbeta; exact absurd rfl hbeta
      refine ⟨k + 1, hkl1', hWk1, hck1, ?_⟩
      exact isRunEdge_of_diff_prev (triplesOf T) (k + 1) hkl1' (by omega)
        (by rw [show k + 1 - 1 = k from by omega, charRow_getD T (k+1) hkl1',
            charRow_getD T k hkl]; omega)

  rcases Nat.lt_trichotomy a b with hlt | heq | hgt
  · exact key a b hlt hla hlb hWa hWb (by simp [hca, hcb])
  · rw [← heq] at hcb
    exact absurd hca hcb
  · exact key b a hgt hlb hla hWb hWa (by simp [hca, hcb])


/-! ## Layer 5: the assembly -/

/-- `rightMaximal.isSuffix = true` gives a plain suffix -/
theorem suffix_of_isSuffix (w T : List Nat)
    (h : rightMaximal.isSuffix w T = true) : SuffixOf w T := by
  unfold rightMaximal.isSuffix at h
  rw [Bool.and_eq_true] at h
  obtain ⟨_, hbeq⟩ := h
  refine ⟨T.take (T.length - w.length), ?_⟩
  have hdrop : List.drop (T.length - w.length) T = w := by
    simp at hbeq
    exact hbeq.symm
  calc T = T.take (T.length - w.length) ++ List.drop (T.length - w.length) T :=
        (List.take_append_drop _ _).symm
    _ = T.take (T.length - w.length) ++ w := by rw [hdrop]


/-! ### O1 campaign: O1Interval.lean (assembled) -/

/-- A nonempty convex set of valid row indices has inclusive endpoints. -/
theorem finite_convex_interval (n : Nat) (P : Nat → Prop)
    (hconv : ∀ a j b, a < n → b < n → a < j → j < b → P a → P b → P j)
    (k : Nat) (hk : k < n) (hPk : P k) :
    ∃ a b, a ≤ k ∧ k ≤ b ∧ b < n ∧
      (∀ j, a ≤ j → j ≤ b → P j) ∧
      (∀ j, j < n → P j → a ≤ j ∧ j ≤ b) ∧
      (a = 0 ∨ ¬ P (a-1)) ∧ (b+1 = n ∨ ¬ P (b+1)) := by
  classical
  let rows := (List.range n).filter (fun j => decide (P j))
  have mem_rows : ∀ j, j ∈ rows ↔ j < n ∧ P j := by
    intro j
    simp [rows]
  have hkm : k ∈ rows := (mem_rows k).mpr ⟨hk, hPk⟩
  have hne : rows ≠ [] := by
    intro h
    rw [h] at hkm
    exact List.not_mem_nil hkm
  obtain ⟨a, ha, hmin⟩ := exists_min_le rows hne
  obtain ⟨b, hb, hmax⟩ := exists_argmax_f rows hne id
  have ha' := (mem_rows a).mp ha
  have hb' := (mem_rows b).mp hb
  have hak := hmin k hkm
  have hkb : k ≤ b := hmax k hkm
  refine ⟨a, b, hak, hkb, hb'.1, ?_, ?_, ?_, ?_⟩
  · intro j haj hjb
    by_cases hja : j = a
    · simpa [hja] using ha'.2
    by_cases hjb' : j = b
    · simpa [hjb'] using hb'.2
    exact hconv a j b ha'.1 hb'.1 (by omega) (by omega) ha'.2 hb'.2
  · intro j hj hPj
    have hm := (mem_rows j).mpr ⟨hj, hPj⟩
    exact ⟨hmin j hm, hmax j hm⟩
  · by_cases ha0 : a = 0
    · exact Or.inl ha0
    · refine Or.inr ?_
      intro hP
      have hm := hmin (a-1) ((mem_rows (a-1)).mpr ⟨by omega, hP⟩)
      omega
  · by_cases hbn : b+1 = n
    · exact Or.inl hbn
    · refine Or.inr ?_
      intro hP
      have hm : b+1 ≤ b := hmax (b+1) ((mem_rows (b+1)).mpr ⟨by omega, hP⟩)
      omega

/-- A nonzero character row with context w covers the corresponding word. -/
theorem coversAt_of_wRow (T : Text) (hT : positive T = true)
    (w : List Nat) (c k : Nat) (hk : k < (triplesOf T).length)
    (hwpos : ∀ x ∈ w, 1 ≤ x) (hc : 1 ≤ c)
    (hW : wRow T w k) (hchar : charRow T k = c) :
    coversAt (w ++ [c]) (rowPos T k) T = true := by
  have hsa : 1 ≤ saRow T k := by
    by_cases h : 1 ≤ saRow T k
    · exact h
    have hz : saRow T k = 0 := by omega
    have hc0 := charRow_zero T k hk (by rw [← saRow_eq T k hk, hz])
    omega
  have hlt := saRow_lt T k hk
  have hr1 : 1 ≤ rowPos T k := by unfold rowPos; omega
  have hrn : rowPos T k ≤ T.length := by unfold rowPos; omega
  obtain ⟨q, hq⟩ := (wRow_iff_suffix T w k hk hwpos).mp hW
  apply (coversAt_iff_suffix _ _ _).mpr
  refine ⟨q, ?_⟩
  rw [pref, take_succ_last T hr1 hrn, ← charRow_at_pos T k hk hsa,
    hchar, hq, List.append_assoc]


/-! ### O1 campaign: O1LcpBridge.lean (assembled) -/

/-- The stored LCP of a noninitial row compares its suffix with the preceding row. -/
theorem lcpRow_eq (T : Text) (k : Nat) (hk : 1 ≤ k)
    (hkn : k < (triplesOf T).length) :
    ((triplesOf T).getD k ⟨0,0,0⟩).lcp =
      lcpOf ((T.reverse ++ [0]).drop (saRow T k))
        ((T.reverse ++ [0]).drop (saRow T (k - 1))) := by
  have hprev : k - 1 < (triplesOf T).length := by omega
  rw [saRow_eq T k hkn, saRow_eq T (k - 1) hprev]
  have hlen : k < (T.reverse ++ [0]).length := by
    simpa [triplesOf_length] using hkn
  unfold triplesOf
  rw [getD_map_range _ _ _ _ hlen]
  dsimp only
  rw [getElem!_pos _ k (by simpa using hlen)]
  simp only [List.getElem_map, List.getElem_range]
  simp [show k ≠ 0 by omega]

/-- A row begins with the reversed word exactly when its word-length take is that word. -/
theorem wRow_iff_take (T : Text) (w : List Nat) (k : Nat) :
    wRow T w k ↔
      ((T.reverse ++ [0]).drop (saRow T k)).take w.length = w.reverse := by
  constructor
  · rintro ⟨rest, hrest⟩
    have hwlen : w.reverse.length = w.length := List.length_reverse
    rw [hrest, ← hwlen, List.take_append_length]
  · intro htake
    refine ⟨((T.reverse ++ [0]).drop (saRow T k)).drop w.length, ?_⟩
    rw [← htake, List.take_append_drop]

theorem wRow_length_le (T : Text) (w : List Nat) (k : Nat)
    (hw : wRow T w k) :
    w.length ≤ ((T.reverse ++ [0]).drop (saRow T k)).length := by
  obtain ⟨rest, hrest⟩ := hw
  rw [hrest, List.length_append, List.length_reverse]
  omega

/-- From a word row, crossing the next LCP preserves the word exactly at its length threshold. -/
theorem wRow_lcp_step (T : Text) (w : List Nat) (k : Nat)
    (hk : 1 ≤ k) (hkn : k < (triplesOf T).length)
    (hw : wRow T w (k - 1)) :
    (wRow T w k ↔ w.length ≤ ((triplesOf T).getD k ⟨0,0,0⟩).lcp) := by
  rw [lcpRow_eq T k hk hkn]
  have hprev := (wRow_iff_take T w (k - 1)).mp hw
  constructor
  · intro hcur
    apply (take_eq_iff_le_lcpOf _ _ w.length
      ⟨wRow_length_le T w k hcur, wRow_length_le T w (k - 1) hw⟩).mp
    exact ((wRow_iff_take T w k).mp hcur).trans hprev.symm
  · intro hlcp
    have hleft := Nat.le_trans hlcp (lcpOf_le_left _ _)
    have hright := Nat.le_trans hlcp (lcpOf_le_right _ _)
    apply (wRow_iff_take T w k).mpr
    exact ((take_eq_iff_le_lcpOf _ _ w.length ⟨hleft, hright⟩).mpr hlcp).trans hprev

/-- LCP transport in the reverse direction, from the current row to its predecessor. -/
theorem wRow_lcp_prev (T : Text) (w : List Nat) (k : Nat)
    (hk : 1 ≤ k) (hkn : k < (triplesOf T).length)
    (hw : wRow T w k)
    (hlcp : w.length ≤ ((triplesOf T).getD k ⟨0,0,0⟩).lcp) :
    wRow T w (k - 1) := by
  rw [lcpRow_eq T k hk hkn] at hlcp
  have hleft := Nat.le_trans hlcp (lcpOf_le_left _ _)
  have hright := Nat.le_trans hlcp (lcpOf_le_right _ _)
  apply (wRow_iff_take T w (k - 1)).mpr
  exact ((take_eq_iff_le_lcpOf _ _ w.length ⟨hleft, hright⟩).mpr hlcp).symm.trans
    ((wRow_iff_take T w k).mp hw)


/-! ### O1 campaign: O1SemanticInterval.lean (assembled) -/

/-- The maximal interval of rows beginning with a word has LCP barriers at
both external boundaries and reaches the word-length threshold internally. -/
theorem wRow_lcp_interval (T : Text) (w : List Nat) (k : Nat)
    (hk : k < (triplesOf T).length) (hw : wRow T w k) :
    ∃ a b, a ≤ k ∧ k ≤ b ∧ b < (triplesOf T).length ∧
      (∀ j, a ≤ j → j ≤ b → wRow T w j) ∧
      (∀ j, j < (triplesOf T).length → wRow T w j → a ≤ j ∧ j ≤ b) ∧
      (∀ j, a < j → j ≤ b →
        w.length ≤ ((triplesOf T).getD j ⟨0,0,0⟩).lcp) ∧
      (a = 0 ∨ ((triplesOf T).getD a ⟨0,0,0⟩).lcp < w.length) ∧
      (b + 1 = (triplesOf T).length ∨
        ((triplesOf T).getD (b + 1) ⟨0,0,0⟩).lcp < w.length) := by
  obtain ⟨a, b, hak, hkb, hbn, hinside, hall, hleft, hright⟩ :=
    finite_convex_interval (triplesOf T).length (wRow T w)
      (fun _ _ _ ha hb haj hjb hWa hWb =>
        wRow_convex T w ha hb haj hjb hWa hWb) k hk hw
  have hab : a ≤ b := Nat.le_trans hak hkb
  have han : a < (triplesOf T).length := by omega
  have hWa : wRow T w a := hinside a (Nat.le_refl _) hab
  have hWb : wRow T w b := hinside b hab (Nat.le_refl _)
  refine ⟨a, b, hak, hkb, hbn, hinside, hall, ?_, ?_, ?_⟩
  · intro j haj hjb
    exact (wRow_lcp_step T w j (by omega) (by omega)
      (hinside (j - 1) (by omega) (by omega))).mp
      (hinside j (by omega) hjb)
  · rcases hleft with ha0 | hnot
    · exact Or.inl ha0
    · by_cases ha0 : a = 0
      · exact Or.inl ha0
      · refine Or.inr (Nat.lt_of_not_ge ?_)
        intro hlcp
        exact hnot (wRow_lcp_prev T w a (by omega) han hWa hlcp)
  · rcases hright with hlast | hnot
    · exact Or.inl hlast
    · by_cases hlast : b + 1 = (triplesOf T).length
      · exact Or.inl hlast
      · refine Or.inr (Nat.lt_of_not_ge ?_)
        intro hlcp
        have hprev : wRow T w (b + 1 - 1) := by simpa using hWb
        exact hnot ((wRow_lcp_step T w (b + 1) (by omega) (by omega) hprev).mpr hlcp)


/-! ### O1 campaign: O1RequirementBoundary.lean (assembled) -/

/-- Two differently colored rows in a word interval force an internal
boundary with a row of the specified color on one side. -/
theorem boundary_of_mixed_wRows (T : Text) (w : List Nat) (c : Nat) {a b : Nat}
    (hla : a < (triplesOf T).length) (hlb : b < (triplesOf T).length)
    (hWa : wRow T w a) (hca : charRow T a = c)
    (hWb : wRow T w b) (hcb : charRow T b ≠ c) :
    ∃ k, 1 ≤ k ∧ k < (triplesOf T).length ∧
      charRow T (k-1) ≠ charRow T k ∧ wRow T w (k-1) ∧ wRow T w k ∧
      (charRow T (k-1) = c ∨ charRow T k = c) := by
  classical
  have key : ∀ x y : Nat, x < y → x < (triplesOf T).length → y < (triplesOf T).length →
      wRow T w x → wRow T w y →
      decide (charRow T x = c) ≠ decide (charRow T y = c) →
      ∃ k, 1 ≤ k ∧ k < (triplesOf T).length ∧
        charRow T (k-1) ≠ charRow T k ∧ wRow T w (k-1) ∧ wRow T w k ∧
        (charRow T (k-1) = c ∨ charRow T k = c) := by
    intro x y hxy hlx hly hWx hWy hne
    obtain ⟨k, hk1, hk2, hk3⟩ :=
      exists_bool_change (fun k => decide (charRow T k = c)) (y - x) x (by
        have hxy' : x + (y - x) = y := by omega
        intro hcon
        apply hne
        rw [hxy'] at hcon
        exact hcon)
    have hkl1 : x ≤ k := hk1
    have hkl2 : k + 1 ≤ y := by omega
    have hkl : k < (triplesOf T).length := by
      have := triplesOf_length T; omega
    have hkl1' : k + 1 < (triplesOf T).length := by
      have := triplesOf_length T; omega
    -- both are w-rows (convexity)
    have hWk : wRow T w k := by
      rcases Nat.eq_or_lt_of_le hkl1 with heq | hlt
      · rw [← heq]; exact hWx
      · exact wRow_convex T w hlx hly hlt (by omega) hWx hWy
    have hWk1 : wRow T w (k + 1) := by
      rcases Nat.eq_or_lt_of_le hkl2 with heq | hlt
      · rw [heq]; exact hWy
      · exact wRow_convex T w hlx hly (by omega) hlt hWx hWy
    -- the status change gives a char difference
    have hcdiff : charRow T k ≠ charRow T (k + 1) := by
      intro heqchar
      have hbeta : decide (charRow T k = c) ≠ decide (charRow T (k + 1) = c) := hk3
      rw [heqchar] at hbeta
      exact absurd hbeta (by simp)
    have hcolor : charRow T k = c ∨ charRow T (k+1) = c := by
      by_cases hck : charRow T k = c
      · exact Or.inl hck
      · by_cases hn : charRow T (k+1) = c
        · exact Or.inr hn
        · simp [hck, hn] at hk3
    refine ⟨k+1, by omega, hkl1', ?_⟩
    rw [show k + 1 - 1 = k from by omega]
    exact ⟨hcdiff, hWk, hWk1, hcolor⟩
  rcases Nat.lt_trichotomy a b with hlt | heq | hgt
  · exact key a b hlt hla hlb hWa hWb (by simp [hca, hcb])
  · rw [← heq] at hcb
    exact absurd hca hcb
  · exact key b a hgt hlb hla hWb hWa (by simp [hca, hcb])

/-- Every requirement has an internal mixed-color boundary in its word interval. -/
theorem requirement_boundary (T : Text) (hT : positive T = true)
    (w : List Nat) (c : Nat) (hp : (w,c) ∈ requirements T) :
    ∃ k, 1 ≤ k ∧ k < (triplesOf T).length ∧
      charRow T (k-1) ≠ charRow T k ∧ wRow T w (k-1) ∧ wRow T w k ∧
      (charRow T (k-1) = c ∨ charRow T k = c) := by
  classical
  obtain ⟨hwSub, hwMax, hcExt⟩ := (mem_requirements w c T).mp hp
  obtain ⟨m, hmP, hpCover⟩ := exists_covers_of_occurs (w ++ [c]) T
    ((mem_rightExts w c T).mp hcExt) (by simp)
  have hm1 : 1 ≤ m ∧ m ≤ T.length := by
    rcases List.mem_map.mp hmP with ⟨i, hi, rfl⟩
    rw [List.mem_range] at hi
    omega
  obtain ⟨hm1, hmT⟩ := hm1
  -- the cover decomposition
  obtain ⟨u, hu⟩ := (coversAt_iff_suffix (w ++ [c]) m T).mp hpCover
  rw [pref] at hu
  have hsplit : T.take m = T.take (m - 1) ++ [T[m - 1]!] := take_succ_last T hm1 hmT
  have hlast : c = T[m - 1]! ∧ u ++ w = T.take (m - 1) := by
    have hassoc : (u ++ w) ++ [c] = T.take (m - 1) ++ [T[m - 1]!] := by
      rw [show (u ++ w) ++ [c] = u ++ (w ++ [c]) from List.append_assoc u w [c],
        ← hu, hsplit]
    exact append_last_inj _ _ _ _ hassoc
  -- positivity
  have hwpos : ∀ x ∈ w, 1 ≤ x := by
    intro x hx
    have hxw : x ∈ T.take (m - 1) := by
      rw [← hlast.2]
      exact List.mem_append_right _ hx
    have hxT : x ∈ T := List.mem_of_mem_take hxw
    exact (positive_of_mem T hT hxT).1
  have hcpos : 1 ≤ c := by
    have hcT : c ∈ T := occurs_append_last w c T ((mem_rightExts w c T).mp hcExt)
    exact (positive_of_mem T hT hcT).1
  -- the row of m
  obtain ⟨k₀, hk₀, hsa₀⟩ := saRow_surj T (T.length + 1 - m) (by omega)
  have hrp₀ : rowPos T k₀ = m := by
    unfold rowPos; rw [hsa₀]; omega
  have hchar₀ : charRow T k₀ = c := by
    rw [charRow_at_pos T k₀ hk₀ (by rw [hsa₀]; omega), hrp₀]
    exact hlast.1.symm
  have hW₀ : wRow T w k₀ := by
    refine (wRow_iff_suffix T w k₀ hk₀ hwpos).mpr ⟨u, ?_⟩
    rw [hrp₀]
    exact hlast.2.symm
  by_cases hB : ∃ k₁, k₁ < (triplesOf T).length ∧ wRow T w k₁ ∧ charRow T k₁ ≠ c
  · obtain ⟨k₁, hk₁, hW₁, hc₁⟩ := hB
    exact boundary_of_mixed_wRows T w c hk₀ hk₁ hW₀ hchar₀ hW₁ hc₁
  · exfalso
    have hall : ∀ k, k < (triplesOf T).length → wRow T w k → charRow T k = c := by
      intro k hk hW
      by_cases hne : charRow T k = c
      · exact hne
      · exact absurd ⟨k, hk, hW, hne⟩ hB
    by_cases hSuf : SuffixOf w T
    · -- the sentinel row is a w-row of char 0 ≠ c
      obtain ⟨k₀', hk₀', hsa₀'⟩ := saRow_surj T 0 (by omega)
      obtain ⟨q₀, hq₀⟩ := hSuf
      have hW₀' : wRow T w k₀' := by
        refine (wRow_iff_suffix T w k₀' hk₀' hwpos).mpr ⟨q₀, ?_⟩
        have hrp : rowPos T k₀' - 1 = T.length := by
          unfold rowPos; rw [hsa₀']; omega
        rw [hrp, take_full T T.length (by omega)]
        exact hq₀
      have hc₀' : charRow T k₀' = 0 := by
        refine charRow_zero T k₀' hk₀' ?_
        rw [← saRow_eq T k₀' hk₀', hsa₀']
      have hcc := hall k₀' hk₀' hW₀'
      omega
    · -- every occurrence of w is followed by c
      have hfoll : ∀ e, e ≤ T.length - 1 → SuffixOf w (T.take e) → T[e]! = c := by
        intro e he hsuf
        obtain ⟨q, hq⟩ := hsuf
        obtain ⟨k, hk, hsak⟩ :=
          saRow_surj T (T.length + 1 - (e + 1)) (by omega)
        have hsa1 : 1 ≤ saRow T k := by rw [hsak]; omega
        have hrp : rowPos T k = e + 1 := by
          unfold rowPos; rw [hsak]; omega
        have hWk : wRow T w k := by
          refine (wRow_iff_suffix T w k hk hwpos).mpr ⟨q, ?_⟩
          rw [hrp, show e + 1 - 1 = e from by omega]
          exact hq
        have hck := hall k hk hWk
        rw [charRow_at_pos T k hk hsa1, hrp,
          show e + 1 - 1 = e from by omega] at hck
        exact hck
      -- every right extension equals c
      have hocc : ∀ x, occurs (w ++ [x]) T = true → x = c := by
        intro x hoccx
        obtain ⟨i, hi, htake⟩ := (occurs_eq_true (w ++ [x]) T (by simp)).mp hoccx
        have hlen : (w ++ [x]).length = w.length + 1 := by simp
        have hf : i + w.length + 1 ≤ T.length := by omega
        have hfull : T.take (i + w.length + 1) = T.take i ++ w ++ [x] := by
          rw [show i + w.length + 1 = i + (w ++ [x]).length from by
                rw [hlen]; omega,
            List.take_add, htake, List.append_assoc]
        have hsucc : T.take (i + w.length + 1)
            = T.take (i + w.length) ++ [T[i + w.length]!] :=
          take_succ_last T (by omega) (by omega)
        rw [hsucc] at hfull
        obtain ⟨hx, hqw⟩ := append_last_inj _ _ _ _ hfull
        have he : i + w.length ≤ T.length - 1 := by omega
        have hfolli := hfoll (i + w.length) he ⟨T.take i, hqw⟩
        omega
      have hext : ∀ x ∈ rightExts w T, x = c :=
        fun x hx => hocc x ((mem_rightExts w x T).mp hx)
      have hlen1 : (rightExts w T).length = 1 := by
        have hle : (rightExts w T).length ≤ 1 := by
          rw [rightExts]
          refine dedup_all_eq_le_one c _ ?_
          intro x hx
          obtain ⟨_, hxocc⟩ := List.mem_filter.mp hx
          exact hocc x hxocc
        have hge : 1 ≤ (rightExts w T).length := List.length_pos_of_mem hcExt
        omega
      -- w nonempty would contradict ¬SuffixOf
      cases w with
      | nil => exact absurd (show SuffixOf [] T from ⟨T, by simp⟩) hSuf
      | cons a as =>
        simp only [rightMaximal] at hwMax
        rw [Bool.and_eq_true, Bool.or_eq_true] at hwMax
        obtain ⟨_, hisf | hlen2⟩ := hwMax
        · exact absurd (suffix_of_isSuffix _ _ hisf) hSuf
        · exact absurd (of_decide_eq_true hlen2) (by omega)


/-! ### O1 campaign: O1ScanThreshold.lean (assembled) -/

set_option maxRecDepth 4096

/-- A color has a pending witness at or above the component threshold. -/
def ThresholdPending (P : Nat → Prop) (d c : Nat) (R : List Cand) : Prop :=
  (d : Int) ≤ (getR R c).len ∧ (getR R c).active = true ∧ P (getR R c).pos

/-- A suitable position has already been emitted. -/
def ThresholdHit (P : Nat → Prop) (out : List Nat) : Prop :=
  ∃ x, x ∈ out ∧ P x

/-- Before and during a component, a color's slot is below its threshold or
contains a witness from that component. -/
def ThresholdClean (P : Nat → Prop) (d c : Nat) (R : List Cand) : Prop :=
  (getR R c).len < (d : Int) ∨ ThresholdPending P d c R

theorem thresholdHit_eval (P : Nat → Prop) (w : Int) (R : List Cand)
    (out : List Nat) (h : ThresholdHit P out) :
    ThresholdHit P (evalStep w R out).1 := by
  obtain ⟨x, hx, hP⟩ := h
  exact ⟨x, (evalStep_mem w R out x).mpr (Or.inl hx), hP⟩

/-- Evaluation either emits the pending witness or preserves it exactly. -/
theorem thresholdPending_eval (P : Nat → Prop) (d c : Nat) (w : Int)
    (R : List Cand) (out : List Nat) (hc1 : 1 ≤ c) (hc2 : c < SIGMA)
    (hR : SIGMA ≤ R.length) (h : ThresholdPending P d c R) :
    ThresholdHit P (evalStep w R out).1 ∨
      ThresholdPending P d c (evalStep w R out).2 := by
  by_cases hw : w < (getR R c).len
  · exact Or.inl ⟨(getR R c).pos,
      (evalStep_mem w R out _).mpr (Or.inr ⟨c, hc1, hc2, hw, h.2.1, rfl⟩), h.2.2⟩
  · right
    unfold ThresholdPending
    rw [evalStep_getR w R out c hc1 hc2 hR, if_neg hw]
    exact h

/-- A below-threshold evaluation necessarily emits a pending witness. -/
theorem thresholdPending_emit (P : Nat → Prop) (d c : Nat) (w : Int)
    (R : List Cand) (out : List Nat) (hc1 : 1 ≤ c) (hc2 : c < SIGMA)
    (hw : w < (d : Int)) (h : ThresholdPending P d c R) :
    ThresholdHit P (evalStep w R out).1 := by
  exact ⟨(getR R c).pos,
    (evalStep_mem w R out _).mpr
      (Or.inr ⟨c, hc1, hc2, by have := h.1; omega, h.2.1, rfl⟩), h.2.2⟩

/-- An update cannot lose a pending witness: an overwrite uses a strictly
higher LCP and therefore another qualifying position. -/
theorem thresholdPending_upd (P : Nat → Prop) (d c q l pos : Nat)
    (R : List Cand) (hc : c < R.length)
    (hpos : q = c → d ≤ l → P pos) (h : ThresholdPending P d c R) :
    ThresholdPending P d c (upd R q l pos) := by
  by_cases hqc : q = c
  · subst q
    unfold ThresholdPending
    rw [upd_getR_self R c l pos hc]
    by_cases hl : (l : Int) > (getR R c).len
    · rw [if_pos hl]
      have hdl : d ≤ l := by have := h.1; omega
      exact ⟨by simpa using hdl, rfl, hpos rfl hdl⟩
    · rw [if_neg hl]
      exact h
  · unfold ThresholdPending
    rw [upd_getR_ne R q l pos c (Ne.symm hqc)]
    exact h

/-- Evaluations clamp every represented real-color slot to their level. -/
theorem threshold_eval_clamp (d c : Nat) (w : Int) (R : List Cand)
    (out : List Nat) (hc1 : 1 ≤ c) (hc2 : c < SIGMA)
    (hR : SIGMA ≤ R.length) (hw : w < (d : Int)) :
    (getR (evalStep w R out).2 c).len < (d : Int) := by
  rw [evalStep_getR w R out c hc1 hc2 hR]
  split <;> simp_all <;> omega

theorem thresholdClean_eval (P : Nat → Prop) (d c : Nat) (w : Int)
    (R : List Cand) (out : List Nat) (hc1 : 1 ≤ c) (hc2 : c < SIGMA)
    (hR : SIGMA ≤ R.length) (h : ThresholdClean P d c R) :
    ThresholdHit P (evalStep w R out).1 ∨
      ThresholdClean P d c (evalStep w R out).2 := by
  rcases h with h | h
  · right; left
    rw [evalStep_getR w R out c hc1 hc2 hR]
    split <;> simp_all <;> omega
  · rcases thresholdPending_eval P d c w R out hc1 hc2 hR h with h | h
    · exact Or.inl h
    · exact Or.inr (Or.inr h)

theorem thresholdClean_upd (P : Nat → Prop) (d c q l pos : Nat)
    (R : List Cand) (hc : c < R.length)
    (hpos : q = c → d ≤ l → P pos) (h : ThresholdClean P d c R) :
    ThresholdClean P d c (upd R q l pos) := by
  rcases h with h | h
  · by_cases hqc : q = c
    · subst q
      unfold ThresholdClean ThresholdPending
      rw [upd_getR_self R c l pos hc]
      by_cases hl : (l : Int) > (getR R c).len
      · rw [if_pos hl]
        by_cases hdl : d ≤ l
        · exact Or.inr ⟨by simpa using hdl, rfl, hpos rfl hdl⟩
        · exact Or.inl (by simpa using (Nat.lt_of_not_ge hdl))
      · rw [if_neg hl]
        exact Or.inl h
    · unfold ThresholdClean ThresholdPending
      rw [upd_getR_ne R q l pos c (Ne.symm hqc)]
      exact Or.inl h
  · exact Or.inr (thresholdPending_upd P d c q l pos R hc hpos h)

/-- A high-LCP participating edge arms the color once its slot is clean. -/
theorem thresholdClean_arm (P : Nat → Prop) (d c l pos : Nat)
    (R : List Cand) (hc : c < R.length) (hdl : d ≤ l) (hpos : P pos)
    (h : ThresholdClean P d c R) :
    ThresholdPending P d c (upd R c l pos) := by
  rcases h with h | h
  · unfold ThresholdPending
    rw [upd_getR_self R c l pos hc, if_pos (by omega)]
    exact ⟨by simpa using hdl, rfl, hpos⟩
  · exact thresholdPending_upd P d c c l pos R hc (fun _ _ => hpos) h

/-- Output membership is monotone throughout the scan. -/
theorem thresholdHit_scanAux (N : Nat) (P : Nat → Prop) (ts : List Triple)
    (p pSa : Nat) (m : Int) (R : List Cand) (out : List Nat)
    (h : ThresholdHit P out) : ThresholdHit P (scanAux N ts p pSa m R out) := by
  induction ts generalizing p pSa m R out with
  | nil => exact thresholdHit_eval P (-1) R out h
  | cons t rest ih =>
    by_cases hne : (t.c != p) = true
    · rw [scanAux_cons_bnd N t rest p pSa m R out hne]
      exact ih _ _ _ _ _ (thresholdHit_eval P _ R out h)
    · rw [scanAux_cons_nonbnd N t rest p pSa m R out (Bool.eq_false_iff.mpr hne)]
      exact ih _ _ _ _ _ h

/-- Once the running minimum falls below the threshold, a pending witness
must be emitted at the next run boundary, or by the final flush. -/
theorem thresholdPending_scanAux_low (N : Nat) (P : Nat → Prop) (d c : Nat)
    (ts : List Triple) (p pSa : Nat) (m : Int) (R : List Cand) (out : List Nat)
    (hc1 : 1 ≤ c) (hc2 : c < SIGMA) (hm : m < (d : Int))
    (h : ThresholdPending P d c R) :
    ThresholdHit P (scanAux N ts p pSa m R out) := by
  induction ts generalizing p pSa m R out with
  | nil => exact thresholdPending_emit P d c (-1) R out hc1 hc2 (by omega) h
  | cons t rest ih =>
    by_cases hne : (t.c != p) = true
    · rw [scanAux_cons_bnd N t rest p pSa m R out hne]
      apply thresholdHit_scanAux
      exact thresholdPending_emit P d c _ R out hc1 hc2
        (Int.lt_of_le_of_lt (Int.min_le_left _ _) hm) h
    · rw [scanAux_cons_nonbnd N t rest p pSa m R out (Bool.eq_false_iff.mpr hne)]
      exact ih _ _ _ R out (Int.lt_of_le_of_lt (Int.min_le_left _ _) hm) h

/-- The first row outside a component starts below the component threshold. -/
def ThresholdExit (d : Nat) : List Triple → Prop
  | [] => True
  | t :: _ => t.lcp < d

/-- Pending component witnesses survive every internal update and are
emitted on exit. No upper bound on the LCP values or MAXINT is needed. -/
theorem thresholdPending_scanAux_block (N : Nat) (P : Nat → Prop) (d c : Nat)
    (inside after : List Triple) (p pSa : Nat) (m : Int)
    (R : List Cand) (out : List Nat) (hc1 : 1 ≤ c) (hc2 : c < SIGMA)
    (hR : SIGMA ≤ R.length) (hp : p = c → P (N - pSa))
    (hinside : ∀ t ∈ inside, t.c = c → P (N - t.sa))
    (hexit : ThresholdExit d after) (h : ThresholdPending P d c R) :
    ThresholdHit P (scanAux N (inside ++ after) p pSa m R out) := by
  induction inside generalizing p pSa m R out with
  | nil =>
    cases after with
    | nil => exact thresholdPending_emit P d c (-1) R out hc1 hc2 (by omega) h
    | cons t rest =>
      have ht : (t.lcp : Int) < (d : Int) := by have : t.lcp < d := hexit; omega
      simp only [List.nil_append, scanAux]
      split
      · apply thresholdHit_scanAux
        exact thresholdPending_emit P d c _ R out hc1 hc2
          (Int.lt_of_le_of_lt (Int.min_le_right _ _) ht) h
      · exact thresholdPending_scanAux_low N P d c rest t.c t.sa _ R out hc1 hc2
          (Int.lt_of_le_of_lt (Int.min_le_right _ _) ht) h
  | cons t rest ih =>
    have ht := hinside t (by simp)
    have hrs : ∀ u ∈ rest, u.c = c → P (N - u.sa) := by
      intro u hu; exact hinside u (by simp [hu])
    simp only [List.cons_append, scanAux]
    split
    · rcases thresholdPending_eval P d c (min m t.lcp) R out hc1 hc2 hR h with he | he
      · exact thresholdHit_scanAux N P (rest ++ after) t.c t.sa MAXINT _ _ he
      · apply ih _ _ _ _ _ (by simpa [upd_len, evalStep_len] using hR) ht hrs
        apply thresholdPending_upd P d c t.c t.lcp (N - t.sa) _
          (by simpa [upd_len, evalStep_len] using Nat.lt_of_lt_of_le hc2 hR)
          (fun htc _ => ht htc)
        exact thresholdPending_upd P d c p t.lcp (N - pSa) _
          (by simpa [evalStep_len] using Nat.lt_of_lt_of_le hc2 hR)
          (fun hpc _ => hp hpc) he
    · exact ih _ _ _ R out hR ht hrs h

/-- An internal run edge involving the selected color. -/
def HasColorEdge (c : Nat) : Nat → List Triple → Prop
  | _, [] => False
  | p, t :: rest => (p ≠ t.c ∧ (p = c ∨ t.c = c)) ∨ HasColorEdge c t.c rest

/-- Every mixed component is hit when entered with a clean slot or with a
below-threshold running minimum. -/
theorem thresholdClean_scanAux_block (N : Nat) (P : Nat → Prop) (d c : Nat)
    (inside after : List Triple) (p pSa : Nat) (m : Int)
    (R : List Cand) (out : List Nat) (hc1 : 1 ≤ c) (hc2 : c < SIGMA)
    (hR : SIGMA ≤ R.length) (hp : p = c → P (N - pSa))
    (hinside : ∀ t ∈ inside, d ≤ t.lcp ∧ (t.c = c → P (N - t.sa)))
    (hexit : ThresholdExit d after)
    (hclean : ThresholdClean P d c R ∨ m < (d : Int))
    (hmix : HasColorEdge c p inside) :
    ThresholdHit P (scanAux N (inside ++ after) p pSa m R out) := by
  induction inside generalizing p pSa m R out with
  | nil => exact False.elim hmix
  | cons t rest ih =>
    have ht := hinside t (by simp)
    have hrs : ∀ u ∈ rest, d ≤ u.lcp ∧ (u.c = c → P (N - u.sa)) := by
      intro u hu; exact hinside u (by simp [hu])
    simp only [List.cons_append]
    by_cases hne : (t.c != p) = true
    · rw [scanAux_cons_bnd N t (rest ++ after) p pSa m R out hne]
      let E := evalStep (min m t.lcp) R out
      let R₂ := upd E.2 p t.lcp (N - pSa)
      let R₃ := upd R₂ t.c t.lcp (N - t.sa)
      have hElen : SIGMA ≤ E.2.length := by simpa [E, evalStep_len] using hR
      have h₂len : SIGMA ≤ R₂.length := by simpa [R₂, upd_len] using hElen
      have h₃len : SIGMA ≤ R₃.length := by simpa [R₃, upd_len] using h₂len
      have hEc : ThresholdHit P E.1 ∨ ThresholdClean P d c E.2 := by
        rcases hclean with hclean | hm
        · exact thresholdClean_eval P d c _ R out hc1 hc2 hR hclean
        · exact Or.inr (Or.inl (threshold_eval_clamp d c _ R out hc1 hc2 hR
            (Int.lt_of_le_of_lt (Int.min_le_left _ _) hm)))
      rcases hEc with hhit | hEc
      · exact thresholdHit_scanAux N P (rest ++ after) t.c t.sa MAXINT R₃ E.1 hhit
      · have h₂c : ThresholdClean P d c R₂ :=
          thresholdClean_upd P d c p t.lcp (N - pSa) E.2
            (Nat.lt_of_lt_of_le hc2 hElen) (fun hpc _ => hp hpc) hEc
        have h₃c : ThresholdClean P d c R₃ :=
          thresholdClean_upd P d c t.c t.lcp (N - t.sa) R₂
            (Nat.lt_of_lt_of_le hc2 h₂len) (fun htc _ => ht.2 htc) h₂c
        rcases hmix with hhere | hlater
        · have hpend : ThresholdPending P d c R₃ := by
            rcases hhere.2 with hpc | htc
            · have h₂p : ThresholdPending P d c R₂ := by
                unfold R₂
                rw [hpc]
                exact thresholdClean_arm P d c t.lcp (N - pSa) E.2
                  (Nat.lt_of_lt_of_le hc2 hElen) ht.1 (hp hpc) hEc
              exact thresholdPending_upd P d c t.c t.lcp (N - t.sa) R₂
                (Nat.lt_of_lt_of_le hc2 h₂len) (fun htc _ => ht.2 htc) h₂p
            · unfold R₃
              rw [htc]
              exact thresholdClean_arm P d c t.lcp (N - t.sa) R₂
                (Nat.lt_of_lt_of_le hc2 h₂len) ht.1 (ht.2 htc) h₂c
          exact thresholdPending_scanAux_block N P d c rest after t.c t.sa MAXINT R₃ E.1
            hc1 hc2 h₃len ht.2 (fun u hu => (hrs u hu).2) hexit hpend
        · exact ih _ _ _ R₃ E.1 h₃len ht.2 hrs (Or.inl h₃c) hlater
    · have heq : p = t.c := by
        have : t.c = p := by simpa using hne
        exact this.symm
      rw [scanAux_cons_nonbnd N t (rest ++ after) p pSa m R out
        (by simpa using heq.symm)]
      apply ih _ _ _ R out hR ht.2 hrs
      · rcases hclean with hclean | hm
        · exact Or.inl hclean
        · exact Or.inr (Int.lt_of_le_of_lt (Int.min_le_left _ _) hm)
      · rcases hmix with hhere | hlater
        · exact False.elim (hhere.1 heq)
        · exact hlater

/-- An indexed adjacent edge supplies the structural mixed-edge witness used
by the block theorem. -/
theorem hasColorEdge_of_index (c : Nat) (prev : Triple) (inside : List Triple)
    (k : Nat) (hk1 : 1 ≤ k) (hkl : k ≤ inside.length)
    (hne : ((prev :: inside).getD (k - 1) dT).c ≠
      ((prev :: inside).getD k dT).c)
    (hc : ((prev :: inside).getD (k - 1) dT).c = c ∨
      ((prev :: inside).getD k dT).c = c) :
    HasColorEdge c prev.c inside := by
  induction inside generalizing prev k with
  | nil => simp at hkl; omega
  | cons t rest ih =>
    cases k with
    | zero => omega
    | succ j =>
      cases j with
      | zero => exact Or.inl ⟨hne, hc⟩
      | succ j =>
        right
        apply ih t (j + 1) (by omega) (by simp only [List.length_cons] at hkl; omega)
        · simpa using hne
        · simpa using hc


/-! ### O1 campaign: O1ScanEntry.lean (assembled) -/

set_option maxRecDepth 2048

/-- Consume an arbitrary prefix without flushing the candidate table. -/
theorem scanAux_prefix_state (N : Nat) (before after : List Triple)
    (p pSa : Nat) (m : Int) (R : List Cand) (out : List Nat) :
    ∃ p' pSa' m' R' out', R'.length = R.length ∧
      scanAux N (before ++ after) p pSa m R out =
        scanAux N after p' pSa' m' R' out' := by
  induction before generalizing p pSa m R out with
  | nil => exact ⟨p, pSa, m, R, out, rfl, rfl⟩
  | cons t rest ih =>
    simp only [List.cons_append, scanAux]
    split
    · obtain ⟨p', pSa', m', R', out', hlen, heq⟩ :=
        ih t.c t.sa MAXINT
          (upd (upd (evalStep (min m t.lcp) R out).2 p t.lcp (N-pSa))
            t.c t.lcp (N-t.sa)) (evalStep (min m t.lcp) R out).1
      exact ⟨p', pSa', m', R', out', by simpa only [upd_len, evalStep_len] using hlen, heq⟩
    · exact ih t.c t.sa (min m t.lcp) R out

/-- A low-LCP entry clears the threshold invariant, even inside a BWT run. -/
theorem thresholdClean_scanAux_entry (N : Nat) (P : Nat → Prop) (d c : Nat)
    (first : Triple) (inside after : List Triple) (p pSa : Nat) (m : Int)
    (R : List Cand) (out : List Nat) (hc1 : 1 ≤ c) (hc2 : c < SIGMA)
    (hR : SIGMA ≤ R.length) (hfirst : first.lcp < d)
    (hp : first.c = c → P (N-first.sa))
    (hinside : ∀ t ∈ inside, d ≤ t.lcp ∧ (t.c = c → P (N-t.sa)))
    (hexit : ThresholdExit d after) (hmix : HasColorEdge c first.c inside) :
    ThresholdHit P (scanAux N (first :: (inside ++ after)) p pSa m R out) := by
  have hmin : min m (first.lcp : Int) < (d : Int) := by
    have hh := Int.min_le_right m (first.lcp : Int)
    omega
  by_cases hne : (first.c != p) = true
  · rw [scanAux_cons_bnd N first (inside ++ after) p pSa m R out hne]
    apply thresholdClean_scanAux_block N P d c inside after first.c first.sa MAXINT
      _ _ hc1 hc2 (by simpa only [upd_len, evalStep_len] using hR) hp hinside hexit
      (Or.inl ?_) hmix
    apply thresholdClean_upd P d c first.c first.lcp (N-first.sa) _
      (by simpa only [upd_len, evalStep_len] using Nat.lt_of_lt_of_le hc2 hR)
      (fun _ h => by omega)
    apply thresholdClean_upd P d c p first.lcp (N-pSa) _
      (by simpa only [evalStep_len] using Nat.lt_of_lt_of_le hc2 hR)
      (fun _ h => by omega)
    exact Or.inl (threshold_eval_clamp d c _ R out hc1 hc2 hR hmin)
  · rw [scanAux_cons_nonbnd N first (inside ++ after) p pSa m R out (Bool.eq_false_iff.mpr hne)]
    exact thresholdClean_scanAux_block N P d c inside after first.c first.sa
      _ R out hc1 hc2 hR hp hinside hexit (Or.inr hmin) hmix

/-- Every internally mixed LCP block emits a position of its participating color. -/
theorem scan_hits_block (N : Nat) (P : Nat → Prop) (d c : Nat)
    (before : List Triple) (first : Triple) (inside after : List Triple)
    (hc1 : 1 ≤ c) (hc2 : c < SIGMA)
    (hentry : before = [] ∨ first.lcp < d)
    (hp : first.c = c → P (N-first.sa))
    (hinside : ∀ t ∈ inside, d ≤ t.lcp ∧ (t.c = c → P (N-t.sa)))
    (hexit : ThresholdExit d after) (hmix : HasColorEdge c first.c inside) :
    ThresholdHit P (scan N (before ++ first :: (inside ++ after))) := by
  cases before with
  | nil =>
    simp only [List.nil_append, scan]
    apply thresholdClean_scanAux_block N P d c inside after first.c first.sa
      MAXINT defaultR [] hc1 hc2 (by rw [defaultR_len]; exact Nat.le_refl _) hp hinside hexit
      (Or.inl (Or.inl ?_)) hmix
    rw [defaultR_getR]
    simp only
    omega
  | cons t rest =>
    have hlow : first.lcp < d := by
      rcases hentry with h | h
      · simp at h
      · exact h
    simp only [List.cons_append, scan]
    obtain ⟨p', pSa', m', R', out', hlen, heq⟩ :=
      scanAux_prefix_state N rest (first :: (inside ++ after)) t.c t.sa MAXINT defaultR []
    rw [heq]
    exact thresholdClean_scanAux_entry N P d c first inside after p' pSa' m' R' out'
      hc1 hc2 (by rw [hlen, defaultR_len]; exact Nat.le_refl _) hlow hp hinside hexit hmix


/-! ### O1 campaign: O1ScanComponent.lean (assembled) -/

private theorem component_getD_drop (ts : List Triple) (a i : Nat) :
    (ts.drop a).getD i dT = ts.getD (a + i) dT := by
  simp only [List.getD_eq_getElem?_getD, List.getElem?_drop]

private theorem component_getD_take (ts : List Triple) (a i : Nat) (hi : i < a) :
    (ts.take a).getD i dT = ts.getD i dT := by
  simp only [List.getD_eq_getElem?_getD, List.getElem?_take, if_pos hi]

/-- Every color participating in a mixed LCP component has an emitted row
inside that component. LCP values need not be bounded by MAXINT. -/
theorem scan_hits_component (N : Nat) (ts : List Triple) (a b d c : Nat)
    (hab : a ≤ b) (hbn : b < ts.length) (hc1 : 1 ≤ c) (hc2 : c < SIGMA)
    (hin : ∀ j, a < j → j ≤ b → d ≤ (ts.getD j dT).lcp)
    (hleft : a = 0 ∨ (ts.getD a dT).lcp < d)
    (hright : b + 1 = ts.length ∨ (ts.getD (b + 1) dT).lcp < d)
    (hmix : ∃ k, a < k ∧ k ≤ b ∧
      (ts.getD (k - 1) dT).c ≠ (ts.getD k dT).c ∧
      ((ts.getD (k - 1) dT).c = c ∨ (ts.getD k dT).c = c)) :
    ∃ j, a ≤ j ∧ j ≤ b ∧ (ts.getD j dT).c = c ∧
      N - (ts.getD j dT).sa ∈ scan N ts := by
  let first := ts.getD a dT
  let before := ts.take a
  let inside := (ts.drop (a + 1)).take (b - a)
  let after := ts.drop (b + 1)
  let P := fun x => ∃ j, a ≤ j ∧ j ≤ b ∧ (ts.getD j dT).c = c ∧
    x = N - (ts.getD j dT).sa
  have hdropa : ts.drop a = first :: ts.drop (a + 1) := by
    rw [List.drop_eq_getElem_cons (by omega : a < ts.length)]
    congr 1
    simp only [first, List.getD_eq_getElem?_getD,
      List.getElem?_eq_getElem (by omega : a < ts.length), Option.getD_some]
  have hinner : inside ++ after = ts.drop (a + 1) := by
    have hh := List.take_append_drop (b - a) (ts.drop (a + 1))
    rw [List.drop_drop] at hh
    have hi : a + 1 + (b - a) = b + 1 := by omega
    simpa only [inside, after, hi] using hh
  have hsplit : before ++ first :: (inside ++ after) = ts := by
    rw [hinner, ← hdropa]
    exact List.take_append_drop a ts
  have hblock : first :: inside = (ts.drop a).take (b - a + 1) := by
    rw [hdropa]
    rfl
  have hrow (q : Nat) (hq : q ≤ b - a) :
      ((first :: inside).getD q dT) = ts.getD (a + q) dT := by
    rw [hblock, component_getD_take _ _ _ (by omega), component_getD_drop]
  have hlen : inside.length = b - a := by
    simp only [inside, List.length_take, List.length_drop]
    omega
  have hp : first.c = c → P (N - first.sa) := by
    intro h
    exact ⟨a, Nat.le_refl _, hab, h, rfl⟩
  have hinside : ∀ t ∈ inside, d ≤ t.lcp ∧ (t.c = c → P (N - t.sa)) := by
    intro t ht
    obtain ⟨i, hi, hit⟩ := List.mem_iff_getElem.mp ht
    have hib : i < b - a := by omega
    have heq : inside.getD i dT = t := by
      simp only [List.getD_eq_getElem?_getD, List.getElem?_eq_getElem hi,
        Option.getD_some, hit]
    have htrow : ts.getD (a + 1 + i) dT = t := by
      rw [← component_getD_drop ts (a + 1) i,
        ← component_getD_take (ts.drop (a + 1)) (b - a) i hib]
      exact heq
    constructor
    · rw [← htrow]
      exact hin _ (by omega) (by omega)
    · intro htc
      refine ⟨a + 1 + i, by omega, by omega, ?_, ?_⟩ <;> rw [htrow]
      exact htc
  have hexit : ThresholdExit d after := by
    rcases hright with hright | hright
    · have he : after = [] := by simp [after, hright]
      rw [he]; trivial
    · cases he : after with
      | nil => trivial
      | cons t rest =>
        change t.lcp < d
        have ht : ts.getD (b + 1) dT = t := by
          have hh := component_getD_drop ts (b + 1) 0
          change after.getD 0 dT = _ at hh
          simpa [he] using hh.symm
        simpa only [ht] using hright
  have hedge : HasColorEdge c first.c inside := by
    clear hp hinside hexit
    obtain ⟨k, hak, hkb, hne, hkc⟩ := hmix
    apply hasColorEdge_of_index c first inside (k - a) (by omega) (by omega)
    · rw [hrow (k - a - 1) (by omega), hrow (k - a) (by omega)]
      have he₁ : a + (k - a - 1) = k - 1 := by omega
      have he₂ : a + (k - a) = k := by omega
      simpa only [he₁, he₂] using hne
    · rw [hrow (k - a - 1) (by omega), hrow (k - a) (by omega)]
      have he₁ : a + (k - a - 1) = k - 1 := by omega
      have he₂ : a + (k - a) = k := by omega
      simpa only [he₁, he₂] using hkc
  have hentry : before = [] ∨ first.lcp < d := by
    rcases hleft with hleft | hleft
    · left; simp [before, hleft]
    · exact Or.inr hleft
  obtain ⟨x, hx, j, haj, hjb, hjc, hxj⟩ :=
    scan_hits_block N P d c before first inside after hc1 hc2 hentry hp hinside hexit hedge
  rw [hsplit, hxj] at hx
  exact ⟨j, haj, hjb, hjc, hx⟩


/-! ### O1 campaign: O1Covering.lean (assembled) -/

/-- The scan covers each requirement by hitting its mixed context interval. -/
theorem scan_covers_requirement (T : Text) (hT : positive T = true)
    (w : List Nat) (c : Nat) (hp : (w,c) ∈ requirements T) :
    ∃ x, x ∈ scan (T.length+1) (triplesOf T) ∧ coversAt (w ++ [c]) x T = true := by
  obtain ⟨k, hk1, hkn, hdiff, hprev, hcur, hcolor⟩ :=
    requirement_boundary T hT w c hp
  obtain ⟨a, b, hak, hkb, hbn, hinside, hall, hlcp, hleft, hright⟩ :=
    wRow_lcp_interval T w k hkn hcur
  have hprevbounds := hall (k-1) (by omega) hprev
  have hcExt := (mem_requirements w c T).mp hp |>.2.2
  have hocc := (mem_rightExts w c T).mp hcExt
  have hcT : c ∈ T := occurs_append_last w c T hocc
  have hc := positive_of_mem T hT hcT
  have hmix : ∃ j, a < j ∧ j ≤ b ∧
      ((triplesOf T).getD (j-1) dT).c ≠ ((triplesOf T).getD j dT).c ∧
      (((triplesOf T).getD (j-1) dT).c = c ∨ ((triplesOf T).getD j dT).c = c) := by
    refine ⟨k, by omega, hkb, ?_, ?_⟩
    · simpa only [dT, charRow_getD T (k-1) (by omega), charRow_getD T k hkn] using hdiff
    · simpa only [dT, charRow_getD T (k-1) (by omega), charRow_getD T k hkn] using hcolor
  obtain ⟨j, haj, hjb, hjc, hmem⟩ :=
    scan_hits_component (T.length+1) (triplesOf T) a b w.length c
      (by omega) hbn hc.1 hc.2 hlcp hleft hright hmix
  have hjn : j < (triplesOf T).length := by omega
  have hwpos : ∀ v ∈ w, 1 ≤ v := by
    obtain ⟨x, _, hx⟩ := exists_covers_of_occurs (w ++ [c]) T hocc (by simp)
    obtain ⟨q, hq⟩ := (coversAt_iff_suffix _ _ _).mp hx
    intro v hv
    have hvtake : v ∈ T.take x := by
      change v ∈ pref T x
      rw [hq]
      exact List.mem_append_right _ (List.mem_append_left _ hv)
    exact (positive_of_mem T hT (List.mem_of_mem_take hvtake)).1
  have hjc' : charRow T j = c := by
    simpa only [dT, charRow_getD T j hjn] using hjc
  have hpos : T.length+1 - ((triplesOf T).getD j dT).sa = rowPos T j := by
    rw [dT, saRow_getD T j hjn]
    rfl
  refine ⟨rowPos T j, by rwa [hpos] at hmem, ?_⟩
  exact coversAt_of_wRow T hT w c j hjn hwpos hc.1 (hinside j haj hjb) hjc'

/-- Unbounded covering: the machine threshold argument needs no LCP cap. -/
theorem covering_from_components (T : Text) (hT : positive T = true) :
    suffixient (scan (T.length+1) (triplesOf T)) T = true := by
  apply suffixient_of_witnesses
  intro p hp
  exact scan_covers_requirement T hT p.1 p.2 hp

/-- Every text position is dominated by an emitted position. -/
theorem scan_domination (T : Text) (hT : positive T = true)
    (x : Nat) (hx : x ∈ positionsT T) :
    ∃ y, y ∈ scan (T.length+1) (triplesOf T) ∧ ScopeLe T x y := by
  obtain ⟨p, hpcov, hmax⟩ := exists_maxW (covSet_ne_of_mem_positionsT T hx)
  obtain ⟨hpreq, hpc⟩ := (mem_covSet T x p).mp hpcov
  obtain ⟨y, hy, hpy⟩ := scan_covers_requirement T hT p.1 p.2 hpreq
  refine ⟨y, hy, ?_⟩
  intro q hq
  obtain ⟨hqreq, hqc⟩ := (mem_covSet T x q).mp hq
  have hlen : (q.1 ++ [q.2]).length ≤ (p.1 ++ [p.2]).length := by
    have hh := Nat.le_trans (wlen_le_maxW T x q hq) hmax
    simpa only [List.length_append, List.length_singleton, wlen] using hh
  exact (mem_covSet T y q).mpr
    ⟨hqreq, coversAt_of_suffix hpy (coversAt_suffix_of_coversAt hqc hpc hlen)⟩


/-- every requirement (w,c) is covered by some emitted position.
Requires `positive T` (see the domain-convention note above). -/
theorem covering_given_stream (T : Text) (hT : positive T = true) :
    suffixient (scan (T.length + 1) (triplesOf T)) T := by
  exact covering_from_components T hT

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
  · -- REDUCED (2026-10-02 lane): `minimality_lower_of_scan_classes` proves
    -- |scan| ≤ chi T from (O2) no duplicates, (O3) maximality, (O4) distinct
    -- classes; combined with the upper half (covering) this completes minimality
    -- via `minimality_of_scan_classes`.  Those are the open obligations (0
    -- counterexamples on 729 exhaustive + 320 random/binary/repetitive texts).
    --
    -- REMAINING (Lemma 34 tie-breaking / minima lower bound): |scan| ≤ chi T,
    -- i.e. no suffixient set of positions 1..T.length is smaller than the
    -- emitted one.  Requires the LCP-maxima characterisation; see the lane
    -- report for the precise missing statement and the FM event bridge.
    sorry
  · exact hle


end Sxgc
