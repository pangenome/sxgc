#!/usr/bin/env python3
"""Real-tool publication, provenance, and single-byte preflight drift gate."""
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from tool_manifest import BINARIES, sha256

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--prefix', type=pathlib.Path, required=True)
p.add_argument('--text', type=pathlib.Path, required=True)
p.add_argument('--heads', type=pathlib.Path, required=True)
p.add_argument('--ri4', type=pathlib.Path, required=True)
p.add_argument('--log-dir', type=pathlib.Path, required=True)
a = p.parse_args()
a.log_dir.mkdir(parents=True, exist_ok=True)
work = pathlib.Path(tempfile.mkdtemp(prefix='sxi-distribution-gate-'))
env = {k: v for k, v in os.environ.items() if not k.startswith('XSA_')}
env['XSA_PIPELINE'] = str(ROOT / 'bit6/sxi_pipeline.py')
output = work / 'battery.sxi'
command = [str(a.prefix / 'xsa'), 'build', '--text', str(a.text),
           '-o', str(output), '--scratch', str(work), '--threads', '4',
           '--expect-heads', str(a.heads), '--expect-ri4', str(a.ri4),
           '--log-dir', str(a.log_dir), '--verbose']
result = subprocess.run(command, env=env, capture_output=True, text=True)
(a.log_dir / 'battery.log').write_text(result.stdout + result.stderr)
assert result.returncode == 0, result.stderr
publication = json.loads(result.stdout.splitlines()[-1])
rows = [json.loads(l) for l in pathlib.Path(publication['log']).read_text().splitlines()]
first, last = rows[0], rows[-1]
assert first['stage'] == 'tool-provenance' and first['verified'] and not first['allow_drift']
assert first['manifest'] == (a.prefix / 'MANIFEST.sha256').read_text()
assert first['manifest_sha256'] == sha256(a.prefix / 'MANIFEST.sha256')
assert last['status'] == 'PASS' and last['output_sha256'] == sha256(output)
assert last['output'] == str(output)
assert last['manifest_sha256'] == first['manifest_sha256']
for name in BINARIES:
    row = next(r for r in first['artifacts'] if r['artifact'] == 'bin/' + name)
    assert row['sha256'] == row['expected_sha256'] == sha256(a.prefix / name)

# Corrupt exactly one byte of an otherwise complete copied distribution.
# Audit is intentionally unused by this build: all tools must be checked anyway.
drift = work / 'drift'; drift.mkdir()
for name in (*BINARIES, 'MANIFEST.sha256'):
    shutil.copy2(a.prefix / name, drift / name)
tool = drift / 'sxi_text_audit'
with tool.open('r+b') as f:
    byte = f.read(1); f.seek(0); f.write(bytes([byte[0] ^ 1]))
scratch = work / 'must-not-exist'
rejected = work / 'rejected.sxi'
command = [str(drift / 'xsa'), 'build', '--text', str(a.text), '-o', str(rejected),
           '--scratch', str(scratch), '--log-dir', str(work / 'no-logs')]
result = subprocess.run(command, env=env, capture_output=True, text=True)
(a.log_dir / 'one-byte-drift.log').write_text(result.stdout + result.stderr)
line = next(l for l in first['manifest'].splitlines() if 'bin/sxi_text_audit #' in l)
assert result.returncode != 0 and 'SHA256 mismatch' in result.stderr and line in result.stderr
assert not rejected.exists() and not scratch.exists() and not (work / 'no-logs').exists()
evidence = dict(status='PASS', published=str(output), work=str(work),
                manifest_sha256=first['manifest_sha256'], output_sha256=last['output_sha256'],
                chi=last['chi'], checked_binaries=list(BINARIES),
                drift_tool=str(tool), differing_manifest_line=line,
                drift_refused_before_scratch=True, endpoint_byte_gates=True)
(a.log_dir / 'distribution-gate.json').write_text(json.dumps(evidence, indent=2) + '\n')
print(json.dumps(evidence))
