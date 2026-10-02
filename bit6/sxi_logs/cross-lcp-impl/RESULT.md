# Cross-LCP merge: final result (all gates pass)

Tool: `bit6/cross_lcp_merge.cpp` - exact pairwise rleBWT merge of SXCR
cyclic-BWT chunks. Bounded fingerprint LCE comparisons (SLIM_FP discipline:
every hash proposal is fully verified against the text; disagreement aborts),
anchor-block order repair for cyclic cross-boundary order changes, linear
two-pointer cross merge, four-file / SXCR emitters matching the banked
producers byte for byte.

## Gate (a) SMALL - PASS
- `test_cross_lcp_merge.py`: 26 collections x feasible chunk counts (1..26),
  tree + serial + `--pair` + `--finalize`, four-file byte identity vs the
  direct BCR front end everywhere; pairwise cyclic intermediates byte-equal
  the single-chunk front end on the same concatenation; clobber refusal.
- `--selftest`: >150k random tiny two-chunk cases (periodic, all-equal,
  minimal alphabets, forced repeating suffixes) brute-force-checked in both
  cyclic and $ modes.

## Gate (b) FULL 16-chunk sequence - PASS
- Tree merge (22:15 wall, peak RSS 27.5 GB / 200 GB budget):
  `.rlebwt/.meta/.ssa/.ssa_t` byte-identical to the banked batch-BCR output;
  SHA-256 equal on all four files (see `merged-hashes.txt`).
- Serial fold (50:01 wall): the same four files byte-identical again
  (independent merge schedule).
- Banked finish sequence on the tree output: `fresh.ri4` 3,527,979,803 B,
  `fresh.head_sa` 3,181,784,080 B, `fresh.agg` 12,727,136,332 B,
  `fresh.sA` 2,449,318,120 B - all exactly the banked sizes - and
  `xsa chi-rspace: chi = 306164765 (N=1082130213, R=397723010)`.

## Gate (c) COST - PASS (see COST_TABLE.md)
- Tree: 1,331.4 s of pairwise merge walls vs banked 19,596.80 s -> 14.7x.
- Serial: 2,991.9 s vs banked 20,599.00 s -> 6.9x.
- Exactly 1.00 comparisons per text symbol; 17.9 verified symbols per symbol
  (tree total 77.4 G symbol reads); 2.1 hash probes per comparison;
  anchors 3..135 per chunk side; max LCE 97,059 (measurement-lane banked max
  for this fragment: 96,914 - same order).
- No per-character insertion anywhere; output run count 397,723,016 exactly
  as banked.

## Honesty notes
- The merge is exact, not bounded-approximate: hash proposals are always
  fully verified, and a disagreement aborts (this fired during development and
  caught two real parallel-hash-build bugs instead of emitting wrong bytes).
- Comparisons are per-position (one per text symbol), not per-run: chunk runs
  genuinely split and interleave in the merged order, so a strictly
  run-granular merge cannot emit the exact run structure. The per-position
  work is O(1) amortized (16-symbol checked fast path + ~2.1 hash probes),
  which is what delivers the 14.7x/6.9x walls over per-character insertion.
- Periodic chunks: every position is an anchor; tiny fixtures cover this, a
  large fully-periodic input would be slower (still exact).
- No long-run processes remain. Run artifacts (~173 GB) are under
  /home/erikg/cross-lcp-run (kept for review; deletable).
