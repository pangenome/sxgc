#!/usr/bin/env python3
"""c_probe_r1g.py — hybrid candidate rule:
   D(δ,d) = boundary ∪ run-ends ∪ (row-window δ around each boundary row)
            ∪ (piece-boundary position windows of distance d, via R3)
Row-window candidates are free (O(r·δ) row scans, no lookups); the answers
were measured to be row-close to their queries (p50 2, p90 8-352).
"""
import os, sys, time
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
        for ip in (i - 1, i):
            c = BWT[ip]
            if c != 0:
                if R[c][0] <= rp.get(i, -1):
                    if R[c][3] < i:
                        out.add(R[c][1])
                    R[c] = (i, N - sa[ip], True, rn.get(i, N))
    for c in range(1, sigma):
        if R[c][2]:
            out.add(R[c][1])
    return len(out)


def main():
    os.makedirs("/tmp/cpr1g", exist_ok=True); os.chdir("/tmp/cpr1g")
    combos = [(2, 1), (8, 1), (8, 3), (32, 3), (128, 0), (128, 3)]
    print(f"{'text':16s} {'n':>7s} {'r':>7s} {'chi':>7s} | " +
          " ".join(f"D(d{d},w{w})" for (w, d) in combos))
    for name, x in battery_texts():
        t0 = time.time()
        S = build(x)
        N, LCP, sa, BWT = S["N"], S["LCP"], S["sa"], S["BWT"]
        r = len(S["runs"]); iv = S["n_pieces"]
        bnd = [i for i in range(1, N) if BWT[i] != BWT[i - 1]]
        runends = set(e for (_, e) in S["runs"])
        full_psv, full_nsv = sv(LCP)
        chi_true = chi_full(S)
        run_pfp(x, "b_" + name)
        R = InvResolve("b_" + name); W = R.w; nT = len(x)
        ps, pe = S["piece_start"], S["piece_end"]
        line = f"{name:16s} {N:7d} {r:7d} {chi_true:7d} |"
        for (w, d) in combos:
            C = set(bnd) | runends
            for i in bnd:
                lo, hi = max(0, i - w), min(N, i + w + 1)
                for jj in range(lo, hi):
                    C.add(jj)
            if d > 0:
                for k in range(iv):
                    for p in range(max(0, ps[k] - d), min(nT, ps[k] + d + 1)):
                        rw, err = R.resolve_inv(p)
                        if rw is not None and rw >= W: C.add(rw - W + 1)
                    for p in range(max(0, pe[k] - d), min(nT, pe[k] + d + 1)):
                        rw, err = R.resolve_inv(p)
                        if rw is not None and rw >= W: C.add(rw - W + 1)
            bp, bn, sz, _ = check_set(C, LCP, N, bnd, full_psv, full_nsv)
            ch = sweep_restricted(S, C)
            line += f" {sz}({sz/r:.2f}r,{bp},{bn},{'ok' if ch==chi_true else 'CHIBAD'})"
        print(line + f" [{time.time()-t0:.1f}s]")
    print("R1g DONE")


if __name__ == "__main__":
    main()
