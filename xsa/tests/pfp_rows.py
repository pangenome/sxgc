"""Qualify parse -> ordered row43 on tiny byte suffix oracles, not on a corpus job."""
import os
from pathlib import Path
import random
import re
import struct
import subprocess as sp
import sys
import tempfile
from pfp_parse import run, trigger


def expected(text):
    n = len(text); prev = n
    for p in sorted(range(n+1), key=lambda i: text[i:]):
        h = 0
        while max(prev,p)+h<n and text[prev+h]==text[p+h]: h+=1
        yield p,h,text[p-1] if p else 256
        prev = p


def check(tools, text, vary=False, shared=False):
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp); raw=d/'raw'; raw.write_bytes(text); cols=d/'cols'; cols.mkdir()
        roomy=64<<20; run([tools/'pfp_parse',raw,cols,1,roomy,roomy])
        run([tools/'stream_rank','inverse',cols/'parse.u64',cols/'inverse',1,roomy])
        want=list(expected(text))
        def emit(name,workers,grant):
            inputs=d/name; inputs.mkdir()
            for f in ('parse.u64','phrases.ref','inverse'): os.link(cols/f,inputs/f)
            got=sp.run(list(map(str,[tools/'pfp_rows',raw,inputs,workers,grant,roomy])),capture_output=True,timeout=15)
            assert got.returncode==0,got.stderr
            rows=[]
            for p,h,b in struct.iter_unpack('<III',got.stdout):
                assert b>>31==1
                rows.append((p|((b>>9)&2047)<<32,h|((b>>20)&2047)<<32,b&511))
            assert rows==want,name
            m={k.decode():int(v) for k,v in re.findall(rb'([a-z_]+)=(\d+)',got.stderr.split(b'PFP_ROWS ')[1])}
            assert m['offsets']==m['counted']==m['routed']==m['ordered'] and m['rows']==len(want)
            assert {p.name for p in inputs.iterdir()}=={'postings'}
            return m
        a=emit('roomy',1,roomy)
        if shared: assert a['shared_groups']>0,a
        if vary:
            b=emit('small',1,a['emit_fixed']+16*(a['offsets']//8))
            assert a['ranges']<b['ranges'] and a['counted']==b['counted'] and a['groups']==b['groups'],(a,b)
            if len(os.sched_getaffinity(0))>=2: emit('parallel',2,roomy)
            print(f"ranges {a['ranges']} -> {b['ranges']}; {a['offsets']} dictionary offsets each counted/routed/ordered once; {a['rows']} exact rows")


def main(tools):
    tr=next(i.to_bytes(8,'big') for i in range(1000) if trigger(i.to_bytes(8,'big')))
    for text in [b'x',bytes([255,0,30]),((b'a'+b'b'*15)*5)[:70]+b'Z',b','*135,b'a'*129,tr+tr]: check(tools,text)
    check(tools,b''.join(tr+bytes([i])+b'common-tail!' for i in range(1,64))+tr,shared=True)
    check(tools,random.Random(733).randbytes(8192).replace(b'\xff',b'\x00'),vary=True)
    print('PASS positions, adjacent byte LCPs across ranges, predecessor, shared representatives, final overlap, END')

if __name__=='__main__': main(Path(sys.argv[1]).resolve())
