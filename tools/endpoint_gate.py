#!/usr/bin/env python3
"""endpoint_gate.py — the ENDPOINT-RULE lane's single self-contained gate.

Runs (1) the exactness gate (endpoint sweep vs full-FM oracle, all 7
battery texts, exact witness-SET equality) and (2) the exhaustive
dangerous-case check (every boundary query, every class block in its
PSV/NSV range: does any block contain a sub-tau row while BOTH its
endpoint rows are >= tau — the only way the endpoint block-min can
wrongly skip).  Both must be clean for GREEN.

Usage: python3 tools/endpoint_gate.py [text-name ...]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) or ".")
from endpoint_common import TextCase, battery
from endpoint_sweep import EndpointSweep


def oracle_witness_set(tc):
    from scatter_probe import sv
    N, LCP, BWT, sa = tc.N, tc.LCP, tc.BWT, tc.sa
    psvL, nsvL = sv(LCP)
    INF = 1 << 62
    R = [(-2, 0, False, INF)] * 256
    O = []
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            for ip in (i - 1, i):
                c = BWT[ip]
                if c != 0:
                    if R[c][0] <= psvL[i]:
                        if R[c][3] < i:
                            O.append(R[c][1])
                        R[c] = (i, N - sa[ip], True, nsvL[i])
    for c in range(1, 256):
        if R[c][2]:
            O.append(R[c][1])
    return set(O)


def exhaustive_dangerous(tc):
    N, LCP, blocks = tc.N, tc.LCP, tc.blocks
    bnd = [i for i in range(1, N) if tc.BWT[i] != tc.BWT[i - 1]]
    parts = danger = 0
    for i in bnd:
        tau = LCP[i]
        for (a, b) in ((max(0, tc.psvL[i]) + 1, i), (i + 1, min(N, tc.nsvL[i]))):
            if b <= a:
                continue
            ba = tc.block_of[a] if a < N else len(blocks) - 1
            bb = tc.block_of[b - 1]
            for bi in range(ba, bb + 1):
                blo, bhi = blocks[bi]
                p1, p2 = max(blo, a), min(bhi, b)
                if p2 - p1 < 2:
                    continue
                parts += 1
                sl = [LCP[j] for j in range(p1, p2)]
                mn = min(sl)
                if mn < tau and sl[0] >= tau and sl[-1] >= tau:
                    danger += 1
    return parts, danger


def main():
    only = sys.argv[1:] or None
    allok = True
    print(f"{'text':16s} {'n':>7s} {'r':>6s} {'|M|':>6s} {'chi':>6s} {'built':>6s} "
          f"{'seteq':>5s} {'probes':>7s} {'exh-parts':>9s} {'danger':>6s}")
    for name, T in battery():
        if only and name not in only:
            continue
        tc = TextCase(name, T, workdir="/tmp/endpoint")
        es = EndpointSweep(tc)
        got, nb = es.witness_set()
        want = oracle_witness_set(tc)
        eq = got == want
        parts, danger = exhaustive_dangerous(tc)
        ok = eq and danger == 0
        allok &= ok
        print(f"{name:16s} {tc.N:7d} {len(tc.runs):6d} {len(tc.R.M):6d} {len(want):6d} "
              f"{len(got):6d} {('YES' if eq else 'NO'):>5s} {es.ops['probe']:7d} "
              f"{parts:9d} {danger:6d}" + ("" if ok else "   <-- FAIL"))
        if not eq:
            print(f"    missing {sorted(want - got)[:5]} spurious {sorted(got - want)[:5]}")
    print("ENDPOINT GATE:", "GREEN" if allok else "RED")
    print("GATE DONE")


if __name__ == "__main__":
    main()
