"""Brute raw suffix order -> distinct heads/offsets; no C++ page index or EF decoder in reference."""
import hashlib
import random
import struct
import subprocess as sp
import sys
import tempfile
import time
from pathlib import Path
from overlap_resident_test import trigger
from overlap_finite_route64_plan import CELLS,P,bands,bits,ceil,reports
from overlap_raw_view64_test import fields

def reference(t,target):
    n=len(t);starts=[0]+[i for i in range(1,n-7) if trigger(t[i:i+8])];order=sorted(range(len(starts)),key=lambda j:t[starts[j]:]);m=len(order);inv=[0]*m
    groups={}
    for r,j in enumerate(order,1):
        inv[j]=r;p=starts[j];end=j==m-1;word=t[p:(n if end else starts[j+1]+8)];groups.setdefault((word,end),[]).append(r)
    rows=[0]*CELLS;items=[];owned={}
    for (word,end),rs in groups.items():
        assert rs==list(range(rs[0],rs[0]+len(rs)));s=len(word)-(0 if end else 8);r=rs[0];owned[r]=s
        for o in range(s):
            cell=word[o]*257+(word[o+1]+1 if o+1<len(word) else 0);rows[cell]+=len(rs);items.append((r,o,cell))
    assert sum(rows)==n and items==sorted(items);bounds=bands(rows,target);by=[[] for _ in bounds];mapping=[0]*CELLS
    for i,(lo,end) in enumerate(bounds):mapping[lo:end]=[i]*(end-lo)
    for r,o,c in items:by[mapping[c]].append((r,o))
    root=hashlib.sha256();largest=max(map(len,by));oversize=0
    for (lo,hi),arr in zip(bounds,by):
        mass=sum(rows[lo:hi]);oversize+=mass>target;h=hashlib.sha256(struct.pack('<4Q',lo,hi,len(arr),mass));h.update(b''.join(struct.pack('<2Q',*x) for x in arr));root.update(h.digest())
    def shape(team):
        wc=pc=0
        for lane in range(team):
            lo=m*lane//team;hi=m*(lane+1)//team;pages=hi-lo+sum((s-1)//P for r,s in owned.items() if lo<r<=hi);pc+=pages;v=pages*P
            for arr in by:
                c=sum(lo<r<=hi for r,o in arr)
                if c:
                    _,l=bits(v,c);wc+=8*(ceil(c*l,64)+ceil(c+((v-1)>>l),64))
        return pc,wc
    return inv,dict(root=root.hexdigest(),descriptors=len(items),k=len(groups),max_band_descriptors=largest,oversize_cells=oversize,bands=len(by)),shape

def run(exe):
    rng=random.Random(1500224);tag=next(i.to_bytes(8,'little') for i in range(9999) if trigger(i.to_bytes(8,'little')))
    texts=[b'Q',tag,b'@'*513,bytes(range(256))*24,b'x'*4444+b'\xff', (tag+b'ab')*99+tag,(tag+b'abcde'+tag+b'xyz')*80+tag[:3],b'$'*17+tag+b'x'*10104+tag+b'z'*120]
    for i in range(10):
        v=bytes(rng.choices(rng.sample(range(256),2+(i*23)),k=rng.randint(600,2100)));texts.append(v*(1+i%3)+rng.randbytes(i*3))
    accepted=refused=0;end=str(int(time.time())+200)
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);raw=d/'raw';inv=d/'inverse';stop=d/'stop';payload=d/'postings'
        for i,text in enumerate(texts):
            n=len(text);target=max(64,n//12);ii,ex,sh=reference(text,target);m=len(ii);raw.write_bytes(text);inv.write_bytes(struct.pack(f'<{m}Q',*ii))
            for w in (1,2,4):
                page,wire=sh(w);args=[exe,raw,inv,payload,str(w),str(128<<20),str(8*m),str(target),stop,end];p=sp.run(args,capture_output=True)
                if wire+8*m>2*n:
                    assert p.returncode==75 and b'scratch' in p.stderr and not payload.exists(),(i,w,p.stderr);refused+=1;continue
                assert p.returncode==0,(i,w,p.stderr);f=fields(p.stdout.decode(),'FINITE_ROUTE');assert (int(f['wire']),int(f['pages']),int(f['bound_files']))==(wire,page,wire+8*m) and payload.stat().st_size==wire
                assert all(f[k]==str(v) for k,v in ex.items()),(i,w,ex,f);accepted+=1
                if i==7 and w==4:
                    before=hashlib.sha256(payload.read_bytes()).digest();assert sp.run(args,capture_output=True).returncode==75 and hashlib.sha256(payload.read_bytes()).digest()==before
                    payload.unlink();args[5]=str(int(f['arrays'])-1);assert sp.run(args,capture_output=True).returncode==75 and not payload.exists();args[5]=str(128<<20);args[6]=str(2*n-wire+1);assert sp.run(args,capture_output=True).returncode==75 and not payload.exists()
                else:payload.unlink()
        stop.touch();assert sp.run(args,capture_output=True).returncode==75 and not payload.exists()
    model=reports()['results'];assert model['100g_occurrence_upper']['with_private_inverse_and_margin']==166448027569
    assert accepted and refused;print(f'PASS {accepted} exact descriptor routes / {refused} predicted 2N refusals across 1/2/4 workers, byte-identical ordered proof vs brute raw suffix groups, width/extent/old-output/grant/STOP checks, archived bound')
if __name__=='__main__':run(Path(sys.argv[1]).resolve())
