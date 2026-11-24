"""No corpus. Byte SA/PLCP and independently packed EF per whole wave/epoch, refusal before END/file creation."""
import hashlib
import os
from pathlib import Path
import random
import selectors
import struct
import subprocess as sp
import sys
import tempfile
import time
from overlap_finite_direct64_test import probe
from overlap_finite_emit64_test import byte_reference,large_class
from overlap_raw_view64_test import fields
from overlap_resident_test import trigger

def periods(bands,slots,t,cap):
    if any(b[0]>slots for b in bands):return 'range exceeds slots'
    waves=[];at=0
    while at<len(bands):
        hi=at+1
        while hi<len(bands) and hi-at<t and sum(x[0] for x in bands[at:hi+1])<=slots:hi+=1
        waves.append((at,hi,sum(len(b[2])+len(b[3]) for b in bands[at:hi])));at=hi
    out=[]
    for lo,hi,size in waves:
        if size>cap:return 'wave exceeds scratch'
        if out and size<=cap-out[-1][2]:
            a,_,v,w=out.pop();out.append((a,hi,v+size,w+1))
        else:out.append((lo,hi,size,1))
    return 'epoch walk bound' if len(out)>8 else out

def run(exe,mode='epochs'):
    assert mode in ('epochs','staged-epochs')
    rng=random.Random(8481332);tag=next(i.to_bytes(8,'little') for i in range(999) if trigger(i.to_bytes(8,'little')));big=large_class(rng,tag);random_text=rng.randbytes(64000)
    cases=[(random_text,820,4300),(rng.randbytes(52200),8600,36500),(bytes(range(256))*109,300,1600),(big,1600,8192),(big,2200,15100),((tag+b'hello!')*2450+tag,800,2800),(b'\0'*401+tag+b'z'*899,202,1600)]
    ok=multiple=mixed=0;fail={};witness=False;stall=None;end=str(int(time.time())+260)
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);raw=d/'raw';inv=d/'inverse';slot=d/'file';stop=d/'stop'
        for text,tar,slots in cases:
            ii,rows=byte_reference(text);raw.write_bytes(text);inv.write_bytes(struct.pack(f'<{len(ii)}Q',*ii));n=len(text);available=2*n-inv.stat().st_size
            for t in (1,2,4):
                bs=probe(text,ii,t,tar,images=True)
                for cap in sorted({max(1,n//9),min(available,n//2),available}):
                    args=[exe,mode,raw,inv,slot,str(t),str(160<<20),str(2*n-cap),str(tar),str(slots),stop,end];ex=periods(bs,slots,t,cap);p=sp.run(args,capture_output=True)
                    if isinstance(ex,str):
                        assert p.returncode==75 and ex.encode() in p.stderr and not p.stdout and not slot.exists(),(len(text),t,cap,ex,p.stderr);fail[ex]=fail.get(ex,0)+1;continue
                    assert p.returncode==0 and p.stdout==rows,(len(text),t,cap,p.stderr);log=p.stderr.decode();f=fields(log,'EPOCH_INDEX');s=fields(log,'FINITE_EMIT');U=sum(b[0] for b in bs);assert int(f['epochs'])==len(ex) and int(f['encode_offset_visits'])==len(ex)*U and int(s['decoded'])==U and int(s['leaves'])==len(bs);maxsize=max(z[2] for z in ex);size=sum(z[2] for z in ex);assert int(f['peak_extent'])==maxsize<=cap and int(f['file_cap'])==cap and int(f['hash_readback'])==int(f['total_extent'])==int(s['wire'])==size and int(s['counted'])==2*n-cap+maxsize<=2*n
                    lines=[l for l in log.splitlines() if l.startswith('EPOCH_PART ')];assert len(lines)==len(ex)
                    for k,((a,b,z,waves),line) in enumerate(zip(ex,lines)):
                        main=b''.join(x[2] for x in bs[a:b]);tail=b''.join(x[3] for x in bs[a:b]);blob=main+tail;q=fields(line,'EPOCH_PART');assert tuple(int(q[s]) for s in ('i','lo','hi','bytes','interior','desc','rows','waves','populated'))==(k,a,b,len(blob),len(main),sum(x[0] for x in bs[a:b]),sum(x[1] for x in bs[a:b]),waves,sum(x[4] for x in bs[a:b])) and len(blob)==z and hashlib.sha256(blob).hexdigest()==q['sha256'];mixed+=bool(main and tail)
                    assert slot.read_bytes()==blob
                    if not witness:
                        before=slot.stat();r=sp.run(args,capture_output=True);after=slot.stat();assert r.returncode==75 and not r.stdout and b'fresh rolling slot' in r.stderr and before.st_ino==after.st_ino and before.st_mtime_ns==after.st_mtime_ns and slot.read_bytes()==blob;witness=True
                    if stall is None and len(ex)>1 and t==4 and text==random_text:stall=(ii,rows,args,text,1+sum(b[1] for b in bs[:ex[0][1]]))
                    slot.unlink();ok+=1;multiple+=len(ex)>1
        # Stop a blocked consumer only AFTER first-slot retirement/reuse. No partial stream may certify.
        assert stall;ii,rows,args,text,firstrows=stall;raw.write_bytes(text);inv.write_bytes(struct.pack(f'<{len(ii)}Q',*ii));p=sp.Popen(args,stdout=sp.PIPE,stderr=sp.PIPE,bufsize=0);want=12*(firstrows+1);got=bytearray()
        try:
            with selectors.DefaultSelector() as rd:
                rd.register(p.stdout,selectors.EVENT_READ)
                while len(got)<want:
                    assert rd.select(5);x=os.read(p.stdout.fileno(),min(16384,want-len(got)));assert x;got.extend(x)
            assert got==rows[:want] and slot.exists();stop.touch();assert p.wait(timeout=3)==75;msg=p.stderr.read();assert b'EPOCH_PART i=0' in msg and b'deadline/stop' in msg and b'FINITE_EMIT n=' not in msg
        finally:
            if p.poll() is None:p.kill();p.wait()
            p.stdout.close();p.stderr.close()
    assert ok and multiple and mixed and witness and set(fail)=={'epoch walk bound','range exceeds slots','wave exceeds scratch'};print(f'PASS {ok} independent complete byte streams/epoch images ({multiple} multi-epoch, {mixed} mixed interior/tail); predicted pre-open refusals {fail}; no overwrite, one END, exact seams and stalled STOP after reuse')
if __name__=='__main__':run(Path(sys.argv[1]).resolve())
