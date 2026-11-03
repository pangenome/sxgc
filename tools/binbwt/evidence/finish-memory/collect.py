#!/usr/bin/env python3
"""Collect the named-stage measurements; never infer a missing gate result."""
import hashlib
import json
from pathlib import Path
import re
import shutil

source = Path('/mnt/nvme2n1/erikg/finish-memory-evidence')
out = Path(__file__).resolve().parent
result = {'evidence_root': str(source), 'base_commit': 'abf2415dfee435133e2589617b8ff4f507a0ac39', 'runs': {}}
for tag in ('baseline10', 'fixed8', 'packaged8', 'fixed150', 'fixed10', 'fixed100'):
    root = source / tag
    log = root / ('finish.log' if tag == 'fixed8' else 'finish-v2.log')
    text = log.read_text()
    rows = [{'stage': s, 'rss_kb': int(r), 'anon_kb': int(a)} for s, r, a in
            re.findall(r'XSA_FINISH_RSS_AT stage=(\S+) rss_kb=(\d+) anon_kb=(\d+)', text)]
    peak = re.search(r'XSA_FINISH_PEAK_RSS kib=(\d+)', text)
    done = re.search(r'XSA_FINISH_DONE chi=(\d+) n=(\d+) r=(\d+)', text)
    run = {'stages': rows, 'kernel_peak_rss_kib': int(peak[1]) if peak else None,
           'completed': bool(done)}
    if done: run.update(chi=int(done[1]), n=int(done[2]), runs=int(done[3]))
    dest = out / tag
    dest.mkdir(exist_ok=True)
    (dest / 'finish-summary.log').write_text('\n'.join(l for l in text.splitlines() if
        l.startswith(('XSA_FINISH_RSS_AT', 'XSA_FINISH_CHI_', 'XSA_FINISH_DONE', 'XSA_FINISH_PEAK_RSS',
                      'XSA_FINISH_KASAI_CLOSURE', 'XSA_FINISH_PFCK ', 'SLIM_SEEDS'))) + '\n')
    for name in ('commands.json', 'rc.json', 'm5.json', 'query.log', 'count.log', 'validation.json',
                 'external-peak.json', 'smaps-latest.json', 'smaps-readiness.json', 'marker.json',
                 'baseline-marker-match.json', 'bitmap-cache-warmer.log', 'scratch-cleanup.json'):
        if (root / name).exists(): shutil.copyfile(root / name, dest / name)
    if (root / 'marker.json').exists():
        run['marker'] = json.loads((root / 'marker.json').read_text())
        run['marker_high_water_rss_kib'] = run['marker']['status']['VmHWM']
    result['runs'][tag] = run
for name in ('probe100.log', 'probe100-smaps.json', 'bulk-test.log', 'bulk-baseline-test.log',
             'all-unit-tests.log', 'branches.log', 'mem.log', 'seeds.log', 'run.py', 'monitor.py'):
    shutil.copyfile(source / name, out / name)
result['finish_source_sha256'] = hashlib.sha256((out.parents[3] / 'bit6/xsa_finish.cpp').read_bytes()).hexdigest()
result['binaries'] = {}
for name in ('finish-baseline', 'finish-fixed'):
    result['binaries'][name] = hashlib.sha256((source / name).read_bytes()).hexdigest()
(out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
rows = result['runs']['baseline10']['stages']
table = ['| 10 GB baseline stage | RSS KiB | Anonymous KiB |', '|---|---:|---:|']
for r in rows:
    if r['stage'] == 'sweep-progress': continue
    table.append(f"| {r['stage']} | {r['rss_kb']:,} | {r['anon_kb']:,} |")
probe_lines = [line for line in (source / 'probe100.log').read_text().splitlines()
               if line.startswith('XSA_FINISH_RSS_AT') and 'stage=start ' not in line]
(out / 'G1.md').write_text('\n'.join(table) + '\n\n100 GB readiness confirmation:\n\n```text\n' +
                           '\n'.join(probe_lines) + '\n```\n')
print(json.dumps({tag: {k:v for k,v in r.items() if k != 'stages'} for tag,r in result['runs'].items()}, indent=2))
