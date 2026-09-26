#!/usr/bin/env python3
"""Probe 2: validate the .ri4-only position decode against the .agg.

Model (mirrors xsa/src/main.rs `s_at`, v4 mirrored-sample convention):
  sample(i) = (n-1) - SA[run_end(i)]          [verified by probe 1]
  SA[row]   = (n-1) - (sample(run') - steps)   where the walk from `row`
              applies LF until it lands on a run END of run r', steps = #LF.
  Hence: run end  -> steps=0, O(1).
         run head -> walk.
Checks saLast[i], saFirst[i], and (for interiorMin's LCE) the previous row.
Reports walk-length distribution and whether any walk touches a 0x0A row."""
import struct, sys
from collections import Counter

def load_agg(p):
    b = open(p, 'rb').read()
    R, = struct.unpack_from('<Q', b, 4); off = 12
    arr = {}
    for name in ('topLCP', 'saFirst', 'saLast', 'interiorMin'):
        arr[name] = struct.unpack_from('<%dQ' % R, b, off); off += 8 * R
    return R, arr

def load_ri4(p):
    b = open(p, 'rb').read()
    magic, ver, n, k, R = struct.unpack_from('<IIQQQ', b, 0)
    assert magic == 0x52585349 and ver == 4
    off = 32 + 2048
    a = b[off:off + R]; off += R
    l = struct.unpack_from('<%dI' % R, b, off); off += 4 * R
    bits, width = struct.unpack_from('<QB', b, off); off += 9
    nwords = (bits + 63) // 64
    words = struct.unpack_from('<%dQ' % nwords, b, off)
    def get(i):
        bit0 = i * width; w = bit0 >> 6; b0 = bit0 & 63
        if b0 + width <= 64:
            return (words[w] >> b0) & ((1 << width) - 1)
        return ((words[w] >> b0) | (words[w + 1] << (64 - b0))) & ((1 << width) - 1)
    return n, k, R, a, l, width, get

def build_lf(n, R, a, l):
    # row starts
    starts = [0] * (R + 1)
    acc = 0
    for i in range(R):
        starts[i] = acc; acc += l[i]
    starts[R] = acc
    assert acc == n, (acc, n)
    C = [0] * 256
    for c in range(256):
        C[c] = sum(l[i] for i in range(R) if a[i] < c)
    # per-char run lists + prefix sums of lengths
    cruns = [[] for _ in range(256)]
    csum = [[] for _ in range(256)]
    cnt = [0] * 256
    for i in range(R):
        c = a[i]; cruns[c].append(i); csum[c].append(cnt[c]); cnt[c] += l[i]
    for c in range(256):
        csum[c].append(cnt[c])
    def run_of(row):
        lo, hi = 0, R - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if starts[mid] <= row: lo = mid
            else: hi = mid - 1
        return lo
    def rank_c(c, row):
        if row == 0: return 0
        if row >= n: return cnt[c]
        r = run_of(row); s = starts[r]
        v = cruns[c]
        # count runs of c before r
        j = 0
        lo, hi = 0, len(v)
        while lo < hi:
            mid = (lo + hi) // 2
            if v[mid] < r: lo = mid + 1
            else: hi = mid
        j = lo
        if a[r] == c: return csum[c][j] + (row - s)
        return csum[c][j]
    def lf(row):
        c = a[run_of(row)]
        return C[c] + rank_c(c, row)
    return starts, run_of, lf

def main(aggp, ri4p):
    Ra, agg = load_agg(aggp)
    n, k, R, a, l, width, get = load_ri4(ri4p)
    assert Ra == R
    starts, run_of, lf = build_lf(n, R, a, l)
    N1 = n - 1
    def walk_sa(row):
        pos, steps = row, 0
        touch0a = False
        while True:
            r = run_of(pos)
            e = starts[r] + l[r]
            if pos == e - 1:
                return N1 - ((get(r) - steps) & ((1 << 64) - 1)), steps, touch0a
            if a[r] == 0x0A:
                touch0a = True
                return None, steps, touch0a
            pos = lf(pos); steps += 1
    n0a = sum(1 for i in range(R) if a[i] == 0x0A)
    okL = okF = 0; badL = badF = 0
    hist = Counter(); maxwalk = 0; touch = 0; fail = 0
    for i in range(R):
        s = get(i)
        st = starts[i]; b = st + l[i] - 1
        # saLast: row b is a run end -> O(1)
        sl = N1 - s
        if sl == agg['saLast'][i]: okL += 1
        else: badL += 1
        # saFirst: walk from head
        v, w, t = walk_sa(st)
        if v is None:
            fail += 1
            if t: touch += 1
            continue
        maxwalk = max(maxwalk, w); hist[min(w, 20)] += 1
        if v == agg['saFirst'][i]: okF += 1
        else: badF += 1
    print(f"{ri4p.split('/')[-1]}: n={n} R={R} runs_0x0A={n0a}")
    print(f"  saLast by O(1) mirror      : {okL}/{R} (bad {badL})")
    print(f"  saFirst by LF walk         : {okF}/{R} (bad {badF}, walk-hit-0x0A {fail})")
    print(f"  walk steps: max={maxwalk} hist(capped@20)={dict(sorted(hist.items()))}")

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
