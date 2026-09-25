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


/-- every requirement (w,c) is covered by some emitted position.
Requires `positive T` (see the domain-convention note above). -/
theorem covering_given_stream (T : Text) (hT : positive T = true) :
    suffixient (scan (T.length + 1) (triplesOf T)) T := by
  apply suffixient_of_witnesses
  intro p hp
  -- REMAINING (Bit 1b, covering direction of the LCP-maxima characterisation):
  -- for every requirement p = (w,c), the one-pass scan emits some position x
  -- with `wc` a suffix of `T[0:x)`.  This is the core of Lemma 34.
  --
  -- FOUNDATION NOW IN PLACE (Lane L, Task 2b):
  --   * `take_eq_iff_le_lcpOf` — `lcpOf` is the longest common prefix, so the
  --     `lcp` field of `triplesOf T` is the true LCP of SA-adjacent suffixes;
  --   * `saOrder_pairwise` / `saOrder_sorted_getElem` — `saOrder` really is the
  --     lexicographic suffix order.
  -- REDUCED (2026-10-02 lane): `covering_of_domination` proves this from the
  -- domination property (O1) of the scan — see the "Reductions isolating the
  -- remaining obligations" block below.  The open obligation is (O1):
  --   ∀ x ∈ positionsT T, ∃ y ∈ scan …, ScopeLe T x y
  -- (0 counterexamples on 729 exhaustive + 320 random/binary/repetitive texts).
  --
  -- PRECISE REMAINING OBLIGATION: a "scan-covers-requirements" invariant over
  -- `scanAux`: for every requirement (w,c) occurring in `T`, some emitted
  -- position x has `w ++ [c]` as a suffix of `T.take x`.  The invariant must be
  -- maintained across `evalStep`/`upd` (the candidate table's LCP-maxima are
  -- exactly the right-maximal contexts of `T.reverse`), using the two facts
  -- above to line the stream up with the suffix order.
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

end Sxgc
