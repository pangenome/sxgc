"""Independent brute occurrence/owned-tail enumeration, no corpus access."""
from collections import Counter
import ctypes as C
import hashlib
from io import BytesIO
from pathlib import Path
import random
import sys

from overlap_scanplan_census import CELLS, U64, census, checked, native_chunk, plan


def cut(window):
    h = 1469598103934665603
    for x in window:
        h = ((h ^ x) * 1099511628211) & U64
    return h % 31 == 0


def brute(data):
    starts = [0] + [i for i in range(1, len(data)-7) if cut(data[i:i+8])]
    hist, lengths, strides = Counter(), [], []
    distinct = {}
    for j, s in enumerate(starts):
        terminal = j+1 == len(starts)
        end = len(data) if terminal else starts[j+1]
        word = data[s:end if terminal else end+8]
        lengths.append(len(word)); strides.append(end-s)
        keys = []
        for i in range(end-s):
            tail = word[i:]
            key = tail[0]*257 + (tail[1]+1 if len(tail)>1 else 0)
            hist[key] += 1; keys.append(key)
        distinct[(word, terminal)] = keys
    return starts, lengths, strides, hist, Counter(k for keys in distinct.values() for k in keys)


def fixtures(lib):
    if not __debug__:
        raise RuntimeError('fixtures require assertions')
    last = next(bytes([b])*8 for b in range(256) if cut(bytes([b])*8))
    # Two adjacent triggers straddle UINT32_MAX; seeded prefix state avoids
    # any large input, and the count must exclude the equal-to-MAX start.
    s = 0xffffffff
    state = (C.c_uint64 * 5)(s-1, 1, 0, 0, 0)
    native_chunk(lib)(last + last[:2], 8, 10, s+7, (C.c_uint64 * CELLS)(), state)
    assert list(state) == [s+1, 3, 1, 9, 1]
    for data in (b'\xff', last, b'z'+last, last*9, bytes(range(256)), random.Random(325).randbytes(701)):
        starts, lengths, strides, hist, unique = brute(data)
        for block in (1, 7, 16, 1<<20):
            c = census(lib, BytesIO(data), len(data), lambda: None, block)
            assert dict(c['cells']) == hist and c['global_end_rows']==1
            assert c['sha256']==hashlib.sha256(data).hexdigest() and c['alphabet']==len(set(data))
            assert (c['m'], c['max_phrase'], c['max_owned_stride'], c['starts_above_u32'],
                    c['eof_start'], c['eof_raw_length']) == (len(starts), max(lengths), max(strides), 0, starts[-1], lengths[-1])
        cap = max(hist.values())
        p = plan(c, 8*cap)
        assert sum(v for _,_,v in p['ranges']) == len(data)
        for a,b,v in p['ranges']:
            assert sum(z for i,z in unique.items() if a<=i<=b) <= v == sum(z for i,z in hist.items() if a<=i<=b) <= cap
        if cap>1:
            assert plan(c,8*(cap-1))['ranges'] is None
    try:
        checked(U64+1)
    except OverflowError:
        pass
    else:
        raise AssertionError('unchecked extent')


if __name__ == '__main__':
    fixtures(Path(sys.argv[1]).resolve()); print('PASS brute trigger, EOF, row-mass and packing fixtures')
