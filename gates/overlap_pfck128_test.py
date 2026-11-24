"""No corpus. Independent whole-text reverse Horner, including empty final blocks."""
from pathlib import Path
import random
import struct
import subprocess as sp
import sys
import tempfile
import time

def fixtures(exe):
    assert __debug__; rng=random.Random(87125); positive=negative=0; end=str(int(time.time())+60)
    with tempfile.TemporaryDirectory() as td:
        d=Path(td);raw=d/'raw';map_=d/'remap';pf=d/'pfck';stop=d/'stop';tab=bytes(min(i+1,255) for i in range(256));map_.write_bytes(tab)
        for size in (1,125,126,127,128,129,254,255,256,257,8192*128-1,8192*128+17):
            t=rng.randbytes(size).translate(bytes(i%251 for i in range(256)));raw.write_bytes(t);s=t.translate(tab)+b'\0';n=len(s);h=0
            a=[0]*(n//128+1)
            for i in reversed(range(n)):
                h=(s[i]+1+0x9e3779b185ebca87*h)&((1<<64)-1)
                if i%128==0:a[i//128]=h
            payload=struct.pack('<II4Q',0x4b434650,1,n,128,len(a),0)+struct.pack(f'<{len(a)}Q',*a)
            cmd=[exe,'--fixture',raw,map_,pf,stop,end]
            def run(args):return sp.run(args,capture_output=True,timeout=8)
            pf.write_bytes(payload);p=run(cmd);assert p.returncode==0 and f'checkpoints={len(a)} '.encode() in p.stdout and b'virtual_end=1' in p.stdout;positive+=1
            for wire in (payload[:-1],payload+b'x',payload[:16]+struct.pack('<Q',22)+payload[24:],payload[:-8]+struct.pack('<Q',a[-1]^1),payload[:40]+struct.pack('<Q',a[0]^1)+payload[48:]):
                pf.write_bytes(wire);p=run(cmd);assert p.returncode!=0 and not p.stdout;negative+=1
        pf.write_bytes(payload);assert run([exe,'--v8-100m',*cmd[2:]]).returncode!=0
        stop.write_bytes(b'');assert run(cmd).returncode==75;stop.unlink();assert run([*cmd[:-1],'0']).returncode==75
    print(f'PASS {positive} independent PFCK images, {negative} header/extent/value refusals, IO seam, virtual END/empty checkpoint, wrong rung, STOP/deadline')
if __name__=='__main__':fixtures(Path(sys.argv[1]).resolve())
