#!/usr/bin/env python3
"""Forward-BWT semantics explorer.

Runs the full FM oracle (chi_rspace_proto.py's full_fm_oracle, verbatim logic)
over the LINEAR text X = T + 0x00 (T = the toy 3-record stream), then:
  - brute-forces the forward requirements covSet(T) (w right-maximal in the
    CYCLIC text, wc suffix of prefix pos);
  - for each emitted witness pos, reports which requirements it covers and
    how the FM candidate's char relates to the text at pos.
"""
import sys
sys.path.insert(0, '.')
from toy_explore import build

SEP = 0x1e


def suffix_array(X):
    k = 1
    rank = [X[i] for i in range(len(X))]
    sa = list(range(len(X)))
    tmp = [0] * len(X)
    while True:
        key = lambda i: (rank[i], rank[i + k] if i + k < len(X) else -1)
        sa.sort(key=key)
        tmp[sa[0]] = 0
        for j in range(1, len(X)):
            tmp[sa[j]] = tmp[sa[j - 1]] + (key(sa[j - 1]) < key(sa[j]))
        rank = tmp[:]
        if rank[sa[-1]] == len(X) - 1:
            break
        k *= 2
    return sa


def kasai(X, sa):
    n = len(X)
    isa = [0] * n
    for i, p in enumerate(sa):
        isa[p] = i
    lcp = [0] * n
    h = 0
    for p in range(n):
        if isa[p] > 0:
            q = sa[isa[p] - 1]
            while p + h < n and q + h < n and X[p + h] == X[q + h]:
                h += 1
            lcp[isa[p]] = h
            if h:
                h -= 1
        else:
            h = 0
    return lcp


def sv(L):
    n = len(L)
    psv = [-1] * n
    nsv = [n + 1] * n
    st = []
    for i in range(n):
        while st and L[st[-1]] >= L[i]:
            nsv[st.pop()] = i
        psv[i] = st[-1] if st else -1
        st.append(i)
    return psv, nsv


def main():
    records = [b"AACGAAAT", b"AGGAGCCT", b"AATCACCT"]
    S, _, n, _, _, _, _ = build(records)
    T = S
    X = T + b"\x00"
    N = len(X)
    sa = suffix_array(X)
    lcp = kasai(X, sa)
    bwt = [X[i - 1] if i else 0 for i in sa]
    psvL, nsvL = sv(lcp)
    SIGMA = 256
    R = [(-2, 0, False, 1 << 62)] * SIGMA
    Sset = []
    for i in range(1, N):
        if bwt[i] != bwt[i - 1]:
            for ip in (i - 1, i):
                c = bwt[ip]
                if c != 0:
                    if R[c][0] <= psvL[i]:
                        if R[c][3] < i:
                            Sset.append(R[c][1])
                        R[c] = (i, N - sa[ip], True, nsvL[i])
    for c in range(1, SIGMA):
        if R[c][2]:
            Sset.append(R[c][1])
    wits = sorted(set(Sset))
    print("forward FM oracle witnesses (pos = N - sa):", wits)

    # brute cyclic covSet: (w,c) with w right-maximal (cyclic), wc suffix of prefix pos
    occ = {}
    for L in range(0, n):
        for i in range(n):
            w = bytes(S[(i + t) % n] for t in range(L + 1))
            occ.setdefault(w, set()).add((i + L) % n)  # 0-based inclusive end
    reqs = set()
    for w, ends in occ.items():
        nxt = {S[(e + 1) % n] for e in ends}
        if w == b"" or len(nxt) >= 2:
            for c in nxt:
                reqs.add((w, c))
    covsets = {}
    for pos in wits:
        cs = []
        for (w, c) in reqs:
            wc = w + bytes([c])
            # wc suffix of 0-based prefix [0, pos)  <=> wc ends at pos-1
            ok = all(S[(pos - 1 - t) % n] == wc[len(wc) - 1 - t] for t in range(len(wc)))
            if ok:
                cs.append((w, bytes([c])))
        covsets[pos] = cs
    print("\nper-witness covered requirements (forward text, cyclic):")
    for pos in wits:
        cs = covsets[pos]
        c_at_pos = S[pos % n]
        print(f"  pos={pos:3d} S[pos]={chr(c_at_pos)} covsize={len(cs)} depths={sorted({len(w) for w, _ in cs})}")


if __name__ == "__main__":
    main()
