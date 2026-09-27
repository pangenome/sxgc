#!/usr/bin/env python3
"""Fail-closed source CLI regression. This is NOT a successful source-build gate."""
import argparse
import pathlib
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--xsa', default='xsa/target/release/xsa')
a = p.parse_args()

def run(args, expected):
    proc = subprocess.run([a.xsa, 'build', *map(str, args)], capture_output=True, text=True)
    assert proc.returncode != 0, proc.stdout
    assert expected in proc.stderr, proc.stderr

with tempfile.TemporaryDirectory(prefix='sxi-build-contract-') as directory:
    root = pathlib.Path(directory)
    source = root / 'input with spaces.txt'
    source.write_text('ACGT\n')
    out = root / 'out.sxi'
    for mode in ['--agc', '--fasta', '--text']:
        run([mode, source, '-o', out, '--verbose'], 'source construction blocked')
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
assert help_result.returncode == 0 and 'currently unavailable' in help_result.stdout
print('PASS fail-closed CLI: three source modes, invalid arguments, no artifacts, existing output/symlink preservation')
print('NOT source-build acceptance: endpoint construction remains unavailable')
