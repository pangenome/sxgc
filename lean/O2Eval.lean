import SxgcBounds
open Sxgc
namespace SxgcBounds
/-- Same 3^6 generator as Sxgc.fmTexts: 729 entries, 127 distinct texts. -/
def o2Texts : Nat → List Text
  | 0 => [[]]
  | k+1 => let r := o2Texts k
           r ++ r.map (fun t => 1 :: t) ++ r.map (fun t => 2 :: t)
#eval ("O2 bounded warrant: entries, bounded, violations = ",
  (o2Texts 6).length,
  ((o2Texts 6).filter (fun T => (triplesOf T).all (fun t => t.lcp ≤ MAXINT.toNat))).length,
  ((o2Texts 6).filter (fun T =>
    (triplesOf T).all (fun t => t.lcp ≤ MAXINT.toNat) &&
    !decide (scan (T.length+1) (triplesOf T)).Nodup)).length)
#eval ("O2 family distinct texts = ",
  ((o2Texts 6).foldl (fun seen t => if t ∈ seen then seen else t :: seen) []).length)
#eval ("saturation witness sorted scan, Nodup = ",
  (scan 100 saturationWitness).mergeSort (· ≤ ·),
  decide (scan 100 saturationWitness).Nodup)
#eval do
  let ts := o2Texts 6
  unless ts.length == 729 && ts.all (fun T =>
      (triplesOf T).all (fun t => t.lcp ≤ MAXINT.toNat) &&
      decide (scan (T.length+1) (triplesOf T)).Nodup) do
    throw (IO.userError "O2 bounded warrant changed")
  IO.println "O2 BOUNDED STATEMENT-LOCK GATE: GREEN (finite evidence only)"
end SxgcBounds
