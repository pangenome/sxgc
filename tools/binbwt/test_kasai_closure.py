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
# Environment variables alone must never enable O(r)/O(n) diagnostics.
for key in ('XSA_FINISH_DUMP_STREAM', 'XSA_FINISH_DUMP_ANSWERS', 'XSA_FINISH_ISA_DEBUG'):
    d = r / key
    shutil.copytree(pristine, d)
    target = d / 'forbidden-artifact'
    cmd = [a.finish, '--prefix', str(d / 'frag'), '--corpus', a.corpus,
           '--remap', a.remap, '--output', str(d / 'product.sxi2'), '--threads', '4']
    result = subprocess.run(cmd, env=dict(os.environ, **{key: str(target)}),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (d / 'finish.log').write_text(result.stdout)
    assert result.returncode == 2 and 'require explicit --debug-artifacts' in result.stdout
    assert not target.exists() and not (d / 'product.sxi2').exists()
    print(key, 'PRODUCT_REFUSAL_PASS')
for name, exe, kasai, anchors in (
    ('baseline', a.baseline, False, False), ('walk', a.finish, False, False),
    ('debug-walk', a.finish, False, False),
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
    env = dict(os.environ)
    if name == 'debug-walk':
        cmd += ['--debug-artifacts']
        env.update(XSA_FINISH_DUMP_STREAM=str(d / 'stream'),
                   XSA_FINISH_DUMP_ANSWERS=str(d / 'answers'))
    with (d / 'finish.log').open('w') as log:
        result = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
    if kasai:
        assert result.returncode == 2, (name, result.returncode)
        assert 'KASAI STAMP:' in (d / 'finish.log').read_text()
        assert not out.exists() and not Path(str(out) + '.partial').exists()
    else:
        assert result.returncode == 0, (name, result.returncode)
        assert out.exists()
    print(name, 'PASS', 'rc=' + str(result.returncode), 'published=' + str(out.exists()))
baseline = (r / 'baseline/product.sxi2').read_bytes()
assert baseline == (r / 'walk/product.sxi2').read_bytes()
print('WALK_BASELINE_IDENTICAL sha256=' + hashlib.sha256(baseline).hexdigest())

assert baseline == (r / 'debug-walk/product.sxi2').read_bytes()
assert (r / 'debug-walk/stream').stat().st_size % 40 == 0
assert (r / 'debug-walk/answers').stat().st_size * 40 == (r / 'debug-walk/stream').stat().st_size * 16
print('EXPLICIT_DEBUG_DUMPS_PASS walk_bytes_identical=1')
