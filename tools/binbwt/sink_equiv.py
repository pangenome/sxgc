#!/usr/bin/env python3
"""Sink equivalence oracle: the v2 parallel publisher must reproduce the v1
serial sink BYTE FOR BYTE, and must not move a single construction counter.

Both sinks run from the same binary and the same bin_order:
  --bin-sink serial   SequentialV2, one row() call per row (the v1 sink)
  (default)           BinBandSink, the parallel band publisher
The whole published stream is compared: .rlebwt, .rlebwt.meta, and every file
under .pieces/ (start, p-<band>-0, g-<band>, terminal) by name and byte, for
every thread count and live-cap (multi-window) shape. Construction counters are
compared token by token out of the BIN_WORK journal line.

usage: sink_equiv.py [BIN] [SCRATCH]
"""
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

BIN = sys.argv[1] if len(sys.argv) > 1 else '/mnt/nvme2n1/erikg/binbwt-v2-build/binbwt'
ROOT = Path(sys.argv[2] if len(sys.argv) > 2 else '/mnt/nvme2n1/erikg/binbwt-sink-equiv')
REMAP = Path('/mnt/nvme2n1/erikg/sol6c-gate100m/remap-256.bin')
mapping = REMAP.read_bytes()
inverse = bytes(mapping.index(i) for i in range(256))

# Adversarial fixtures: every one of these exercises a different publication
# edge - a single run, one giant periodic bin with deep tail ties, the padding
# rows colliding with the corpus's own last byte, deep/nested seam classes,
# separator-only text, both byte edges.
fixtures = {
    'single':        b'\x1e',
    'two':           b'ab',
    'all-same-64':   b'a' * 64,
    'periodic-ab':   b'ab' * 31 + b'\x1e',
    'nul-and-05':    b'\x00\x05\x1e\x00\x05\x1e',
    'sep-run':       b'\x1e' * 31,
    'nested':        b'xyxyxyxyxyxyxyxyxy\x1e' + b'xyxyxyxyxyxyxyxyxy\x1e' + b'xy',
    'mixed':         b'aaaaabaaaabaaaabaaaabaaba\x1e',
    'sep-last':      b'abcabcabc\x1e',                # last byte == the separator
    'all-sep':       b'\x1e' * 64,
    'byte-edges':    b'\xfb\x00\xfb\x00\xfb\x1e',
    'nul-only':      b'\x00' * 20 + b'\x05\x1e',
    'seam-deep':     b'\x1e\x0a\x1e' * 12,
    'long-run':      b'a' * 200000 + b'b' * 70000 + b'\x1e',
}
# Real corpus slices: real bins, real runs and real band boundaries at 260k
# rows, with a single-occurrence terminal byte so the sparse seam stays inside
# its discovery budget (the protocol wants a RARE terminal byte; the 100 MB and
# 1 GB gates below exercise real seam classes on the real corpora).
with (Path('/mnt/nvme2n1/erikg/extcols-scratch/mergperf-100m-snap.txt')).open('rb') as f:
    for i, (off, ln) in enumerate([(0, 4093), (10003, 65537), (831123, 262147),
                                   (2000003, 3000011)]):
        f.seek(off)
        piece = f.read(ln).translate(mapping)
        term = next(b for b in range(256) if mapping[b] not in (1, 2, 3, 4)
                    and b not in piece)
        fixtures[f'slice-{i}'] = piece + bytes([term])


# A real seam-capture load: many terminal-byte occurrences with a shallow but
# non-trivial class chain, so the sink's Hit/class path runs over thousands of
# rows, hundreds of runs per class and several bands.
_rng = __import__('random').Random(7)
_blocks = []
for _ in range(1200):
    _body = bytes(0x41 + _rng.randrange(8) for _ in range(1998))
    _blocks.append(_body + bytes([0x41 + _rng.randrange(8)]) + b'\x1e')
fixtures['many-hits'] = b''.join(_blocks)


def run(corpus, out, extra, env_extra):
    env = dict(os.environ, CROSS_V2_PIECES='1', CROSS_V2_PIECE_WINDOW='100000000',
               CROSS_POOL_SLOTS='33554432', **env_extra)
    cmd = [BIN, '--binbwt', str(corpus), '--remap', str(REMAP),
           '--out-prefix', str(out)] + extra
    r = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE)
    if r.returncode:
        sys.stderr.write(r.stderr.decode()[-4000:])
        raise SystemExit(f'producer rc={r.returncode} {extra}')
    work = re.search(rb'^BIN_WORK .*$', r.stderr, re.M)
    assert work, 'no BIN_WORK'
    return work.group(0).decode()


def counters(line):
    kv = {}
    for tok in line.split()[1:]:
        k, _, v = tok.partition('=')
        if v.lstrip('-').replace('.', '').isdigit() and '.' not in v:
            kv[k] = v
    return kv


CONSTRUCTION = ['sweep_bytes', 'sweep_passes', 'level_passes', 'level_elems',
                'bin_passes', 'comparisons', 'symbols', 'cert_decided', 'ties',
                'leaf_sorts', 'leaf_elems', 'peak_live_slots', 'emitted_rows',
                'bins', 'windows', 'full_SA', 'persistent_positions']


def tree(prefix):
    out = {}
    out['rlebwt'] = Path(str(prefix) + '.rlebwt').read_bytes()
    out['meta'] = Path(str(prefix) + '.rlebwt.meta').read_bytes()
    pdir = Path(str(prefix) + '.pieces')
    for f in sorted(pdir.iterdir()):
        out['pieces/' + f.name] = f.read_bytes()
    return out


cases = []
shapes = [('t1', ['--threads', '1']),
          ('t3', ['--threads', '3']),
          ('t8', ['--threads', '8']),
          ('t8-leaf4', ['--threads', '8', '--bin-leaf', '4', '--bin-depth-cap', '4']),
          ('t3-cap7', ['--threads', '3', '--bin-live-cap', '7']),
          ('t8-cap1', ['--threads', '8', '--bin-live-cap', '1'])]
for name, corpus_bytes in fixtures.items():
    # The container's terminal coverage requires the padding symbol 2 to occur
    # exactly 10 times, i.e. the remapped corpus must not carry bytes 1..4
    # (tiny_product.py asserts the same for its fixtures): a fixture containing
    # one of those is not a legal corpus, not a sink difference.
    assert not any(mapping[b] in (1, 2, 3, 4) for b in corpus_bytes), name
    corpus = ROOT / name / 'corpus'
    corpus.parent.mkdir(parents=True, exist_ok=True)
    corpus.write_bytes(corpus_bytes.translate(inverse))
    for shape, extra in shapes:
        a = ROOT / name / f'{shape}-serial' / 'frag'
        b = ROOT / name / f'{shape}-parallel' / 'frag'
        for d in (a.parent, b.parent):
            shutil.rmtree(d, ignore_errors=True)
            d.mkdir(parents=True)
        la = run(corpus, a, extra + ['--bin-sink', 'serial'], {})
        lb = run(corpus, b, extra, {})
        ca, cb = counters(la), counters(lb)
        for key in CONSTRUCTION:
            assert ca.get(key) == cb.get(key), (name, shape, key, ca.get(key), cb.get(key))
        ta, tb = tree(a), tree(b)
        assert ta.keys() == tb.keys(), (name, shape, sorted(ta.keys()), sorted(tb.keys()))
        for k in ta:
            assert ta[k] == tb[k], (name, shape, k, len(ta[k]), len(tb[k]))
        cases.append((name, shape, len(ta), len(ta['rlebwt']), int(cb['runs']),
                      int(cb['bands']), int(cb['target_hits'])))
        for d in (a.parent, b.parent):
            shutil.rmtree(d, ignore_errors=True)
print(f"SINK_EQUIV_PASS cases={len(cases)} fixtures={len(fixtures)} "
      f"shapes={len(shapes)} threads=1,3,8 windows=1,2,3+ "
      f"rlebwt=byte_identical meta=byte_identical pieces=byte_identical "
      f"construction_counters=identical "
      f"max_piece_files={max(c[2] for c in cases)} "
      f"max_rlebwt_bytes={max(c[3] for c in cases)} "
      f"max_runs={max(c[4] for c in cases)} max_bands={max(c[5] for c in cases)} "
      f"max_target_hits={max(c[6] for c in cases)}")
