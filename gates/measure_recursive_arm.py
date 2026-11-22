"""Measure the Rust 100 MB sandbox dial harness; sample Linux process and disk peaks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('--dial', type=int, required=True, help='producer workspace in bytes')
p.add_argument('--harness', default='/tmp/tr_smoke/run_dial_r.sh')
a = p.parse_args()
assert a.dial > 0
name = f'measured_recursive_{a.dial}_{time.time_ns()}'
root = Path('/mnt/nvme2n1/erikg/TAPEREADER/sandbox') / f'br_{name}'
print('arm=', root, flush=True)
start = time.monotonic()
child = subprocess.Popen([a.harness, name, str(a.dial)], env=dict(os.environ, XSA_SA_CEILING='2000000'))
peaks = {}

def peak(key, n):
    peaks[key] = max(peaks.get(key, 0), n)

last = -10
while child.poll() is None:
    for pid in Path(f'/proc/{child.pid}/task/{child.pid}/children').read_text().split():
        try:
            fields = dict(line.split(':', 1) for line in Path(f'/proc/{pid}/status').read_text().splitlines() if ':' in line)
            who = fields['Name'].strip()
            if who in ('pfp_sort_r', 'xsa_finish_cur'):
                for field in ['RssAnon', 'VmRSS', 'VmHWM']:
                    if field in fields:
                        peak(who + '_' + field + '_bytes', int(fields[field].split()[0]) * 1024)
        except (FileNotFoundError, ProcessLookupError):
            pass
    now = time.monotonic() - start
    if now - last >= 1 and root.exists():
        last = now
        blocks = 0
        for d, _, files in os.walk(root):
            for f in files:
                try:
                    blocks += (Path(d) / f).stat().st_blocks
                except FileNotFoundError:
                    pass
        peak('arm_disk_allocated_bytes', blocks * 512)
    time.sleep(.05)
sha = hashlib.sha256()
product = root / 'product.sxi2'
if product.exists():
    with product.open('rb') as f:
        for b in iter(lambda: f.read(1048576), b''):
            sha.update(b)
rc = child.returncode
receipt = dict(peaks, wall_s=round(time.monotonic()-start, 2), dial_bytes=a.dial,
               harness_rc=rc, product_sha256=sha.hexdigest() if product.exists() else None)
(root / 'resources.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(receipt, flush=True)
assert rc == 0 and 'kernel=induced' in (root / 'run.log').read_text()
assert receipt['product_sha256'] == '6325c843c0d9aa9be00d69260aafdd794936e2e79f16174efb00c43db831f341'
assert 0 < peaks['pfp_sort_r_RssAnon_bytes'] <= a.dial, 'producer sampled anonymous RSS exceeded dial'
