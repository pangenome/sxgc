import SxgcBounds
open Sxgc SxgcBuild SxgcBuild.Parse SxgcBounds

/-! Executable spot checks, including strict-prefix recursion, short fuel,
empty/end positions, unequal offsets, periodic and duplicate-heavy parses.
The identity hash tests a lawful collision-free algebra; constant hashes
exercise rejection. These evals are tests, never proof axioms. -/
namespace SxgcBoundsEval

def windowParse (T : Text) (k : Nat) : Parse :=
  let chunks := ((List.range ((T.length+k-1)/k)).map
    (fun q => (T.drop (q*k)).take k)).filter (fun c => c ≠ [])
  let dict := chunks.foldl (fun d c => if c ∈ d then d else d ++ [c]) []
  ⟨dict, chunks.map (fun c => dict.idxOf c)⟩

def texts : List Text :=
  [[], [1], [1,1], [1,2], [2,1], [1,2,3], [1,2,1,2],
   [1,1,2,1,1,2], List.replicate 12 7, (List.range 12).map (· % 3)]

def parses : List Parse :=
  (texts.flatMap (fun t => [1,2,3,5].map (windowParse t))) ++
  [⟨[[1], [1,2], [2], [2,1]], [0,2,1,0,3,1]⟩,
   ⟨[[1], [1,1], [1,1,1]], [0,1,2,0,2,1]⟩]

def pairs (n : Nat) : List (Nat × Nat) :=
  (List.range (n+1)).flatMap (fun i => (List.range (n+1)).map (fun j => (i,j)))

def identityHash : HashOps (List Nat) := ⟨id, List.cons, fun x _ n => x.take n⟩
def collisionHash : HashOps Nat := ⟨fun _ => 0, fun _ _ => 0, fun _ _ _ => 0⟩

/-- A finite modular backend for executable algebra/instance checks only;
no number-theoretic property of this hash is added as an axiom. -/
def modularHash : HashOps Nat :=
  { hash := fun xs => xs.foldr (fun a h => (a+1+257*h) % 1000000007) 0
    prepend := fun a h => (a+1+257*h) % 1000000007
    cut := fun x y n => (x+1000000007 -
      ((257^n % 1000000007)*y % 1000000007)) % 1000000007 }

def assertGate (name : String) (ok : Bool) : IO Unit := do
  if !ok then throw (IO.userError (name ++ ": FAILED"))
  IO.println (name ++ ": GREEN")

#eval assertGate "parse fixtures wellformed" (parses.all (fun P =>
  P.ids.all (fun id => id < P.dict.length) &&
  P.dict.all (fun c => c ≠ []) && decide P.dict.Nodup))

#eval assertGate "reference erasure + step inequality (all position pairs, 4 fuels)"
  (parses.all (fun P =>
    (pairs P.parseText.length).all (fun q =>
      [0,1,2,P.parseText.length].all (fun f =>
        let r := viaRun idLCE P f q.1 q.2
        r.answer == textLCEvia P P.parseText f q.1 q.2 &&
        r.steps ≤ 8*(1+r.pieces)))))

#eval assertGate "correct answers at sufficient fuel"
  (parses.all (fun P => (pairs P.parseText.length).all (fun q =>
    (viaRun idLCE P P.parseText.length q.1 q.2).answer ==
      lcpOf (P.parseText.drop q.1) (P.parseText.drop q.2))))

#eval assertGate "sampled suffix/chunk algebra + fingerprint answer and steps"
  (parses.all (fun P => [1,2,3,5].all (fun τ =>
    (pairs P.count).all (fun q =>
      let x := P.ids.drop q.1
      let y := P.ids.drop q.2
      suffixHash identityHash P.ids τ q.1 == P.ids.drop q.1 &&
      chunkHash identityHash P.ids τ q.1 τ == (P.ids.drop q.1).take τ &&
      fpParseLCE identityHash τ P q.1 q.2 == some (idLCE P q.1 q.2) &&
      fpSteps identityHash τ P.count x y ≤ 12*(1+τ+idLCE P q.1 q.2)))))

#eval assertGate "hash-to-text substitution"
  (parses.all (fun P => [1,3].all (fun τ => (pairs P.parseText.length).all (fun q =>
    fpTextLCE identityHash τ P P.parseText.length q.1 q.2 ==
      textLCEvia P P.parseText P.parseText.length q.1 q.2))))

#eval assertGate "r-query total-work inequalities"
  (parses.all (fun P => [1,2,5].all (fun τ =>
    let qs := pairs P.count
    totalWork P P.parseText.length qs ≤
      16*(P.count+dictionarySize P+qs.length*τ+verificationSum P P.parseText.length qs) &&
    fpTotalWork identityHash τ P qs ≤
      24*(P.count+dictionarySize P+qs.length*τ+idVerificationSum P qs))))

#eval assertGate "two-sided rejection: overestimate, underestimate, earlier-block collision"
  (verifyLCE [1,2,3] [1,9,3] 2 == none &&
   verifyLCE [1,2,3] [1,9,3] 0 == none &&
   verifyLCE [1,2,3] [1,9,3] 1 == some 1 &&
   verifyLCE [1,2,3] [1,9,3] (fpGuess collisionHash 2 3 [1,2,3] [1,9,3]) == none)

#eval assertGate "collision-independent soundness and fallback (all fixtures)"
  (parses.all (fun P => (pairs P.count).all (fun q =>
    (match fpParseLCE collisionHash 2 P q.1 q.2 with
     | none => true
     | some n => n == idLCE P q.1 q.2) &&
    fpExtension collisionHash 2 P q.1 q.2 == idLCE P q.1 q.2)))

#eval assertGate "finite modular hash: local HashGood and sampled chunk algebra"
  (parses.all (fun P => (pairs P.count).all (fun q =>
    (List.range (P.count+1)).all (fun n =>
      let x := (P.ids.drop q.1).take n
      let y := (P.ids.drop q.2).take n
      (modularHash.hash x != modularHash.hash y || x == y) &&
      [1,2,3,5].all (fun τ =>
        chunkHash modularHash P.ids τ q.1 n == modularHash.hash x)))))

#eval assertGate "finite modular hash: exact queries and step bound"
  (parses.all (fun P => [1,2,3,5].all (fun τ => (pairs P.count).all (fun q =>
    fpParseLCE modularHash τ P q.1 q.2 == some (idLCE P q.1 q.2) &&
    fpSteps modularHash τ P.count (P.ids.drop q.1) (P.ids.drop q.2) ≤
      12*(1+τ+idLCE P q.1 q.2)))))

-- Concrete numerical spots: (text size, query, steps, l_i, proven RHS at τ=2).
#eval (parses.take 12).map (fun P =>
  let f := P.parseText.length
  (f, (0,1), textLCESteps P f 0 1, verificationLength P f 0 1,
   8*(1+2+verificationLength P f 0 1)))
#eval [0,1,2,5,12].map (fun n =>
  let x := List.replicate n 1
  (n, fpSteps identityHash 2 (n+1) x x, 12*(1+2+lcpOf x x)))

#eval assertGate "direct comparison counter and construction query log"
  (texts.all (fun T =>
    let R := T.reverse ++ [0]
    let P := windowParse R 2
    let qs := parseTripleQueries T
    qs.length == T.length &&
    qs.all (fun q => lcpProbes (R.drop q.1) (R.drop q.2) ==
      lcpOf (R.drop q.1) (R.drop q.2) + 1) &&
    parseTriplesLCEWork T P ≤ 16*(P.count+dictionarySize P+T.length*2+
      verificationSum P R.length qs)))

end SxgcBoundsEval
