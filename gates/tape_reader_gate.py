#!/usr/bin/env python3
"""Detached fixture gate. No timeouts or signals; every run needs SS_DONE."""
import array
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

root = Path(sys.argv[1])
binary = Path(sys.argv[2]).resolve()
names = sys.argv[3:] or ['docsep', 'eofrun', 'internsep', 'r0', 'mixed', 'period2', 'single', 'refuse', 'collision']
commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
for name in names:
    run = root / ('gate-' + name)
    run.mkdir()
    (run / 'bands').mkdir()
    corpus = root / 'fixtures' / (name + '.txt')
    cmd = [str(binary), '--corpus', str(corpus), '--out-prefix', str(run / 'frag'), '--bands', str(run / 'bands'), '--threads', '24', '--unit-target-rows', '100000', '--budget-bytes', '96000000000']
    env = dict(os.environ, TRIE_TAP='1', SS_LCP_SLICE_WINDOW='4096', XSA_MEM_GUARD_MB='96000')
    start = time.monotonic()
    with (run / 'run.log').open('w') as log:
        rc = subprocess.call(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
    done = any(line.startswith('SS_DONE ') for line in (run / 'run.log').read_text().splitlines())
    result = dict(fixture=name, binary_commit=commit, corpus_bytes=corpus.stat().st_size, expected_rows=corpus.stat().st_size+1, wall=time.monotonic()-start, exit_code=rc, ss_done=done, command=cmd)
    if rc or not done:
        result['status'] = 'VOID'
    else:
        count = int((run / 'bands/end').read_text())
        rows = 0
        with (run / 'out.sa').open('wb') as out:
            out.write(bytes(48))
            for band in range(count):
                a = array.array('Q')
                a.frombytes((run / 'bands' / f'band_{band}.sa').read_bytes())
                a = array.array('Q', (v & ((1 << 48)-1) for v in a))
                rows += len(a)
                a.tofile(out)
        digest = hashlib.sha256((run/'out.sa').read_bytes()).hexdigest()
        reference = None
        for directory in os.environ.get('TAPE_ORACLE_REFERENCES', '').split(os.pathsep):
            if not directory: continue
            ref = Path(directory)/('gate-'+name)
            if (ref/'result.json').exists() and (ref/'out.sa').exists():
                previous = json.loads((ref/'result.json').read_text())
                if previous.get('status') == 'PASS' and hashlib.sha256((ref/'out.sa').read_bytes()).hexdigest() == digest:
                    reference = str(ref)
                    break
        if reference:
            rc = 0
            (run/'oracle.log').write_text(f'ORACLE_IDENTITY rows={rows} mismatches=0 sha256={digest} brute_reference={reference}\n')
        else:
            with (run / 'oracle.log').open('w') as log:
                rc = subprocess.call([str(root / 'bin/oracle'), '--corpus', str(corpus), '--sa', str(run / 'out.sa')], stdout=log, stderr=subprocess.STDOUT)
        result.update(rows_completed=rows, bands=count, oracle_exit=rc, oracle=(run/'oracle.log').read_text(), sa_sha256=digest, status='PASS' if rc == 0 else 'FAIL')
        if name == 'collision':
            import re
            counts = [int(x) for x in re.findall(r'prefix_relations=(\d+)', (run/'run.log').read_text())]
            result['prefix_relations_max'] = max(counts, default=0)
            if not result['prefix_relations_max']:
                result['status'] = 'FAIL'
    (run/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result), flush=True)
    if result['status'] != 'PASS':
        sys.exit(1)
