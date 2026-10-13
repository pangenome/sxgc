import PiWall

namespace Sxgc.PiWall

/-- Enumerate permutations by inserting each new symbol at every position. -/
def smallPermutations : Nat → List (List Nat)
  | 0 => [[]]
  | n + 1 => (smallPermutations n).flatMap (fun xs =>
      (List.range (n + 1)).map (fun k => xs.take k ++ [n] ++ xs.drop k))

def rowsOf (π : List Nat) : List (Nat × Nat) :=
  (List.range π.length).map (fun i => (i, π.getD i 0))

/-- Independent brute specification: index of every rank in the input. -/
def bruteInverse (π : List Nat) : List Nat :=
  (List.range π.length).map π.idxOf

def endpointCheck (π : List Nat) : Bool :=
  let rows := rowsOf π
  scanRows π.length rows == bruteInverse π &&
    (scatterRows π.length rows).toList == bruteInverse π &&
    rows.length * π.length == scalarSteps π.length &&
    rows.length == denseSteps π.length

def repeatMotif (motif : List Nat) (copies : Nat) : List Nat :=
  (List.replicate copies motif).flatten

def periodicFixtures : List (List Nat × Nat) :=
  [[9], [2, 1], [3, 1, 2], [7, 1, 6, 2, 5, 3, 4]].flatMap (fun motif =>
    (List.range 6).map (fun k => (repeatMotif motif (k + 1), motif.length)))

def periodicCheck (fixture : List Nat × Nat) : Bool :=
  let (text, p) := fixture
  cyclicPeriod text == p && periodicOrder text p == referenceOrder text &&
    (List.range text.length).all (fun i =>
      (List.range text.length).all (fun j =>
        (cyclicRotation text i == cyclicRotation text j) == (i % p == j % p)))

/-- Binary texts of each exact length, including aperiodic and non-minimal
period candidates. Every nonempty binary text ≤9 is checked, not just motifs. -/
def binaryTexts : Nat → List (List Nat)
  | 0 => [[]]
  | n + 1 => (binaryTexts n).flatMap (fun xs => [0 :: xs, 1 :: xs])

def allBinary : List (List Nat) :=
  (List.range 9).flatMap (fun k => binaryTexts (k + 1))

#eval do
  let perms := (List.range 8).flatMap smallPermutations
  unless perms.length == 5914 && perms.all endpointCheck do
    throw (IO.userError "PI WALL inversion endpoint gate failed")
  IO.println s!"PI WALL ENDPOINT GATE: GREEN ({perms.length} permutations, n=0..7; scalar n^2, dense n)"
  unless periodicFixtures.length == 24 && periodicFixtures.all periodicCheck do
    throw (IO.userError "PI WALL periodic fixture gate failed")
  IO.println "PERIODIC FIXTURE GATE: GREEN (24 texts; periods 1/2/3/7, copies 1..6; ties and full order)"
  unless allBinary.length == 1022 && allBinary.all (fun text =>
      periodicCheck (text, cyclicPeriod text)) do
    throw (IO.userError "PI WALL exhaustive binary gate failed")
  IO.println "PERIODIC EXHAUSTIVE GATE: GREEN (1022 nonempty binary texts, n=1..9)"
  unless referenceOrder [2, 1, 2, 1] == [1, 3, 0, 2] &&
      periodicOrder [2, 1, 2, 1] 2 == [1, 3, 0, 2] &&
      runCount [] == 0 && runCount [1, 1, 2, 2, 1] == 3 do
    throw (IO.userError "PI WALL convention gate failed")
  IO.println "CONVENTION GATE: GREEN (ascending ties, linear BWT runs, p=n singleton classes)"

end Sxgc.PiWall
