"""Small-only independent histogram/digest/corruption checks; no archive reads."""
import array
import hashlib
from pathlib import Path
import random
import struct
import subprocess as sp
import sys
import tempfile
import time
from overlap_resident_test import trigger

def main(exe,prod):
    digest=lambda x:hashlib.sha256(x).hexdigest();end=str(int(time.time())+60)
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);f=d/'tape';flag=d/'stop'
        def run(mode='--fixture',b=exe,deadline=end):return sp.run([b,mode,f,flag,deadline],capture_output=True)
        def record(s,e):return struct.pack('<I',len(s)|(int(e)<<31))+s
        def check(words):
            words=sorted(set(words));t=b''.join(record(s,e) for s,e in words);f.write_bytes(t)
            cells=array.array('Q',[0])*(256*257);assert sys.byteorder=='little' and cells.itemsize==8
            for s,e in words:
                for i in range(len(s) if e else len(s)-8):cells[s[i]*257+(0 if i+1==len(s) else s[i+1]+1)]+=1
            r=run();assert r.returncode==0,r.stderr;v=dict(x.split('=') for x in r.stdout.decode().split()[1:])
            assert v['descriptor_cells_sha256']==digest(cells.tobytes()) and v['tape_sha256']==digest(t)
            assert int(v['k'])==len(words) and int(v['descriptors'])==sum(cells) and int(v['largest'])==max(cells) and int(v['D'])==sum(cells)+9*len(words)-7
        rng=random.Random(667)
        for raw in [b'\xff',b'ba'*170+rng.randbytes(700),(b'left\0mid\xff'*10+rng.randbytes(8))*17]:
            cuts=[0]+[i for i in range(1,len(raw)-7) if trigger(raw[i:i+8])];check([(raw[i:j+8],False) for i,j in zip(cuts,cuts[1:])]+[(raw[cuts[-1]:],True)])
        check([(b'x'*9,False),(b'x'*9,True)]);check([(b'a'*((1<<20)-6),False),(b'z\xff',True)])
        for data in [b'',b'\x02',record(b'x',False),record(b'x'*9,False),record(b'z'*9,False)+record(b'a',True),record(b'abc',True)*2,record(b'x'*9,False)+b'\x05\0\0\x80Q',record(b'x'*((1<<20)+1),True)]:
            f.write_bytes(data);r=run();assert r.returncode==70 and not r.stdout
        flag.touch();assert run().returncode==75;flag.unlink();assert run(deadline='1').returncode==75
        assert run('--fixture',prod).returncode==70 and run('--ten-reference',prod).returncode==70
    print('PASS all 65,792 descriptor cells vs independent little-endian hash; EOF, same-word end tag, split buffer, malformed, stop and exact-rung refusals')
if __name__=='__main__':main(*sys.argv[1:])
