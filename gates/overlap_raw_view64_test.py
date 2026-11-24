"""Independent raw suffix sort, PFP tuple keys and explicit shard-wire oracle. No external datasets."""
import hashlib
import itertools
from pathlib import Path
import random
import struct
import subprocess as sp
import sys
import tempfile
import time
from overlap_resident_test import trigger


def dimensions(n,m,w):
    ceil=lambda a,b:(a+b-1)//b
    p=max(1,ceil(n.bit_length(),8));r=max(1,ceil(m.bit_length(),8));blocks=ceil(m,64)
    return n+(p+r)*m+8*blocks+max(8*ceil(n,64),p*m+16*blocks)+(4<<20)+(w<<21)


def fields(text,tag):
    line,=[s for s in text.splitlines() if s.startswith(tag+' ')]
    return dict(x.split('=',1) for x in line.split()[1:])


def reference(t):
    n=len(t);starts=[0]+[p for p in range(1,n-7) if trigger(t[p:p+8])];m=len(starts)
    words=[(t[p:(starts[j+1]+8 if j+1<m else n)],j+1==m) for j,p in enumerate(starts)]
    address={};at=0
    for x in sorted(set(words)):
        address[x]=at;at+=8+len(x[0])
    order=sorted(range(m),key=lambda j:t[starts[j]:]);inv=[0]*m
    for i,j in enumerate(order,1):inv[j]=i
    def h(a,b):
        if not a or not b:return 0
        x,y=(starts[order[z-1]] for z in (a,b))
        return n-x if x==y else sum(1 for _ in itertools.takewhile(lambda z:z[0]==z[1],zip(t[x:],t[y:])))
    parts=[bytearray() for _ in range(3)];total=high=0
    bounds=[min(m,(((m+63)//64)*s//64)*64) for s in range(65)]
    for s,(lo,end) in enumerate(zip(bounds,bounds[1:])):
        stream=[bytearray() for _ in range(3)]
        for r in range(lo+1,end+1):
            j=order[r-1];p=starts[j];l=h(r-1,r);total+=l;high=max(high,l)
            stream[0]+=struct.pack('<QQH',p,l,t[p-1] if p else 256);stream[1]+=struct.pack('<QQ',address[words[j]],inv[j+1] if j+1<m else 0)
        a=lo+1
        if a<=end:
            for u,v in [(0,a),(a,a),(a,end),(end,min(m,end+1)),(end,1+(end*5851+17)%m)]:stream[2]+=struct.pack('<QQQ',u,v,h(u,v))
        for acc,shard in zip(parts,stream):acc+=hashlib.sha256(shard).digest()
    roots=tuple(hashlib.sha256(p).hexdigest() for p in parts)
    return inv,[address[x] for x in words],roots,total,high,len(address),at


def run(view,oracle):
    rng=random.Random(865501);e=str(int(time.time())+180)
    tag=next(i.to_bytes(8,'big') for i in range(9999) if trigger(i.to_bytes(8,'big')))
    texts=[b'a',tag,b'z'*15+tag,bytes(range(256))*6,b'@'*250,b'@'*1450,b'x'*5500+tag+b'x'*1170,
           (b'a'+tag)*99+tag,(tag+b'x')*72+tag[:3],(b'x'+tag+b'z'+tag+b'\x00')*211,bytes(range(60))*70]
    for i in range(28):
        base=bytes(rng.choices(rng.sample(range(256),(2,5,255)[i%3]),k=rng.randrange(15,450)))
        texts.append(base*(1+i%7)+rng.randbytes(i*2))
    accepted=capped=carry=0
    with tempfile.TemporaryDirectory() as t:
        d=Path(t);raw=d/'raw';inv=d/'ranks';keys=d/'keys';stop=d/'stop'
        for text in texts:
            raw.write_bytes(text);r,k,expect,total,maxh,groups,tape=reference(text);m=len(r);n=len(text)
            inv.write_bytes(struct.pack(f'<{m}Q',*r));keys.write_bytes(struct.pack(f'<{m}Q',*k));args=[oracle,raw,keys,inv,str(64<<20),stop,e];v=sp.run(args,capture_output=True)
            if total>384*n:assert v.returncode==75 and b'comparison guard' in v.stderr;capped+=1
            else:
                assert v.returncode==0,(n,v.stderr);o=fields(v.stdout.decode(),'RAW_VIEW_ORACLE');assert tuple(o[key] for key in ('raw_root','nav_root','query_root'))==expect and int(o['sum_lcp'])==total;accepted+=1
            for w in (1,2,4):
                args=[view,raw,inv,str(w),str(dimensions(n,m,w)),stop,e];z=sp.run(args,capture_output=True);assert z.returncode==0,(n,m,w,z.stderr)
                f=fields(z.stdout.decode(),'RAW_VIEW');pr=fields(z.stdout.decode(),'VIEW_PROOF');assert tuple(pr[key] for key in ('raw_root','nav_root','query_root'))==expect,(n,m,w,f,pr)
                assert (int(f['k']),int(f['tape']),int(f['max_lcp']),int(f['extended'])+int(f['carried']))==(groups,tape,maxh,total);carry+=int(f['carried']);args[4]=str(dimensions(n,m,w)-1);assert sp.run(args,capture_output=True).returncode==75
        # Invalid shared destination must be detected BEFORE either thread races on packed writes.
        text=b'@'*256;raw.write_bytes(text);r,k,*_=reference(text);m=len(r);keys.write_bytes(struct.pack(f'<{m}Q',*k))
        def bad(v):
            inv.write_bytes(struct.pack(f'<{len(v)}Q',*v));assert sp.run([view,raw,inv,'4',str(64<<20),stop,e],capture_output=True).returncode==75
        bad([r[0]]*m);bad([0]+r[1:]);bad([2**64-1]+r[1:]);bad(r[:-1]);r[0],r[-1]=r[-1],r[0];bad(r)
        inv.write_bytes(b'bad');assert sp.run([view,raw,inv,'1',str(64<<20),stop,e],capture_output=True).returncode==75
        stop.touch();assert sp.run([view,raw,inv,'4',str(64<<20),stop,e],capture_output=True).returncode==75
    for n,m in [(2**32,2**32-1),(2**32+1,2**32+1),(10**11,3181334488),(2**42+7,2**33+7)]:
        z=fields(sp.check_output([view,'plan',str(n),str(m),'4'],text=True),'RAW_VIEW_PLAN');assert int(z['arrays'])==dimensions(n,m,4)
    assert accepted and capped and carry>0;print(f'PASS {len(texts)} raw+inverse views at 1/2/4 workers; {accepted} direct raw oracles, {capped} predicted quadratic-work refusals, independent Python shard hashes and exact malformed-rank/grant/STOP refusals')
if __name__=='__main__':run(*(Path(x).resolve() for x in sys.argv[1:]))
