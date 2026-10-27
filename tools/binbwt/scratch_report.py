#!/usr/bin/env python3
"""Reconcile scratch evidence and fail the requested whole-run space gate."""
import argparse
import json
from pathlib import Path
import statistics

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--results', required=True)
p.add_argument('--series', required=True)
p.add_argument('--output-dir', required=True)
p.add_argument('--target-over-n', type=float, default=3.0)
a = p.parse_args()
result = json.loads(Path(a.results).read_text())
assert result.get('passed'), 'product gate must complete before the space attestation'
n = result['n']
rows = []
metadata = {'other:order/waves': 'waves', 'other:order/bands': 'packed_bands',
            'other:product/frag.pieces': 'pieces',
            'other:product/product.sxi2.partial': 'container',
            **{'other:product/frag.' + k: k for k in ('bits-head', 'bits-len', 'bits-high', 'bits-low')}}
keys = ['waves', 'packed_bands', 'column', 'pieces', 'rlebwt', 'sparse2',
        'pfck2', 'witness_spills', 'container', 'bits-head', 'bits-len', 'bits-high',
        'bits-low', 'isa_sample', 'debug_dump', 'other']
for line in Path(a.series).read_text().splitlines():
    row = json.loads(line)
    parts = dict.fromkeys(keys, 0)
    for key, value in row['components'].items():
        key = metadata.get(key, key)
        parts[key if key in parts else 'other'] += value
    assert sum(parts.values()) == row['total_bytes'], row['elapsed']
    rows.append(dict(row, components=parts))
assert rows
peak = max(rows, key=lambda row: row['total_bytes'])
if 'peak_all_allocated' in result:
    assert peak['total_bytes'] == result['peak_all_allocated'], 'series and product gate peaks disagree'
peaks = {k: max(row['components'][k] for row in rows) for k in keys}
intervals = [b['elapsed']-a['elapsed'] for a, b in zip(rows, rows[1:])]
phase_cadence = {}
for phase in sorted({r['phase'] for r in rows}):
    spans = [b['elapsed']-a['elapsed'] for a,b in zip(rows, rows[1:])
             if a['phase'] == b['phase'] == phase]
    phase_cadence[phase] = dict(samples=sum(r['phase'] == phase for r in rows),
                                median_interval_seconds=statistics.median(spans) if spans else None,
                                max_interval_seconds=max(spans, default=0))
report = dict(n=n, product_sha256=result['sha256'], product_passed=True,
              observed_whole_peak=peak, component_peaks=peaks,
              observed_whole_peak_over_n=peak['total_bytes']/n,
              target_over_n=a.target_over_n,
              space_gate_passed=peak['total_bytes'] <= a.target_over_n*n,
              continuous_upper_bound_attested=False, sampling_cadence_by_phase=phase_cadence,
              samples=len(rows), median_sample_interval_seconds=statistics.median(intervals) if intervals else None,
              max_sample_interval_seconds=max(intervals, default=0),
              qualification='Allocated-block du observations; traversal is not atomic; samples are a lower bound on continuous highwater and exclude open unlinked files. Independent component peaks are not additive.')
out = Path(a.output_dir); out.mkdir(parents=True, exist_ok=True)
(out/'space-gate.json').write_text(json.dumps(report, indent=2)+'\n')
table = ['| Component | Bytes at whole peak | / n | Independent peak bytes | / n |',
         '|---|---:|---:|---:|---:|']
for key in keys:
    value = peak['components'][key]
    table.append(f'| {key} | {value:,} | {value/n:.9f} | {peaks[key]:,} | {peaks[key]/n:.9f} |')
table.append(f"| TOTAL | {peak['total_bytes']:,} | {peak['total_bytes']/n:.9f} | | |")
(out/'stacked-peak.md').write_text('\n'.join(table)+'\n')
print(json.dumps({k:v for k,v in report.items() if k not in ('observed_whole_peak','component_peaks')}, indent=2))
raise SystemExit(0 if report['space_gate_passed'] else 1)
