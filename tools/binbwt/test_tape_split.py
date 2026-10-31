#!/usr/bin/env python3
"""Check recursive sampled planning against an independent finite-suffix oracle."""
import argparse
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--scatter',required=True)
p.add_argument('--standalone',action='store_true')
a=p.parse_args()
rng=random.Random(12)
fixtures=[b'm'*2000+b'a'+b'm'*2000+b'z'+b'm'*2000, b'a'*6000, b'abcd'*2000,
          b''.join(b'prefix'*6+bytes([rng.randrange(3,250)]) for _ in range(300)),
          bytes(rng.randrange(3,250) for _ in range(5000)), b'a'*1800]
with tempfile.TemporaryDirectory(prefix='tape-split-') as tmp:
    root=Path(tmp);(root/'remap').write_bytes(bytes(range(256)))
    for i,data in enumerate(fixtures):
        case=root/str(i);case.mkdir();(case/'waves').mkdir();(case/'corpus').write_bytes(data)
        run=subprocess.run([a.scatter,'--corpus',str(case/'corpus'),'--remap',str(root/'remap'),
            '--scratch',str(case/'waves'),'--out',str(case/'sa'),'--threads','2',
            *(['--ram-gb','0.000064' if i==5 else '2'] if a.standalone else ['--budget-bytes',str(2<<30)]),'--scratch-cap-gb','1','--no-lcp-slices'],capture_output=True,text=True,
            timeout=30,env=os.environ,stdin=subprocess.DEVNULL)
        assert run.returncode==0,run.stderr
        wire=(case/'sa').read_bytes()[48:]
        positions=list(struct.unpack('<'+'Q'*(len(wire)//8),wire))
        expected=sorted(range(len(data)+1),key=lambda p:data[p:])
        assert positions==expected,(i,positions[:20],expected[:20])
        symbols=(case/'sa.sym').read_bytes()
        assert symbols==bytes(0 if p==0 else data[p-1] for p in expected),i
        assert 'TAPE_DEPTH_HISTOGRAM ' in run.stderr,run.stderr
        assert 'SS_SPLIT ' not in run.stderr,run.stderr
        if i==5 and a.standalone: assert 'order=analytic' in run.stderr,run.stderr
        print(f'PASS finite suffix order and BWT symbols fixture={i} rows={len(expected)}')

    if a.standalone:
        case=root/'cap';case.mkdir();(case/'waves').mkdir()
        (case/'corpus').write_bytes(b'a'*400+b'z'+b'a'*400+b'b')
        run=subprocess.run([a.scatter,'--corpus',str(case/'corpus'),
            '--scratch',str(case/'waves'),'--out',str(case/'sa'),
            '--threads','2','--ram-gb','0.0000096','--scratch-cap-gb','1',
            '--no-lcp-slices'],capture_output=True,text=True,timeout=30,stdin=subprocess.DEVNULL)
        assert run.returncode==2,run.stderr
        assert 'TAPE_LEAF_AT_CAP rows=418' in run.stderr,run.stderr
        assert 'planner bug: completed band exceeds target x 1.1' in run.stderr,run.stderr
        print('PASS depth-cap defect and row-bound refusal')
