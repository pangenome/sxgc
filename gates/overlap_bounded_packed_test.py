"""Prepared fixture columns only. Compare every row (including END) and inverse rank."""
import os
import random
import re
import struct
import subprocess
import tempfile
from itertools import takewhile
from pathlib import Path

from overlap_resident_test import BIN, prepare
from overlap_row43_test import wire

PRODUCER = BIN.with_name('overlap_bounded_packed')
BINS = 16 * 65536


def brute(text):
    rows = bytearray()
    previous = b''
    for p in sorted(range(len(text) + 1), key=lambda p: text[p:]):
        suffix = text[p:]
        h = sum(1 for _ in takewhile(lambda pair: pair[0] == pair[1], zip(suffix, previous)))
        rows.extend(wire(p, h, text[p-1] if p else 256))
        previous = suffix
    return bytes(rows)


def tie_columns(d, distinct=2, common=0, repeats=1):
    # Multiple offsets/IDs share '9ABCDEFGH'; their continuation ranks interleave.
    words = [b'ABCDEFGH' + bytes([97+i]) + b'z'*common + b'9ABCDEFGH'
             for i in range(distinct)] + [b'ABCDEFGHx']
    parse = ([*range(distinct), 0, *reversed(range(distinct))] * repeats) + [distinct]
    cols = d/'columns'
    (cols/'parse.u32').write_bytes(struct.pack(f'<{len(parse)}I', *parse))
    (cols/'dictionary.packed').write_bytes(b''.join(
        struct.pack('<I', len(w) | (int(i == distinct) << 31)) + w for i, w in enumerate(words)))
    ranks = [0] * len(parse)
    for r, i in enumerate(sorted(range(len(parse)), key=lambda j: parse[j:]), 1):
        ranks[i] = r
    (d/'expected').write_bytes(struct.pack(f'<{len(parse)}I', *ranks))
    return b''.join(words[i][:-8] for i in parse[:-1]) + words[-1]


def main():
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        cols, flag = d/'columns', d/'stop'
        env = dict(os.environ, XSA_TEST_ROW43_FIXTURE='1', XSA_RESIDENT_PACKED='1',
                   XSA_RESIDENT_WIDE_DICT='1')

        def run(arena=BINS+100_000, grant=2_000_000, oracle=True):
            args = [PRODUCER, '--fixture', cols, flag, str(grant), str(arena)]
            return subprocess.run(args + ([d/'expected'] if oracle else []), capture_output=True)

        def compact(cap, arena=100_000):
            return subprocess.run([PRODUCER, '--fixture-compact', cols, flag, '2000000',
                                   str(arena), str(cap), d/'expected'], capture_output=True)

        def check(text, arena):
            a = run(arena)
            b = subprocess.run([BIN, cols, flag, '200000000'], env=env, capture_output=True)
            assert a.returncode == b.returncode == 0, (a.stderr, b.stderr)
            assert a.stdout == brute(text) == b.stdout
            for cap in (1, 2):
                c = compact(cap)
                assert c.returncode == 0 and c.stdout == a.stdout, c.stderr
            narrow = subprocess.run([BIN, cols, flag, '200000000', d/'expected'],
                                    env=dict(os.environ, XSA_RESIDENT_PACKED='1'), capture_output=True)
            assert narrow.returncode == 0 and b'RESIDENT_CHECK inverse_ranks=' in narrow.stderr
            return a.stderr

        for profile, n in [('100m', 100_000_503), ('1g', 1_000_000_665)]:
            plan = subprocess.run([PRODUCER, '--plan-component', profile], capture_output=True, check=True)
            assert f'n={n} '.encode() in plan.stdout and b'launch=false' in plan.stdout
        wrong = subprocess.run([PRODUCER, '--plan-component', '100g'], capture_output=True)
        assert wrong.returncode and not wrong.stdout
        rng = random.Random(88789)
        for text in [b'x', b'banana', b'\xff\0'*50, b'a'*901,
                     bytes(rng.choices(range(32), k=2300))]:
            prepare(d, text)
            check(text, BINS+100_000)
        log = check(tie_columns(d), BINS+6*36)
        assert int(re.search(rb'max_ties=(\d+)', log)[1]) >= 2
        assert int(re.search(rb'ranges=(\d+)', log)[1]) > 1

        def refuse(result, reason):
            assert result.returncode and not result.stdout and reason in result.stderr, result.stderr

        text = tie_columns(d, distinct=8, common=95, repeats=2)
        check(text, BINS+100_000)
        # A band seam plus a windowed eight-word tie under the same small grant.
        c = compact(2, arena=14_336+40+8*768)
        assert c.returncode == 0 and c.stdout == brute(text), c.stderr
        assert int(re.search(rb'ranges=(\d+)', c.stderr)[1]) > 1
        refuse(compact(0), b'empty cursor cap')
        refuse(compact(2, arena=39), b'cursor budget')
        refuse(compact(1, arena=14_336+20+7*8), b'ExactClass')
        # Many short lists / windows: admission refuses before even the END row.
        (cols/'parse.u32').write_bytes(struct.pack('<8I', *range(8))*1000 + struct.pack('<I',8))
        result = subprocess.run([PRODUCER, '--fixture-compact', cols, flag, '2000000',
                                 '100000', '1'], capture_output=True)
        refuse(result, b'rank window work cap')
        # Loose all-descriptor estimate fails; exact admission sees only two-way
        # ties. A long unique EOF word need not pay for rank windows.
        words = [b'ABCDEFGHa9ABCDEFGH', b'ABCDEFGHb9ABCDEFGH',
                 b'ABCDEFGHx' + bytes(rng.choices(range(32), k=5000))]
        (cols/'dictionary.packed').write_bytes(b''.join(
            struct.pack('<I', len(w) | ((i==2)<<31)) + w for i,w in enumerate(words)))
        (cols/'parse.u32').write_bytes(struct.pack('<II',0,1)*1000 + struct.pack('<I',2))
        c = subprocess.run([PRODUCER, '--fixture-compact', cols, flag, '2000000',
                            '100000', '2'], capture_output=True)
        a = run(oracle=False)
        b = subprocess.run([BIN, cols, flag, '200000000'], env=env, capture_output=True)
        assert a.returncode == b.returncode == c.returncode == 0, (a.stderr,b.stderr,c.stderr)
        assert a.stdout == b.stdout == c.stdout and b'window_probes_bound=0' in c.stderr
        tie_columns(d)
        refuse(run(BINS+36), b'Bucket')
        refuse(run(BINS, BINS), b'phase grant')
        refuse(run(1), b'arena census')
        (d/'expected').write_bytes(bytes(24))
        refuse(run(), b'rank oracle')
        path = cols/'dictionary.packed'
        for raw, reason in [(b'aaaaaaaaa', b'broken packed overlap'),
                            (b'zzzzzzzzz', b'packed lexical order')]:
            (cols/'parse.u32').write_bytes(struct.pack('<II', 0, 1))
            path.write_bytes(struct.pack('<I', 9)+raw+struct.pack('<I', 0x80000008)+b'bbbbbbbb')
            refuse(run(oracle=False), reason)
        path.write_bytes(path.read_bytes()[:6])
        refuse(run(oracle=False), b'truncated packed word')
        path.write_bytes(struct.pack('<I', 9) + b'a'*9 + struct.pack('<I', 0x8000000a) + b'a'*10)
        invalid = run(oracle=False)
        assert invalid.returncode and b'finite tails not prefix-free' in invalid.stderr
        prepare(d, bytes(range(256)))
        refuse(run(), b'two free codes')
        # Refuse oversized reconstructed text before finite-tail distribution/sort.
        (cols/'parse.u32').write_bytes(bytes(4))
        length = 2**20 + 1
        path.write_bytes(struct.pack('<I', 0x80000000 + length) + b'x' * length)
        refuse(run(grant=3_000_000, oracle=False), b'text exceeds fixture cap')
        # Hold the output pipe open but bounded; a stop after the first row must
        # invalidate the partial stream, not be reported as a completed fixture.
        prepare(d, bytes(rng.choices(range(32), k=6000)))
        with subprocess.Popen([PRODUCER, '--fixture', cols, flag, '2000000', str(BINS+100000)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, pipesize=4096) as proc:
            assert len(proc.stdout.read(12)) == 12
            flag.touch()
            proc.communicate(timeout=15)
            assert proc.returncode == 75
        for a in (run(), compact(1)):
            assert a.returncode == 75 and not a.stdout
    print('PASS full row43 streams, inverse ranks, multi-range ties, cap/input/rank/stop refusals')


if __name__ == '__main__':
    main()
