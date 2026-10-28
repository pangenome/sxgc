# The unique-sentinel simplification: tested and REFUTED (keep the seam machinery)

Hypothesis: the seam machinery (terminal-class discovery, `seamSplice`, repair reordering) exists
only to repair a cyclic-wrap artifact. If the corpus ended with a byte that occurs nowhere else,
the terminal class would have one member, the classes would vanish, and the whole subsystem could
be deleted (two corpus-wide scans, per-band class records, per-class sorts, the splice).

## Experiments (release binaries, `/tmp/psi_tiny.txt`, n = 454,752; ground truth = verified seed-first SA)

| corpus | terminal symbol | classes | result |
|---|---|---|---|
| as-is (ends `0x1e`, 101 occurrences) | `0x1e` (30) | 1 class, 101 rows | **SA == ground truth, byte-identical over all 454,752 entries** |
| + `0xFF` | 1 | — | refused: collides with the dollar symbol |
| + `0xF5` | 245 | **0** | runs, χ = 145,685 identical — but **SA differs from truth at rank 5615** |
| + `0x00` (absent ⇒ unique *and* minimal) | 0 | **0** | refused: `unsupported terminal byte` |

## What this proves

1. **The classes are exactly the terminal-byte occurrence chain.** Any unique terminal yields
   `depth=1, raw_classes=0, unique_targets=0` — the discovery work disappears by itself.
2. **Finite-order equivalence requires the sentinel be *minimal*, not merely unique.** With `0xF5`
   (symbol 245) the last separator's suffix becomes `[0x1e, 0xF5]`, sorting at symbol 245 deep
   inside the `0x1e` bucket instead of first. The divergence appears at **rank 5615** — precisely
   where the historical cyclic/seed-first discrepancy lived. The seam repair is what puts the
   "seed first" there; it is load-bearing and correct.
3. **A minimal sentinel is not available in the production product alphabet**: symbol 0 is NUL
   (present in the corpus), symbols 1–4 belong to the finish's dollar/pad machinery, and no symbol
   in `[6,32)` is free. Even if one were, the finish refuses a terminal symbol `< 6` by design.

## What it would cost, against what it would save (10 GB Route B)

```
XSA_FINISH_PHASE seam-classes wall = 0.000 s   of 2281.441 s finish
per-band seam rebuild              = 1.147 + 5.383 + ~0.007  ≈ 6.5 s   → 0.3 %
```

Deleting the machinery requires re-collating the product alphabet (evict NUL from symbol 0, relax
the terminal guard, rework `counts[2]`/m1 histogram equality) — a full convention change with new
baselines, a 10 GB re-bank, and a re-run of the 100 GB rung — to save **~0.3 % of the finish**.

**Verdict: not worth it. The seam machinery stays.**

## What the same measurement says about where the wall actually is

```
10 GB Route B finish: readiness-pfck 13.8 | fingerprints 0.003 | seam-classes 0.000 |
                      SWEEP 2196.2 | m5-ef 57.7 | m1-runs 12.8 | publish 0.99   → 2281.4 s
```

The sweep is **96 %** of the finish, and the finish dominates the wall. That is the key, not the seam.
