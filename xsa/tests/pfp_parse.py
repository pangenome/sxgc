"""Independent column and parse-rank oracle: pfp_parse.py PFP_PARSE STREAM_RANK."""
from collections import Counter
import os
from pathlib import Path
import random
import re
import struct
import subprocess as sp
import sys
import tempfile

W = 8


def trigger(word):
    h = 1469598103934665603
    for b in word:
        h = ((h ^ b) * 1099511628211) & ((1 << 64) - 1)
    return h % 31 == 0


def model(text):
    starts = [0] + [i for i in range(1, len(text)-W+1) if trigger(text[i:i+W])]
    words = [(text[a:b+W], False) for a, b in zip(starts, starts[1:])] + [(text[starts[-1]:], True)]
    lex = sorted(set(words)); ids = {word: i for i, word in enumerate(lex)}
    tape = [ids[w] for w in words]; counts = Counter(tape)
    first = dict(zip(reversed(tape), reversed(starts)))
    refs = [(first[i], len(w) if eof else len(w)-W, counts[i]) for i, (w, eof) in enumerate(lex)]
    return tape, refs


def run(cmd, ok=True):
    p = sp.run(list(map(str, cmd)), capture_output=True, timeout=60)
    assert (p.returncode == 0) == ok, (cmd, p.stderr[-2000:])
    return p.stderr


def check(front, rank, text, workers=1, mem=64<<20):
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp); raw = d/'raw'; out = d/'out'; out.mkdir(); raw.write_bytes(text)
        log = run([front, raw, out, workers, mem, 64<<20])
        assert {p.name for p in out.iterdir()} == {'parse.u64', 'phrases.ref', 'histogram'}
        tape, refs = model(text); m = len(tape)
        assert (out/'parse.u64').read_bytes() == struct.pack(f'<{m}Q', *tape)
        assert (out/'phrases.ref').read_bytes() == b''.join(struct.pack('<QQQ', *r) for r in refs)
        freq = Counter(text)
        assert (out/'histogram').read_bytes() == struct.pack('<256Q', *(freq[i] for i in range(256)))
        if mem == 512<<10:
            assert int(re.search(rb'external_merge_passes=(\d+)', log)[1]) > 0, log
        run([rank, 'inverse', out/'parse.u64', d/'inverse', workers, 64<<20])
        inv = struct.unpack(f'<{m}Q', (d/'inverse').read_bytes()); order = sorted(range(m), key=inv.__getitem__)
        # Permutation + strictly increasing (first token, successor rank), END=0,
        # uniquely characterizes suffix order. No quadratic suffix slices on long runs.
        prev = (-1, 0)
        for r, j in enumerate(order, 1):
            key = (tape[j], inv[j+1] if j+1 < m else 0)
            assert inv[j] == r and key > prev; prev = key
        return tape, refs


def main(front, rank):
    witness = ((b'a'+15*b'b')*5)[:70] + b'Z'
    assert model(witness) == ([0,2,2,2,1], [(0,11,1),(59,12,1),(11,16,3)])
    for n in range(1, 8): check(front, rank, b'abcdefg'[:n])
    for text in [witness, 32*b',', b'AB\x00CD\x1eEF\xffGHaaabbbab',
                 random.Random(732).randbytes(8192).replace(b'\xff', b'\x00'), 65537*b'a']:
        check(front, rank, text)
    if len(os.sched_getaffinity(0)) >= 2: check(front, rank, witness, workers=2)
    # Final-window and overlapping triggers, read-buffer seam, many sort runs.
    check(front, rank, 320000*b',', mem=512<<10)
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp); raw=d/'raw'; raw.write_bytes(witness); out=d/'a'; out.mkdir()
        assert b'scratch' in run([front,raw,out,1,64<<20,8*5+24*3+2048-1], False)
        raw.write_bytes(bytes(range(256))); out=d/'b'; out.mkdir()
        assert b'256' in run([front,raw,out,1,64<<20,64<<20], False)
    print('PASS lexical tape, first references, census, histogram, parse suffix ranks, external naming and refusals')

if __name__ == '__main__': main(*(Path(s).resolve() for s in sys.argv[1:]))
