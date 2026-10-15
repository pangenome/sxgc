#!/usr/bin/env python3
"""Compare honestly charged native planner/heap work at S=1 and S=8.

This exercises the standing exact sharded core, not a multi-producer v2 sink.
The first fixture is fixed by the production selftest (k copies of 1e0a1e).
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

binary = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
results = {}
for shards in (1, 8):
    path = out / f'work-shards{shards}.log'
    cmd = [str(binary), '--selftest', '1', '--seed', '1', '--kway', '8', '--threads', '8']
    with path.open('w') as log:
        run = subprocess.run(cmd, env=dict(os.environ, CROSS_SHARDS=str(shards)), stdout=log, stderr=subprocess.STDOUT)
    assert run.returncode == 0, path
    stages = []
    for line in path.read_text().splitlines():
        match = re.match(r'CROSS_STAGE_WORK stage=(\w+) pass=0 comparisons=(\d+) symbols_compared=(\d+) probes=(\d+)', line)
        if match:
            stages.append(dict(stage=match[1], comparisons=int(match[2]), symbols=int(match[3]), probes=int(match[4])))
    # Invocation order: cyclic dense, cyclic legacy sparse, dollar dense,
    # dollar legacy sparse, dollar production-v2 (forced S=1).
    assert len(stages) == 15
    modes = {'cyclic': stages[:3], 'dollar': stages[6:9]}
    for items in modes.values():
        assert [item['stage'] for item in items] == ['prepare', 'plan', 'heap']
    results[str(shards)] = dict(command=cmd, environment={'CROSS_SHARDS': str(shards)},
                               modes=modes, native_selftest_rc=run.returncode)
for mode in ('cyclic', 'dollar'):
    assert results['1']['modes'][mode][0] == results['8']['modes'][mode][0]
    assert results['1']['modes'][mode][1] != results['8']['modes'][mode][1]
    assert results['1']['modes'][mode][2] != results['8']['modes'][mode][2]
print(json.dumps(dict(result='PASS', byte_order_oracles='native brute cyclic/dollar and sparse-v2 checks passed',
    work_identity=False, caveat='S denotes logical shards, not physical worker count; this is not a 1GB gate',
    results=results), indent=2))
