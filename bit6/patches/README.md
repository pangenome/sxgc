# Vendored patches (reproducibility record)

- `ropebwt3-vendored.patch`: apply inside `TeraTools/ropebwt3/` (untracked
  ropebwt3 checkout at r285). RB3_ASIZE 6->16 (+RLD_MAX_ASIZE 31), _DNA_ONLY
  disabled. Required for mixed-case (soft-masked) collections.
- `teratools-vendored.patch`: apply in the TeraTools root (tracked tree).
  rld_init block geometry: bbits must scale with alphabet (bbits=3 is exactly
  right for DNA-6 but overruns 8-word blocks at asize=16, silently corrupting
  the heap -> rld_rank_index spins over a garbage block count).
All three patches are gate-validated at tiny (ft30), mid (s200), and yeast
scale; see BIT_LADDER rung 4a.1.

- `teratools-vendored.patch` (tracked tree) also carries vendored patch #4
  (2026-09-21): TeraIndex.h charToBits N/T swap — buildRldFromRlbwt assigns
  codes in BYTE order (A=1,C=2,G=3,N=4,T=5) but charToBits mapped T=4,N=5.
  Corrupted matching statistics on any text containing N (HPRC N-runs!).
  Caught by the s200 MS gate: 9,722/10,000 lens mismatches -> 0 after fix;
  all pos values verified valid (suffix at pos matches read for len chars).
  Note: charToBits accepts only uppercase ACGTN — production human is fine;
  soft-masked collections need a generic pattern-alphabet map (future
  patch #5, yeast-scale MS gating blocked on it).
