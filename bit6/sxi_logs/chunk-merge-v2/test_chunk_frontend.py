#!/usr/bin/env python3
"""Independent tiny cyclic-SA/BWT and endpoint oracle for chunk_frontend."""
import pathlib
import os
import random
import struct
import subprocess
import tempfile
import unittest
import itertools

ROOT = pathlib.Path(__file__).resolve().parents[3]
BUILDER = pathlib.Path(os.environ.get('CHUNK_FRONTEND',
                                    str(ROOT / 'vendor/chunk-merge-v2/chunk_frontend')))


def cyclic_rows(text):
    sa = sorted(range(len(text)), key=lambda i: (text[i:] + text[:i], i))
    rows = []
    for pos in sa:
        char = text[(pos - 1) % len(text)]
        if not rows or rows[-1][0] != char:
            rows.append([char, 0, pos, pos])
        rows[-1][1] += 1
        rows[-1][3] = pos
    return [tuple(r) for r in rows]


def decode(path):
    data = path.read_bytes()
    assert data[:4] == b'SXCR'
    version, offset, n, r = struct.unpack_from('<IQQQ', data, 4)
    assert version == 1 and len(data) == 32 + 21*r
    rows = [struct.unpack_from('<BIQQ', data, 32 + 21*i) for i in range(r)]
    return offset, n, rows


class ChunkFrontendTests(unittest.TestCase):
    def test_exhaustive_short_cyclic_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            path, out = tmp/'input', tmp/'out'
            for length in range(1, 8):
                for symbols in itertools.product((6, 7), repeat=length):
                    source = bytes(symbols) + b'\x1e'
                    path.write_bytes(source)
                    out.mkdir()
                    completed = subprocess.run([str(BUILDER), str(path), '1', str(out)],
                                               capture_output=True, text=True)
                    self.assertEqual(completed.returncode, 0, completed.stderr)
                    offset, n, actual = decode(out/'chunk-0.crle')
                    self.assertEqual((offset, n, actual), (0, len(source), cyclic_rows(source)))
                    (out/'chunk-0.crle').unlink()
                    out.rmdir()

    def test_twenty_collections_plus_periodic(self):
        rng = random.Random(0xC4A6)
        examples = [b'aaaa\x1eaaaa\x1eaaaa\x1eaaaa\x1e',
                    b'abc\x1eabc\x1eabc\x1eabc\x1e']
        for _ in range(20):
            examples.append(b''.join(bytes(rng.randrange(6, 10) for _ in range(4))
                                     + b'\x1e' for _ in range(4)))
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            for i, source in enumerate(examples):
                path = tmp / f'input-{i}'
                out = tmp / f'out-{i}'
                path.write_bytes(source)
                out.mkdir()
                completed = subprocess.run([str(BUILDER), str(path), '2', str(out)],
                                           capture_output=True, text=True)
                self.assertEqual(completed.returncode, 0, completed.stderr)
                cursor = 0
                for part in range(2):
                    offset, n, actual = decode(out / f'chunk-{part}.crle')
                    self.assertEqual(offset, cursor)
                    self.assertEqual(actual, cyclic_rows(source[offset:offset+n]))
                    self.assertEqual(source[offset+n-1], 0x1e)
                    cursor += n
                self.assertEqual(cursor, len(source))

    def test_in_stream_remap_and_no_clobber(self):
        source = b'\x01\x02\x1e\x01\x02\x1e'
        sigma = bytearray(range(256))
        sigma[1], sigma[255] = sigma[255], sigma[1]
        sigma[2], sigma[254] = sigma[254], sigma[2]
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            path, out, remap = tmp/'input', tmp/'out', tmp/'remap'
            path.write_bytes(source)
            out.mkdir()
            remap.write_bytes(sigma)
            command = [str(BUILDER), str(path), '2', str(out), str(remap)]
            first = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            for i in range(2):
                offset, n, actual = decode(out/f'chunk-{i}.crle')
                mapped = bytes(sigma[c] for c in source[offset:offset+n])
                self.assertEqual(actual, cyclic_rows(mapped))
            before = (out/'chunk-0.crle').read_bytes()
            second = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(second.returncode, 0)
            self.assertEqual(before, (out/'chunk-0.crle').read_bytes())

    def test_mid_document_cut_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            path, out = tmp/'input', tmp/'out'
            path.write_bytes(b'abcdef\x1e')
            out.mkdir()
            result = subprocess.run([str(BUILDER), str(path), '2', str(out)],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('CHUNK_FRONTEND_FATAL', result.stderr)
            self.assertFalse((out/'chunk-1.crle').exists())


if __name__ == '__main__':
    unittest.main()
