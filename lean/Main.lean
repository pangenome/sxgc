import Sxgc

/-! Executable brute-force verification: random small texts over {1,2}; the
one-pass scan's output must satisfy the covering predicate AND minimality. -/

open Sxgc

partial def nextRand (s : Nat) : Nat := (s * 1103515245 + 12345) % 2147483648

partial def randText (seed : Nat) (len : Nat) : Text × Nat :=
  let rec go (s : Nat) (k : Nat) (acc : List Nat) : Text × Nat :=
    if k = 0 then (acc.reverse, s)
    else let s' := nextRand s; go s' (k - 1) ((s' % 2 + 1) :: acc)
  go seed len []

partial def runTrials (n : Nat) (seed : Nat) (maxLen : Nat) : Nat × Nat :=
  if n = 0 then (0, 0)
  else
    let (T, s) := randText seed ((n % maxLen) + 1)
    let ts := triplesOf T
    let S := scan (T.length + 1) ts
    let cov := suffixient S T
    let min' := (S.length == chi T)
    let (p, f) := runTrials (n - 1) (nextRand s) maxLen
    (p + (if cov && min' then 1 else 0), f + (if cov && min' then 0 else 1))

def main : IO Unit := do
  let (pass, fail) := runTrials 10 42 6
  IO.println s!"suffixient-scan brute-force: {pass} pass, {fail} fail (10 random texts, |T| <= 6)"
  IO.println s!"checks per text: covering (Def. 9) AND minimality (|S| = chi)"
  if fail > 0 then throw (IO.userError "FAILURES — do not translate to Rust")
