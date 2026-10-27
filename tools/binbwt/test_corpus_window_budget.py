#!/usr/bin/env python3
"""Check actual adapter admission on the 100 GB corpus without scanning it."""
import argparse
import json
import os
from pathlib import Path
import re
import resource
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--finish', required=True)
p.add_argument('--corpus', required=True)
p.add_argument('--remap', required=True)
a = p.parse_args()
n = Path(a.corpus).stat().st_size
assert n > 32 << 30 and n % 16384, 'fixture must exercise the page-rounding boundary'
results = []
with tempfile.TemporaryDirectory() as directory:
    for label, limit in [('unlimited', resource.RLIM_INFINITY), ('limited', 120 << 30)]:
        def set_limit():
            resource.setrlimit(resource.RLIMIT_AS, (limit, limit))
        env = {k: v for k, v in os.environ.items() if not k.startswith('XSA_FINISH_')}
        proc = subprocess.run([a.finish, '--check-claims', directory, '--corpus', a.corpus,
                               '--remap', a.remap, '--threads', '1'],
                              capture_output=True, text=True, env=env,
                              preexec_fn=set_limit, timeout=15)
        # An empty claims fixture must still refuse; only constructor admission
        # is under test. It does not create PFCK or run the product pipeline.
        assert proc.returncode != 0, proc.stderr
        match = re.search(r'regions=(\d+) region_bytes=(\d+) mapped_cap=(\d+) need_windows=(\d+) pinned=(\d+)', proc.stderr)
        assert match, proc.stderr
        regions, region, capacity, need, pinned = map(int, match.groups())
        assert regions == 4 and region % 4096 == 0 and capacity == regions * region
        if label == 'unlimited':
            assert need == pinned == 4 and n <= capacity < n + 16384
        else:
            assert capacity <= limit // 3 and pinned == 0 and need > 4
        results.append(dict(case=label, n=n, capacity=capacity, need=need,
                            pinned=pinned, claims_refused=True, log=proc.stderr))
print(json.dumps(dict(passed=True, cases=results), indent=2))
