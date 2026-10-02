# Cross-LCP pairwise rleBWT merge: implementation journal

Tool: `bit6/cross_lcp_merge.cpp` (built with
`c++ -O3 -std=c++17 -pthread bit6/cross_lcp_merge.cpp -o /tmp/cross_lcp_merge`).
All changes are unstaged; retained artifacts under `/home/erikg/sxgc*`,
`/tmp/rpfbwt-64-466` and `/mnt/nvme3n1` are read-only inputs.

## What is implemented

An exact pairwise merge of two SXCR cyclic-BWT chunks
(`bit6/chunk_frontend.cpp` format) into either

* a merged SXCR chunk holding the cyclic rotation order of the concatenated
  text (composable: a tree or serial fold of pairwise merges), or
* the final four files (`rlebwt/meta/ssa/ssa_t`) byte-identical to the banked
  batch-BCR output (`bit6/chunk_bcr_merge.cpp` / `bit6/bcr_frontend_v2.cpp`).

The merge never inserts text symbols into a dynamic BWT. Chunk B's runs are
placed among chunk A's runs through a total-order comparator on merged
rotation keys evaluated with bounded fingerprint LCE probes; every probe is
fully verified against the text (SLIM_FP discipline, `bit6/slim_lce.hpp`), and
every probe/verified symbol is journaled.

## Theory (all statements checked by `--selftest` brute force and the gates)

1. The final four files describe the padded cyclic BWT of `P = M . 0x02^10`
   (`M = A . B`): rows are the ten padding rotations `n..n+9` followed by the
   strict `$`-suffix order of `M`. Head/tail samples are rotation indices.
   Verified against the banked merged bytes (first runs `[0x1e:1][0x02:9]`,
   head samples `n, n+1`, tails `n, n+9`, `counts[2]=10`).
2. For two rows of one chunk, the local cyclic comparison and the merged
   comparison read identical characters until the end of the shorter local
   suffix. Orders can differ only for prefix pairs: pairs where the shorter
   chunk suffix `S_q` also occurs at the longer suffix's position. The short
   side of every prefix pair is an anchor; its occurrence block is found by
   the classic backward-search step over the chunk's own BWT, walking suffixes
   leftward from the last position until the block drops below two members
   (the backward step never enlarges an interval).
3. The merged order equals the local order with every anchor block's member
   sequence sorted in place by merged key: real-prefix pairs always share the
   short side's block; pairs sharing a block end up merged-ordered; pairs
   sharing no block are never prefix pairs; prefix blocks nest or are
   disjoint, so the in-place interval sorts commute and are idempotent.
4. Cross placement is a two-pointer linear merge of the two repaired orders
   under the same comparator. Chunk runs therefore "split" exactly as needed:
   no stable row permutation or whole-run interleave is assumed.
5. Periodic chunks break the classic LF formula inside the tie class that
   wraps position 0 (the banked BCR merger survives because it only recovers
   text, where emitted characters depend only on the step index modulo the
   period). This tool recovers text the same safe way, then:
   aperiodic chunks recover the full row->position map by a multi-seed LF
   walk; periodic chunks (minimal cyclic period `d < n`) write the order
   directly from the sorted residue-class streams, and their anchor blocks
   are the class runs whose infinite keys start with the suffix string
   (`L >= d`: exactly the suffix's own class; short tail suffixes: the
   contiguous run of classes whose streams start with the tail).

## Cost model

Each merge costs O(positions) comparator calls; each call has a 16-symbol
directly-checked fast path, then O(log LCE) hash probes plus full direct
verification of the proposed prefix and its mismatch boundary (the journaled
`symbols_compared`). Dense prefix fingerprints over `M.M` give O(1) probe
access (16(n+1) bytes per merge; the measurement lane's design lock permits
dense access explicitly in its cost model). Anchor discovery is a handful of
rank queries on real text (4-5 walk steps per Pile chunk, blocks ~0.02% of n).
A bounded-approximation mode does not exist: hash/verification disagreement
aborts the merge (`CROSS_LCP_FATAL`), so every emitted byte is certified.

Honest worst-case statements: comparisons are per-position (not per-run),
because chunk runs genuinely split and interleave in the merged order; the
amortized work per comparison is O(1)+small. Periodic inputs make every
position an anchor (repairs are then full-block sorts); tiny fixtures cover
this, but a large fully-periodic input would be expensive - the tool stays
exact, only slower. Full-LCE fallbacks (LCE >= 10k) are counted per merge
(`lce_over_10k` etc. in the CROSS_PAIR rows); on the 0->1 Pile pair the
observed maximum LCE is 33,999 symbols, matching the banked measurement lane
exactly.

## Gates

* (a) SMALL: `test_cross_lcp_merge.py` - 26+ synthetic collections (periodic
  `aaaa\x1e`/`abc\x1e`-style texts, minimal alphabets, 1..26 chunks), tree,
  serial, `--pair`, `--finalize`, and clobber-refusal; four-file byte identity
  against the direct BCR front end on every collection, plus pairwise
  intermediate identity against the single-chunk front end on the same
  concatenation. `--selftest` additionally brute-forces the merged cyclic and
  `$` orders for ~100k random tiny two-chunk cases. **PASS** (all modes).
* (b) FULL: **PASS** on the 16 banked Pile fragment chunks
  (1,082,130,213 bytes): tree and serial schedules both produce four files
  byte-identical to the banked BCR output (SHA-256 match), `fresh.ri4`,
  `fresh.head_sa`, `fresh.agg` and `fresh.sA` all exactly the banked sizes,
  and `chi = 306164765 (N=1082130213, R=397723010)` with
  `fresh.sA` = 2,449,318,120 bytes.
* (c) COST: `COST_TABLE.md` from the journaled CROSS_PAIR rows vs the banked
  batch-BCR walls (20,599.00 s serial, 19,596.80 s tree). **PASS**:
  tree total merge wall 1,331.4 s (**14.7x** vs banked BCR tree), serial fold
  2,991.9 s (**6.9x** vs banked BCR serial); exactly 1.00 comparisons per
  text symbol, 17.9 verified symbols per symbol (tree), 2.1 hash probes per
  comparison; 3-135 anchors per chunk side (anchor repairs are negligible);
  max observed LCE 97,059 (banked measurement-lane max for this fragment:
  96,914 at the 5->6 boundary - same order).

## Files

- `merge-tree.log/.err/.time`  - tree merge journal (per-pair rows + phases)
- `merge-serial.log/.err/.time` - serial fold journal
- `four-file-gate.log`, `serial-four-file-gate.log`, `merged-hashes.txt`
- `merged-endpoints.*`, `merged-slim.*`, `merged-chi.*`, `merged-chi.combined`,
  `chi-gate.log`
- `COST_TABLE.md`, `make_cost_table.py`, `run_full_gate.sh`
- `test_cross_lcp_merge.py` (gate (a)), `no-staged-files.log`

## Run artifacts (outside the worktree, ~173 GB, kept for review)

`/home/erikg/cross-lcp-run/` holds the merged four files (byte-identical to
the banked ones), the tree/serial work directories with intermediate SXCR
chunks + sidecars, and the finish-sequence outputs (`fresh.ri4`,
`fresh.head_sa`, `fresh.agg`, `fresh.sA`). No process is left running.
