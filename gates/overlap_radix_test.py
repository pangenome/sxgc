"""Adversarial range cuts, occurrence skew exceeding block credits, and stop."""
import os
from pathlib import Path
import re
import struct
import subprocess as sp
import sys
import tempfile
from overlap_resident_test import prepare,trigger

def rows(text):
    sa=sorted(range(len(text)+1),key=lambda i:text[i:]);old=len(text);out=bytearray()
    for j in sa:
        h=0
        while max(j,old)+h<len(text) and text[j+h]==text[old+h]:h+=1
        out.extend(struct.pack('<III',j,h,text[j-1] if j else 256));old=j
    return out

def main(exe):
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);cmd=[exe,d/'columns',d/'stop','200000000'];env=dict(os.environ,XSA_RESIDENT_PACKED='1',XSA_RESIDENT_WIDE_DICT='1',XSA_RESIDENT_RADIX_LANES='4');tr=next(i.to_bytes(8,'big') for i in range(10000) if trigger(i.to_bytes(8,'big')))
        text=b''.join(tr+bytes([i])+b'common-tail!' for i in range(1,65))+tr;prepare(d,text);want=rows(text);adjusted=absorbed=0
        for step in [1,3,7,17]:
            p=sp.run(cmd,env=dict(env,XSA_RESIDENT_RADIX_STEP=str(step)),capture_output=True,timeout=10);assert p.returncode==0 and p.stdout==want,p.stderr
            a,b=re.search(rb'adjusted=(\d+) absorbed=(\d+)',p.stderr).groups();adjusted+=int(a);absorbed+=int(b)
        assert adjusted and absorbed,(adjusted,absorbed)
        # All 8-comma windows trigger. One group has >3 full row blocks;
        # the join must drain the lane, not accumulate the whole group/tile.
        n=210007;c=44;assert trigger(bytes([c])*8);col=d/'columns'
        (col/'parse.u32').write_bytes(struct.pack('<I',1)*(n-8)+bytes(4))
        (col/'dictionary.packed').write_bytes(struct.pack('<I',0x80000008)+bytes([c])*8+struct.pack('<I',9)+bytes([c])*9)
        env['XSA_RESIDENT_RADIX_STEP']='1';p=sp.run(cmd,env=env,capture_output=True,timeout=15);assert p.returncode==0,p.stderr
        assert p.stdout==b''.join(struct.pack('<III',n-i,max(0,i-1),256 if i==n else c) for i in range(n+1))
        assert int(re.search(rb'max_range_rows=(\d+)',p.stderr)[1])>=n-8
        p=sp.Popen(cmd,env=env,stdout=sp.PIPE,stderr=sp.PIPE);assert len(p.stdout.read(12))==12;(d/'stop').touch();_,log=p.communicate(timeout=10);assert p.returncode==75 and b'RESIDENT_STOP' in log
        (d/'stop').unlink();bad=sp.run(cmd,env=dict(env,XSA_RESIDENT_RADIX_LANES='200'),capture_output=True);assert bad.returncode and not bad.stdout and b'outside grant' in bad.stderr
    print('PASS global seam LCP, moved/absorbed tied cuts, skew >3 blocks, cooperative stop and lane grant refusal')
if __name__=='__main__':main(Path(sys.argv[1]))
