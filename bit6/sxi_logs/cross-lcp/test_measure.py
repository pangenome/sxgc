#!/usr/bin/env python3
"""Independent tiny-input differential checks; no retained corpus reads."""
import json
import pathlib
import struct
import subprocess
import tempfile
import unittest

BIN = '/tmp/cross-lcp-measure'


def crle(path, text, offset):
    order = sorted(range(len(text)), key=lambda i: (text[i:] + text[:i], i))
    runs = []
    for pos in order:
        char = text[(pos - 1) % len(text)]
        if not runs or runs[-1][0] != char:
            runs.append([char, 0, pos, pos])
        runs[-1][1] += 1
        runs[-1][3] = pos
    data = struct.pack('<4sIQQQ', b'SXCR', 1, offset, len(text), len(runs))
    data += b''.join(struct.pack('<BIQQ', *r) for r in runs)
    path.write_bytes(data)
    return runs


def lce(a, x, b, y):
    import math
    cap = len(a) + len(b) - math.gcd(len(a), len(b))
    return next((k for k in range(cap) if a[(x+k) % len(a)] != b[(y+k) % len(b)]), cap)


class MeasurementTest(unittest.TestCase):
    def run_case(self, a, b, corrupt=None, parse=False):
        with tempfile.TemporaryDirectory(prefix='cross-lcp-unit-') as directory:
            root = pathlib.Path(directory)
            (root / 'source').write_bytes(a+b)
            (root / 'remap').write_bytes(bytes(range(256)))
            ra = crle(root / 'chunk-0.crle', a, 0)
            crle(root / 'chunk-1.crle', b, len(a))
            if corrupt:
                corrupt(root)
            if parse:
                # Whole source represented by one phrase: leading dollar is
                # virtually extended to window 10; terminal padding is 10.
                if callable(parse):
                    parse(root, a+b)
                else:
                    (root / 'parse.dict').write_bytes(b'\x02'+a+b+b'\x02'*10+b'\x01\x00')
                    (root / 'parse.parse').write_bytes(struct.pack('<I', 1))
            result = subprocess.run([BIN, str(root/'source'), str(root), '2',
                                     str(root/'remap'), str(root/'parse') if parse else '-',
                                     '100', '71'], capture_output=True, text=True)
            return result, ra

    def test_nearest_oracle_one_b_run(self):
        # B has one run and one possible sampled head, eliminating RNG dependence.
        for a, b in [(b'zazb\x1e', b'\x1e'*5), (b'\x1e'*7, b'\x1e'*5),
                     (b'abc\x1e', b'\x1e')]:
            result, ra = self.run_case(a, b)
            self.assertEqual(result.returncode, 0, result.stderr)
            rows = [json.loads(x) for x in result.stdout.splitlines()]
            measured = next(x for x in rows if x['type'] == 'boundary')
            endpoints = [p for _, length, head, tail in ra for p in ([head, tail] if length > 1 else [head])]
            def less(x):
                k = lce(a, x, b, 0)
                import math
                return k < len(a)+len(b)-math.gcd(len(a), len(b)) and a[(x+k)%len(a)] < b[k%len(b)]
            insertion = next((j for j,x in enumerate(endpoints) if not less(x)), len(endpoints))
            candidates = ([endpoints[insertion-1]] if insertion else []) + ([endpoints[insertion]] if insertion < len(endpoints) else [])
            values = [lce(a, x, b, 0) for x in candidates]
            self.assertEqual(measured['both']['count'], 100*len(values))
            self.assertEqual(measured['both']['sum'], 100*sum(values))
            self.assertEqual(measured['best']['sum'], 100*max(values))
            self.assertEqual(measured['not_fully_verified_queries'], 0)

    def test_projected_dictionary_excludes_seam_phrase(self):
        result, _ = self.run_case(b'ab\x1e', b'\x1e'*3, parse=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        row = next(json.loads(x) for x in result.stdout.splitlines() if json.loads(x)['type'] == 'dictionary')
        self.assertEqual((row['distinct_a'], row['distinct_b'], row['intersection']), (0, 0, 0))

    def test_rejects_truncated_chunk(self):
        def truncate(root):
            p = root/'chunk-0.crle'
            p.write_bytes(p.read_bytes()[:-1])
        result, _ = self.run_case(b'ab\x1e', b'\x1e', truncate)
        self.assertEqual(result.returncode, 2)
        self.assertIn('short chunk word', result.stderr)

    def test_projected_dictionary_shared_phrases(self):
        def write_parse(root, source):
            padded = b'\x02'*10 + source + b'\x02'*10
            occurrences = [padded[i:i+30] for i in range(0, 121, 20)]
            dictionary = sorted(set(occurrences))
            ids = [dictionary.index(p)+1 for p in occurrences]
            encoded = [p[9:] if p.startswith(b'\x02'*10) else p for p in dictionary]
            (root/'parse.dict').write_bytes(b'\x01'.join(encoded)+b'\x01\x00')
            (root/'parse.parse').write_bytes(struct.pack('<'+'I'*len(ids), *ids))
        text = (b'a'*29+b'\x1e')*2
        result, _ = self.run_case(text, text, parse=write_parse)
        self.assertEqual(result.returncode, 0, result.stderr)
        row = next(json.loads(x) for x in result.stdout.splitlines() if json.loads(x)['type'] == 'dictionary')
        self.assertEqual((row['distinct_a'], row['distinct_b'], row['intersection']), (2, 2, 2))
        self.assertEqual((row['shared_occurrences_a'], row['shared_occurrences_b']), (2, 2))
        self.assertEqual((row['shared_bytes_a'], row['shared_bytes_b']), (40, 40))

    def test_rejects_wrong_source_size(self):
        result, _ = self.run_case(b'ab\x1e', b'\x1e', lambda root: (root/'source').write_bytes(b'x'))
        self.assertEqual(result.returncode, 2)
        self.assertIn('source size mismatch', result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
