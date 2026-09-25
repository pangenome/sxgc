import Sxgc
open Sxgc

/-!
# The fm_equivalence saturation refutation (warrant for the RETIRED-FALSE
# record in Sxgc.lean, 2026-09-26 statement-lock event)

The unbounded `fm_equivalence` is REFUTED: strictly above MAXINT the
scan machine's running-min saturates and emits spurious DUPLICATE
positions while fmSpec's PSV/NSV arithmetic stays exact. The boundary
is sharp: at MAXINT exactly, and everywhere below, both machines agree.

Run: cd lean && lake env lean counterexamples/saturation_refutation.lean
-/

def tsAt : List Triple :=
  [⟨1, 9223372036854775807, 6⟩, ⟨2, 9223372036854775806, 5⟩, ⟨1, 9223372036854775805, 4⟩,
   ⟨2, 9223372036854775804, 3⟩, ⟨1, 9223372036854775803, 2⟩, ⟨2, 9223372036854775802, 1⟩]

def tsAbove : List Triple :=
  [⟨1, 9223372036854775807 + 4, 6⟩, ⟨2, 9223372036854775807 + 3, 5⟩, ⟨1, 9223372036854775807 + 2, 4⟩,
   ⟨2, 9223372036854775807 + 1, 3⟩, ⟨1, 9223372036854775807, 2⟩, ⟨2, 9223372036854775807 - 1, 1⟩]

#eval "at-MAXINT scan = " ++ toString ((scan 100 tsAt).mergeSort (· ≤ ·))
#eval "at-MAXINT fm   = " ++ toString ((fmSpec 100 tsAt).mergeSort (· ≤ ·))
#eval "above scan = " ++ toString ((scan 100 tsAbove).mergeSort (· ≤ ·))
#eval "above fm   = " ++ toString ((fmSpec 100 tsAbove).mergeSort (· ≤ ·))
-- Expected: at-MAXINT both [94, 95]; above: scan [94, 95, 95, 96, 96, 97]
-- vs fm [94, 95] — the divergence (supervisor-verified 2026-09-26).
