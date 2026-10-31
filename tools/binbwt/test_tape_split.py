#!/usr/bin/env python3
"""Check next-byte subdivision against an independent finite-suffix oracle."""
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
          bytes(rng.randrange(3,250) for _ in range(5000))]
with tempfile.TemporaryDirectory(prefix='tape-split-') as tmp:
    root=Path(tmp);(root/'remap').write_bytes(bytes(range(256)))
    for i,data in enumerate(fixtures):
        case=root/str(i);case.mkdir();(case/'waves').mkdir();(case/'corpus').write_bytes(data)
        run=subprocess.run([a.scatter,'--corpus',str(case/'corpus'),'--remap',str(root/'remap'),
            '--scratch',str(case/'waves'),'--out',str(case/'sa'),'--threads','2',
            *(['--ram-gb','2'] if a.standalone else ['--budget-bytes',str(2<<30)]),'--scratch-cap-gb','1','--no-lcp-slices'],capture_output=True,text=True,
            timeout=30,env=dict(os.environ,SS_SORT_SPLIT_S='0.000001'))
        assert run.returncode==0,run.stderr
        wire=(case/'sa').read_bytes()[48:]
        positions=list(struct.unpack('<'+'Q'*(len(wire)//8),wire))
        expected=sorted(range(len(data)+1),key=lambda p:data[p:])
        assert positions==expected,(i,positions[:20],expected[:20])
        symbols=(case/'sa.sym').read_bytes()
        assert symbols==bytes(0 if p==0 else data[p-1] for p in expected),i
        if i<3:assert 'SS_SPLIT ' in run.stderr,run.stderr
        print(f'PASS finite suffix order and BWT symbols fixture={i} rows={len(expected)}')
