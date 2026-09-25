#!/usr/bin/env python3
"""c_probe_r1c.py — R1 with the R3 inverse available.

Candidate rule tested: rows whose POSITION lies within distance d of a
piece boundary (piece starts/ends).  With R3 (resolve_inv) such positions
are enumerable in position space without touching all rows:
    |candidates| = O((2d+1) * iv)   position->row lookups, each O(log).
Measured: exact PSV/NSV preservation at boundary rows, chi preservation,
|C|/r and |C|/n, plus the exact closure-position-distance distribution.
"""
import bisect
import os
import sys
import time

sys.path.insert(0, "tools")
from c_probe_common import battery_texts, build, chi_full
from c_probe_r1 import psv_nsv_on, check_set
from c_probe_r3 import InvResolve
from parse_resolve_proto import run_pfp
from scatter_probe import sv


def sweep_restricted(S, C):
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
    os.makedirs("/tmp/cpr1c", exist_ok=True)
    os.chdir("/tmp/cpr1c")
    texts = battery_texts()
    ds = (1, 2, 3, 5, 10)
    print(f"{'text':16s} {'n':>7s} {'r':>7s} {'iv':>6s} {'chi':>7s} | "
          f"closure dist: p90 p99 max | " +
          " ".join(f"d{d}" for d in ds))
    for name, x in texts:
        t0 = time.time()
        S = build(x)
        N, LCP, sa, BWT, isa = S["N"], S["LCP"], S["sa"], S["BWT"], S["isa"]
        r = len(S["runs"])
        iv = S["n_pieces"]
        bnd = [i for i in range(1, N) if BWT[i] != BWT[i - 1]]
        full_psv, full_nsv = sv(LCP)
        chi_true = chi_full(S)
        # closure distances
        clos = set()
        for i in bnd:
            if full_psv[i] >= 0:
                clos.add(full_psv[i])
            if full_nsv[i] < N:
                clos.add(full_nsv[i])
        pid_of_pos = S["piece_id"]
        ps, pe = S["piece_start"], S["piece_end"]
        dists = []
        for j in clos:
            p = sa[j]
            k = pid_of_pos[p]
            dists.append(min(p - ps[k], pe[k] - p))
        dists.sort()
        q = lambda f: dists[min(len(dists) - 1, int(f * len(dists)))] if dists else 0
        # PFP inverse for candidate enumeration
        run_pfp(x, "b_" + name)
        R = InvResolve("b_" + name)
        W = R.w
        nT = len(x)               # |T| (text positions are [0, nT))

        def to_brute(rw):
            # machinery row -> brute row (linear SA of T + 0x00):
            # row i >= W  <->  brute row (i - W + 1)
            if rw is None or rw < W:
                return None
            return rw - W + 1
        line = (f"{name:16s} {N:7d} {r:7d} {iv:6d} {chi_true:7d} | "
                f"p90={q(.9)} p99={q(.99)} max={dists[-1] if dists else 0} |")
        for d in ds:
            # enumerate positions within d of piece boundaries, map to rows
            C = set(bnd)
            nlookup = 0
            for k in range(iv):
                for p in range(max(0, ps[k] - d), min(nT, ps[k] + d + 1)):
                    rw, err = R.resolve_inv(p)
                    b = to_brute(rw)
                    if b is not None:
                        C.add(b)
                    nlookup += 1
                for p in range(max(0, pe[k] - d), min(nT, pe[k] + d + 1)):
                    rw, err = R.resolve_inv(p)
                    b = to_brute(rw)
                    if b is not None:
                        C.add(b)
                    nlookup += 1
            bp, bn, sz, fb = check_set(C, LCP, N, bnd, full_psv, full_nsv)
            ch = sweep_restricted(S, C)
            line += f" |{sz}({sz/r:.2f}r,{bp},{bn},{'ok' if ch == chi_true else 'CHIBAD'})"
        print(line + f" [{time.time()-t0:.1f}s]")
    print("R1c DONE")


if __name__ == "__main__":
    main()
