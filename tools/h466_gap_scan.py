#!/usr/bin/env python3
"""h466_gap_scan.py — the gap conjecture at full scale, r-space only.

Reads the 466 .ri4 sample array (R = 2,739,735,806 run-end positions,
41-bit packed), plus the anchor table (k = 38,790 string-start positions),
computes the sorted sample-position set, gap sizes, and for the top gaps
checks LOCAL PERIODICITY by seeking into the revlines flat text (still on
disk until iteration 2).  No walking, no O(n) pass — the whole scan is
O(r) reads.  Output: gap distribution + the conjecture numbers.
"""
import struct
import sys

RI4 = "/mnt/nvme3n1/erikg/sxgc-pilot/k466/h466.ri4"
ANCH = "/mnt/nvme3n1/erikg/sxgc-pilot/k466/h466.anchors"
FLAT = "/mnt/nvme3n1/erikg/sxgc-pilot/k466/h466_rl.txt"
W = 41  # sa width per the .ri4 header


def read_samples():
    import numpy as np
    with open(RI4, "rb") as f:
        head = f.read(32)
        magic, ver, n, k, r = struct.unpack("<IIQQQ", head)
        f.seek(32 + 2048 + r + 4 * r)
        hb = f.read(9)
        bits, width = struct.unpack("<QB", hb)
        assert width == W, (bits, width)
        print(f"n={n} k={k} r={r} samples bits={bits} width={width}", flush=True)
        nbytes = ((bits + 63) // 64) * 8
        raw = np.fromfile(f, dtype=np.uint64, count=nbytes // 8)
    # unpack 41-bit values
    vals = np.zeros(r, dtype=np.uint64)
    idx = np.arange(r, dtype=np.uint64) * np.uint64(W)
    w_i = (idx >> np.uint64(6)).astype(np.int64)
    sh = (idx & np.uint64(63)).astype(np.uint64)
    lo = raw[w_i] >> sh
    over = sh + np.uint64(W) > np.uint64(64)
    nxt = raw[np.minimum(w_i + 1, len(raw) - 1)]
    hi = np.where(over, nxt << np.maximum(np.uint64(64) - sh, np.uint64(0)), np.uint64(0))
    vals = (lo | hi.astype(np.uint64)) & np.uint64((1 << W) - 1)
    return vals, n, k, r


def main():
    import numpy as np
    samples, n, k, r = read_samples()
    with open(ANCH, "rb") as f:
        magic, kk = struct.unpack("<IQ", f.read(12))
        a = np.fromfile(f, dtype=np.uint64, count=2 * kk)
        anchors = a[1::2].copy()  # S values = positions
    print(f"samples={r} anchors={kk}", flush=True)
    allpos = np.concatenate([samples, anchors])
    allpos.sort()
    allpos = np.unique(allpos)
    gaps = np.diff(allpos)
    tot = int(gaps.sum())
    print(f"resolving positions: {len(allpos)}  gap-bytes total: {tot} ({tot/n:.3f}n)", flush=True)
    # distribution
    print("gap size quantiles:", np.percentile(gaps, [50, 90, 99, 99.9, 99.99, 100]).astype(int), flush=True)
    big = np.argsort(gaps)[-50:][::-1]
    print("top-50 gaps (size, bottom position):", [(int(gaps[i]), int(allpos[i])) for i in big[:10]], flush=True)
    # periodicity of the top 20 gaps via flat seeks
    with open(FLAT, "rb") as flat:
        nonper_sum = 0
        checked = 0
        for i in big[:20]:
            lo = int(allpos[i]) + 1
            ln = int(gaps[i])
            flat.seek(lo)
            span = flat.read(min(ln, 4096))
            per = None
            for q in range(1, min(len(span) // 2, 64) + 1):
                if all(span[j] == span[j - q] for j in range(q, len(span))):
                    per = q
                    break
            checked += 1
            tag = f"period={per}" if per else "NONPERIODIC"
            if per is None:
                nonper_sum += ln
            print(f"  gap at {lo}: len={ln} {tag}", flush=True)
        print(f"top-20 checked; nonperiodic bytes among them: {nonper_sum}", flush=True)
    print("H466 GAP SCAN DONE", flush=True)


if __name__ == "__main__":
    main()
