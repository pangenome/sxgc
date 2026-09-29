#!/usr/bin/env python3
"""Resume the 466 validation from the audit (VALIDATION-ONLY).

Slim and sweep completed (validation.agg, validation.sA, chi=2,250,211,129);
the audit failed on a names.tsv that a parent-session agc2flat prepare verb
had truncated to zero bytes (sidecar next to its -o output). The file is
restored from the original work dir. This driver runs audit -> write ->
validate with the same journaling and ceilings as resume_validation.py.
"""
import json
import os
import re
import subprocess
import time
from pathlib import Path

LOG = Path(__file__).resolve().parent
WORK = Path('/tmp/rpfbwt-64-466')
TOOLS = Path('/tmp/sxgc-dist-final')
LIMIT = 850_000_000_000
AGC = '/home/erikg/hprcv2/HPRC_r2_assemblies_0.6.1.agc'

def record(**event):
    with (LOG / 'validation-resume.jsonl').open('a') as out:
        out.write(json.dumps(dict(time=time.time(), validation_only=True, **event)) + '\n')

def run(stage, command):
    command = list(map(str, command))
    env = dict(os.environ, OMP_NUM_THREADS='48', TMPDIR=str(WORK))
    record(stage=stage, command=command, status='START', address_space_bytes=LIMIT)
    start = time.monotonic()
    with (LOG / (stage + '.log')).open('x') as output:
        proc = subprocess.Popen(['/usr/bin/time', '-v', '-o', str(LOG / (stage + '.time')),
                                 '/usr/bin/prlimit', '--as=' + str(LIMIT), *command],
                                stdout=output, stderr=output, env=env)
        record(stage=stage, pid=proc.pid)
        code = proc.wait()
    text = (LOG / (stage + '.time')).read_text()
    rss = re.search(r'Maximum resident set size \(kbytes\): (\d+)', text)
    record(stage=stage, status='PASS' if code == 0 else 'FAIL', returncode=code,
           wall_seconds=time.monotonic() - start,
           peak_rss_kib=int(rss.group(1)) if rss else None)
    if code:
        raise RuntimeError(f'{stage}: exit {code}')

def main():
    ri, heads = WORK / 'validation.ri4', WORK / 'validation.head_sa'
    agg, chi = WORK / 'validation.agg', WORK / 'validation.sA'
    for name in [ri, heads, agg, chi]:
        assert name.exists(), f'missing completed-stage output: {name}'
    count = chi.stat().st_size // 8
    assert count == 2250211129, count
    record(stage='names-restore', status='PASS',
           note='collection.txt.names.tsv restored from original work dir after parent-session agc2flat prepare verb truncated the work-dir copy to zero bytes (sidecar clobber at 00:00:07Z Sep 29; 69GB materialization already logged as near-miss, sidecar truncation was unnoticed)',
           rows=38790, sum_len_plus_one=1403221068481)
    names = WORK / 'collection.txt.names.tsv'
    run('resume-audit', [TOOLS / 'sxi_text_audit', WORK / 'collection.txt', ri, agg, chi, 1000,
                         '--agc', AGC, '--names', names, '--agc2flat', TOOLS / 'agc2flat'])
    output = WORK / 'hprc.validation.sxi'
    run('resume-write', [TOOLS / 'sxi_write', '--ri4', ri, '--heads', heads, '--chi', chi,
                         '--output', output, '--mode', 'dna', '--orientation', 'reversed',
                         '--names', names])
    run('resume-validate', [TOOLS / 'xsa', 'sxi-info', output])
    record(status='PASS', chi=count, output=str(output),
           note='VALIDATION-ONLY from retained parse; not the from-zero milestone')

if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        record(status='FAIL', error=str(error))
        raise
