import LowerBound

-- Axiom audit for every theorem declared in LowerBound (mirrors
-- PhiAxioms.lean / BoundsAxioms.lean).  Flagship theorems admit no
-- native_decide; axioms must be propext / Classical.choice / Quot.sound
-- at most (expect: entirely axiom-free — the scaffold is constructive).

#print axioms Sxgc.LowerBound.le_of_coversAt
#print axioms Sxgc.LowerBound.emitted_suffixient
#print axioms Sxgc.LowerBound.chi_le_of_oracle
#print axioms Sxgc.LowerBound.nodup_length_le_of_subset'
#print axioms Sxgc.LowerBound.nodup_map_of_injOn
#print axioms Sxgc.LowerBound.finite_choice_index
#print axioms Sxgc.LowerBound.length_boolListsExact
#print axioms Sxgc.LowerBound.mem_boolListsExact
#print axioms Sxgc.LowerBound.length_allShort
#print axioms Sxgc.LowerBound.mem_allShort
#print axioms Sxgc.LowerBound.family_counting
#print axioms Sxgc.LowerBound.family_counting_lt
#print axioms Sxgc.LowerBound.mem_validCovers
#print axioms Sxgc.LowerBound.occurs_mem_validCovers
#print axioms Sxgc.LowerBound.fFirst_correct
#print axioms Sxgc.LowerBound.forced_unique
#print axioms Sxgc.LowerBound.incompat_of_forced
