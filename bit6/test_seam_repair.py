#!/usr/bin/env python3
"""Bounded, independent dense cyclic tests; expanded arrays exist only here."""
import argparse
import itertools
import json
import pathlib
import random
import struct
import subprocess
import time
from test_sxi_separator import cyclic_frame


def put_frame(prefix, text):
    _, _, runs = cyclic_frame(text + b'\x02' * 10)
    pathlib.Path(str(prefix) + '.rlebwt.meta').write_bytes(struct.pack('<2Q', len(text)+10, len(runs)))
    pathlib.Path(str(prefix) + '.rlebwt').write_bytes(b''.join(struct.pack('<I', c | length << 8) for c, length, _, _ in runs))
    for ext, col in [('.ssa', 2), ('.ssa_t', 3)]:
        values = [len(runs)] + [r[col] for r in runs]
        pathlib.Path(str(prefix) + ext).write_bytes(struct.pack('<' + 'Q'*len(values), *values))
    pad = b'\x02'*10
    pathlib.Path(str(prefix) + '.dict').write_bytes(b'\x02'+(text+pad)[:10]+b'\x01'+text+pad+b'\x01\x00')
    pathlib.Path(str(prefix) + '.parse').write_bytes(struct.pack('<2I', 1, 2))


def read_frame(ri, heads):
    raw = ri.read_bytes()
    n, k, r = struct.unpack_from('<3Q', raw, 8)
    chars = raw[2080:2080+r]
    lengths = struct.unpack_from('<'+'I'*r, raw, 2080+r)
    hs = struct.unpack('<'+'Q'*r, heads.read_bytes())
    bits, width = struct.unpack_from('<QB', raw, 2080+5*r)
    assert bits == r*width
    packed = int.from_bytes(raw[2089+5*r:], 'little')
    tails = [n-1-((packed >> (i*width)) & ((1 << width)-1)) for i in range(r)]
    return [list(v) for v in zip(chars, lengths, hs, tails)]


def dense_witnesses(text, sa, bwt):
    """Full-row scan: no PFP, Phi samples, or per-run aggregate shortcuts."""
    n = len(text)
    lcp = [0]
    for a, b in zip(sa, sa[1:]):
        z = 0
        while z < n and text[(a+z) % n] == text[(b+z) % n]:
            z += 1
        lcp.append(z)
    length, position, active = {}, {}, set()
    result, minimum = [], n+1
    for i in range(1, n):
        minimum = min(minimum, lcp[i])
        if bwt[i] == bwt[i-1]:
            continue
        for c in list(length):
            if minimum < length[c]:
                if c in active:
                    result.append(position[c])
                    active.remove(c)
                length[c] = minimum
        for row in (i-1, i):
            c = bwt[row]
            if lcp[i] > length.get(c, -1):
                length[c] = lcp[i]
                # Zero-based coordinate of the preceding character in reverse T.
                position[c] = n-1-((sa[row]-1) % n)
                active.add(c)
        minimum = n+1
    result.extend(position[c] for c in active)
    return sorted(result), lcp


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tools', required=True)
    p.add_argument('--xsa', required=True)
    p.add_argument('--work', required=True)
    a = p.parse_args()
    work, tools = pathlib.Path(a.work), pathlib.Path(a.tools)
    work.mkdir(parents=True, exist_ok=True)
    prefix, ri, heads = work/'parse', work/'out.ri4', work/'out.heads'
    count = 0
    repaired = 0
    fixtures = [bytes(t) for n in range(1, 10) for t in itertools.product(b'AB', repeat=n)]
    fixtures += [bytes(t) for n in range(1, 7) for t in itertools.product(b'ABC', repeat=n)]
    rng = random.Random(30927)
    fixtures += [bytes(rng.choice(b'ACGT\x1e') for _ in range(rng.randint(12, 240))) for _ in range(100)]
    for text in fixtures:
        put_frame(prefix, text)
        result = subprocess.run([str(tools/'rpfbwt_endpoints'), str(prefix), str(ri), str(heads), f'{text[-1]:02x}'], capture_output=True)
        assert result.returncode == 0, (text, result.stderr)
        assert read_frame(ri, heads) == cyclic_frame(text)[2], text
        repaired += b'CYCLIC_SEAM_REPAIRED' in result.stderr
        count += 1
    print(json.dumps(dict(endpoint_cases=count, repaired=repaired, bad=0)), flush=True)
    # Two billion conceptual text bytes, represented by six raw runs. There
    # is deliberately no text, dictionary or parse for this refusal gate.
    m = 10**9
    huge = work/'huge'
    runs = [(65, 1, 2*m+1, 2*m+1), (2, 9, 2*m+2, 2*m+10),
            (65, m-1, 2*m, m+2), (66, 1, m+1, m+1),
            (2, 1, 0, 0), (65, m, 1, m)]
    pathlib.Path(str(huge)+'.rlebwt.meta').write_bytes(struct.pack('<2Q', 2*m+11, 6))
    words = []
    for c, size, _, _ in runs:
        while size:
            part = min(size, 0x7fffff)
            size -= part
            words.append(c | part << 8 | (0x80000000 if size else 0))
    pathlib.Path(str(huge)+'.rlebwt').write_bytes(struct.pack('<'+'I'*len(words), *words))
    for ext, col in [('.ssa', 2), ('.ssa_t', 3)]:
        pathlib.Path(str(huge)+ext).write_bytes(struct.pack('<7Q', 6, *(r[col] for r in runs)))
    huge_ri, huge_heads = work/'huge.ri4', work/'huge.heads'
    began = time.monotonic()
    result = subprocess.run([str(tools/'rpfbwt_endpoints'), str(huge), str(huge_ri), str(huge_heads), '41'], capture_output=True)
    assert result.returncode != 0 and b'CYCLIC_SEAM_REFUSED class_size=2000000000 limit=1000' in result.stderr, result.stderr
    assert not huge_ri.exists() and not huge_heads.exists()
    print(json.dumps(dict(refusal_n=2*m+1, raw_runs=6, rlebwt_bytes=4*len(words),
                          wall_seconds=time.monotonic()-began, stderr=result.stderr.decode())), flush=True)
    witnesses = 0
    for text in [b'A', b'AAAA', b'ABA'*17, b'ACG\x1e'*21, b'A'*128+b'B'+b'A'*128,
                 b'AC\nGT\x1eAC\x1e', b'GATTACA\x1e', *fixtures[-30:]]:
        put_frame(prefix, text)
        subprocess.run([str(tools/'rpfbwt_endpoints'), str(prefix), str(ri), str(heads), f'{text[-1]:02x}'], check=True, capture_output=True)
        agg, chi = work/'out.agg', work/'out.chi'
        result = subprocess.run([str(tools/'slim_dump'), '--slim', '--resolve-ri4', '--dict-stream', '--ri4', str(ri), '--head-sa', str(heads), '--parse', str(prefix), '-t', '2', '-o', str(agg)], capture_output=True)
        assert result.returncode == 0, (text, result.stderr)
        sa, bwt, runs = cyclic_frame(text)
        want, lcp = dense_witnesses(text, sa, bwt)
        raw = agg.read_bytes()
        assert struct.unpack_from('<IQ', raw) == (0x31415243, len(runs))
        fields = struct.unpack_from('<'+'Q'*(4*len(runs)), raw, 12)
        row = 0
        for i, (_, length, first, last) in enumerate(runs):
            # The minimum of adjacent full-row LCP values equals endpoint LCE.
            expected = [lcp[row], first, last, min(lcp[row+1:row+length]) if length > 1 else 2**64-1]
            assert [fields[j*len(runs)+i] for j in range(4)] == expected, (text, i, expected)
            row += length
        for stream in (False, True):
            command = [a.xsa, 'chi-rspace', '--ri4', str(ri), '--agg', str(agg), '-o', str(chi)]
            if stream:
                command += ['--stream-agg']
            result = subprocess.run(command, capture_output=True)
            assert result.returncode == 0, result.stderr
            got = sorted(v[0] for v in struct.iter_unpack('<Q', chi.read_bytes()))
            assert got == want and all(0 <= x < len(text) for x in got), (text, got, want)
            witnesses += 1
    print(json.dumps(dict(dense_witness_and_aggregate_checks=witnesses, bad=0)), flush=True)


if __name__ == '__main__':
    main()
