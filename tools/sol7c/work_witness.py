#!/usr/bin/env python3
"""Reject unaccounted heap changes before promoting the historical planner.

Uses the same native fixture as sol7b/work_witness.py. P denotes logical
shards in the dense core, NOT physical producers in production v2.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

binary = Path(sys.argv[1]).resolve()
scratch = Path(sys.argv[2]).resolve()
scratch.mkdir(parents=True, exist_ok=True)
metrics = ('comparisons', 'symbols', 'probes')
results = {}
for shards in (1, 4, 8, 16):
    log = scratch / f'work-shards{shards}.log'
    cmd = [str(binary), '--selftest', '1', '--seed', '1', '--kway', '8', '--threads', '16']
    with log.open('w') as output:
        run = subprocess.run(cmd, env=dict(os.environ, CROSS_SHARDS=str(shards)),
                             stdout=output, stderr=subprocess.STDOUT)
    assert run.returncode == 0, log
    content = log.read_text()
    assert 'SELFTEST_PASS count=1 seed=1 kway=8' in content, log
    stages = []
    for line in content.splitlines():
        match = re.fullmatch(r'CROSS_STAGE_WORK stage=(\w+) pass=0 comparisons=(\d+) symbols_compared=(\d+) probes=(\d+)', line)
        if match:
            stages.append(dict(stage=match[1], **dict(zip(metrics, map(int, match.groups()[1:])))))
    assert len(stages) == 15, (log, len(stages))
    modes = {'cyclic': stages[:3], 'dollar': stages[6:9]}
    for mode, items in modes.items():
        assert [item['stage'] for item in items] == ['prepare', 'plan', 'heap']
        prepare, plan, heap = items
        baseline = modes[mode] if shards == 1 else results['1']['modes'][mode]
        assert prepare == baseline[0]
        if shards == 1:
            assert all(plan[key] == 0 for key in metrics)
        # No duplicated rows are walked by this candidate. All range ends
        # are exclusive and every input row has exactly one shard owner.
        items.append(dict(stage='overlap', **{key: 0 for key in metrics}))
        items.append(dict(stage='unaccounted_heap_delta', **{
            key: heap[key] - baseline[2][key] for key in metrics}))
        items.append(dict(stage='planning_over_serial_heap', **{
            key: plan[key] / baseline[2][key] if baseline[2][key] else None
            for key in metrics}))
    results[str(shards)] = dict(command=cmd, environment={'CROSS_SHARDS': str(shards)},
                               modes=modes, production_v2=stages[12:15], log=str(log))
    assert results[str(shards)]['production_v2'] == results['1']['production_v2']

rejections = []
for shards in ('4', '8', '16'):
    for mode, stages in results[shards]['modes'].items():
        residual = stages[4]
        assert any(residual[key] != 0 for key in metrics)
        rejections.append(dict(shards=int(shards), mode=mode,
                               signed_residual={key: residual[key] for key in metrics}))
assert any(any(r['signed_residual'][key] < 0 for key in metrics) for r in rejections)
print(json.dumps(dict(result='PASS', candidate_contract='REJECTED',
    reason='Nonzero heap residual with no duplicated overlap rows; negative residual cannot be nonnegative overhead.',
    native_order_checks='dense cyclic/dollar brute order and legacy sparse seam/run checks passed',
    production_parallel_validated=False, one_gb_validated=False,
    new_seam_mechanism=False, results=results, rejections=rejections), indent=2))
