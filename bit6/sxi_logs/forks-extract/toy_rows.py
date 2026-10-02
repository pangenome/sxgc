#!/usr/bin/env python3
"""Dump toy BWT rows: sa, x, read prefix, BWT char, run id; then verify the
witnessing-boundary fork structure (shared context, classes, strings)."""
import sys
sys.path.insert(0, '.')
from toy_explore import build, requirements_and_covsets, SEP

def main():
    records = [b"AACGAAAT", b"AGGAGCCT", b"AATCACCT"]
    S, R, n, sa, bwt, lcp, runs = build(records)
    runid = [0]*n
    for r,(ch,st,ln) in enumerate(runs):
        for j in range(st, st+ln): runid[j]=r
    print("S:", S.decode('latin1'))
    hdr = f"{'row':>4} {'sa':>3} {'x':>3} {'bwt':>4} {'lcp':>4} run  read[0..5)"
    print(hdr)
    for i in range(n):
        x = (n - sa[i]) % n
        read = bytes(S[(x-1-t) % n] for t in range(6))
        print(f"{i:>4} {sa[i]:>3} {x:>3} {chr(bwt[i]):>4} {lcp[i]:>4} {runid[i]:>3}  {read.decode('latin1')}")
    print()
    # witnesses with their boundary rows and fork analysis
    from toy_depths import scan_depths
    depths = scan_depths(n, sa, bwt, lcp)
    reqs, covsets = requirements_and_covsets(S, n)
    run_of_head, run_of_tail = {}, {}
    for r,(ch,st,ln) in enumerate(runs):
        run_of_head[(n - sa[st]) % n] = r
        run_of_tail[(n - sa[st+ln-1]) % n] = r
    for x in sorted(depths):
        r = run_of_head.get(x, run_of_tail.get(x))
        side = 'H' if x in run_of_head else 'T'
        ch, st, ln = runs[r]
        # boundary rows: head -> boundary (st-1, st); tail -> (st+ln-1, st+ln)
        if side=='H': b1,b2 = (st-1)%n, st
        else: b1,b2 = st+ln-1, (st+ln) % n
        d = lcp[b2]  # lcp between rows b1,b2 (lcp[i] = between i-1,i)
        # fork context w: shared read prefix of depth d = S[x_b2-1 .. x_b2-d]
        xb2 = (n - sa[b2]) % n
        w = bytes(S[(xb2-1-t) % n] for t in range(d))[::-1]
        # w-interval: maximal run of rows with lcp >= d around the boundary
        lo = b2
        while lo-1 >= 0 and lcp[lo] >= d: lo -= 1
        hi = b2
        while hi+1 <= n-1 and lcp[hi+1] >= d: hi += 1
        # classes: BWT chars among rows lo..hi
        classes = {}
        for j in range(lo, hi+1):
            classes.setdefault(bwt[j], set()).add((n - sa[j]) % n)
        # strings: record id of each occurrence end position p=x-1
        def rec_of(p):
            # p = end of context; the string containing position p
            bounds = [0,8,17]  # record starts in S? records at 0..7, 9..16, 18..25, sep at 8,17,26
            for k,(s0) in enumerate([0,9,18]):
                if s0 <= p <= s0+7: return k
            return None
        print(f"wit x={x:2d} run={r}{side} boundary=({b1},{b2}) depth={d} w={w}")
        for c in sorted(classes):
            xs = sorted(classes[c])
            print(f"    class char={chr(c) if c!=SEP else '␞'} occ_x={xs} strings={sorted({rec_of(xx-1) for xx in xs})}")
    print()
    print("covSet sanity for witness x=2:", sorted((w,c) for (w,c) in covsets[2]))

if __name__ == "__main__":
    main()
