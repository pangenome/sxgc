"""Small, complete streams from an independent brute SA. Reject broken/old frames."""
from pathlib import Path
import random
import subprocess as sp
import sys
import tempfile

def wire(p,h,b):return ((p&0xffffffff) | ((h&0xffffffff)<<32) | ((0x80000000+b+(p>>32)*512+(h>>32)*1048576)<<64)).to_bytes(12,'little')

def main(exe):
    assert wire(0x112345678,0x298765432,256).hex()=='785634123254769800032080'
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);corpus=d/'text';flag=d/'stop';cmd=[exe,corpus,flag,'--fixture'];rng=random.Random(981)
        for text in [b'a',b'\0\xff\xff',b'banana',b'a'*211,b'ab\x00'*40,rng.randbytes(520),bytes(range(256))]:
            corpus.write_bytes(text);sa=sorted(range(len(text)+1),key=lambda i:text[i:]);frames=[];prev=len(text)
            for j,p in enumerate(sa):
                h=0
                if j:
                    while max(prev,p)+h<len(text) and text[p+h]==text[prev+h]:h+=1
                frames.append(wire(p,h,text[p-1] if p else 256));prev=p
            stream=b''.join(frames)
            def run(data):return sp.run(cmd,input=data,capture_output=True)
            r=run(stream);assert r.returncode==0 and f'ORACLE rows={len(sa)} independent_final_rank={len(sa)} all_adjacent_sa_lcp_bwt_verified=1'.encode() in r.stdout,r.stderr
            for bad in (stream[:-1],stream[:-12],stream+frames[0],b'\0'*12+stream[12:],wire(len(text)+(1<<32),0,text[-1])+stream[12:],wire(len(text),1<<32,text[-1])+stream[12:],wire(len(text),0,257)+stream[12:],wire(len(text),0,text[-1]^1)+stream[12:]):
                r=run(bad);assert r.returncode==70 and not r.stdout,r.stderr
        flag.touch();assert run(stream).returncode==75;assert sp.run(cmd[:-1],input=stream,capture_output=True).returncode==70
    print('PASS 7 full brute row43 streams; truncated, extra, narrow, invalid wide extent, BWT and stop/rung refusals')

if __name__=='__main__':main(Path(sys.argv[1]))
