"""Independent learned-range census, exact EF payload and complete byte stream oracle; generated inputs only."""
from bisect import bisect_right
from pathlib import Path
import random
import struct
import subprocess as sp
import sys
import tempfile
import time
from overlap_finite_emit64_test import byte_reference,large_class
from overlap_finite_route64_plan import bits,ceil
from overlap_raw_view64_test import fields
from overlap_resident_test import trigger
MASK=2**64-1

def mix(x):
    x=(x+0x9e3779b97f4a7c15)&MASK;x=((x^x>>30)*0xbf58476d1ce4e5b9)&MASK;x=((x^x>>27)*0x94d049bb133111eb)&MASK;return x^x>>31

def upper(u,v,b,t):return ceil(min(u*(l+1)+b*((v-1)>>l) for l in range(64)),8)+16*b*t

def probe(text,inv,t,target):
    n=len(text);m=len(inv);start=[0]+[i for i in range(1,n-7) if trigger(text[i:i+8])];groups={}
    for j,r in enumerate(inv):
        p=start[j];terminal=(j+1==m);word=text[p:n if terminal else start[j+1]+8];groups.setdefault((word,terminal),[]).append(r)
    head={min(rs):(w,terminal,len(rs)) for (w,terminal),rs in groups.items()};u=sum(len(w)-(0 if end else 8) for w,end,_ in head.values());pg=m+(u-len(head))//32;available=2*n-8*m
    ok=[b for b in range(1,1+min(16384,u,ceil(u,target))) if upper(u,32*pg,b,t)<=available]
    if not ok:return None
    b=max(ok);data=[];universes=[]
    for lane in range(t):
        page=0
        for r in range(1+m*lane//t,1+m*(lane+1)//t):
            if r in head:
                w,end,f=head[r];stride=len(w)-(0 if end else 8)
                for o in range(stride):data.append(((w[o:],end),lane,page*32+o,f))
                page+=(stride-1)//32
            page+=1
        universes.append(page*32)
    ns=min(u,b*64);pick=[]
    for j in range(ns):
        lo=u*j//ns;hi=u*(j+1)//ns;pick.append(data[lo+mix(j)%(hi-lo)][0])
    pick.sort();piv=sorted(set(pick[ns*i//b] for i in range(1,b))-{pick[0]});count=[[0]*t for _ in range(len(piv)+1)];last=[[0]*t for _ in count];mass=[0]*len(count)
    for k,lane,x,f in data:
        z=bisect_right(piv,k);count[z][lane]+=1;last[z][lane]=x;mass[z]+=f
    wire=pop=explicit=tails=0
    for cs,xs in zip(count,last):
        for c,x,v in zip(cs,xs,universes):
            if not c:continue
            _,l=bits(v,c);low=ceil(l*c,64);high=ceil(c+((v-1)>>l),64);used=ceil(c+(x>>l),64);wire+=8*(low+high);pop+=8*(low+used);tails+=8*(low%512+high%512);explicit+=8*(low+min(used,high//512*512)+high%512)
    return dict(bands=len(count),target_bands=b,samples=ns,max_range_descriptors=max(map(sum,count)),max_range_rows=max(mass),guaranteed_wire_upper=upper(u,pg*32,b,t),fragment_bytes=tails,explicit_write=explicit),dict(wire=wire,populated=pop,descriptors=u)

def run(exe):
    rng=random.Random(643729);tag=next(i.to_bytes(8,'little') for i in range(2000) if trigger(i.to_bytes(8,'little')));big=large_class(rng,tag);cases=[b'A',b'@'*71,(b'\0'+tag+b'\0')*211,bytes(range(256))*8,(tag+b'\xff\0xy')*312+b'A',b'z'*1500+tag+b'z'*170+tag[:3],big]
    for i in range(10):cases.append(bytes(rng.choices(rng.sample(range(256),3+i*19),k=rng.randrange(320,1400)))*(1+i%3)+rng.randbytes(i))
    ok=bad=full=huge=limited=0
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);raw=d/'src';inv=d/'inverse';idx=d/'index';stop=d/'stop';end=str(int(time.time())+180)
        for text in cases:
            raw.write_bytes(text);r,expect=byte_reference(text);inv.write_bytes(struct.pack(f'<{len(r)}Q',*r));slots=8192 if text==big else max(128,len(text)//8);target=max(1,slots//5)
            for t in (1,2,4):
                pred=probe(text,r,t,target);args=[exe,'sampled',raw,inv,idx,str(t),str(160<<20),str(8*len(r)),str(target),str(slots),stop,end];z=sp.run(args,capture_output=True)
                if pred is None:assert z.returncode==75 and not idx.exists() and not z.stdout and b'scratch' in z.stderr,(len(text),z.stderr);bad+=1;continue
                assert z.returncode==0 and z.stdout==expect,(len(text),t,z.stderr);a,b=pred;measured=fields(z.stderr.decode(),'DIRECT_RANGES');stream=fields(z.stderr.decode(),'FINITE_EMIT');assert all(int(measured[k])==v for k,v in a.items()) and all(int(stream[k])==v for k,v in b.items()),(len(text),t,a,b,measured,stream);assert b['wire']==idx.stat().st_size and a['explicit_write']<=b['wire']<=a['guaranteed_wire_upper']<=2*len(text)-8*len(r);idx.unlink();ok+=1;full+=a['fragment_bytes']<b['wire'];limited+=a['target_bands']<ceil(b['descriptors'],target)
                if text==big:assert int(stream['max_tie'])>=8311 and int(stream['exact_bypass']);huge+=1
    assert ok and bad and full and huge==3 and limited;print(f'PASS {ok} exact sampled layouts/full byte streams at 1/2/4 workers, {bad} certified pre-open scratch refusals, {full} mixed page/fragment payloads, {limited} budget-limited splitter counts and 3 skewed >8192-slot ties')
if __name__=='__main__':run(Path(sys.argv[1]).resolve())
