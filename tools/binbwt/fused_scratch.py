#!/usr/bin/env python3
"""Independent one-second allocated-space sampler for the wave/band scratch law.

Columns and sampled ISA outputs are excluded here and included by fused_gate's
order/all accounting. Start before the first scatter file is created.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--root', required=True)
p.add_argument('--n', type=int, required=True)
a = p.parse_args()
root = Path(a.root)
peak = samples = max_live = max_band = 0
start = time.monotonic()
with (root/'du-wave-bands.tsv').open('w', buffering=1) as out:
    out.write('elapsed\tallocated_wave_bands\tlive_band_files\tlargest_live_band\n')
    while True:
        total = 0
        for d in [root/'order/waves', root/'order/bands']:
            r = subprocess.run(['du', '-s', '-B1', str(d)], capture_output=True, text=True)
            if r.stdout: total += int(r.stdout.split()[0])
        files = list((root/'order/bands').glob('*.sa'))
        largest = 0
        for f in files:
            try: largest = max(largest, f.stat().st_size)
            except FileNotFoundError: pass
        peak, max_live, max_band = max(peak,total), max(max_live,len(files)), max(max_band,largest)
        samples += 1
        out.write(f'{time.monotonic()-start:.3f}\t{total}\t{len(files)}\t{largest}\n')
        result = dict(peak_allocated_wave_bands=peak, peak_over_n=peak/a.n,
                      max_live_band_files=max_live, max_observed_band_bytes=max_band,
                      samples=samples, period_seconds=1,
                      scope='scatter wave parts plus unconsumed packed bands and protocol metadata; column/ISA counted separately by fused_gate',
                      measurement='du allocated blocks; one-second samples are a lower bound on continuous peak')
        (root/'scratch-wave-bands.json').write_text(json.dumps(result,indent=2)+'\n')
        try: state = json.loads((root/'results.json').read_text())
        except (FileNotFoundError, json.JSONDecodeError): state = {}
        if state.get('passed') or state.get('error'): break
        time.sleep(1)
print(json.dumps(result,indent=2))
