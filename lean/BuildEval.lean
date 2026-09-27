import SxgcBuild
import Sxgc
open Sxgc SxgcBuild

/-! ### Section 5: differential battery (`#eval`) — Rust-oracle weld gate -/

/-- eval-only parse builder: fixed-width `k` phrases, dictionary = distinct
chunks in first-use order, ids via `idxOf`. -/
def parseBuild (T : Text) (k : Nat) : Parse :=
  let chunks := ((List.range ((T.length + k - 1) / k)).map
    (fun q => (T.drop (q * k)).take k)).filter (fun c => c ≠ [])
  let dict := chunks.foldl (fun d c => if c ∈ d then d else d ++ [c]) []
  let ids := chunks.map (fun c => dict.idxOf c)
  ⟨dict, ids⟩

/-- battery texts: empty, singletons, swaps, alternating, ascending/descending,
scattered quadratic residues, and the MANDATORY duplicates-heavy text
(period-1 at length 100), periodic `abab` at 60, mod-cycles at 60. -/
def batTexts : List Text :=
  [ [], [1], [1,1], [1,2], [2,1], [1,2,3], [3,2,1], [1,1,1,1],
    [1,2,1,2,1,2,1,2],
    (List.range 30).map (· % 3),
    (List.range 30).map (· % 2),
    (List.range 100).map (fun _ => 7),          -- duplicates-heavy (period 1)
    (List.range 60).map (fun x => if x % 2 == 0 then 1 else 2),  -- abab (period 2)
    (List.range 60).map (fun x => x * x % 7),
    (List.range 60).map (· % 4) ]

def tripleEq (a b : Triple) : Bool :=
  a.c == b.c && a.lcp == b.lcp && a.sa == b.sa
def triplesEq (xs ys : List Triple) : Bool :=
  xs.length == ys.length && (xs.zipWith tripleEq ys).all id

-- GATE 1: parse-based triples == production oracle, for k ∈ {2,3,5} (45 runs)
#eval batTexts.all (fun T =>
  [2,3,5].all (fun k =>
    triplesEq (parseTriplesOf T (parseBuild (T.reverse ++ [0]) k))
              (triplesOf T)))

-- GATE 2: parse-based scan == production scan (the χ-construction weld)
#eval batTexts.all (fun T =>
  [2,3,5].all (fun k =>
    scan (T.length+1) (parseTriplesOf T (parseBuild (T.reverse ++ [0]) k))
      == scan (T.length+1) (triplesOf T)))

-- GATE 3: parse-built |χ| == the χ oracle on the small texts
-- (`chi` is the powerset minimality oracle, O(2^n) by definition — it is
-- only computable on tiny texts; at scale the oracle is the Rust/sA side)
#eval (batTexts.take 9).all (fun T =>
  (scan (T.length+1) (parseTriplesOf T (parseBuild (T.reverse ++ [0]) 3))).length
    == chi T)

-- GATE 4 (negative control — the evals above are not vacuous):
-- a parse of the WRONG text must NOT reproduce the stream
#eval triplesEq (parseTriplesOf [1,2,3,4] (parseBuild ([9,9,9,9].reverse ++ [0]) 2))
               (triplesOf [1,2,3,4])
