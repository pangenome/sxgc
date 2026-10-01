#!/usr/bin/env python3
"""Cross-producer byte gate for SXCR batches and the independent BCR input path."""
import pathlib
import random
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
FRONT = ROOT / 'vendor/chunk-merge-v3/chunk_frontend'
MERGE = ROOT / 'vendor/chunk-merge-v3/chunk_bcr_merge'
BCR = pathlib.Path('/tmp/bcr_frontend_v2')
EXTS = ('.rlebwt', '.rlebwt.meta', '.ssa', '.ssa_t')


class ChunkBcrMergeTests(unittest.TestCase):
    def test_cross_producer_four_files(self):
        rng = random.Random(0xB0C3)
        cases = [
            b'aaaa\x1eaaaa\x1eaaaa\x1eaaaa\x1e',
            b'abc\x1eabc\x1eabc\x1eabc\x1e',
            b''.join(bytes(rng.randrange(6, 22) for _ in range(rng.randrange(8, 35)))
                     + b'\x1e' for _ in range(30)),
            b''.join(bytes(rng.randrange(6, 40) for _ in range(rng.randrange(30, 80)))
                     + b'\x1e' for _ in range(120)),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            for i, source in enumerate(cases):
                work = root / str(i)
                work.mkdir()
                (work / 'chunks').mkdir()
                (work / 'source').write_bytes(source)
                count = 2 if i < 2 else 3
                subprocess.run([FRONT, work/'source', str(count), work/'chunks'],
                               check=True, capture_output=True)
                subprocess.run([MERGE, work/'chunks', str(count), str(len(source)),
                                work/'merged'], check=True, capture_output=True)
                subprocess.run([BCR, work/'source', work/'direct'],
                               check=True, capture_output=True)
                for ext in EXTS:
                    self.assertEqual((work/('merged'+ext)).read_bytes(),
                                     (work/('direct'+ext)).read_bytes(), (i, ext))
                retry = subprocess.run([MERGE, work/'chunks', str(count),
                                        str(len(source)), work/'merged'],
                                       capture_output=True)
                self.assertNotEqual(retry.returncode, 0)
                self.assertIn(b'output exists', retry.stderr)


if __name__ == '__main__':
    unittest.main()
