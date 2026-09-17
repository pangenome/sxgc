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

/-! ## Theorems (Bit 1b: proofs via the LCP-maxima characterization, Lemma 34) -/

/-- every requirement (w,c) is covered by some emitted position -/
theorem covering_given_stream (T : Text) :
    suffixient (scan (T.length + 1) (triplesOf T)) T := sorry

/-- the scan emits a *smallest* suffixient set (Lemma 34 tie-breaking) -/
theorem minimality (T : Text) :
    (scan (T.length + 1) (triplesOf T)).length = chi T := sorry

end Sxgc
