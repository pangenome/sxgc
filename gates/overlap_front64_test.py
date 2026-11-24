"""Independent byte-exact format and seam tests. No corpus or preserved intermediate reads."""
from pathlib import Path
import random
import struct
import subprocess as sp
import sys
import tempfile
import time
from overlap_resident_test import prepare,trigger

def main(front,verify):
    rng=random.Random(84166);tag=next(i.to_bytes(8,'big') for i in range(9999) if trigger(i.to_bytes(8,'big')));end=str(int(time.time())+180)
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);out=d/'out';stop=d/'stop';raw=d/'corpus';a=64<<20
        def build(w,cap=None,grant=a):return sp.run([front,raw,out,str(w),str(grant),str(cap if cap is not None else raw.stat().st_size*2),stop,end],capture_output=True)
        for text in [bytes(range(256))*3,b'abcdefghi'*100,b'\xff\x00'*513,tag*35,(b'12'+tag)*71+tag,b'x'*97+tag]+[rng.randbytes(n) for n in (77,333,4097,15000)]:
            prepare(d,text);old=d/'columns';t=(old/'dictionary.packed').read_bytes();at=0;wide=bytearray();offset=[]
            while at<len(t):h=struct.unpack_from('<I',t,at)[0];n=h&0x7fffffff;offset.append(len(wide));wide.extend(struct.pack('<Q',2*n+(h>>31))+t[at+4:at+4+n]);at+=4+n
            keys=b''.join(struct.pack('<Q',offset[i]) for i, in struct.iter_unpack('<I',(old/'parse.u32').read_bytes()));good=len(wide)+len(keys)<=2*len(text)
            for w in [1,4]:
                p=build(w)
                if not good:assert p.returncode==75 and not out.exists();continue
                assert p.returncode==0 and (out/'dictionary.lex64').read_bytes()==wide and (out/'keys.u64').read_bytes()==keys,(p.stdout,p.stderr)
                args=[verify,out/'dictionary.lex64',out/'keys.u64',old/'dictionary.packed',old/'parse.u32',str(len(offset)),str(a),stop,end];assert sp.run(args,capture_output=True).returncode==0
                (out/'keys.u64').write_bytes(bytes([255])*len(keys));assert sp.run(args,capture_output=True).returncode!=0
                for q in out.iterdir():q.unlink()
                out.rmdir();assert build(w,len(wide)+len(keys)-1).returncode==75 and not out.exists()
        raw.write_bytes(b'x');assert build(4).returncode==75;raw.write_bytes(rng.randbytes(2000));assert build(4,grant=100).returncode==75;stop.touch();assert build(1).returncode==75 and not out.exists()
        n,m=100_000_000_000,3181334488;assert sp.check_output([front,'plan',str(n),str(m),'4'],text=True).strip()==f'front64_array_bound={n+24*m+8+(12<<20)}'
    print('PASS independent FNV and complete LE64 bytes, text-order addresses, 1/4 threads, tagged EOF, malformed column and cap/STOP refusals')
if __name__=='__main__':main(*(Path(x).resolve() for x in sys.argv[1:]))
