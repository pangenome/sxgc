#!/usr/bin/env python3
"""Warm one-worker HTTP gate. Starts and stops only its own xsa child."""
import json
import pathlib
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.request

binary, artifact, witness, queries, mode, label, port = sys.argv[1:]
port = int(port)
patterns = [line.rstrip('\n').split('\t', 1) for line in open(queries)
            if line.startswith('probe')]
assert len(patterns) == 1000
cmd = [binary, 'serve', '--sxi', artifact, '--mode',
       'dna' if label == 'yeast235' else 'text', '-j', '1',
       '--bind', f'127.0.0.1:{port}']
if mode == 'first':
    cmd += ['--first', '--witness-index', witness]
elif mode == 'sample':
    cmd += ['--sample', '1']
elif mode != 'full':
    raise ValueError(mode)
base = pathlib.Path('bit6/sxi_logs/witness-locate')
log = (base / f'{label}.{mode}.server.log').open('w')
child = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
url = f'http://127.0.0.1:{port}/query'

def query(name, pattern):
    data = json.dumps({'name': name, 'pattern': pattern}).encode()
    req = urllib.request.Request(url, data=data, headers={'Content-Type':'application/json'})
    start = time.perf_counter_ns()
    with urllib.request.urlopen(req, timeout=120) as resp:
        body = resp.read()
        if resp.status != 200:
            raise RuntimeError((resp.status, body[:500]))
    return (time.perf_counter_ns() - start) / 1e6, body

try:
    for attempt in range(1800):
        if child.poll() is not None:
            raise RuntimeError('xsa exited before ready')
        try:
            with urllib.request.urlopen(f'http://127.0.0.1:{port}/stats', timeout=1):
                break
        except (OSError, urllib.error.URLError):
            time.sleep(0.2)
    else:
        raise RuntimeError('server readiness timeout')
    for name, pattern in patterns[:10]:
        query(name, pattern)
    times = []
    output_bytes = 0
    with (base / f'{label}.{mode}.jsonl').open('wb') as out:
        for name, pattern in patterns:
            elapsed, body = query(name, pattern)
            times.append(elapsed)
            out.write(body)
            output_bytes += len(body)
    result = {'corpus':label, 'mode':mode, 'patterns':len(patterns),
              'median_ms':statistics.median(times), 'mean_ms':statistics.mean(times),
              'p95_ms':sorted(times)[949], 'total_ms':sum(times),
              'output_bytes':output_bytes}
    (base / f'{label}.{mode}.time.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))
finally:
    child.terminate()
    try:
        child.wait(timeout=15)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait()
    log.close()
