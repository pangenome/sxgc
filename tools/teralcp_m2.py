#!/usr/bin/env python3
"""teralcp_m2.py — M2 (THE gate): can the DIRECT-LAW piece starts be
enumerated from an O(r+parse) candidate structure, with no per-position
tests?

S_law = { p : p=0 or PLCP(p-1) != PLCP(p)+1 }  (direct-law piece starts,
<= 2r+2 measured).  We test candidate families for CONTAINMENT of S_law
and their size:

  C_phi      : phi-parallel breaks  { p : phi(p) != phi(p-1)-1 }
  C_runedge  : positions adjacent to BWT run boundaries (resolve rows)
  C_revrun   : positions associated with runs of BWT(rev T) (r-bar side)
  C_parse    : PFP phrase-occurrence start positions (parse coordinates)
  C_union    : the union of the above

Reported per text: |S_law|, and for each family rate = |S_law & C|/|S_law|
and |C|.  A family with rate 1.0 at size O(r+parse) is the M3 input.
"""
import os
import sys
import time
import random

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) or ".")
from c_probe_common import suffix_array_np, build  # gated
from parse_resolve_proto import PFPResolve, W, run_pfp  # gated

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) or ".")
from endpoint_common import battery  # 7-text battery (0x0b..0x0e)


def rev_bwt_positions(X):
    """positions in T-coords associated with runs of BWT(rev X)."""
    rev = X[::-1]
    sa = suffix_array_np(rev)
    N = len(rev)
    isa = [0] * N
    for i, p in enumerate(sa):
        isa[p] = i
    bwt = [rev[sa[i] - 1] if sa[i] else 0 for i in range(N)]
    out = set()
    for i in range(1, N):
        if bwt[i] != bwt[i - 1]:
            for j in (i, i - 1):
                # rev-position sa[j] -> T-position N-1-sa[j]
                out.add(N - 1 - sa[j])
    # also the extreme rows
    out.add(N - 1 - sa[0])
    out.add(N - 1 - sa[N - 1])
    return out


def phi_breaks(S):
    N, sa, isa = S["N"], S["sa"], S["isa"]
    phi = [sa[isa[p] - 1] if isa[p] > 0 else -1 for p in range(N)]
    out = {0}
    for p in range(1, N):
        q1 = phi[p]
        q0 = phi[p - 1]
        if q1 == -1 or q0 == -1 or q0 != q1 - 1:
            out.add(p)
    return out


def run_edge_positions(S):
    N, BWT, sa = S["N"], S["BWT"], S["sa"]
    out = {0, N - 1}
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            out.add(sa[i])
            out.add(sa[i - 1])
    return out


def parse_starts(T, prefix):
    """phrase-occurrence start positions in text coords."""
    run_pfp(T, prefix)
    R = PFPResolve(prefix)
    pos = 0
    out = set()
    for pid in R.parse:
        out.add(pos)
        pos += R.phrase_len[pid - 1] - W
    out.add(pos)
    return out, R


def analyse(name, T, workdir="/tmp/tlc_m2"):
    os.makedirs(workdir, exist_ok=True)
    S = build(T)
    S_law = set(S["piece_start"])
    C_phi = phi_breaks(S)
    C_run = run_edge_positions(S)
    C_rev = rev_bwt_positions(S["X"])
    try:
        C_parse, R = parse_starts(T, os.path.join(workdir, name))
        n_parse = len(R.parse)
    except Exception as e:  # noqa
        C_parse, n_parse = set(), -1
    C_union = C_phi | C_run | C_rev | C_parse
    for CS in (C_run, C_rev, C_parse, C_phi):
        pass
    # containment may need a "shift": piece start p vs candidates at p or p-1/p+1
    def rate(C, shift=0):
        Cs = set(c + shift for c in C if 0 <= c + shift < S["N"])
        return len(S_law & Cs) / max(len(S_law), 1), len(Cs)
    rows = []
    for label, C in (("phi", C_phi), ("runedge", C_run), ("revrun", C_rev), ("parse", C_parse),
                     ("union", C_union)):
        r0, s0 = rate(C, 0)
        r1, _ = rate(C, 1)
        rm1, _ = rate(C, -1)
        rbest = max(r0, r1, rm1)
        rows.append((label, len(C), r0, r1, rm1, rbest))
    print(f"\n[{name}] n={S['N']} r={len(S['runs'])} |S_law|={len(S_law)} parse_occ={n_parse}")
    for label, sz, r0, r1, rm1, rbest in rows:
        print(f"    {label:8s} |C|={sz:8d}  hit@0={r0:.4f}  hit@+1={r1:.4f}  hit@-1={rm1:.4f}  best={rbest:.4f}")
    return dict(name=name, N=S["N"], r=len(S["runs"]), nlaw=len(S_law),
                sizes={lbl: sz for lbl, sz, *_ in rows},
                rates={lbl: rbest for lbl, *_r in [(x[0], x[5]) for x in rows]})


def main():
    only = sys.argv[1:]
    res = []
    for name, T in battery():
        if only and name not in only:
            continue
        res.append(analyse(name, T))
    print("\n=== M2 SUMMARY ===")
    for x in res:
        print(f"{x['name']:18s} n={x['N']:8d} r={x['r']:7d} |S_law|={x['nlaw']:7d} "
              + " ".join(f"{k}={v:.3f}({x['sizes'][k]})" for k, v in x["rates"].items()))
    print("M2 DONE")


if __name__ == "__main__":
    main()
