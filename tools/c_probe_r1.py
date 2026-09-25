#!/usr/bin/env python3
"""c_probe_r1.py — R1: PIECE-TAIL CONCENTRATION / minimal sufficient
restricted sets for boundary-row PSV/NSV.

TARGET (statement-locked): compute PSV[i], NSV[i] for every run-boundary
row i in o(n) total.  The v3 model gives ONE sufficient event set
(boundary rows UNION both cell extremes) but that set can be Theta(n)
(measured: duplicates-600k has P=595k=n, random-4 has P=n).  So R1 asks:
  (Q1) how big is the ANSWER CLOSURE = { PSV[i], NSV[i] : i boundary }?
  (Q2) do small candidate sets (piece boundaries!) preserve boundary-row
       PSV/NSV, i.e. restricted_stack(C) == full at all boundary rows?
  (Q3) where do closure rows live: distance of their positions to piece
       boundaries; are they piece starts/ends?
Everything gated against the full sv() arrays.
"""
import sys
import time

sys.path.insert(0, "tools")
from c_probe_common import battery_texts, build
from scatter_probe import sv


def psv_nsv_on(C, LCP, N):
    """restricted PSV/NSV: standard stack over the subsequence C (rows
    sorted ascending). Returns dicts row -> answer row (or -1 / N)."""
    m = len(C)
    vals = [LCP[k] for k in C]
    nsv = [N] * m
    psv = [-1] * m
    st_n, st_p = [], []
    for j in range(m):
        while st_n and vals[j] < vals[st_n[-1]]:
            nsv[st_n[-1]] = j
            st_n.pop()
        st_n.append(j)
        while st_p and vals[j] <= vals[st_p[-1]]:
            st_p.pop()
        if st_p:
            psv[j] = st_p[-1]
        st_p.append(j)
    out_psv = {}
    out_nsv = {}
    for j, k in enumerate(C):
        out_psv[k] = C[psv[j]] if psv[j] >= 0 else -1
        out_nsv[k] = C[nsv[j]] if nsv[j] < m else N
    return out_psv, out_nsv


def check_set(C, LCP, N, bnd, full_psv, full_nsv):
    C = sorted(set(C))
    rp, rn = psv_nsv_on(C, LCP, N)
    bad_p = bad_n = 0
    first_bad = None
    for i in bnd:
        if i not in rp:
            continue
        if rp[i] != full_psv[i]:
            bad_p += 1
            if first_bad is None:
                first_bad = ("PSV", i, full_psv[i], rp[i], LCP[i], full_psv[i] if full_psv[i] >= 0 else None)
        if rn[i] != full_nsv[i]:
            bad_n += 1
            if first_bad is None:
                first_bad = ("NSV", i, full_nsv[i], rn[i], LCP[i], full_nsv[i])
    return bad_p, bad_n, len(C), first_bad


def main():
    texts = battery_texts()
    print(f"{'text':16s} {'n':>7s} {'r':>7s} {'iv':>6s} | "
          f"{'|clos|':>7s} {'clos/r':>6s} {'clos/n':>6s} | "
          f"candidate sets: (name: |C|/r, badPSV, badNSV)  [only failures >0 listed]")
    for name, x in texts:
        t0 = time.time()
        S = build(x)
        N, LCP, sa, BWT = S["N"], S["LCP"], S["sa"], S["BWT"]
        r = len(S["runs"])
        iv = S["n_pieces"]
        bnd = [i for i in range(1, N) if BWT[i] != BWT[i - 1]]
        full_psv, full_nsv = sv(LCP)
        # Q1: answer closure
        clos = set()
        for i in bnd:
            if full_psv[i] >= 0:
                clos.add(full_psv[i])
            if full_nsv[i] < N:
                clos.add(full_nsv[i])
        # Q3: closure row structure
        # piece boundaries: rows whose position is a piece start or end
        pos_is_pstart = [False] * N
        pos_is_pend = [False] * N
        for ps in S["piece_start"]:
            pos_is_pstart[ps] = True
        for pe in S["piece_end"]:
            pos_is_pend[pe] = True
        at_pstart = at_pend = 0
        dists = []
        for j in clos:
            p = sa[j]
            if pos_is_pstart[p]:
                at_pstart += 1
            if pos_is_pend[p]:
                at_pend += 1
            pid = S["piece_id"][p]
            d0 = p - S["piece_start"][pid]
            d1 = S["piece_end"][pid] - p
            dists.append(min(d0, d1))
        dists.sort()
        med = dists[len(dists) // 2] if dists else 0
        p90 = dists[int(0.9 * len(dists))] if dists else 0
        # Q2: candidate sets
        cands = {
            "v3cells": set(bnd) | set(S["cell_max_row"].values()) | set(S["cell_min_row"].values()),
            "pstart+pend": set(bnd) | set(sa[p] for p in S["piece_start"]) | set(sa[p] for p in S["piece_end"]),
            "pend": set(bnd) | set(sa[p] for p in S["piece_end"]),
            "pstart": set(bnd) | set(sa[p] for p in S["piece_start"]),
        }
        # note: rows whose POSITION is a piece boundary = isa[pstart] etc.
        cands["pstart+pend"] = set(bnd) | set(S["isa"][p] for p in S["piece_start"]) | set(S["isa"][p] for p in S["piece_end"])
        cands["pend"] = set(bnd) | set(S["isa"][p] for p in S["piece_end"])
        cands["pstart"] = set(bnd) | set(S["isa"][p] for p in S["piece_start"])
        msgs = []
        for cname, C in cands.items():
            bp, bn, sz, fb = check_set(C, LCP, N, bnd, full_psv, full_nsv)
            if bp or bn:
                msgs.append(f"{cname}:|C|={sz}({sz/r:.2f}r) badPSV={bp} badNSV={bn} fb={fb}")
        print(f"{name:16s} {N:7d} {r:7d} {iv:6d} | "
              f"{len(clos):7d} {len(clos)/max(r,1):6.2f} {len(clos)/N:6.3f} | "
              f"pstart={at_pstart} pend={at_pend} med-dist={med} p90-dist={p90}"
              + ("  FAILS: " + "; ".join(msgs) if msgs else "  all candidate sets OK"))
        print(f"    [{time.time()-t0:.1f}s]")
    print("R1 DONE")


if __name__ == "__main__":
    main()
