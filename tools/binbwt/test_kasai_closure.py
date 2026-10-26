#!/usr/bin/env python3
"""Regression for a known nonclosing fixture: reject Kasai, preserve walk bytes.

Uses independently prepared column/anchors; never constructs a full ISA.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
for name in ('emit', 'finish', 'baseline', 'corpus', 'remap', 'column', 'isa-sample', 'root'):
    p.add_argument('--' + name, required=True)
a = p.parse_args()
r = Path(a.root)
r.mkdir(parents=True, exist_ok=False)
pristine = r / 'pristine'
pristine.mkdir()
cmd = [a.emit, '--binbwt', a.corpus, '--remap', a.remap,
       '--out-prefix', str(pristine / 'frag'), '--threads', '4']
with (r / 'emit.log').open('w') as log:
    subprocess.run(cmd, env=dict(os.environ, CROSS_V2_PIECE_WINDOW='100000000'),
                   stdout=log, stderr=subprocess.STDOUT, check=True)
for name, exe, kasai, anchors in (
    ('baseline', a.baseline, False, False), ('walk', a.finish, False, False),
    ('kasai', a.finish, True, True), ('kasai-no-anchor', a.finish, True, False)):
    d = r / name
    shutil.copytree(pristine, d)
    out = d / 'product.sxi2'
    cmd = [exe, '--prefix', str(d / 'frag'), '--corpus', a.corpus,
           '--remap', a.remap, '--output', str(out), '--threads', '4',
           '--sample-count', '64', '--poll-timeout', '10']
    if kasai:
        cmd += ['--lcp-column', a.column]
    if anchors:
        cmd += ['--isa-sample', a.isa_sample]
    with (d / 'finish.log').open('w') as log:
        result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
    if kasai:
        assert result.returncode == 2, (name, result.returncode)
        assert 'KASAI CLOSURE:' in (d / 'finish.log').read_text()
        assert not out.exists() and not Path(str(out) + '.partial').exists()
    else:
        assert result.returncode == 0, (name, result.returncode)
        assert out.exists()
    print(name, 'PASS', 'rc=' + str(result.returncode), 'published=' + str(out.exists()))
baseline = (r / 'baseline/product.sxi2').read_bytes()
assert baseline == (r / 'walk/product.sxi2').read_bytes()
print('WALK_BASELINE_IDENTICAL sha256=' + hashlib.sha256(baseline).hexdigest())
