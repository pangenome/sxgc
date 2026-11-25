"""Read only a pinned v10 header/directory and m12. No raw text, other payload, builder or row pipe.
Compute constraints on a *proposed* seam-free fixed-run-piece/fixed-row-slice schedule, not on the
historic producer's band partition. Retain O(run pieces) integer samples, never all 1024-run starts.
"""
from array import array
from bisect import bisect_left
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import stat
import struct
import sys
import time
import zlib

G=Path(__file__).resolve().parent
BASE=Path('/mnt/nvme2n1/erikg')
S=BASE/'TAPEREADER/sandbox'
PROFILES=[('real100m',S/'real-v10-block100-y2Zifw/reference/index.sxi2',87192432,100000504,38647521,'d0a5f6b02a13774335b009abb7a7a892569d58a62bce49b4952c80153f76025c',306720,'9caf232e24a36ea74acffae93e6a2f728e492716eb284f8cb5237dda55e84397'),
          ('target100g',BASE/'PILE100-REHEARSAL/pile-head100.sxi2',65533578480,100000000001,31381597694,'f6f15d33c46370f12d4e910a30d549685d05137c0d26104a68934e6ded559804',248999568,None)]
BLOCK,PIECE,SLICE=1024,65536,262144

def need(ok,msg):
    if not ok:raise ValueError(msg)
def ceil(n,d):return (n+d-1)//d

def words(buf):
    a=array('Q');need(a.itemsize==8,'requires uint64');a.frombytes(buf)
    if sys.byteorder!='little':a.byteswap()
    return a

def bounds(rows,runs,piece,block,ends,chunks,guard):
    """y[r] = exclusive rows before run r. Bracket y[max(0,r-2)] for every
    piece cut INCLUDING the final short one. Each unsampled run is >=1 row.
    """
    need(0<runs<=rows and piece>=block>0 and piece%block==0,'sample geometry');k=ceil(runs,piece);h=ceil(runs,block);stride=piece//block
    need(len(ends)==k+1 and ends[0]==0 and ends[k]==rows,'super samples/EOF');lo=array('Q');hi=array('Q');base=i=0;prev=None
    for a in chunks:
        guard();n=len(a);need(n>0 and base+n<=h and (prev is None or a[0]>=prev+block) and all(b>=x+block for x,b in zip(a,a[1:])),'bad block progression')
        start=(-base)%stride;first=(base+start)//stride
        sub=a[start::stride];need(sub==ends[first:first+len(sub)],'super/block sample mismatch')
        while i<=k:
            r=min(i*piece,runs);t=max(0,r-2);j,rem=divmod(t,block)
            if j>=base+n:break
            need(j>=base,'lost delayed sample');v=a[j-base];l=v+rem;u=(ends[i]-(r-t)) if rem else v
            need(l<=u<=ends[i],'inconsistent delayed run frontier');lo.append(l);hi.append(u);i+=1
        prev=a[-1];base+=n
    need(base==h and i==k+1 and prev+runs-(h-1)*block<=rows,'tail sample/coverage');return lo,hi

def geometry(rows,ends,lo,hi,slice_rows,guard):
    k=len(ends)-1;need(k and len(lo)==len(hi)==k+1 and slice_rows>0,'schedule input')
    best={};at={};worst_fold={};fold_peak=0
    def stage(i,a,b):
        nonlocal worst_fold,fold_peak
        base=max(0,a-1);need(0<=base<=b<=rows,'bad fold envelope')
        if base==b:return
        first,last=base//slice_rows,(b-1)//slice_rows
        src=min(rows,(last+1)*slice_rows)-first*slice_rows;span=b-base;h=span+17*src # one marker + up to one 16 B escape per source row
        if h>fold_peak:fold_peak=h;worst_fold=dict(piece_or_eof=i,staged_rows_upper=span,source_rows_upper=src,intersected_slices_upper=last-first+1)
    for i in range(k):
        if not i%4096:guard()
        a,b=ends[i],ends[i+1];need(a<b and lo[i]<=hi[i]<=a,'piece/frontier order')
        # Internal lookahead cuts PRC1 before pushing that row's LCP. At EOF the
        # final SXLS must be published BEFORE PRC1/BPT1. Credits are not unlinks.
        made=b//slice_rows if i+1<k else ceil(rows,slice_rows)
        through=min(rows,ceil(b,slice_rows)*slice_rows);pn=bisect_left(ends,through,1,k)-1
        vals=dict(slice_sufficient=max(1,made-ceil(lo[i],slice_rows)),slice_necessary=max(1,made-ceil(hi[i],slice_rows)),piece_sufficient=max(1,pn-(i+1)),largest_piece_rows=b-a)
        for key,v in vals.items():
            if v>best.get(key,-1):best[key]=v;at[key]=dict(piece=i,first_row=a,past_row=b,lag_lower=lo[i],lag_upper=hi[i],slices_before_piece=made)
        stage(i,lo[i],hi[i+1])
    stage(k,lo[k],rows);need(best['slice_sufficient']>=best['slice_necessary'],'credit inequality')
    s,p=best['slice_sufficient'],best['piece_sufficient'];status='insufficient' if best['slice_necessary']>5 else 'sufficient_for_credit_schedule' if s<=5 and p<=6 else 'unproven'
    return dict(**best,witness=at,six_piece_five_slice=status,fold_worst_case_bytes_upper=fold_peak,fold_upper_witness=worst_fold)

def extract(prof,guard):
    tag,path,size,rows,runs,head_pin,member_bytes,member_pin=prof;need(path==path.resolve(),'not the banked path');key=lambda s:(s.st_dev,s.st_ino,s.st_size,s.st_ctime_ns)
    with os.fdopen(os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC),'rb',buffering=0) as f:
        st=os.fstat(f.fileno());need(stat.S_ISREG(st.st_mode) and st.st_size==size,'container extent');head=f.read(64);f.seek(-400,2);directory=f.read(400)
        need(hashlib.sha256(head+directory).hexdigest()==head_pin,'header/directory pin');magic,version,n,sets,r,count,_,length,_,crc=struct.unpack('<4sIQQQIIQQQ',head)
        need((magic,version,n,sets,r,count,length)==(b'SXI2',10,rows,1,runs,10,size) and zlib.crc32(head[:56]+bytes(4)+head[60:]+directory)==crc,'header CRC/shape')
        table=list(struct.iter_unpack('<IIQQQQ',directory));need([x[0] for x in table]==[1,4,5,7,10,11,12,13,15,20],'member IDs');_,codec,off,extent,h,want=table[6];k=ceil(runs,PIECE)
        need(codec==112 and extent==member_bytes==64+8*(k+h) and h==ceil(runs,BLOCK) and 64<=off<=size-400-extent,'m12 range');f.seek(off);left=extent;md=hashlib.sha256();crc=0
        def take(b):
            nonlocal crc,left
            guard();need(0<b<=left,'read outside m12');buf=f.read(b);need(len(buf)==b,'short m12');left-=b;md.update(buf);crc=zlib.crc32(buf,crc);return buf
        need(struct.unpack('<8Q',take(64))==(BLOCK,h,runs,rows,PIECE,k,64,64+8*k),'m12 dimensions');ends=words(take(8*k));ends.append(rows)
        def chunks():
            while left:yield words(take(min(left,1<<20)))
        lo,hi=bounds(rows,runs,PIECE,BLOCK,ends,chunks(),guard);need(not left and f.tell()==off+extent and crc==want and (member_pin is None or md.hexdigest()==member_pin),'m12 content integrity');g=geometry(rows,ends,lo,hi,SLICE,guard);need(key(st)==key(os.fstat(f.fileno()))==key(path.stat()),'bank changed')
    return dict(path=str(path),head_dir_sha256=head_pin,member_offset=off,member_bytes=extent,member_crc32=crc,member_sha256=md.hexdigest(),total_input_requested_bytes=464+extent,rows=rows,runs=runs,slice_rows=SLICE,piece_runs=PIECE,block_runs=BLOCK,pieces=k,slices=ceil(rows,SLICE),integer_array_bytes=3*8*(k+1),credit_schedule=g)

def main(d,end):
    need(d.parent==S and re.fullmatch(r'v10-target-credits-[A-Za-z0-9]{6}',d.name) and d==d.resolve() and d.stat().st_uid==os.getuid() and stat.S_IMODE(d.stat().st_mode)==0o700 and int(time.time())<end<=int(time.time())+120,'private metadata-only admission');cg=Path('/sys/fs/cgroup'+Path('/proc/self/cgroup').read_text().strip().split('::')[1]);get=lambda name:(cg/name).read_text().strip()
    need((get('memory.max'),get('memory.swap.max'),get('cpu.max'))==(str(128<<20),'0','100000 100000') and len(os.sched_getaffinity(0))==1,'unmatched review grant');t=time.monotonic()
    def guard():need(time.time()<end and not (d/'stop.requested').exists(),'metadata STOP/deadline')
    arms={p[0]:extract(p,guard) for p in PROFILES};old=arms['real100m']['credit_schedule'];need(old['slice_necessary']==old['slice_sufficient']==4 and old['piece_sufficient']==3 and old['largest_piece_rows']==1126176 and old['fold_worst_case_bytes_upper']>=3958321,'100 MB cross-check vs archived witness');guard();events=get('memory.events');need({'oom 0','oom_kill 0'}<=set(events.splitlines()),'metadata OOM')
    ru=resource.getrusage(resource.RUSAGE_SELF);need(ru.ru_inblock*512<=280<<20 and ru.ru_oublock*512<=1<<20,'metadata IO limit')
    report=dict(arms=arms,utc=time.strftime('%FT%TZ',time.gmtime()),service_body_s=time.monotonic()-t,attributed_read_bytes=ru.ru_inblock*512,attributed_write_bytes_before_receipt=ru.ru_oublock*512,memory_peak=int(get('memory.peak')),memory_events=events,core=sorted(os.sched_getaffinity(0)),scope='Read-only metadata reduction of two archived indexes for a proposed seam-free maximal-run producer. No corpus, row scan, full target hash, measured allocator/IO savings or product admission.')
    with (d/'result.json').open('x') as f:json.dump(report,f,sort_keys=True,indent=2);f.write('\n');f.flush();os.fsync(f.fileno())
    print(json.dumps({k:v['credit_schedule'] for k,v in arms.items()},sort_keys=True))
if __name__=='__main__':
    need(__debug__ and len(sys.argv)==3,'private directory and short deadline required');main(Path(sys.argv[1]),int(sys.argv[2]))
