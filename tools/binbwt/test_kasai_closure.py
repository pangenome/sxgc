#!/usr/bin/env python3
"""Seed-first regression: closing publication plus unconditional refusal guards.

No historical bank is read. Checks anchored/unanchored columns and the finite
LCE arm, explicit diagnostics, malformed anchors, and env-only debug refusal.
The packaged --selftest-stream-guard separately injects bad head/tail stamps,
all four closure counters, zero lengths and overflowing extents.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess

p = argparse.ArgumentParser(description=__doc__)
for name in ('emit', 'finish', 'corpus', 'remap', 'column', 'isa-sample', 'root'):
    p.add_argument('--' + name, required=True)
p.add_argument('--sa')
p.add_argument('--chi', type=int, default=145685)
a = p.parse_args()
r = Path(a.root)
r.mkdir(parents=True, exist_ok=False)
pristine = r / 'pristine'
pristine.mkdir()
clean = {k: v for k, v in os.environ.items() if not k.startswith(('XSA_FINISH_', 'CROSS_'))}
cmd = [a.emit, '--binbwt', a.corpus, '--remap', a.remap,
       '--out-prefix', str(pristine/'frag'), '--threads', '4']
if a.sa:
    cmd += ['--tape-emit', a.sa]
with (r/'emit.log').open('w') as log:
    subprocess.run(cmd, env=dict(clean, CROSS_V2_PIECE_WINDOW='100000000'),
                   stdout=log, stderr=subprocess.STDOUT, check=True)
assert (pristine/'frag.pieces').exists() and (pristine/'frag.rlebwt').exists()


def finish(name, args=(), env=None, fail=None):
    d = r/name
    shutil.copytree(pristine, d)
    out = d/'product.sxi2'
    cmd = [a.finish, '--prefix', str(d/'frag'), '--corpus', a.corpus,
           '--remap', a.remap, '--output', str(out), '--threads', '4',
           '--sample-count', '64', '--poll-timeout', '10'] + list(args)
    result = subprocess.run(cmd, env=dict(clean, **(env or {})),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (d/'finish.log').write_text(result.stdout)
    if fail:
        assert result.returncode == 2 and fail in result.stdout, (name, result.stdout)
        assert not out.exists() and not Path(str(out)+'.partial').exists()
    else:
        assert result.returncode == 0 and out.is_file(), (name, result.stdout)
        assert f'XSA_FINISH_DONE chi={a.chi} ' in result.stdout
    print(name, 'PASS', 'rc='+str(result.returncode), 'published='+str(out.exists()))
    return d


for key in ('XSA_FINISH_DUMP_STREAM', 'XSA_FINISH_DUMP_ANSWERS', 'XSA_FINISH_ISA_DEBUG'):
    target = r/(key+'-forbidden')
    finish(key, env={key: str(target)}, fail='require explicit --debug-artifacts')
    assert not target.exists()
finish('bridge-without-debug', ['--bridge-dir', str(r/'forbidden-bridge')],
       fail='bridge dumps require explicit --debug-artifacts')
assert not (r/'forbidden-bridge').exists()
finish('rank-exceptions-without-debug', ['--run-ranks', '/nonexistent'],
       fail='offline rank exceptions require explicit --debug-artifacts')
finish('full-ISA-env-audit', ['--lcp-column', a.column, '--sa-locate', '/nonexistent'],
       fail='full ISA audit requires explicit --debug-artifacts')
finish('column-env-disable', ['--lcp-column', a.column], env={'XSA_FINISH_KASAI': '0'},
       fail='a supplied LCP column cannot be disabled')
finish('column-env-disable-debug', ['--debug-artifacts', '--lcp-column', a.column],
       env={'XSA_FINISH_KASAI': '0'}, fail='a supplied LCP column cannot be disabled')
col = ['--lcp-column', a.column, '--chi-expect', str(a.chi)]
finish('kasai', col+['--isa-sample', a.isa_sample])
finish('kasai-no-anchor', col)
finish('finite-walk', ['--chi-expect', str(a.chi)])
finish('debug-kasai', col+['--debug-artifacts'], env={
       'XSA_FINISH_DUMP_STREAM': str(r/'stream'),
       'XSA_FINISH_DUMP_ANSWERS': str(r/'answers')})
bad = bytearray(Path(a.isa_sample).read_bytes())
# Sample rank for position zero: first payload entry after the 32-byte header.
assert len(bad) >= 40
old, = struct.unpack_from('<Q', bad, 32)
struct.pack_into('<Q', bad, 32, old+1)
(r/'bad.isamp').write_bytes(bad)
finish('bad-anchor', col+['--isa-sample', str(r/'bad.isamp')], fail='ANCHOR')
finish('bad-anchor-debug', col+['--debug-artifacts', '--isa-sample', str(r/'bad.isamp')], fail='ANCHOR')
reference = (r/'kasai/product.sxi2').read_bytes()
for name in ('kasai-no-anchor', 'finite-walk', 'debug-kasai'):
    assert reference == (r/name/'product.sxi2').read_bytes(), name
assert (r/'stream').stat().st_size % 40 == 0
assert (r/'answers').stat().st_size * 40 == (r/'stream').stat().st_size * 16
print('SEED_FIRST_REGRESSION_PASS sha256='+hashlib.sha256(reference).hexdigest())
