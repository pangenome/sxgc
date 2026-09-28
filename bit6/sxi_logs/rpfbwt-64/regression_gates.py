#!/usr/bin/env python3
"""Compare retained accepted outputs only after each producer has exited."""
import json
from pathlib import Path
import subprocess
import time

LOG = Path(__file__).resolve().parent
CASES = {
    'yeast': (Path('/tmp/rpfbwt-64-yeast/parse'),
              Path('/tmp/sxgc-distribution-yeast/xsa-build-wj3k_ccs/parse'),
              ['.rlebwt', '.rlebwt.meta', '.ssa', '.ssa_t']),
    'k10': (Path('/tmp/rpfbwt-64-k10/parse'),
            Path('/mnt/nvme3n1/erikg/sxgc-pilot/k10/h10ss_pfp'),
            ['.rlebwt', '.rlebwt.meta', '.ssa']),
    'full466': (Path('/tmp/rpfbwt-64-466/parse'), None, []),
}

with (LOG/'regression-gates.jsonl').open('a', buffering=1) as report:
    while CASES:
        for name, (prefix, reference, extensions) in list(CASES.items()):
            timing = LOG/(name+'-rpfbwt.time')
            text = timing.read_text() if timing.exists() else ''
            if 'Exit status:' not in text:
                continue
            try:
                assert '\tExit status: 0' in text and 'Command terminated' not in text, text
                for extension in extensions:
                    command = ['/usr/bin/cmp', str(prefix)+extension, str(reference)+extension]
                    subprocess.run(command, check=True)
                    report.write(json.dumps(dict(time=time.time(), case=name, file=extension,
                                                 status='BYTE_IDENTICAL'))+'\n')
                with (LOG/(name+'-tap-structure.log')).open('wb') as out:
                    subprocess.run(['/usr/bin/time', '-v', '-o', str(LOG/(name+'-tap-structure.time')),
                                    str(LOG/'check_tap'), str(prefix)], stdout=out, stderr=out, check=True)
                report.write(json.dumps(dict(time=time.time(), case=name, status='PASS',
                                             check='tap structure and applicable retained byte gates'))+'\n')
            except Exception as error:
                report.write(json.dumps(dict(time=time.time(), case=name, status='FAIL', error=str(error)))+'\n')
            del CASES[name]
        if CASES:
            time.sleep(5)
