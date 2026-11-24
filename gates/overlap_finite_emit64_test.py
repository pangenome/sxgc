"""Independent complete byte-SA / byte-Kasai oracle on generated inputs. No corpus or file of output rows."""
import os
from pathlib import Path
import random
import selectors
import struct
import subprocess as sp
import sys
import tempfile
import time
from overlap_resident_test import trigger
from overlap_raw_view64_test import fields
from overlap_finite_route64_test import reference

def byte_reference(t):
    n=len(t);a=[x+1 for x in t]+[0];sa=list(range(n+1));step=1
    while True:
        def key(i):return a[i],a[i+step] if i+step<=n else -1
        sa.sort(key=key);b=[0]*(n+1)
        for i in range(1,n+1):b[sa[i]]=b[sa[i-1]]+int(key(sa[i-1])!=key(sa[i]))
        a=b
        if b[sa[-1]]==n:break
        step*=2
    lcp=[0]*(n+1);h=0
    for p in range(n):
        r=a[p];q=sa[r-1]
        while max(p,q)+h<n and t[p+h]==t[q+h]:h+=1
        lcp[r]=h;h=max(0,h-1)
    data=bytearray(12*(n+1))
    for i,p in enumerate(sa):struct.pack_into('<III',data,12*i,p,lcp[i],0x80000000+(t[p-1] if p else 256))
    starts=[0]+[i for i in range(1,n-7) if trigger(t[i:i+8])];order=sorted(range(len(starts)),key=lambda j:a[starts[j]]);inv=[0]*len(order)
    for i,j in enumerate(order,1):inv[j]=i
    return inv,data

def large_class(rng,tag):
    pieces=set()
    for _ in range(200000):
        s=tag+rng.randbytes(5)+b'!';w=s+tag
        if all(not trigger(w[i:i+8]) for i in range(1,len(s))):pieces.add(s)
        if len(pieces)==8311:break
    assert len(pieces)==8311;return b''.join(sorted(pieces))+tag

def run(exe):
    rng=random.Random(5544831);tag=next(i.to_bytes(8,'little') for i in range(999) if trigger(i.to_bytes(8,'little')))
    cases=[bytes(range(256))*7,tag+b'x'*99+tag+b'xyz',b'!'+tag+b't'*1800,(b'x'+tag+b'!'+tag)*130+tag[:4],(b'\0'+tag+b'\xff')*187+tag,b'z'*800+tag+b'z'*142,b'@'*190]
    for i in range(12):cases.append(bytes(rng.choices(rng.sample(range(256),7+i*13),k=250+i*91))*(1+i%3)+rng.randbytes(i))
    big=large_class(rng,tag);cases.append(big);verified=refused=huge=bitmap=0;teams={1:0,2:0,4:0};end=str(int(time.time())+180)
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);raw=d/'raw';inv=d/'inverse';idx=d/'index';stop=d/'stop'
        for t in cases:
            ii,rows=byte_reference(t);n=len(t);target=max(64,n//16);m=len(ii);raw.write_bytes(t);inv.write_bytes(struct.pack(f'<{m}Q',*ii));slots=n if t!=big else 20000
            for w in teams:
                args=[exe,raw,inv,idx,str(w),str(128<<20),str(8*m),str(target),str(slots),stop,end];p=sp.run(args,capture_output=True)
                if p.returncode==75 and (b'scratch' in p.stderr or b'input geometry' in p.stderr or b'geometry/grant' in p.stderr):
                    assert n<10000 and 8*m+reference(t,target)[2](w)[1]>2*n and not p.stdout and not idx.exists();refused+=1;continue
                assert p.returncode==0,(n,m,w,p.stderr);f=fields(p.stderr.decode(),'FINITE_EMIT');assert p.stdout==rows and int(f['rows'])==n+1 and int(f['counted'])<=2*n,(n,w,f);verified+=1;teams[w]+=1;bitmap+=int(f['bitmap_groups']);idx.unlink()
                if t==big:
                    assert int(f['max_tie'])>=8311 and int(f['bitmap_groups'])>0 and int(f['bands'])>4 and int(f['local_scans'])>0;huge+=1
        assert all(teams.values()) and huge==3 and bitmap and refused
        args[8]='8192';p=sp.run(args,capture_output=True);assert p.returncode==0 and p.stdout==rows and int(fields(p.stderr.decode(),'FINITE_EMIT')['exact_bypass'])>0,p.stderr;idx.unlink()
        args[8]='1';p=sp.run(args,capture_output=True);assert p.returncode==75 and b'local replay work bound' in p.stderr and not p.stdout,p.stderr;idx.unlink();args[8]=str(slots)
        # Never turn stdout into a spool. Never wait indefinitely for a reader of the active band.
        with (d/'empty').open('xb') as f:
            p=sp.run(args,stdout=f,stderr=sp.PIPE);assert p.returncode==75 and b'pipe' in p.stderr and not idx.exists()
        p=sp.Popen(args,stdout=sp.PIPE,stderr=sp.PIPE,bufsize=0)
        try:
            with selectors.DefaultSelector() as ready:
                ready.register(p.stdout,selectors.EVENT_READ);assert ready.select(15) and len(os.read(p.stdout.fileno(),12))==12
            stop.touch();assert p.wait(timeout=3)==75 and b'stop' in p.stderr.read()
        finally:
            if p.poll() is None:p.kill();p.wait()
            p.stdout.close();p.stderr.close()
    print(f'PASS {verified} complete independent byte-SA row43 streams; 1/2/4 ordered lanes, {refused} predicted scratch refusals, {huge} actual 8311-word ties beyond old cursor limit, synthetic wire/seam/backpressure/STOP and no-row-file checks')
if __name__=='__main__':run(Path(sys.argv[1]).resolve())
