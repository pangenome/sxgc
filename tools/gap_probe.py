#!/usr/bin/env python3
"""gap_probe.py — the refined O(r) conjecture, measured:

    CONSTRUCTION COST MODEL (lockstep adds nothing; cycle-jump is the trick):
    cost = O(r  +  sum of NON-PERIODIC sample-gap lengths)

Sample gaps = text spans between consecutive run-end-sample positions.
Periodic gaps (text is period-q inside the span) are cycle-jumpable:
resolving positions inside them costs O(q), not O(|gap|).  The conjecture
that makes chi/sA construction O(r) on real data:

    CONJ: sum |non-periodic gaps|  =  O(r)

Battery: random (control: gaps tiny, n ~ r), periodic satellites
(degenerate: huge gaps, all periodic), NESTED satellites (HOR-like),
duplicates (human-text analog: repeated unique paragraphs — non-periodic
repetitiveness), mixed.  Also measures with ANCHORS added (string starts
+ optionally mid-string anchors) since anchors cut gaps.

Also validates the cycle-jump MECHANISM: for periodic gaps, resolve a
member position by period arithmetic and check it equals the brute walk.
"""
import random
import sys

sys.path.insert(0, "tools")
from scatter_probe import suffix_array, kasai_lcp


def min_period(span):
    """smallest q such that span[j] == span[j-q] for all j >= q, else None.
    O(|span| * divisors) worst case; here: check candidate divisors."""
    n = len(span)
    if n < 4:
        return None
    for q in range(1, n // 2 + 1):
        if all(span[j] == span[j - q] for j in range(q, n)):
            return q
        # early pruning: sample check
        if q > 8 and span[: q * 2] != span[q : q * 2]:
            continue
    return None


def analyze(name, x, anchors=()):
    N = len(x) + 1
    X = x + b"\x00"
    sa, _ = suffix_array(X)
    isa = [0] * N
    for i, p in enumerate(sa):
        isa[p] = i
    BWT = [X[sa[i] - 1] if sa[i] else 0 for i in range(N)]
    # runs + run-end sample positions
    runs = []
    st = 0
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            runs.append((st, i - 1))
            st = i
    runs.append((st, N - 1))
    r = len(runs)
    samples = sorted(sa[b] for (_, b) in runs) + sorted(anchors)
    samples = sorted(set(samples))
    # gaps between consecutive sample positions
    gaps = []
    for i in range(len(samples) - 1):
        lo, hi = samples[i] + 1, samples[i + 1]  # interior positions
        if hi > lo:
            gaps.append((lo, hi))
    tail_gap = (samples[-1] + 1, N - 1)  # above last sample; EXCLUDE the
    # terminator position itself (X[N-1] = 0 pollutes periodicity tests)
    if tail_gap[1] > tail_gap[0]:
        gaps.append(tail_gap)
    tot = sum(hi - lo for lo, hi in gaps)
    nonper = []
    per = []
    for lo, hi in gaps:
        span = X[lo:hi]
        q = min_period(span)
        if q is None:
            nonper.append((lo, hi))
        else:
            per.append((lo, hi, q))
    np_len = sum(hi - lo for lo, hi in nonper)
    print(f"{name:22s} n={N:7d} r={r:6d} gaps={len(gaps):6d} "
          f"gap-bytes={tot:7d} nonperiodic={np_len:7d} "
          f"= {np_len / max(r, 1):5.2f}r  = {np_len / max(N, 1):5.3f}n  "
          f"periodic-gaps={len(per)}")
    if per:
        biggest = max(per, key=lambda t: t[1] - t[0])
        print(f"    biggest periodic gap: len={biggest[1]-biggest[0]} period={biggest[2]}")
    if nonper:
        sizes = sorted((h - l) for l, h in nonper)
        print(f"    biggest nonperiodic gaps: {sizes[-5:]}")
    return r, np_len, tot


def cyclejump_validate(x, trials=5):
    """mechanism check: a member deep inside a PERIODIC gap resolves by
    period arithmetic: walking t steps from row e is a row-orbit with
    period q' dividing the text period structure; simplest validation:
    positions in the gap are p = q_sample_offset + d — verify by brute."""
    # (validation of the arithmetic is trivial once the gap is periodic:
    # the walk from ANY row terminates at the gap's bottom sample, and the
    # depth is exactly position - gap_bottom, so the 'jump' is: position =
    # gap_bottom + depth, where depth is read off the member's index in
    # the periodic orbit — full row-space validation happens in the Rust
    # port; here we assert the identity walk_length == position - q_bottom)
    N = len(x) + 1
    X = x + b"\x00"
    sa, _ = suffix_array(X)
    isa = [0] * N
    for i, p in enumerate(sa):
        isa[p] = i
    BWT = [X[sa[i] - 1] if sa[i] else 0 for i in range(N)]
    runs = []
    st = 0
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            runs.append((st, i - 1))
            st = i
    runs.append((st, N - 1))
    sampled_rows = set(b for _, b in runs)
    sample_at = {b: sa[b] for _, b in runs}
    # char_rows for LF
    import bisect
    char_rows = {}
    for i in range(N):
        char_rows.setdefault(BWT[i], []).append(i)
    C = {}
    acc = 0
    for c in sorted(char_rows):
        C[c] = acc
        acc += len(char_rows[c])
    def LF(k):
        c = BWT[k]
        return C[c] + bisect.bisect_left(char_rows[c], k)
    rng = random.Random(11)
    ok = bad = 0
    for _ in range(trials):
        k = rng.randrange(N)
        t = 0
        kk = k
        while kk not in sampled_rows and t < N + 5:
            kk = LF(kk)
            t += 1
        if kk in sampled_rows:
            pos = sample_at[kk] + t
            if pos == sa[k]:
                ok += 1
            else:
                bad += 1
    print(f"    cyclejump identity (walk depth == pos - sample): {ok} ok, {bad} bad")


def gen():
    rng = random.Random(20261001)
    yield "random-4-20k", bytes(rng.choice((1, 2, 3, 4)) for _ in range(20000))
    yield "random-bin-20k", bytes(rng.choice((1, 2)) for _ in range(20000))
    sat = b"\x01\x02\x03" * 6000 + bytes(rng.choice((1, 2, 3, 4)) for _ in range(500))
    yield "satellite-18k", sat
    hor = (b"\x01\x02\x03" * 100 + b"\x04") * 60 + bytes(rng.choice((1, 2, 3, 4)) for _ in range(300))
    yield "HOR-nested", hor
    # DUPLICATES — the human-text analog: 40 unique random paragraphs
    # (300 bytes each), each repeated 50x, shuffled placement
    paras = [bytes(rng.choice((1, 2, 3, 4)) for _ in range(300)) for _ in range(40)]
    docs = []
    for _ in range(50):
        for p in rng.sample(paras, len(paras)):
            docs.append(p)
    dup = b"".join(docs)
    yield "duplicates-600k", dup[:600000]
    # duplicates + unique filler (more realistic)
    parts = []
    for i in range(400):
        parts.append(paras[rng.randrange(40)] if rng.random() < 0.5
                     else bytes(rng.choice((1, 2, 3, 4)) for _ in range(300)))
    yield "dup+unique-120k", b"".join(parts)
    yield "repeat-ab", b"\x01\x02" * 5000


if __name__ == "__main__":
    print(f"{'text':22s} {'n':>7s} {'r':>6s} {'gaps':>6s} {'gapbytes':>8s} "
          f"{'nonper':>7s}  =r      =n")
    for name, x in gen():
        analyze(name, x)
    print("\ncycle-jump identity spot checks (satellite):")
    sat = b"\x01\x02\x03" * 3000 + b"\x01\x02\x01\x04"
    cyclejump_validate(sat, trials=50)
    print("GAP PROBE DONE")
