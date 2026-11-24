"""Tiny independent suffix sorts on UINT64_MAX keys, including long ties and worker/range seams."""
import itertools
from pathlib import Path
import random
import struct
import subprocess as sp
import sys
import tempfile
import time

MAX=(1<<64)-1

def run(sort,verify):
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);src=d/'s';out=d/'r';stop=d/'stop';end=str(int(time.time())+120)
        def call(*a):return sp.run(list(map(str,a)),capture_output=True)
        def inv(v):
            a=[0]*len(v)
            for r,i in enumerate(sorted(range(len(v)),key=lambda i:v[i:]),1):a[i]=r
            return struct.pack('<%dQ'%len(a),*a)
        def rank(t,grant=128<<20):return call(sort,'rank',src,out,t,grant,stop,end)
        def ck(t,width=8):return call(verify,src,width,out,128<<20,stop,end)
        rng=random.Random(9032);ncase=0
        for v in [[0],[MAX],[MAX,0,MAX],[2**31,2**32-1,2**32,2**63,MAX,0]*11,[1<<41]*1001,list(reversed(range(600))),[MAX]*712+[0]+[MAX]*200]+[list(x)+[MAX,2**32] for x in itertools.product((0,2**32),repeat=3)]+[rng.choices([0,1,2**32,2**63,MAX],k=1200) for _ in range(5)]:
            src.write_bytes(struct.pack('<%dQ'%len(v),*v));want=inv(v)
            for t in (1,4):
                p=rank(t);assert p.returncode==0 and out.read_bytes()==want and ck(t).returncode==0,(p.stdout,p.stderr);out.unlink();ncase+=1
        # Adapter uses real packed offsets and retains the lexical order of a tagged duplicate.
        words=[(b'abcdefghx',0),(b'abcdefghx',1),(b'abcdefghz',0)];tape=d/'d';tape.write_bytes(b''.join(struct.pack('<I',len(s)|(int(e)<<31))+s for s,e in words));legacy=d/'ids';legacy.write_bytes(struct.pack('<4I',2,0,0,1));src.unlink()
        p=call(sort,'lift',tape,legacy,src,3,4,128<<20,stop,end);assert p.returncode==0 and struct.unpack('<4Q',src.read_bytes())==(26,0,0,13),p.stderr
        assert rank(4).returncode==0 and call(verify,legacy,4,out,128<<20,stop,end).returncode==0
        assert rank(1).returncode!=0;original=out.read_bytes()
        for bad in [bytes(32),struct.pack('<4Q',1,2,3,5),struct.pack('<4Q',1,2,3,4),original[:-1]]:
            out.write_bytes(bad);assert ck(1).returncode!=0
        out.unlink();bound=40*4+(4<<20)+(4*2<<20);assert rank(4,bound-1).returncode!=0 and not out.exists();assert rank(4,bound).returncode==0;out.unlink()
        stop.touch();assert rank(4).returncode==75 and not out.exists();stop.unlink();src.write_bytes(bytes(7));assert rank(1).returncode!=0 and not out.exists()
        assert call(sort,'plan',MAX,4).returncode!=0 and call(sort,'plan',MAX+1,4).returncode!=0
        for m in [2**31-1,2**31,3181334488,2**32,2**32+1]: assert call(sort,'plan',m,4).stdout==f'parse64_array_bound={40*m+(12<<20)}\n'.encode()
        print(f'PASS {ncase} exact u64 suffix permutations across 1/4 workers, numeric extrema, lexical offset adapter, separate verifier, grant/refusal and >u32 dimension arithmetic')
if __name__=='__main__':run(*(Path(v).resolve() for v in sys.argv[1:]))
