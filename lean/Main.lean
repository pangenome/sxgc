import Sxgc

/-! Bit 1b gate: EXHAUSTIVE verification over all texts over {1,2}, |T| <= 8.
Covering AND minimality must hold for every text — 0 failures. -/

open Sxgc

partial def dedupT : List Text → List Text
  | [] => []
  | t :: rest => t :: dedupT (rest.filter (· != t))

partial def allTexts (maxLen : Nat) : List Text :=
  let rec go : Nat → List Text
    | 0 => [[]]
    | k + 1 => let r := go k
               r ++ r.map (fun t => 1 :: t) ++ r.map (fun t => 2 :: t)
  dedupT (go maxLen)

partial def exhaustive (ts : List Text) (pass fail : Nat) : Nat × Nat :=
  match ts with
  | [] => (pass, fail)
  | T :: rest =>
    let S := scan (T.length + 1) (triplesOf T)
    let ok := suffixient S T && (S.length == chi T)
    exhaustive rest (pass + (if ok then 1 else 0)) (fail + (if ok then 0 else 1))

def main : IO Unit := do
  let ts := allTexts 8
  IO.println ("exhaustive verification: " ++ toString ts.length ++ " texts over {1,2}, |T| <= 8")
  let (pass, fail) := exhaustive ts 0 0
  IO.println ("covering + minimality: " ++ toString pass ++ " pass, " ++ toString fail ++ " fail")
  if fail > 0 then throw (IO.userError "EXHAUSTIVE FAILURES — do not translate to Rust")
  IO.println "BIT 1B GATE: GREEN"
