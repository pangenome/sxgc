import SxgcPhi

-- Every theorem declared in SxgcPhi, including infrastructure.
#print axioms SxgcPhi.rank_lt
#print axioms SxgcPhi.rowPos_lt
#print axioms SxgcPhi.rowPos_rank
#print axioms SxgcPhi.rank_rowPos
#print axioms SxgcPhi.prevRow_lt
#print axioms SxgcPhi.nextRow_lt
#print axioms SxgcPhi.next_prev
#print axioms SxgcPhi.prev_next
#print axioms SxgcPhi.headFromTail
#print axioms SxgcPhi.runHeadFromTail
#print axioms SxgcPhi.phiInv_phi
#print axioms SxgcPhi.phi_phiInv
#print axioms SxgcPhi.headFromCyclicTail
#print axioms SxgcPhi.piece_inverse
#print axioms SxgcPhi.imageLookup_sound
#print axioms SxgcPhi.imageLookup_complete
#print axioms SxgcPhi.imageLookup_eq_phiInv
#print axioms SxgcPhi.extractHead_correct
#print axioms SxgcPhi.unmirror_tail

-- Criterion C section (gcd-one primitivity and the singleton-domain branch)
#print axioms SxgcPhi.foldl_gcd_dvd_acc
#print axioms SxgcPhi.dvd_foldl_gcd
#print axioms SxgcPhi.count_flatten_replicate
#print axioms SxgcPhi.isPower_dvd_byteFreqGcd
#print axioms SxgcPhi.gcdOne_not_isPower
#print axioms SxgcPhi.criterion_singleton_branch
