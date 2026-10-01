import SxgcPhi
open Sxgc SxgcPhi

set_option maxRecDepth 100000
set_option maxHeartbeats 0

def phiBattery : List Text :=
  [[], [1], [1,1], [1,2], [2,1], [1,2,3], [3,2,1], [1,1,1,1],
   [1,2,1,2,1,2,1,2], (List.range 30).map (fun x => x%3+1),
   (List.range 30).map (fun x => x%2+1), List.replicate 100 7,
   (List.range 60).map (fun x => x%2+1),
   (List.range 60).map (fun x => x*x%7+1),
   (List.range 60).map (fun x => x%4+1)]

/-- Brute directed cycle edges (predecessor, successor), independent of rank. -/
def bruteEdges (R : Text) : List (Nat × Nat) :=
  let ord := saOrder R
  ord.zip (ord.drop 1 ++ ord.take 1)

def phiMismatch (R : Text) : Nat :=
  ((bruteEdges R).filter (fun (p,q) =>
    phi R q != p || phiInv R p != q || phiInv R (phi R p) != p)).length

def boundaryMismatch (R : Text) : Nat :=
  ((List.range R.length).filter (fun a =>
    a > 0 && bwtRow R (a-1) != bwtRow R a &&
    SxgcPhi.rowPos R a != phiInv R (SxgcPhi.rowPos R (a-1)))).length

-- Direct empty/singleton cases plus the sentinel-bearing text family.
#eval do
  let indexed : List Text := [[], [0]] ++ phiBattery.map (fun (T : Text) => T.reverse ++ [0])
  let mismatches := (indexed.map phiMismatch).foldl (·+·) 0
  let boundaries := (indexed.map boundaryMismatch).foldl (·+·) 0
  IO.println s!"PHI: {indexed.length} texts, {(indexed.map List.length).foldl (·+·) 0} positions, {mismatches} mismatches; run boundaries: {boundaries} mismatches"
  unless mismatches == 0 && boundaries == 0 do throw (IO.userError "PHI RED")

-- Singleton pieces form a sorted image index, useful executable oracle.
#eval do
  let bad := phiBattery.filter (fun (T : Text) =>
    let R := T.reverse ++ [0]
    let edges := bruteEdges R
    let es := (List.range R.length).map (fun q =>
      let p := ((edges.find? (fun (e : Nat × Nat) => e.1 == q)).getD (0,0)).2
      ImagePiece.mk q p 1)
    !(List.range R.length).all (fun q => imageLookup es q == some (phiInv R q)))
  IO.println s!"IMAGE LOOKUP: {phiBattery.length} texts, {bad.length} mismatches"
  unless bad.isEmpty do throw (IO.userError "IMAGE LOOKUP RED")
