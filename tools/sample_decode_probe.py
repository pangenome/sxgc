#!/usr/bin/env python3
"""Probe: can saFirst/saLast be decoded from the .ri4's own per-run SA
sample array, with no M/b_bwt/w_wt (no pfp_sa_support)?

.agg = "CRA1" u32, R u64, then 4*R u64 LE: topLCP, saFirst, saLast, interiorMin
.ri4 v4 = u32 magic, u32 ver, u64 n, u64 k, u64 R, C[256] u64,
          a[R] u8, l[R] u32, then sdsl int_vector<w>: u64 bits, u8 width,
          ceil(bits/64)*8 bytes LSB-first words.

Checks per run i (rows a=start[i], b=a+l[i]-1):
  C1 saLast[i] == sample(i)               (sample stored at run END)
  C2 saFirst[i] == saLast[i] + (l[i]-1)   (descending within run)
  C3 saFirst[i] == sample(i)              (sample stored at run HEAD)
  C4 walk-model: saFirst[i] == saLast[i] + (l[i]-1) holds for ALL rows
Report per-text counts; also verify sample(i) != INF (i.e. real samples)."""
import struct, sys

def load_agg(p):
    b = open(p, 'rb').read()
    magic, = struct.unpack_from('<I', b, 0)
    assert magic == 0x31415243, hex(magic)
    R, = struct.unpack_from('<Q', b, 4)
    off = 12
    arr = {}
    for name in ('topLCP', 'saFirst', 'saLast', 'interiorMin'):
        arr[name] = struct.unpack_from('<%dQ' % R, b, off); off += 8 * R
    return R, arr

def load_ri4(p):
    b = open(p, 'rb').read()
    magic, ver, n, k, R = struct.unpack_from('<IIQQQ', b, 0)
    assert magic == 0x52585349 and ver == 4, (hex(magic), ver)
    off = 32 + 2048
    a = b[off:off + R]; off += R
    l = struct.unpack_from('<%dI' % R, b, off); off += 4 * R
    bits, width = struct.unpack_from('<QB', b, off); off += 9
    nwords = (bits + 63) // 64
    words = struct.unpack_from('<%dQ' % nwords, b, off)
    def get(i):
        # LSB-first bit access (sdsl int_vector<width>)
        bit0 = i * width
        w = bit0 >> 6; b0 = bit0 & 63
        v = (words[w] >> b0) & ((1 << width) - 1) if b0 + width <= 64 else \
            ((words[w] >> b0) | (words[w + 1] << (64 - b0))) & ((1 << width) - 1)
        return v
    return n, k, R, a, l, bits, width, get

def main(aggp, ri4p):
    Ra, agg = load_agg(aggp)
    n, k, R, a, l, bits, width, get = load_ri4(ri4p)
    assert Ra == R, (Ra, R)
    INF = (1 << width) - 1
    c1 = c2 = c3 = 0
    s_inf = 0
    examples = []
    for i in range(R):
        s = get(i)
        if s == INF:
            s_inf += 1
            continue
        sf, sl = agg['saFirst'][i], agg['saLast'][i]
        if sf == INF or sl == INF:
            continue
        if sl == s: c1 += 1
        if sl == s and sf == s + (l[i] - 1): c2 += 1
        if sf == s: c3 += 1
        if len(examples) < 4:
            examples.append((i, l[i], s, sf, sl))
    print(f"{ri4p.split('/')[-1]}: n={n} R={R} sa_width={width} samples_INF={s_inf}")
    print(f"  C1 saLast==sample        : {c1}/{R}")
    print(f"  C2 saFirst==saLast+l-1   : {c2}/{R}")
    print(f"  C3 saFirst==sample       : {c3}/{R}")
    for e in examples:
        print(f"  ex run={e[0]} len={e[1]} sample={e[2]} saFirst={e[3]} saLast={e[4]} "
              f"(sF-sL={e[3]-e[4]}, l-1={e[1]-1})")

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
