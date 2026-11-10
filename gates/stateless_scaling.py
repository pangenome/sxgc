#!/usr/bin/env python3
"""Fixed-work next-byte histogram scaling; preserves raw stderr and rusage."""
import json, pathlib, resource, subprocess, sys, time
binary, container, corpus, out = sys.argv[1:5]
positions = int(sys.argv[5]) if len(sys.argv) > 5 else 8192
left = int(sys.argv[6]) if len(sys.argv) > 6 else 8
out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
rows = []
for jobs in [1, 8, 24, 48, 96]:
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.monotonic()
    p = subprocess.run([binary, 'stateless-bench', container, corpus, str(jobs), str(positions), str(left)], capture_output=True, text=True, check=True)
    wall = time.monotonic() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    (out / f'{jobs}.log').write_text(p.stdout + p.stderr)
    line = next(s for s in p.stdout.splitlines() if s.startswith('STATELESS_BENCH'))
    fields = dict(x.split('=', 1) for x in line.split() if '=' in x)
    row = dict(threads=jobs, seconds=float(fields['seconds']), positions_per_s=float(fields['positions_per_s']), checksum=fields['checksum'], wall_including_open=wall,
        peak_rss_kib=after.ru_maxrss, user_seconds=after.ru_utime-before.ru_utime, system_seconds=after.ru_stime-before.ru_stime,
        minor_faults=after.ru_minflt-before.ru_minflt, major_faults=after.ru_majflt-before.ru_majflt,
        voluntary_switches=after.ru_nvcsw-before.ru_nvcsw, involuntary_switches=after.ru_nivcsw-before.ru_nivcsw)
    # ru_maxrss is cumulative max: use the process's own VmHWM for this row.
    row['peak_rss_kib'] = int(line.split('VmHWM:')[1].split()[0])
    row['scaling'] = (rows[0]['seconds'] if rows else row['seconds']) / row['seconds']
    row['efficiency'] = row['scaling'] / jobs
    rows.append(row)
    assert len({r['checksum'] for r in rows}) == 1
    (out / 'scaling.json').write_text(json.dumps(rows, indent=2) + '\n')
    print(json.dumps(row), flush=True)
