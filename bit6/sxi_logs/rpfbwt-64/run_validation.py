#!/usr/bin/env python3
"""Validation only: retained parse -> front end -> audited SXI. No milestone claim."""
import json
import os
from pathlib import Path
import resource
import struct
import subprocess
import time

LOG = Path(__file__).resolve().parent
WORK = Path('/tmp/rpfbwt-64-466')
TOOLS = Path('/tmp/sxgc-dist-final')
RPFBWT = LOG / 'rpfbwt.baseline'
LIMIT = 850_000_000_000

def record(**event):
    with (LOG / 'validation.jsonl').open('a') as out:
        out.write(json.dumps(dict(time=time.time(), **event)) + '\n')

def run(stage, command, trace=False):
    command = list(map(str, command))
    env = dict(os.environ, OMP_NUM_THREADS='48', TMPDIR=str(WORK))
    if trace:
        env['LD_PRELOAD'] = str(LOG / 'trace_new.so')
    record(stage=stage, command=command, status='START', address_space_bytes=LIMIT)
    start = time.monotonic()
    with (LOG / (stage + '.log')).open('xb') as output:
        proc = subprocess.Popen(['/usr/bin/time', '-v', '-o', str(LOG / (stage + '.time')),
                                 '/usr/bin/prlimit', '--as=' + str(LIMIT), *command],
                                stdout=output, stderr=output, env=env)
        record(stage=stage, pid=proc.pid)
        code = proc.wait()
    record(stage=stage, status='PASS' if code == 0 else 'FAIL', returncode=code,
           wall_seconds=time.monotonic()-start)
    if code:
        raise RuntimeError(f'{stage}: exit {code}')

def tap_check(prefix):
    with Path(str(prefix)+'.rlebwt.meta').open('rb') as f:
        n, r = struct.unpack('<QQ', f.read(16))
    for ext in ['.ssa', '.ssa_t']:
        path = Path(str(prefix)+ext)
        with path.open('rb') as f:
            count, = struct.unpack('<Q', f.read(8))
        assert count == r and path.stat().st_size == 8+8*r
    record(stage='tap-count', status='PASS', raw_n=n, raw_r=r)
    return n, r

def main():
    prefix = WORK/'parse'
    assert not (WORK/'parse.rlebwt').exists()
    record(status='VALIDATION_ONLY', note='Retained parse; not the from-zero milestone',
           binary=str(RPFBWT), binary_patch='none: testing address-space diagnosis')
    run('full466-rpfbwt', [RPFBWT, '--l1-prefix', prefix, '--w1', 10, '--w2', 5,
                         '--threads', 48, '--chunks', 50, '--tmp-dir', WORK], trace=True)
    tap_check(prefix)
    run('full466-tap-structure', [LOG/'check_tap', prefix])
    ri, heads, agg, chi = [WORK/('validation.'+ext) for ext in ['ri4','head_sa','agg','sA']]
    run('full466-endpoints', [TOOLS/'rpfbwt_endpoints', prefix, ri, heads, '1e'])
    run('full466-slim', [TOOLS/'slim_dump', '--slim', '--resolve-ri4', '--dict-stream',
                       '--ri4', ri, '--head-sa', heads, '--parse', prefix, '-t', 48, '-o', agg])
    run('full466-sweep', [TOOLS/'xsa', 'chi-rspace', '--stream-agg', '--ri4', ri, '--agg', agg, '-o', chi])
    count = chi.stat().st_size//8
    assert chi.stat().st_size % 8 == 0
    assert f'chi = {count}' in (LOG/'full466-sweep.log').read_text()
    record(stage='chi-count', status='PASS', chi=count, validation_only=True)
    names = WORK/'collection.txt.names.tsv'
    run('full466-audit', [TOOLS/'sxi_text_audit', WORK/'collection.txt', ri, agg, chi, 1000,
                        '--agc', '/home/erikg/hprcv2/HPRC_r2_assemblies_0.6.1.agc',
                        '--names', names, '--agc2flat', TOOLS/'agc2flat'])
    output = WORK/'hprc.validation.sxi'
    run('full466-write', [TOOLS/'sxi_write', '--ri4', ri, '--heads', heads, '--chi', chi,
                        '--output', output, '--mode', 'dna', '--orientation', 'reversed', '--names', names])
    run('full466-validate', [TOOLS/'xsa', 'sxi-info', output])
    record(status='PASS', chi=count, validation_only=True, output=str(output))

if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        record(status='FAIL', error=str(error))
        raise
