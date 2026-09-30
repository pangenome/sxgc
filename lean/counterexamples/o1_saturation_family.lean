import Sxgc

/-!
Modified-cap saturation evidence on an actual text stream. The fidelity
theorems identify this model with the original scan when the cap is MAXINT.
The concrete duplicate below uses cap 1, so it is not a refutation of a
statement about the original fixed MAXINT.
-/

namespace Sxgc.SaturationEvidence

def scanAuxCap (cap : Int) (N : Nat) :
    List Triple → Nat → Nat → Int → List Cand → List Nat → List Nat
  | [], _, _, _, R, out => (evalStep (-1) R out).1
  | t :: rest, p, pSa, m, R, out =>
    let m' := min m t.lcp
    if t.c != p then
      let (out', R') := evalStep m' R out
      let R'' := upd R' p t.lcp (N - pSa)
      let R''' := upd R'' t.c t.lcp (N - t.sa)
      scanAuxCap cap N rest t.c t.sa cap R''' out'
    else
      scanAuxCap cap N rest t.c t.sa m' R out

def scanCap (cap : Int) (N : Nat) (ts : List Triple) : List Nat :=
  match ts with
  | [] => []
  | t :: rest => scanAuxCap cap N rest t.c t.sa cap defaultR []

theorem scanAuxCap_MAXINT (N : Nat) (ts : List Triple)
    (p pSa : Nat) (m : Int) (R : List Cand) (out : List Nat) :
    scanAuxCap MAXINT N ts p pSa m R out = scanAux N ts p pSa m R out := by
  induction ts generalizing p pSa m R out with
  | nil => rfl
  | cons t rest ih => simp only [scanAuxCap, scanAux, ih]

theorem scanCap_MAXINT (N : Nat) (ts : List Triple) :
    scanCap MAXINT N ts = scan N ts := by
  cases ts with
  | nil => rfl
  | cons t rest => exact scanAuxCap_MAXINT N rest t.c t.sa MAXINT defaultR []

def exampleText : Text := [1, 1, 1, 2, 1, 1]

theorem example_positive : positive exampleText = true := by decide

set_option maxRecDepth 100000 in
set_option maxHeartbeats 2000000 in
theorem modified_cap_output :
    scanCap 1 (exampleText.length + 1) (triplesOf exampleText) = [3, 4, 4] := by
  decide

set_option maxRecDepth 100000 in
set_option maxHeartbeats 2000000 in
theorem example_chi : chi exampleText = 2 := by decide

theorem modified_cap_not_nodup :
    ¬ (scanCap 1 (exampleText.length + 1) (triplesOf exampleText)).Nodup := by
  rw [modified_cap_output]
  decide

theorem modified_cap_length_exceeds_chi :
    chi exampleText < (scanCap 1 (exampleText.length + 1) (triplesOf exampleText)).length := by
  rw [example_chi, modified_cap_output]
  decide

end Sxgc.SaturationEvidence
