#!/usr/bin/env python3
"""endpoint_measure.py — TASK 1: measure the endpoint hypothesis directly.

For the sweep's exact range-min we need, per class block b, its min LCP
(for the skip tree) — the ENDPOINT RULE claims min(block slice) is
attained at one of the block's two endpoint rows, giving O(1) per block
(two resolves) and NO O(n) slice materialization.  Measured here:

  A  per-block:      min(LCP[lo..hi-1]) attained at lo or hi-1?
  B  per-query part: for each boundary row i and each block b strictly
     between the query row and its brute answer, min over b's part of
     the range attained at that part's endpoints?  Two directions
     matter: DANGEROUS (true-min < tau but both endpoint values >= tau
     -> the rule skips a block it must enter) vs HARMLESS (extra work).
  |M| vs r vs n per text (the space the rule would cost).

All values from the brute LCP array (this is a measurement, not the
construction)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) or ".")
from endpoint_common import TextCase, battery


def measure(name, T):
    tc = TextCase(name, T, workdir="/tmp/endpoint")
    N, LCP, blocks = tc.N, tc.LCP, tc.blocks
    r = len(tc.runs)

    # ---------- A: per-block endpoint min ----------
    blocks_ge2 = 0
    A_viol = 0
    A_examples = []
    for (lo, hi) in blocks:
        if hi - lo < 2:
            continue
        blocks_ge2 += 1
        sl = [LCP[j] for j in range(lo, hi)]
        mn = min(sl)
        if sl[0] != mn and sl[-1] != mn:
            A_viol += 1
            if len(A_examples) < 4:
                # anatomy: offset of the argmin from the block ends
                am = sl.index(mn)
                A_examples.append((lo, hi, am, hi - lo - 1 - am, mn, sl[0], sl[-1]))
    # ---------- B: per-query ranges ----------
    bnd = [i for i in range(1, N) if tc.BWT[i] != tc.BWT[i - 1]]
    B_parts = 0
    B_viol = 0
    B_danger = 0
    B_examples = []
    for i in bnd:
        tau = LCP[i]
        for direction, rng in (("PSV", (max(0, tc.psvL[i]) + 1, i)),
                               ("NSV", (i + 1, min(N, tc.nsvL[i])))):
            a, b = rng
            if b <= a:
                continue
            # blocks strictly inside (a, b): all blocks overlapping [a, b)
            ba = tc.block_of[a] if a < N else len(blocks) - 1
            bb = tc.block_of[b - 1]
            for bi in range(ba, bb + 1):
                blo, bhi = blocks[bi]
                p1 = max(blo, a)
                p2 = min(bhi, b)   # exclusive
                if p2 - p1 < 2:
                    continue       # endpoint trivial
                # skip the block containing the answer itself (it is entered
                # regardless); the answer block = block of the true answer
                B_parts += 1
                sl = [LCP[j] for j in range(p1, p2)]
                mn = min(sl)
                if sl[0] != mn and sl[-1] != mn:
                    B_viol += 1
                    if mn < tau and sl[0] >= tau and sl[-1] >= tau:
                        B_danger += 1
                    if len(B_examples) < 5:
                        am = p1 + sl.index(mn)
                        B_examples.append((direction, i, tau, p1, p2, am,
                                            am - p1, p2 - 1 - am, mn, sl[0], sl[-1]))
    sizes = [hi - lo for (lo, hi) in blocks]
    big = max(sizes)
    print(f"{name:16s} n={N:7d} r={r:6d} |M|={len(tc.R.M):6d} blocks={len(blocks):6d} "
          f"maxblk={big:6d}")
    print(f"    A: blocks>=2 {blocks_ge2:6d}  endpoint-min violations {A_viol:6d} "
          f"({100*A_viol/max(blocks_ge2,1):5.2f}%)")
    for z in A_examples:
        print(f"      A-viol block rows[{z[0]},{z[1]}) argmin@+{z[2]}/-{z[3]} "
              f"mn={z[4]} ends=({z[5]},{z[6]})")
    print(f"    B: query-parts {B_parts:7d}  endpoint-min violations {B_viol:7d} "
          f"({100*B_viol/max(B_parts,1):5.2f}%)  DANGEROUS (would skip a needed "
          f"block): {B_danger}")
    for z in B_examples:
        print(f"      B-viol {z[0]} q={z[1]} tau={z[2]} part[{z[3]},{z[4]}) argmin row={z[5]} "
              f"@+{z[6]}/-{z[7]} mn={z[8]} ends=({z[9]},{z[10]})")
    return dict(A_viol=A_viol, A_tot=blocks_ge2, B_viol=B_viol, B_parts=B_parts,
                B_danger=B_danger)


if __name__ == "__main__":
    only = sys.argv[1:] or None
    for name, T in battery():
        if only and name not in only:
            continue
        measure(name, T)
    print("MEASURE DONE")
