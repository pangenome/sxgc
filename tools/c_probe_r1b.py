#!/usr/bin/env python3
"""c_probe_r1b.py — R1 refined: how far from piece boundaries do closure
rows live, and do window candidates (rows whose position is within d of a
piece boundary) preserve boundary-row PSV/NSV and/or chi?

Also tests the weaker-but-sufficient target: preserve the FM SWEEP RESULT
(chi), not necessarily every PSV/NSV answer.
"""
import sys
import time

sys.path.insert(0, "tools")
from c_probe_common import battery_texts, build
from scatter_probe import sv

sys.path.insert(0, "tools")
from c_probe_r1 import psv_nsv_on, check_set


def sweep_from_restricted(S, C):
    """run the v3 FM sweep using PSV/NSV restricted to candidate set C."""
    N, LCP, BWT, sa = S["N"], S["LCP"], S["BWT"], S["sa"]
    bnd = [i for i in range(1, N) if BWT[i] != BWT[i - 1]]
    rp, rn = psv_nsv_on(sorted(C), LCP, N)
    sigma = max(BWT) + 1
    R = [(-2, 0, False, 2**62)] * sigma
    out = set()
    for i in bnd:
        psv = rp.get(i, -1)
        nsv = rn.get(i, N)
        for ip in (i - 1, i):
            c = BWT[ip]
            if c != 0:
                if R[c][0] <= psv:
                    if R[c][3] < i:
                        out.add(R[c][1])
                    R[c] = (i, N - sa[ip], True, nsv)
    for c in range(1, sigma):
        if R[c][2]:
            out.add(R[c][1])
    return len(out)


def main():
    texts = battery_texts()
    print(f"{'text':16s} {'n':>7s} {'r':>7s} {'iv':>6s} {'chi':>7s} | closure row-position distance to piece boundary: "
          f"p50 p90 p99 max | window candidates d -> (badPSV,badNSV, chi-ok, |C|/r)")
    for name, x in texts:
        t0 = time.time()
        S = build(x)
        N, LCP, sa, BWT, isa = S["N"], S["LCP"], S["sa"], S["BWT"], S["isa"]
        r = len(S["runs"])
        iv = S["n_pieces"]
        bnd = [i for i in range(1, N) if BWT[i] != BWT[i - 1]]
        full_psv, full_nsv = sv(LCP)
        # ground-truth chi
        sys.path.insert(0, "tools")
        from c_probe_common import chi_full
        chi_true = chi_full(S)
        # closure + distances
        clos = set()
        for i in bnd:
            if full_psv[i] >= 0:
                clos.add(full_psv[i])
            if full_nsv[i] < N:
                clos.add(full_nsv[i])
        # distance of a POSITION to nearest piece boundary
        def posdist(p):
            pid = S["piece_id"][p]
            return min(p - S["piece_start"][pid], S["piece_end"][pid] - p)
        dists = sorted(posdist(sa[j]) for j in clos)
        q = lambda f: dists[min(len(dists) - 1, int(f * len(dists)))] if dists else 0
        line = (f"{name:16s} {N:7d} {r:7d} {iv:6d} {chi_true:7d} | "
                f"p50={q(.5)} p90={q(.9)} p99={q(.99)} max={dists[-1] if dists else 0} |")
        # window candidate sets by distance
        for d in (1, 2, 3, 5, 10):
            C = set(bnd)
            for p in range(N):
                if posdist(p) <= d:
                    C.add(isa[p])
            bp, bn, sz, fb = check_set(C, LCP, N, bnd, full_psv, full_nsv)
            chiC = sweep_from_restricted(S, C)
            line += (f" d{d}:({bp},{bn},{'ok' if chiC == chi_true else 'CHI-BAD'},{sz/r:.2f}r)")
        print(line + f" [{time.time()-t0:.1f}s]")
    print("R1b DONE")


if __name__ == "__main__":
    main()
