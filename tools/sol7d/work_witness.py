#!/usr/bin/env python3
"""Attest exact per-producer logical work, with replay planning fully charged.

Consumes the expanded native sol7c witness, not production band output.
No subtraction/normalization of charged counters is performed.
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
log = scratch / 'counter-witness.log'
with log.open('w') as output:
    run = subprocess.run([str(binary)], env=dict(os.environ, CROSS_SEED_HEAP='1'),
                         stdout=output, stderr=subprocess.STDOUT)
assert run.returncode == 0, log
metrics = ('comparisons', 'symbols', 'probes')
cases = []
current = None
for line in log.read_text().splitlines():
    match = re.fullmatch(r'SEAM_CASE fixture=(\d+) P=(\d+) mode=(\w+)', line)
    if match:
        current = dict(fixture=int(match[1]), P=int(match[2]), mode=match[3], stages={}, producers={})
        cases.append(current)
    match = re.fullmatch(r'CROSS_STAGE_WORK stage=(\w+) pass=(\d+) comparisons=(\d+) symbols_compared=(\d+) probes=(\d+)', line)
    if match:
        assert current is not None
        stage, index = match[1], int(match[2])
        values = list(map(int, match.groups()[2:]))
        target = current['producers'] if stage == 'producer' else current['stages']
        key = index if stage == 'producer' else stage
        assert key not in target
        target[key] = values
assert len(cases) == 40
baselines = {(case['fixture'], case['mode']): case for case in cases if case['P'] == 1}
for case in cases:
    stages = case['stages']
    base = baselines[case['fixture'], case['mode']]['stages']
    assert stages['prepare'] == base['prepare']
    assert len(case['producers']) == case['P']
    walked = [sum(p[i] for p in case['producers'].values()) for i in range(3)]
    assert walked == stages['heap'] == base['heap'], case
    seed = stages.get('planning_seed', [0, 0, 0])
    overlap = stages.get('overlap', [0, 0, 0])
    if case['P'] > 1:
        assert seed == base['heap'], case
    else:
        assert seed == stages['plan'] == [0, 0, 0]
    planning = [stages['plan'][i] + seed[i] for i in range(3)]
    # Observed charged total = planning + producers. Compare with the charter.
    observed = [planning[i] + walked[i] for i in range(3)]
    expected = [base['heap'][i] + planning[i] + overlap[i] for i in range(3)]
    assert observed == expected
    case.update(serial=base['heap'], PLANNING=planning, OVERLAP=overlap,
                SEAM=[observed[i] - expected[i] for i in range(3)], total=observed,
                overhead=[planning[i] / base['heap'][i] if base['heap'][i] else None for i in range(3)])
result = dict(result='PASS', metrics=metrics, cases=cases,
              logical_identity=True, bounded_work_pass=False,
              reason='Exact serial replay contributes 100% of serial heap work before rank planning.',
              production_parallel_validated=False, one_gb_validated=False, log=str(log))
(scratch / 'counter-witness.json').write_text(json.dumps(result, indent=2) + '\n')
for case in cases:
    print('COUNTER_IDENTITY_PASS fixture={fixture} P={P} mode={mode} serial={serial} '
          'PLANNING={PLANNING} OVERLAP={OVERLAP} SEAM={SEAM} total={total}'.format(**case))
print('SEAM_COUNTER_WITNESS_PASS cases=40 fixtures=5 P=1,4,8,16 modes=cyclic,dollar '
      'per_producer_sum=exact SEAM=0 planning_replay=fully_charged bounded_work=FAIL')
