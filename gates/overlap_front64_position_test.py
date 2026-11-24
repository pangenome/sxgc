"""Check the low-reservation builder's actual columns, accounting, failure and geometry. No Pile read."""
from pathlib import Path
import random
import struct
import subprocess as sp
import sys
import tempfile
import time
from overlap_front64_balance import balance
from overlap_resident_test import prepare, trigger


def bound(n, m, w):
    div = lambda a,b: (a+b-1)//b
    return max(n,8*m)+8*div(n,64)+8*(div(n,512)+1)+8*m+8*div(m,64)+(4<<20)+(w<<21)


def run(front):
    end=str(int(time.time())+150);rng=random.Random(9392)
    tag=next(i.to_bytes(8,'big') for i in range(10000) if trigger(i.to_bytes(8,'big')))
    texts=[b'x'*33,bytes(range(256))*10,b'@'*700,(b'ab'+tag)*151+tag,
           (tag+b'x'*5940)*2+tag,b'z'*503+tag+b'\xff'*551+tag,tag*31+b'X',
           *[rng.randbytes(n) for n in (62,63,64,65,511,512,513,1023,4096,37111)]]
    good=0;refused=0
    for raw in texts:
        with tempfile.TemporaryDirectory() as t:
            d=Path(t);prepare(d,raw);inp=d/'corpus';n=len(raw);stop=d/'stop';dest=d/'new';src=d/'columns'
            legacy=(src/'dictionary.packed').read_bytes();addr=[];want=bytearray();at=0
            while at<len(legacy):
                h=struct.unpack_from('<I',legacy,at)[0];span=h&0x7fffffff
                addr.append(len(want));want.extend(struct.pack('<Q',2*span+(h>>31))+legacy[at+4:at+4+span]);at+=4+span
            key=b''.join(struct.pack('<Q',addr[i]) for i, in struct.iter_unpack('<I',(src/'parse.u32').read_bytes()));m=len(key)//8
            for w in [1,4]:
                def build(grant=bound(n,m,w),cap=2*n):
                    return sp.run([front,'pos',inp,dest,str(w),str(grant),str(cap),stop,end],capture_output=True)
                z=build();total=len(want)+len(key)
                if total>2*n:
                    assert z.returncode==75 and not dest.exists() and not (d/'new.partial').exists();refused+=1;continue
                assert z.returncode==0 and (dest/'dictionary.lex64').read_bytes()==want and (dest/'keys.u64').read_bytes()==key,(z.stderr,n,w)
                report=balance(z.stdout.decode());assert sum(report['order']['lane_rows'])==m;good+=1
                assert build().returncode==75 and (dest/'keys.u64').read_bytes()==key
                for f in dest.iterdir():f.unlink()
                dest.rmdir();assert build(grant=bound(n,m,w)-1).returncode==75 and not dest.exists();assert build(cap=total-1).returncode==75 and not dest.exists()
                stop.touch();assert build().returncode==75;stop.unlink()
    for n,m in [(100_000_000_000,3181334488),(10_000_000_000,318164488),((1<<35)+2,(1<<32)+2)]:
        assert sp.check_output([front,'plan-pos',str(n),str(m),'4'],text=True).strip()==f'front64_position_bound={bound(n,m,4)}'
    assert good and refused;print(f'PASS {good} complete position-front column pairs, {refused} dense cap refusals, bitmap seams, long holes, grants, fresh outputs, STOP and wide extents')
if __name__=='__main__':run(Path(sys.argv[1]).resolve())
