#!/usr/bin/env python3
"""Cross-LCP merge gate: merged four files must equal the direct BCR frontend
byte for byte, on synthetic collections spanning periodic texts, tiny alphabets
and many chunk counts. Mirrors the banked chunk-merge-v3 differential
(bit6/sxi_logs/chunk-merge-v3/test_chunk_bcr_merge.py)."""
import os
import pathlib
import random
import subprocess
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve()
ROOT = HERE.parents[3]
FRONT = pathlib.Path(os.environ.get('CHUNK_FRONTEND',
                                   '/home/erikg/sxgc/vendor/chunk-merge-v3/chunk_frontend_buffered'))
CROSS = pathlib.Path(os.environ.get('CROSS_LCP_MERGE', '/tmp/cross_lcp_merge'))
BCR = pathlib.Path(os.environ.get('BCR_FRONTEND_V2', '/tmp/bcr_frontend_v2'))
EXTS = ('.rlebwt', '.rlebwt.meta', '.ssa', '.ssa_t')


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], check=True, capture_output=True, **kw)


class CrossLcpMergeTests(unittest.TestCase):
    def collections(self):
        rng = random.Random(0xC0FFEE)
        cases = [
            b'aaaa\x1eaaaa\x1eaaaa\x1eaaaa\x1e',
            b'abc\x1eabc\x1eabc\x1eabc\x1e',
            b'aaaa\x1eaaaa\x1eaaaa\x1eaaaa\x1eaaaa\x1eaaaa\x1e',
            b'aaaa\x1eaaaa\x1eaaaa\x1e',                      # odd count, all-equal docs
            b'a\x1e',                                          # minimal single doc
            bytes(rng.randrange(6, 22) for _ in range(200)) + b'\x1e',
            b''.join(bytes(rng.randrange(6, 22) for _ in range(rng.randrange(8, 35)))
                     + b'\x1e' for _ in range(30)),
            b''.join(bytes(rng.randrange(6, 40) for _ in range(rng.randrange(30, 80)))
                     + b'\x1e' for _ in range(120)),
            b''.join(b'\x06\x06\x06\x06\x1e' for _ in range(40)),       # minimal alphabet
            b''.join(b'\x06\x07\x06\x07\x06\x1e' for _ in range(50)),    # periodic, tiny alphabet
            b''.join(b'\x06' * (i % 7 + 1) + b'\x1e' for i in range(60)),
            b''.join(bytes((6, 7, 8, 9)) + b'\x1e' for _ in range(26)),  # identical docs
        ]
        # Random collections over a small alphabet with deep repetition.
        for i in range(14):
            alpha = [6, 7, 8, 9, 10, 0x1e]
            src = bytes(rng.choice(alpha) for _ in range(rng.randrange(80, 700)))
            if not src.endswith(b'\x1e'):
                src += b'\x1e'
            if src.count(0x1e) < 3:
                continue
            cases.append(src)
        return cases

    def check(self, work, source, count):
        (work / 'chunks').mkdir()
        (work / 'source').write_bytes(source)
        front = subprocess.run([str(FRONT), str(work / 'source'), str(count), str(work / 'chunks')],
                               capture_output=True)
        if front.returncode != 0:
            # The banked front end refuses some tiny doc shapes (its own
            # target-size split constraint), not a merge concern.
            self.assertIn(b'CHUNK_FRONTEND_FATAL', front.stderr)
            return False
        run([CROSS, work / 'chunks', count, len(source), work / 'merged'])
        run([BCR, work / 'source', work / 'direct'])
        for ext in EXTS:
            self.assertEqual((work / ('merged' + ext)).read_bytes(),
                             (work / ('direct' + ext)).read_bytes(), (source[:16], count, ext))
        if count >= 3:
            # Serial fold must give the same four files as the balanced tree.
            run([CROSS, work / 'chunks', count, len(source), work / 'serial',
                 '--work', work / 'serial-work', '--serial'])
            for ext in EXTS:
                self.assertEqual((work / ('serial' + ext)).read_bytes(),
                                 (work / ('direct' + ext)).read_bytes(), ('serial', count, ext))
        retry = subprocess.run([str(CROSS), str(work / 'chunks'), str(count), str(len(source)),
                                str(work / 'merged')], capture_output=True)
        self.assertNotEqual(retry.returncode, 0)
        self.assertIn(b'output exists', retry.stderr)
        return True

    def test_driver_matches_direct_bcr(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            done = 0
            for i, source in enumerate(self.collections()):
                work = root / str(i)
                work.mkdir()
                docs = source.count(0x1e)
                candidates = [c for c in (1, 2, 3, 4, 5, 8, 16, 26) if c <= max(1, docs)]
                produced = 0
                for count in candidates:
                    sub = work / ('c%d' % count)
                    sub.mkdir()
                    if self.check(sub, source, count):
                        produced += 1
                self.assertGreaterEqual(produced, 1 if docs < 2 else 2, (i, docs))
                done += 1
            self.assertGreaterEqual(done, 20)

    def test_pair_and_finalize(self):
        rng = random.Random(0xD1CE)
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            for i, count in enumerate((2, 3, 3)):
                work = root / str(i)
                work.mkdir()
                source = b''.join(bytes(rng.randrange(6, 16) for _ in range(rng.randrange(4, 12)))
                                  + b'\x1e' for _ in range(16))
                (work / 'source').write_bytes(source)
                (work / 'chunks').mkdir()
                run([FRONT, work / 'source', count, work / 'chunks'])
                # Pairwise cyclic intermediate of chunks 0+1 must equal the
                # single-chunk front end on the same concatenation.
                run([CROSS, '--pair', work / 'chunks' / 'chunk-0.crle',
                     '--right', work / 'chunks' / 'chunk-1.crle',
                     '--sxcr', work / 'pair-intermediate.crle'])
                (work / 'one').mkdir()
                def chunk_len(path):
                    with open(path, 'rb') as f:
                        head = f.read(32)
                    return int.from_bytes(head[16:24], 'little')
                prefix = source[:chunk_len(work / 'chunks' / 'chunk-0.crle')
                                + chunk_len(work / 'chunks' / 'chunk-1.crle')]
                (work / 'prefix-src').write_bytes(prefix)
                run([FRONT, work / 'prefix-src', 1, work / 'one'])
                self.assertEqual((work / 'pair-intermediate.crle').read_bytes(),
                                 (work / 'one' / 'chunk-0.crle').read_bytes(),
                                 'pair intermediate != cyclic order of concatenation')
                # Final pairwise step: intermediate + remaining chunk (count>2),
                # or the direct dollar pair of chunks 0+1 (count==2).
                if count == 2:
                    run([CROSS, '--pair', work / 'chunks' / 'chunk-0.crle',
                         '--right', work / 'chunks' / 'chunk-1.crle',
                         '--out-prefix', work / 'chained'])
                else:
                    run([CROSS, '--pair', work / 'pair-intermediate.crle',
                         '--right', work / 'chunks' / f'chunk-{count - 1}.crle',
                         '--out-prefix', work / 'chained'])
                run([BCR, work / 'source', work / 'direct'])
                for ext in EXTS:
                    self.assertEqual((work / ('chained' + ext)).read_bytes(),
                                     (work / ('direct' + ext)).read_bytes(),
                                     (i, count, ext))
            # Single-chunk finalize path.
            work = root / 'single'
            work.mkdir()
            source = b'xxy\x1exxy\x1ezz\x1e'
            (work / 'source').write_bytes(source)
            (work / 'chunks').mkdir()
            run([FRONT, work / 'source', 1, work / 'chunks'])
            run([CROSS, '--finalize', work / 'chunks' / 'chunk-0.crle',
                 '--out-prefix', work / 'fin'])
            run([BCR, work / 'source', work / 'direct'])
            for ext in EXTS:
                self.assertEqual((work / ('fin' + ext)).read_bytes(),
                                 (work / ('direct' + ext)).read_bytes())


if __name__ == '__main__':
    unittest.main()
