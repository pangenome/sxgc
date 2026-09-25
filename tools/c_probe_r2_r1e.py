#!/usr/bin/env python3
"""c_probe_r2_r1e.py — (a) R1e: how far are closure rows from their query
rows in ROW order (a row-local candidate rule?) and what are the closure
LCP drops; (b) R2: per-run position extremes — is the argmin/argmax SA row
of a run at a sampled (run-end) row, or near one?  Does the LF-image
recursion (SA[k] = SA[LF(k)] + 1 => min/max over a run = 1 + min/max over
its image interval) close on samples?
"""
import os
import sys
import time
from collections import Counter

sys.path.insert(0, "tools")
from c_probe_common import battery_texts, build
from scatter_probe import sv


def main():
    os.makedirs("/tmp/cpr2", exist_ok=True)
    os.chdir("/tmp/cpr2")
    print("=== R1e: closure row-gap (|answer - query| in ROW order) ===")
    print(f"{'text':16s} {'n':>7s} {'r':>7s} | rowgap p50 p90 p99 max | "
          f"closure/r | nsamp: closure rows that are run-end rows | closure LCP drop p50 p90 max")
    for name, x in battery_texts():
        S = build(x)
        N, LCP = S["N"], S["LCP"]
        bnd = [i for i in range(1, N) if S["BWT"][i] != S["BWT"][i - 1]]
        full_psv, full_nsv = sv(LCP)
        # run-end rows (sampled)
        runends = set(e for (_, e) in S["runs"])
        gaps = []
        drops = []
        clos = set()
        nsamp = 0
        for i in bnd:
            for ans in (full_psv[i], full_nsv[i]):
                if 0 <= ans < N and ans != i:
                    gaps.append(abs(ans - i))
                    drops.append(LCP[i] - LCP[ans])
                    clos.add(ans)
                    if ans in runends:
                        nsamp += 1
        gaps.sort()
        drops.sort()
        q = lambda L, f: L[min(len(L) - 1, int(f * len(L)))] if L else 0
        r = len(S["runs"])
        print(f"{name:16s} {N:7d} {r:7d} | rowgap {q(gaps,.5)} {q(gaps,.9)} {q(gaps,.99)} {gaps[-1] if gaps else 0} | "
              f"{len(clos)/r:6.2f} | {nsamp}/{len(gaps)} | drop {q(drops,.5)} {q(drops,.9)} {drops[-1] if drops else 0}")
    print()
    print("=== R2: per-run SA extremes ===")
    print(f"{'text':16s} {'r':>7s} | argmin/argmax row offset from run ends (p50,p90,max) | "
          f"frac runs with extreme AT a run end | frac extremes whose LF image is a run-end row")
    for name, x in battery_texts():
        S = build(x)
        N, sa, LCP, BWT = S["N"], S["sa"], S["LCP"], S["BWT"]
        runs = S["runs"]
        run_id = S["run_id"]
        prefix_char = {}
        # SAMPLED rows = run-end rows (v4 samples live at run ends)
        sampled = set(e for (_, e) in runs)
        descm = []
        at_end = 0
        total = 0
        img_sampled = 0
        img_total = 0
        rng = __import__("random").Random(1)
        r = len(runs)
        sample_runs = runs if r <= 4000 else rng.sample(runs, 4000)
        for (a, b) in sample_runs:
            mn = b
            mx = a
            vmin = sa[a]
            vmax = sa[a]
            for k in range(a, b + 1):
                if sa[k] < vmin:
                    vmin = sa[k]
                    mn = k
                if sa[k] > vmax:
                    vmax = sa[k]
                    mx = k
            for k in (mn, mx):
                total += 1
                if k in sampled:
                    at_end += 1
                # LF image of the extreme row: is it a sampled row?
                c = BWT[k]
                # rank of k among c-rows
                # (use the run structure: LF(k) = C[c] + rank_c(k))
                img_total += 1
            descm.append((mn - a, b - mn, mx - a, b - mx))
        # offsets
        offs = []
        for t in descm:
            offs.extend([t[0], t[1], t[2], t[3]])
        offs.sort()
        q = lambda L, f: L[min(len(L) - 1, int(f * len(L)))] if L else 0
        print(f"{name:16s} {r:7d} | {q(offs,.5)} {q(offs,.9)} {offs[-1] if offs else 0} | "
              f"{at_end}/{total}")
    print("R2/R1e DONE")


if __name__ == "__main__":
    main()
