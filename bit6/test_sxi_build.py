#!/usr/bin/env python3
"""Source CLI validation and real pipeline failure/publication regression."""
import argparse
import pathlib
import subprocess
import tempfile
import os

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--xsa', default='xsa/target/release/xsa')
a = p.parse_args()

def run(args, expected):
    proc = subprocess.run([a.xsa, 'build', *map(str, args)], capture_output=True, text=True, env=dict(os.environ, XSA_RPFBWT='/missing/tap'))
    assert proc.returncode != 0, proc.stdout
    assert expected in proc.stderr, proc.stderr

with tempfile.TemporaryDirectory(prefix='sxi-build-contract-') as directory:
    root = pathlib.Path(directory)
    source = root / 'input with spaces.txt'
    source.write_text('ACGT\n')
    out = root / 'out.sxi'
    for mode in ['--agc', '--fasta', '--text']:
        run([mode, source, '-o', out, '--verbose'], 'collection preparation' if mode == '--fasta' else 'missing executable')
        assert sorted(x.name for x in root.iterdir()) == [source.name]
    run(['--text', source, '--fasta', source, '-o', out], 'exactly one')
    run(['--text', source, '-o', out, '-o', out], 'only once')
    run(['--text', source], 'need -o')
    run(['--text'], 'requires a path')
    run(['--text', source, '-o', out, '--unsupported'], 'unknown option')
    run(['--text', root / 'missing.txt', '-o', out], 'open')
    out.write_bytes(b'preserve existing output')
    run(['--text', source, '-o', out], 'output exists')
    assert out.read_bytes() == b'preserve existing output'
    out.unlink()
    out.symlink_to(root / 'dangling-target')
    run(['--text', source, '-o', out], 'output exists')
    assert out.is_symlink()
help_result = subprocess.run([a.xsa, 'build', '--help'], capture_output=True, text=True)
assert help_result.returncode == 0 and 'endpoint-tap' in help_result.stdout
# Full real stages with a deliberately incorrect chi expectation must not publish.
with tempfile.TemporaryDirectory(prefix='sxi-failure-test-') as directory:
    root = pathlib.Path(directory)
    source = pathlib.Path('/tmp/laneY/bat/random-4-2k.txt')
    out = root / 'rejected.sxi'
    proc = subprocess.run([a.xsa, 'build', '--text', str(source), '-o', str(out),
        '--scratch', str(root), '--expect-chi', '0'], capture_output=True, text=True)
    assert proc.returncode != 0 and 'chi gate mismatch' in proc.stderr, proc.stderr
    assert not out.exists()
    wrong = root / 'wrong.heads'
    wrong.write_bytes(b'bad oracle')
    proc = subprocess.run([a.xsa, 'build', '--text', str(source), '-o', str(out),
        '--scratch', str(root), '--expect-heads', str(wrong)], capture_output=True, text=True)
    assert proc.returncode != 0 and 'gate-heads failed' in proc.stderr, proc.stderr
    assert not out.exists()
    collection = root / 'collection.txt'
    collection.write_bytes(source.read_bytes() * 2)
    proc = subprocess.run([a.xsa, 'build', '--text', str(collection), '-o', str(out),
        '--scratch', str(root)], capture_output=True, text=True)
    assert proc.returncode != 0 and 'collection ordering is unvalidated' in proc.stderr, proc.stderr
    assert not out.exists()
    # A stage failure also cannot publish; /bin/false passes executable preflight.
    proc = subprocess.run([a.xsa, 'build', '--text', str(source), '-o', str(out),
        '--scratch', str(root)], env=dict(os.environ, XSA_PFP='/bin/false'), capture_output=True, text=True)
    assert proc.returncode != 0 and 'parse failed' in proc.stderr, proc.stderr
    assert not out.exists()
print('PASS CLI validation; missing tool, stage failure, chi/head mismatch and unvalidated multi-string input publish no SXI; existing files/symlinks preserved')
