#!/usr/bin/env python3
"""c_probe_r1f.py — refined R1 candidate rule, now using the two measured
structural facts:
  (R1e) most closure rows ARE run-end rows (73-92% of them on all texts but
        duplicates) — and run-end rows' SA values are already known from the
        v4 samples, so including ALL run-end rows costs O(r) and needs no
        lookups;
  (R3)  the remaining rows are enumerable from position space via the
        inverse map (piece-boundary windows of distance d).
Candidate sets tested:
  A: boundary ∪ run-ends
  B: boundary ∪ run-ends ∪ window(d)          [window rows via R3]
  C: boundary ∪ run-ends ∪ cell-extremes (v3)
Measure exact PSV/NSV preservation at boundary rows and chi preservation.
"""
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
    os.makedirs("/tmp/cpr1f", exist_ok=True)
    os.chdir("/tmp/cpr1f")
    ds = (1, 2, 3, 5)
    print(f"{'text':16s} {'n':>7s} {'r':>7s} {'iv':>6s} {'chi':>7s} | "
          f"A(bnd+ends) | " + " ".join(f"B-d{d}" for d in ds) + " | C(v3+bnd+ends)")
    for name, x in battery_texts():
        t0 = time.time()
        S = build(x)
        N, LCP, sa, BWT = S["N"], S["LCP"], S["sa"], S["BWT"]
        r = len(S["runs"])
        iv = S["n_pieces"]
        bnd = [i for i in range(1, N) if BWT[i] != BWT[i - 1]]
        runends = set(e for (_, e) in S["runs"])
        full_psv, full_nsv = sv(LCP)
        chi_true = chi_full(S)
        # A
        A = set(bnd) | runends
        bpA, bnA, szA, _ = check_set(A, LCP, N, bnd, full_psv, full_nsv)
        chA = sweep_restricted(S, A)
        line = (f"{name:16s} {N:7d} {r:7d} {iv:6d} {chi_true:7d} | "
                f"{szA}({szA/r:.2f}r,{bpA},{bnA},{'ok' if chA == chi_true else 'CHIBAD'}) |")
        # B: + windows via R3
        run_pfp(x, "b_" + name)
        R = InvResolve("b_" + name)
        W = R.w
        nT = len(x)
        ps, pe = S["piece_start"], S["piece_end"]
        for d in ds:
            C = set(A)
            for k in range(iv):
                for p in range(max(0, ps[k] - d), min(nT, ps[k] + d + 1)):
                    rw, err = R.resolve_inv(p)
                    if rw is not None and rw >= W:
                        C.add(rw - W + 1)
                for p in range(max(0, pe[k] - d), min(nT, pe[k] + d + 1)):
                    rw, err = R.resolve_inv(p)
                    if rw is not None and rw >= W:
                        C.add(rw - W + 1)
            bp, bn, sz, _ = check_set(C, LCP, N, bnd, full_psv, full_nsv)
            ch = sweep_restricted(S, C)
            line += f" {sz}({sz/r:.2f}r,{bp},{bn},{'ok' if ch == chi_true else 'CHIBAD'}) |"
        # C: v3 + ends
        Cc = set(bnd) | runends | set(S["cell_max_row"].values()) | set(S["cell_min_row"].values())
        bpc, bnc, szc, _ = check_set(Cc, LCP, N, bnd, full_psv, full_nsv)
        chc = sweep_restricted(S, Cc)
        line += f" {szc}({szc/r:.2f}r,{bpc},{bnc},{'ok' if chc == chi_true else 'CHIBAD'})"
        print(line + f" [{time.time()-t0:.1f}s]")
    print("R1f DONE")


if __name__ == "__main__":
    main()
