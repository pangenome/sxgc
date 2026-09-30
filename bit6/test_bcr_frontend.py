#!/usr/bin/env python3
"""Small independent cyclic-SA oracle and optional cross-producer byte gate."""
import argparse
import pathlib
import random
import struct
import subprocess
import tempfile

EXT = ('.rlebwt', '.rlebwt.meta', '.ssa', '.ssa_t')


def oracle(text):
    padded = text + b'\x02' * 10
    sa = sorted(range(len(padded)), key=lambda i: padded[i:] + padded[:i])
    bwt = bytes(padded[(i - 1) % len(padded)] for i in sa)
    runs = []
    for i, c in enumerate(bwt):
        if not i or c != bwt[i - 1]:
            runs.append([c, 0, sa[i], sa[i]])
        runs[-1][1] += 1
        runs[-1][3] = sa[i]
    words = bytearray()
    freq = [bwt.count(c) for c in range(256)]
    rc = [sum(r[0] == c for r in runs) for c in range(256)]
    for c, length, _, _ in runs:
        while length:
            take = min(length, 0x7fffff)
            length -= take
            words += struct.pack('<I', c | take << 8 | (0x80000000 if length else 0))
    return dict(zip(EXT, (
        bytes(words), struct.pack('<514Q', len(padded), len(runs), *freq, *rc),
        struct.pack('<' + 'Q' * (len(runs) + 1), len(runs), *(r[2] for r in runs)),
        struct.pack('<' + 'Q' * (len(runs) + 1), len(runs), *(r[3] for r in runs)),
    )))


def compare(a, b):
    for ext in EXT:
        x = pathlib.Path(str(a) + ext).read_bytes()
        y = pathlib.Path(str(b) + ext).read_bytes()
        assert x == y, (ext, len(x), len(y), next((i for i, p in enumerate(zip(x, y)) if p[0] != p[1]), None))


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--binary', required=True)
    p.add_argument('--pfp')
    p.add_argument('--rpfbwt')
    a = p.parse_args()
    rng = random.Random(466)
    docs = [b'ACGACGTACG', b'ACGACGTACC', b'TTTTGCACTG', b'ACGACGTACG', b'GGTCAATC']
    cases = [b'\x1e'.join(docs) + b'\x1e', b'A' * 53 + b'\x1e' + b'A' * 50 + b'B\x1e', b'\x1e\x1eA\x1e']
    for _ in range(30):
        cases.append(b'\x1e'.join(bytes(rng.choice(b'ACGT') for _ in range(rng.randrange(1, 32))) for _ in range(4)) + b'\x1e')
    with tempfile.TemporaryDirectory(prefix='bcr-gate-') as d:
        d = pathlib.Path(d)
        for j, text in enumerate(cases):
            source = d / f'{j}.txt'
            source.write_bytes(text)
            out = d / f'{j}.bcr'
            subprocess.run([a.binary, source, out], check=True, capture_output=True)
            want = oracle(text)
            for ext in EXT:
                assert pathlib.Path(str(out) + ext).read_bytes() == want[ext], (j, ext)
        print(f'PASS independent cyclic-SA oracle: {len(cases)} collections, all four files')
        if a.pfp and a.rpfbwt:
            # Larger, near-duplicate and distinct records give the two-level
            # PFP route enough phrases for rpfbwt's tiny-input preconditions.
            base = bytes(rng.choice(b'ACGT') for _ in range(2000))
            near = bytearray(base)
            for _ in range(20):
                near[rng.randrange(len(near))] = rng.choice(b'ACGT')
            distinct = bytes(rng.choice(b'ACGT') for _ in range(2200))
            regular = b'\x1e'.join([base, bytes(near), distinct, base, bytes(near[:1000]) + distinct[:500]]) + b'\x1e'
            alphabet = bytes([0, 1, 2, 3, 4, 5, 6, 7, 10, 13, 65, 66, 67, 68])
            remapped = b'\x1e'.join(bytes(rng.choice(alphabet) for _ in range(2000)) for _ in range(5)) + b'\x1e'
            for label, text in [('regular', regular), ('remapped', remapped)]:
                source, pfp, out = d / (label + '.txt'), d / (label + '.pfp'), d / (label + '.bcr')
                source.write_bytes(text)
                subprocess.run([a.pfp, '-t', source, '-o', pfp, '-w', '10', '-p', '100', '-j', '2', '--tmp-dir', d], check=True, capture_output=True)
                subprocess.run([a.pfp, '-i', str(pfp) + '.parse', '-w', '5', '-p', '11', '-j', '2', '--tmp-dir', d], check=True, capture_output=True)
                subprocess.run([a.rpfbwt, '--l1-prefix', pfp, '--w1', '10', '--w2', '5', '--threads', '2', '--chunks', '1', '--tmp-dir', d], check=True, capture_output=True)
                subprocess.run([a.binary, source, out, '--remap', str(pfp) + '.remap'], check=True, capture_output=True)
                compare(out, pfp)
                changes = sum(i != v for i, v in enumerate(pathlib.Path(str(pfp) + '.remap').read_bytes()))
                if label == 'remapped':
                    assert changes > 0
                print(f'PASS PFP byte identity: {label}, {len(text)} bytes, remap entries changed={changes}, all four files')


if __name__ == '__main__':
    main()
