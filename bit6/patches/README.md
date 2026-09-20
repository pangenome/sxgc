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
