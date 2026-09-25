#!/usr/bin/env python3
"""events_probe.py — Layer-2 model check with the CORRECT SA:
FM with PSV/NSV restricted to event rows (boundary + pair-extremes),
both extreme conventions (argmax position = interposing-small-LCP side,
argmin position), vs full FM. Ground-truth battery mirrors the Lean
fmTexts convention (texts over {1,2}) plus richer alphabets."""
import random
import sys

sys.path.insert(0, "tools")
from scatter_probe import suffix_array, kasai_lcp, sv


def build(x):
    N = len(x) + 1
    X = x + b"\x00"
    sa, _ = suffix_array(X)
    isa = [0] * N
    for i, p in enumerate(sa):
        isa[p] = i
    LCP = kasai_lcp(X, sa, isa)
    BWT = [X[sa[i] - 1] if sa[i] else 0 for i in range(N)]
    return N, sa, LCP, BWT


def phi_intervals(N, sa, isa):
    phi = [sa[isa[p] - 1] if isa[p] > 0 else -1 for p in range(N)]
    iv = [0] * N
    cur = 0
    for p in range(1, N):
        if phi[p - 1] == -1 or phi[p] == -1 or phi[p - 1] != phi[p] - 1:
            cur += 1
        iv[p] = cur
    return iv


def chi_events(x, extreme="maxpos"):
    N, sa, LCP, BWT = build(x)
    bnd = set(i for i in range(1, N) if BWT[i] != BWT[i - 1])
    iv = phi_intervals(N, sa, list(range(N)))  # isa rebuilt below
    # rebuild isa correctly
    isa = [0] * N
    for i, p in enumerate(sa):
        isa[p] = i
    iv = phi_intervals(N, sa, isa)
    run_id = [0] * N
    r = 1
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            r += 1
        run_id[i] = r - 1
    # pair extremes: extreme = argmax position (maxpos) or argmin position
    best = {}
    for k in range(N):
        key = (run_id[k], iv[sa[k]])
        cur = sa[k]
        if key not in best:
            best[key] = cur
        else:
            if extreme == "maxpos":
                best[key] = max(best[key], cur)      # argmax position
            else:
                best[key] = min(best[key], cur)      # argmin position
    events = sorted(bnd | set(k for k in range(N) if sa[k] == best[(run_id[k], iv[sa[k]])]))
    evlcp = [(k, LCP[k]) for k in events]
    ev_idx = {k: j for j, k in enumerate(events)}
    RPSV = {}
    RNSV = {}
    for i in bnd:
        j = ev_idx[i]
        p = -1
        for q in range(j - 1, -1, -1):
            if evlcp[q][1] < LCP[i]:
                p = evlcp[q][0]
                break
        RPSV[i] = p
        nn = N
        for q in range(j + 1, len(events)):
            if evlcp[q][1] < LCP[i]:
                nn = evlcp[q][0]
                break
        RNSV[i] = nn
    sigma = max(BWT) + 1
    R = [(-2, 0, False, 2**62)] * sigma
    S = []
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            for ip in (i - 1, i):
                c = BWT[ip]
                if c != 0:
                    if R[c][0] <= RPSV[i]:
                        if R[c][3] < i:
                            S.append(R[c][1])
                        R[c] = (i, N - sa[ip], True, RNSV[i])
    for c in range(1, sigma):
        if R[c][2]:
            S.append(R[c][1])
    return len(set(S))


def chi_full(x):
    N, sa, LCP, BWT = build(x)
    psvL, nsvL = sv(LCP)
    sigma = max(BWT) + 1
    R = [(-2, 0, False, 2**62)] * sigma
    S = []
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            for ip in (i - 1, i):
                c = BWT[ip]
                if c != 0:
                    if R[c][0] <= psvL[i]:
                        if R[c][3] < i:
                            S.append(R[c][1])
                        R[c] = (i, N - sa[ip], True, nsvL[i])
    for c in range(1, sigma):
        if R[c][2]:
            S.append(R[c][1])
    return len(set(S))


def battery_texts():
    rng = random.Random(20260929)
    # mirror Lean fmTexts: all texts over {1,2} up to length 6
    out = []

    def gen(k, cur):
        if len(cur) == k:
            if cur:
                out.append(bytes(cur))
            return
        gen(k, cur + [1])
        gen(k, cur + [2])

    for L in (1, 2, 3, 4, 5):
        gen(L, [])
    out += [bytes(rng.choice((1, 2)) for _ in range(200)) for _ in range(30)]
    out += [bytes(rng.choice((1, 2, 3, 4)) for _ in range(200)) for _ in range(30)]
    out.append(b"\x01\x02\x03" * 100 + bytes(rng.choice((1, 2, 3, 4)) for _ in range(100)))
    out.append(b"\x01\x02" * 150)
    return out


if __name__ == "__main__":
    agree_max = agree_min = tot = 0
    fails = []
    for x in battery_texts():
        f = chi_full(x)
        a = chi_events(x, "maxpos")
        b = chi_events(x, "minpos")
        tot += 1
        agree_max += f == a
        agree_min += f == b
        if f != a and f != b and len(fails) < 5:
            fails.append((x[:12], f, a, b))
    print(f"LAYER-2 RECHECK (correct SA): {tot} texts; argmax-pos {agree_max}/{tot}, argmin-pos {agree_min}/{tot}")
    for t in fails:
        print("  disagree both:", t)
    print("EVENTS PROBE DONE")
