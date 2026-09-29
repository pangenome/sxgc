#!/usr/bin/env python3
"""Resume the 466 validation from completed endpoints outputs (VALIDATION-ONLY).

Front-end (rpfbwt) outputs in WORK are retained READ-ONLY inputs; this driver
never touches parse.* or parse.rlebwt*/.ssa/.ssa_t. Endpoints must already be
complete (validation.ri4/validation.head_sa with ENDPOINT_PASS in the log).
Downstream: slim -> sweep -> audit -> write -> validate, all journaled to
validation-resume.jsonl. Not the from-zero milestone.
"""
import json
import os
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

LOG = Path(__file__).resolve().parent
WORK = Path('/tmp/rpfbwt-64-466')
TOOLS = Path('/tmp/sxgc-dist-final')
SEAM = LOG / 'seam-policy-tools'      # rebuilt from gated workspace sources
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
    wall = re.search(r'Elapsed \(wall clock\) time .*?: (.+)', text)
    record(stage=stage, status='PASS' if code == 0 else 'FAIL', returncode=code,
           wall_seconds=time.monotonic() - start,
           peak_rss_kib=int(rss.group(1)) if rss else None,
           elapsed_field=wall.group(1) if wall else None)
    if code:
        raise RuntimeError(f'{stage}: exit {code}')

def main():
    for name in ['parse.rlebwt', 'parse.rlebwt.meta', 'parse.ssa', 'parse.ssa_t']:
        assert (WORK / name).exists(), f'missing front-end output: {name}'
    ri, heads = WORK / 'validation.ri4', WORK / 'validation.head_sa'
    assert ri.exists() and heads.exists(), 'endpoints outputs missing; run resume-endpoints first'
    log = (LOG / 'resume-endpoints.log').read_text()
    assert 'ENDPOINT_PASS' in log, 'endpoints did not pass'
    seam = re.search(r'CYCLIC_SEAM_WORK max_seam_lce=(\d+) total_work=(\d+) total_limit=(\d+)', log)
    if seam:
        record(stage='endpoints-stats', max_seam_lce=int(seam.group(1)),
               seam_total_work=int(seam.group(2)), seam_total_limit=int(seam.group(3)), exact=1)
    with ri.open('rb') as f:
        n_pad, n, strings, runs = struct.unpack('<4Q', f.read(32))
    record(stage='endpoints-header', n=n, strings=strings, runs=runs)
    agg, chi = WORK / 'validation.agg', WORK / 'validation.sA'
    run('resume-slim', [SEAM / 'slim_dump', '--slim', '--resolve-ri4', '--dict-stream',
                        '--ri4', ri, '--head-sa', heads, '--parse', WORK / 'parse',
                        '-t', 48, '-o', agg])
    run('resume-sweep', [TOOLS / 'xsa', 'chi-rspace', '--stream-agg', '--ri4', ri,
                         '--agg', agg, '-o', chi])
    count = chi.stat().st_size // 8
    assert chi.stat().st_size % 8 == 0
    assert f'chi = {count}' in (LOG / 'resume-sweep.log').read_text()
    record(stage='chi-count', status='PASS', chi=count)
    names = WORK / 'collection.txt.names.tsv'
    # collection.txt is deliberately NOT created: with --agc/--names the audit
    # never opens the text path (sxi_text_audit.cpp: if(!archive){open...}).
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
