"""One fixed read-only study on the archived 100 GB *index*, not the text.
CRC/hash only m20, check m15 head, inspect shallow branches with the qualified
frozen reader. Counts are evidence about cuts, never input to a builder.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shutil
import stat
import struct
import subprocess
import sys
import time
from types import SimpleNamespace
import zlib
from overlap_m20_prefix_cuts import parse_dump,proof
from overlap_prefix_vocab_bounds import inheritance,need
from overlap_real_v10_meter import collect,files
from overlap_target_prefix_certificate import read_meta,reduce,sha,stamp,M,N,SLOTS
from overlap_v10_product_compare import optional_io_stat
from overlap_v10_target_credits import G,S,PROFILES

R=G/'receipts';CERT=R/'target-prefix-vocab';BANK=R/'m20-cut-tools';TOOLS=Path('/tmp/m20-cut-tools-zBIsDt');EXE=TOOLS/'probe'
CERT_SHA='d752071aaed85b4f386ebd3e4da51301dcbd9bc05357c784a989282f5088a82b';EXE_SHA='fe03f3f83cb5b9fafe30c7c5a3ab62712b333c5396701cca281e472e9e30d363'
STAT=(66308,32215060707,65533578480,1794478199606586523,1794478199606586523);DOCS=16119767;OFF=63163489376;EXTENT=2333923624;CRC=475685233

def geometry():
    raw=(CERT/'result.json').read_bytes();need(sha(raw)==CERT_SHA and (CERT/'exits.log').read_text()=='audit=0\nlauncher=0\n','prefix certificate pin/exits');z=json.loads(raw);a=z['auth'];need(a==json.loads((CERT/'authentication.json').read_bytes()),'authentication record changed')
    meta=read_meta();old,new=[dict(a[k]['cells']) for k in ('prefix_eof','target_eof')];need(json.loads(json.dumps(reduce(meta,old,new)))==z['result'],'preceding two-code proof failed replay');dims,op,dp,occ,_,_=meta
    _,lo,hi=inheritance(*dims,N,M,op,dp,occ,old,new);hard=sorted(k for k,h in hi.items() if h>SLOTS);need(len(hard)==115 and len(occ)==36590 and sum(occ.values())==N,'target cut bases');return occ,lo,hi,hard

def worker_pin():
    need(EXE==EXE.resolve() and EXE.stat().st_size==4519016 and sha(EXE.read_bytes())==EXE_SHA,'unqualified observer')
    for k in ('exits.log','sources.sha','tool.sha','frozen.log','check.log','compiler.sha'):
        need((BANK/k).read_bytes()==(TOOLS/k).read_bytes(),'fixture receipt: '+k)
    need((BANK/'exits.log').read_text()=='compile=0\nsetup=0\nobserve=0\ncheck=0\nlauncher=0\n' and not (TOOLS/'stop.requested').exists() and not (TOOLS/'fixture/stop.requested').exists(),'missing/failed qualification')
    for k in ('generated.json','dump'):need((BANK/k).read_bytes()==(TOOLS/'fixture'/k).read_bytes(),'generated witness: '+k)
    for line in (BANK/'sources.sha').read_text().splitlines():
        h,name=line.split();need(sha((TOOLS/'src'/name).read_bytes())==h and (name=='./frozen.rs' or sha((G/name).read_bytes())==h),'observer/score source mismatch')

def sig(s):return (s.st_dev,s.st_ino,s.st_size,s.st_mtime_ns,s.st_ctime_ns)

def authenticate(f,guard):
    head=f.read(64);f.seek(-400,2);direc=f.read(400);need(sha(head+direc)==PROFILES[1][5] and zlib.crc32(head[:56]+bytes(4)+head[60:]+direc)==struct.unpack_from('<I',head,56)[0],'target head/directory')
    tab={k:(c,o,b,n,h) for k,c,o,b,n,h in struct.iter_unpack('<IIQQQQ',direc)};need(tab[20]==(20,OFF,EXTENT,N,CRC) and tab[15][0]==15 and tab[15][2]==36165080,'target member shape');f.seek(tab[15][1]);need(struct.unpack('<8sIIQQ',f.read(32))==(b'XDOCST01',1,64,DOCS,N),'m15 document count')
    md=hashlib.sha256();crc=0;f.seek(OFF);left=EXTENT
    while left:
        guard();b=f.read(min(1<<20,left));need(b,'short m20');md.update(b);crc=zlib.crc32(b,crc)
        if left==EXTENT:
            h=struct.unpack_from('<24Q',b);need(h[:8]==(3,8,1000,N+1,N,21707038,551286102,1000) and h[18:]==(21707037,3,0,1,0,0) and h[8]==192 and all(x<y<=EXTENT for x,y in zip(h[8:18],h[9:18]+(EXTENT,))),'not the pinned clipped format')
        left-=len(b)
    need(crc==CRC and f.tell()==OFF+EXTENT,'m20 CRC/extent refusal');return md.hexdigest()

def observe(f,d,end,guard,sample):
    q=None;status=None
    def inspect(_,p):
        nonlocal status
        if p.returncode is None:
            got=collect(p)
            if got is not None:status=got
    with (d/'observations').open('xb',buffering=0) as out,(d/'observer.err').open('xb',buffering=0) as err:
        os.posix_fallocate(out.fileno(),0,8<<20);os.posix_fallocate(err.fileno(),0,64<<10);sample({},inspect)
        q=subprocess.Popen([EXE,'archive',str(end),str(f.fileno())],cwd=d,stdin=subprocess.DEVNULL,stdout=out,stderr=err,pass_fds=(f.fileno(),),preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_FSIZE,(8<<20,8<<20)))
        try:
            while q.returncode is None:
                inspect('probe',q)
                if q.returncode is not None:break
                guard();sample({'probe':q},inspect)
                try:
                    io=dict(s.split(': ') for s in Path(f'/proc/{q.pid}/io').read_text().splitlines());need(int(io['read_bytes'])<=1<<30 and int(io['write_bytes'])<=16<<20,'observer IO cap')
                except (FileNotFoundError,ProcessLookupError):inspect('probe',q)
                time.sleep(.12)
        except BaseException:
            (d/'stop.requested').touch();until=time.monotonic()+2
            while q.returncode is None and time.monotonic()<until:inspect('probe',q);time.sleep(.02)
            if q.returncode is None:
                try:os.kill(q.pid,9)
                except ProcessLookupError:pass
                status=collect(q,True)
            raise
        finally:
            out.truncate(out.tell());err.truncate(err.tell())
            for z in (out,err):os.fsync(z.fileno())
            with (d/'exits.log').open('a') as log:log.write(f'observer={q.returncode}\n')
            if status is not None:stamp(d/'observer.json',status)
    need(status and status['rc']==0 and status['ru_inblock_bytes']<=1<<30 and status['ru_oublock_bytes']<=16<<20 and not (d/'observer.err').stat().st_size,'observer refused/IO or diagnostics');return status

def main(d,end):
    need(d.resolve()==d and d.parent==S and re.fullmatch(r'm20-target-cuts-[A-Za-z0-9]{6}',d.name) and stat.S_IMODE(d.stat().st_mode)==0o700 and d.stat().st_uid==os.getuid() and time.time()<end<=time.time()+150 and shutil.disk_usage(d).free>128<<20,'private fixed m20 audit')
    cg=Path('/sys/fs/cgroup'+Path('/proc/self/cgroup').read_text().strip().split('::')[1]);get=lambda k:(cg/k).read_text().strip();need((get('cpu.max'),get('memory.max'),get('memory.swap.max'))==('100000 100000',str(256<<20),'0') and len(os.sched_getaffinity(0))==1,'archive grant');own={'self':SimpleNamespace(pid=os.getpid(),returncode=None,poll=lambda:None)};high=[0,0]
    def sample(jobs,inspect):
        a,u,_=files(d,own|jobs,inspect);x=a['scratch'];need(not u and max(x)<=20<<20,'archive controls/unlink');high[:]=[max(x[i],high[i]) for i in range(2)]
    def guard():
        ru=resource.getrusage(resource.RUSAGE_SELF);need(time.time()<end and not (d/'stop.requested').exists() and ru.ru_inblock*512<=EXTENT+(96<<20) and ru.ru_oublock*512<=20<<20,'audit STOP/deadline/parent IO')
    t=time.monotonic();io0=optional_io_stat(cg);worker_pin();occ,lo,hi,hard=geometry();guard();sample({},lambda *_:None)
    with (d/'requests').open('x') as out:out.write(''.join(f'{k}\n' for k in hard));out.flush();os.fsync(out.fileno())
    path=PROFILES[1][1];need(path.resolve()==path and sig(path.stat())==STAT,'target archive identity')
    with os.fdopen(os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC),'rb',buffering=0) as f:
        need(stat.S_ISREG(os.fstat(f.fileno()).st_mode) and sig(os.fstat(f.fileno()))==STAT,'substituted target');msha=authenticate(f,guard);ready=time.monotonic();guard();s=observe(f,d,end,guard,sample);observed=time.monotonic();need(sig(os.fstat(f.fileno()))==sig(path.stat())==STAT,'target changed during observation')
    data=(d/'observations').read_bytes();head,nodes=parse_dump(data);need((head['corpus'],head['nodes'],head['edges'],head['support'])==(N,21707038,551286102,1000),'observer profile');z=proof(occ,lo,hi,DOCS,SLOTS,head,nodes);guard();sample({},lambda *_:None);ru=resource.getrusage(resource.RUSAGE_SELF);events=get('memory.events');need({'oom 0','oom_kill 0'}<=set(events.splitlines()),'archive OOM')
    result=dict(proof=z,archive_stat=STAT,member_offset=OFF,member_bytes=EXTENT,member_crc32=CRC,member_sha256=msha,parent_index_requested_bytes=64+400+32+EXTENT,observation_bytes=len(data),observation_sha256=sha(data),oracle_sha256=EXE_SHA,prior_certificate_sha256=CERT_SHA,
                utc=time.strftime('%FT%TZ',time.gmtime()),setup_auth_s=ready-t,observer_subprocess_s=observed-ready,additional_review_s=time.monotonic()-observed,peak_shared=int(get('memory.peak')),cap_shared=256<<20,swap=0,cpu_max=get('cpu.max'),cores=sorted(os.sched_getaffinity(0)),memory_events=events,parent_attributed_R_W_before_result=[ru.ru_inblock*512,ru.ru_oublock*512],observer=s,io_stat_before=io0,io_stat_after=optional_io_stat(cg),sampled_aux_logical_allocated=high,
                raw_text_bytes=0,position_or_dictionary_scratch_bytes=0,scope='Static prospective through-nine-code range bounds from an archived index + certified raw cell totals. No new row stream, full file rehash, measured emitter cuts, posting layout, physical-fit or performance admission.')
    stamp(d/'result.json',result);print(json.dumps({k:z[k] for k in ('all_prefix_leaves_fit','base_uncertified_parents','parents_now_certified','examined_nodes','leaf_count','raw_rows_in_certified_leaves','residual_raw_rows','residual','complete_range_count_if_certified')},sort_keys=True))
if __name__=='__main__':
    need(__debug__ and len(sys.argv)==3,'private archive-only audit');main(Path(sys.argv[1]),int(sys.argv[2]))
