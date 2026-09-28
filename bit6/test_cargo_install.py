#!/usr/bin/env python3
"""Installed executable gate: clean environment, relocatability and drift rejection."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--xsa', type=Path, required=True)
p.add_argument('--log-dir', type=Path, required=True)
a = p.parse_args()
a.log_dir.mkdir(parents=True, exist_ok=True)
work = Path(tempfile.mkdtemp(prefix='xsa-cargo-gate-'))
binary = work/'bin/xsa'; binary.parent.mkdir(); shutil.copy2(a.xsa, binary)
# Empty home/cache, no XSA variables, unrelated cwd and no source/prebuilt tools.
env = {'PATH': '/usr/bin:/bin', 'HOME': str(work/'home'), 'LANG': 'C.UTF-8'}
rng = random.Random(42)
source = work/'battery.input'
source.write_bytes(bytes(rng.choice(b'ACGT') for _ in range(20000)) + b'\x1e')
output = work/'battery.sxi'
command = [str(binary), 'build', '--text', str(source), '-o', str(output),
           '--threads', '2', '--verify-text-sample', '100', '--log-dir', str(a.log_dir),
           '--scratch', str(work), '--verbose']
result = subprocess.run(command, env=env, cwd=work, capture_output=True, text=True)
(a.log_dir/'publication.log').write_text(result.stdout+result.stderr)
assert result.returncode == 0, result.stderr
publication = json.loads(result.stdout.splitlines()[-1])
rows = [json.loads(s) for s in Path(publication['log']).read_text().splitlines()]
provenance = rows[0]
assert provenance['verified'] and provenance['package']['name'] == 'xsa'
assert rows[-1]['status'] == 'PASS'
assert {r['artifact'][4:] for r in provenance['artifacts'] if r['artifact'].startswith('bin/')} == {
    'xsa', 'pfp++', 'rpfbwt', 'rpfbwt_endpoints', 'slim_dump', 'sxi_write',
    'sxi_text_audit', 'phi_inverse_heads', 'agc2flat'}
cache = Path(provenance['manifest_path']).parent
# Corrupt an unused executable: preflight must inspect the entire bundle.
corrupt = cache/'phi_inverse_heads'
original = corrupt.read_bytes()
corrupt.write_bytes(bytes([original[0] ^ 1])+original[1:])
rejected = work/'rejected.sxi'; scratch = work/'never-created'
command = [str(binary), 'build', '--text', str(source), '-o', str(rejected),
           '--scratch', str(scratch), '--log-dir', str(work/'no-logs')]
result = subprocess.run(command, env=env, cwd=work, capture_output=True, text=True)
(a.log_dir/'drift-rejection.log').write_text(result.stdout+result.stderr)
assert result.returncode != 0 and 'SHA256 mismatch' in result.stderr
assert 'phi_inverse_heads' in result.stderr
assert not rejected.exists() and not scratch.exists() and not (work/'no-logs').exists()
# A modified manifest cannot bless the corrupted file.
manifest = cache/'MANIFEST.sha256'; original_manifest = manifest.read_bytes()
data = json.loads(original_manifest)
data['files']['phi_inverse_heads'] = hashlib.sha256(corrupt.read_bytes()).hexdigest()
manifest.write_text(json.dumps(data))
result = subprocess.run(command, env=env, cwd=work, capture_output=True, text=True)
assert result.returncode != 0 and 'SHA256 mismatch' in result.stderr
manifest.write_bytes(original_manifest); corrupt.write_bytes(original)
# Tool override is respected and checked against the build-bound manifest.
override = work/'override'; shutil.copytree(cache, override)
(override/'sxi_write').write_bytes(b'corrupt override')
result = subprocess.run(command, env=dict(env, XSA_TOOLS=str(override)), cwd=work,
                        capture_output=True, text=True)
(a.log_dir/'override-rejection.log').write_text(result.stdout+result.stderr)
assert result.returncode != 0 and 'SHA256 mismatch: sxi_write' in result.stderr
assert not rejected.exists()
# Explicit development pipelines retain their existing sibling-tool default.
probe = work/'probe.py'
probe.write_text('import os\nassert \"XSA_TOOLS\" not in os.environ\n')
result = subprocess.run(command, env=dict(env, XSA_PIPELINE=str(probe)), cwd=work,
                        capture_output=True, text=True)
assert result.returncode == 0, result.stderr
# Legacy source manifests still work with XSA_TOOLS alone from a checkout.
repo = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(repo/'tools'))
from tool_manifest import create
legacy = work/'legacy'; shutil.copytree(cache, legacy)
shutil.copy2(binary, legacy/'xsa')
create(legacy, legacy/'MANIFEST.sha256', repo)
legacy_command = [str(binary), 'build', '--text', str(source), '-o', str(work/'legacy.sxi'),
                  '--threads', '2', '--log-dir', str(a.log_dir/'legacy')]
result = subprocess.run(legacy_command, env=dict(env, XSA_TOOLS=str(legacy)), cwd=repo,
                        capture_output=True, text=True)
(a.log_dir/'legacy-override.log').write_text(result.stdout+result.stderr)
assert result.returncode == 0, result.stderr
evidence = dict(status='PASS', work=str(work), xsa=str(binary), publication=publication,
                zero_xsa_environment=True, relocated_binary=True,
                source_sample_audit=100, drift_rejected_before_scratch=True,
                manifest_reblessing_rejected=True, development_override_checked=True, legacy_tools_only_override_checked=True,
                package=provenance['package'], manifest_sha256=provenance['manifest_sha256'])
(a.log_dir/'gate.json').write_text(json.dumps(evidence, indent=2)+'\n')
print(json.dumps(evidence))
