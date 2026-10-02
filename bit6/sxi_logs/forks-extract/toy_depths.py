#!/usr/bin/env python3
"""Which depth does the cyclic scan emit each witness for? Compare with the
deepest right-maximal requirement covered at that witness (covSet)."""
import sys
sys.path.insert(0, '.')
from toy_explore import build, requirements_and_covsets, SEP


def scan_depths(n, sa, bwt, lcp, passes=4):
    SIGMA = 256
    R = {c: [-1, 0, False] for c in range(1, SIGMA)}
    prev_c, m = bwt[0], 1 << 62
    out = {}
    for p in range(passes):
        out = {}
        for i in range(n):
            t_c, t_sa, t_lcp = bwt[i], sa[i], lcp[i]
            m2 = min(m, t_lcp)
            if t_c != prev_c:
                for c in range(1, SIGMA):
                    cand = R[c]
                    if m2 < cand[0]:
                        if cand[2] and cand[1] not in out:
                            out[cand[1]] = cand[0]
                        R[c] = [m2, 0, False]
                def upd(c, l, pos):
                    if l > R[c][0]:
                        R[c] = [l, pos, True]
                if prev_c != 0:
                    upd(prev_c, t_lcp, (n - sa[(i - 1) % n]) % n)
                if t_c != 0:
                    upd(t_c, t_lcp, (n - t_sa) % n)
                m = 1 << 62
            else:
                m = m2
            prev_c = t_c
        for c in range(1, SIGMA):
            cand = R[c]
            if cand[2] and cand[1] not in out:
                out[cand[1]] = cand[0]
    return out


def main():
    records = [b"AACGAAAT", b"AGGAGCCT", b"AATCACCT"]
    S, R, n, sa, bwt, lcp, runs = build(records)
    reqs, covsets = requirements_and_covsets(S, n)
    depths = scan_depths(n, sa, bwt, lcp)
    print("witness -> scan depth | covSet depths (all requirements covered) | deepest")
    agree = 0
    for x in sorted(depths):
        cs = covsets.get(x, set())
        dseq = sorted({len(w) for (w, c) in cs})
        deepest = max(dseq) if dseq else None
        ok = depths[x] == deepest
        agree += ok
        print(f"  x={x:3d} scan_depth={depths[x]} cov_depths={dseq} deepest={deepest} {'MATCH' if ok else '*** MISMATCH'}")
    print(f"scan depth == deepest covered: {agree}/{len(depths)}")


if __name__ == "__main__":
    main()
