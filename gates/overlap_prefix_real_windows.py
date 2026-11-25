"""One fixed real 100 MB partial-rank study; no corpus/scale/worker arguments."""
from bisect import bisect_left
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import stat
import struct
import subprocess
import sys
import time
from types import SimpleNamespace
import zlib
from overlap_real_v10_meter import files
from overlap_v10_product_compare import optional_io_stat
from private_cold import cold
from overlap_v10_target_credits import G, S, PROFILES, need

SRC = S/'real-v10-map100-S4bZm0'
BANK = G/'receipts'
RAW_SHA = '3c965f581b46ba72abfd7fee7a0ce2efac5a61cd4dc5725e9253dcebcccc2383'
SRC_SHA = '038ea10a124fc46821ce37f8395f2453a017ca9db6c04186c36728c7e2ce6f1b'
VER_SHA = '8cc282659489ebbe3511b79189c8ea259548db37d36600832d6c840dbc8bb0d0'
N, W, K, CAP = 100000503, 65536, 37742, 4194304

def digest(b): return hashlib.sha256(b).hexdigest()
def sig(s): return (s.st_dev,s.st_ino,s.st_size,s.st_mtime_ns,s.st_ctime_ns,s.st_nlink)

@contextmanager
def original(path,size):
    need(path==path.resolve(),'original path changed')
    with os.fdopen(os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC),'rb') as f:
        before=os.fstat(f.fileno());need(stat.S_ISREG(before.st_mode) and before.st_size==size,'original type/extent')
        yield f
        need(sig(before)==sig(os.fstat(f.fileno()))==sig(path.stat()),'original identity changed')

def member(i):
    _,path,size,rows,runs,pin,_,_=PROFILES[0]
    record=(BANK/'real-v10-product-100m/reference/verified.json').read_bytes()
    need(digest(record)==VER_SHA and rows==N+1 and runs==38647521,'small real proof pin')
    v=json.loads(record)['table'][str(i)]
    with original(path,size) as f:
        head=f.read(64);f.seek(-400,2);direc=f.read(400);need(digest(head+direc)==pin,'SXI2 directory pin')
        tab={k:(c,o,b,n,h) for k,c,o,b,n,h in struct.iter_unpack('<IIQQQQ',direc)}
        c,o,b,n,h=tab[i];need((c,o,b,n,h)==tuple(v[k] for k in ('codec','offset','bytes','count','crc')) and n==K and b<1<<20,'member dimensions')
        f.seek(o);data=f.read(b);need(len(data)==b and digest(data)==v['sha256'] and zlib.crc32(data)==h,'member digest')
    if i==11:
        need(struct.unpack_from('<2Q',data)==(10,K),'m11 layout');words=struct.unpack_from(f'<{K}Q',data,16);need(all(x<=N for x in words),'m11 position range')
    else:
        need(i==12 and struct.unpack_from('<8Q',data)==(1024,K,runs,rows,65536,590,64,64+590*8),'m12 layout')
        words=struct.unpack_from(f'<{K}Q',data,64+590*8);need(words[0]==0 and words[-1]<rows and all(y-x>=1024 for x,y in zip(words,words[1:])),'m12 rank progression')
    return words

def tool():
    bank=BANK/'prefix-window-tools';h,path=(bank/'tool.sha').read_text().split();p=Path(path);d=p.parent
    need(d.parent==Path('/tmp') and re.fullmatch('prefix-window-tools-[a-zA-Z0-9]{6}',d.name) and p.name=='probe' and p.resolve()==p and p.stat().st_size<8<<20 and digest(p.read_bytes())==h,'not qualified window worker')
    for file in ['exits.log','fixture.log','sources.sha','tool.sha','compiler.sha']:
        need((d/file).read_bytes()==(bank/file).read_bytes(),'fixture receipt '+file)
    need((d/'exits.log').read_text()=='compile=0\nfixture=0\nlauncher=0\n' and not (d/'stop.requested').exists(),'not a passed generated fixture')
    for line in (d/'sources.sha').read_text().splitlines():
        sha,name=line.split();need(Path(name).name in ('overlap_prefix_real_windows.cpp','overlap_prefix_window_select.hpp','overlap_raw_prefix_intervals.hpp') and digest((G/name).read_bytes())==sha,'published worker sources differ')
    return p

def inputs(d,guard):
    receipt=(SRC/'source.json').read_bytes();need(digest(receipt)==SRC_SHA and json.loads(receipt)['raw_sha256']==RAW_SHA and (SRC/'exits.log').read_text()=='source=0\nlauncher=0\n','real input source receipt')
    path=d/'text';md=hashlib.sha256()
    with original(SRC/'inputs/checker',N) as f,path.open('xb') as dst:
        os.posix_fallocate(dst.fileno(),0,N);need(os.fstat(dst.fileno()).st_blocks*512<=N+(1<<20),'private input allocation')
        left=N
        while left:
            guard();b=f.read(min(left,1<<20));need(b,'short raw source');left-=len(b);md.update(b);dst.write(b)
        need(md.hexdigest()==RAW_SHA and not f.read(1),'source raw hash');dst.flush();os.fchmod(dst.fileno(),0o400);os.fsync(dst.fileno())
    with original(path,N) as f:need(hashlib.file_digest(f,'sha256').hexdigest()==RAW_SHA and path.stat().st_nlink==1 and not os.path.samefile(path,SRC/'inputs/checker'),'fresh copy verification')
    starts=member(12)
    with (d/'starts.u64').open('xb') as f:
        os.posix_fallocate(f.fileno(),0,K*8);f.write(struct.pack(f'<{K}Q',*starts));f.flush();os.fchmod(f.fileno(),0o400);os.fsync(f.fileno())
    fd=os.open(d,os.O_DIRECTORY|os.O_RDONLY);os.fsync(fd);os.close(fd);cold(path);guard();return starts

def rank(i):
    mask=(1<<64)-1;x=0xF6B8D2A4+i;x=(x^(x>>30))*0xbf58476d1ce4e5b9&mask;x=(x^(x>>27))*0x94d049bb133111eb&mask;x^=x>>31
    lo=(N+1)*i//16;hi=(N+1)*(i+1)//16;return lo+1+x%(hi-lo-W)

def fields(line):
    name,*data=line.split();pairs=[part.split('=') for part in data];v={k:int(x) for k,x in pairs};need(len(pairs)==len(v) and all(x>=0 for x in v.values()),'duplicate/negative observation');return name,v

def score(log,starts,m11):
    covers=[];phases=[];anchors=[];summary=None;usage=None
    for line in log.splitlines():
        tag,v=fields(line)
        if tag=='COVER':covers.append(v)
        elif tag=='PHASE':phases.append(v)
        elif tag=='ANCHOR':anchors.append(v)
        elif tag=='WINDOWS_PASS':need(summary is None,'duplicate EOF');summary=v
        elif tag=='RESOURCE':need(usage is None,'duplicate usage');usage=v
        else:raise ValueError('unknown worker observation')
    need(len(covers)==16 and len(phases)==32 and summary and usage and usage['peak']<=256<<20 and usage['read']<=128<<20,'incomplete bounded worker')
    matched=truncated=0;idx=0
    for i,c in enumerate(covers):
        first=rank(i);past=first+W
        need((c['window'],c['first'],c['count'])==(i,first,W) and c['base']<=first-1 and past<=c['base']+c['extent']<=N+1 and c['key_lo']<=c['key_hi']<257**3 and (not i or covers[i-1]['key_hi']<c['key_lo']),'rank cover or extent')
        for k in range(bisect_left(starts,first),bisect_left(starts,past)):
            need(idx<len(anchors),'missing observed block start');a=anchors[idx];idx+=1
            need((a['window'],a['block'],a['rank'])==(i,k,starts[k]) and starts[k]<=a['tail']<past and a['pos']<=N and a['complete']==int(a['tail']+1<past),'wrong block observation')
            if a['complete']:
                need(a['pos']==m11[k],f'm11 tail position mismatch block {k}');matched+=1
            else:
                need(k+1==len(starts) or starts[k+1]>=past,'truncated interior run');truncated+=1
        for half in (0,1):
            v=phases[i*2+half];b=v['prefix'];eligible=v['eligible_bytes'];e=v['edges']
            need((v['window'],v['half'],e)==(i,half,W//2) and v['ge4096']<=v['ge1024']<=v['escapes']<=e and v['max_h']<=34067 and 1024*v['ge1024']<=eligible<=b<=e*v['max_h'] and v['start_pages']<=(N+4095)//4096 and v['far_pairs']<=e,'bad phase statistics')
            for size in ('small','large'):
                need(v[size+'_compared']+v[size+'_skipped']==b and v[size+'_skipped']<=eligible and v[size+'_lookups']==v['ge1024'] and v[size+'_hits']+v[size+'_evicted']<=v[size+'_lookups'],'proof counts')
    need(idx==len(anchors)==summary['reported_anchor_starts'] and matched>=64 and summary['sampled_edges']==16*W and sum(c['extent'] for c in covers)==summary['sorted_cover_positions']<=CAP and summary['large_table']==397312 and summary['small_table']==24832 and summary['sort_memcmp_request']<=32<<30 and summary['lcp_memcmp_request']<=2<<30,'aggregate window coverage')
    def aggregate(p):
        names='edges prefix escapes ge1024 ge4096 eligible_bytes far_pairs small_compared small_skipped small_evicted large_compared large_skipped large_evicted'.split();z={k:sum(v[k] for v in p) for k in names};z['peak_lcp']=max(v['max_h'] for v in p);z['start_pages_range']=[min(v['start_pages'] for v in p),max(v['start_pages'] for v in p)]
        z['large_reuse_fraction']=z['large_skipped']/max(1,z['prefix']);z['small_reuse_fraction']=z['small_skipped']/max(1,z['prefix']);return z
    warm=phases[1::2];sumwarm=aggregate(warm);votes=sum(v['prefix']>0 and 10*v['large_skipped']>=v['prefix'] for v in warm)
    return dict(covers=covers,phases=phases,summary=summary,native_resource=usage,anchor_complete=matched,anchor_truncated=truncated,cold=aggregate(phases[::2]),warm=sumwarm,all=aggregate(phases),warm_windows_ge_10pct=votes,signal=bool(sumwarm['prefix'] and 4*sumwarm['large_skipped']>=sumwarm['prefix'] and votes>=8))

def footprint(d,jobs):
    # Same audited recursive/deduplicating scanner as the product. No file can
    # be deleted in this fixed probe. Separate the unique immutable input inode.
    sums,unlinked,aliases=files(d,jobs,lambda name,p:p.poll())
    t=(d/'text').stat();need(t.st_nlink==1 and t.st_size==N and not unlinked,'input alias/extent or private unlinked file')
    n,b=sums['scratch'];aux=n-N;alloc=b-t.st_blocks*512
    need(0<=aux<=4<<20 and 0<=alloc<=4<<20,'probe control/scratch allocation')
    return dict(aux_logical=aux,aux_allocated_including_dirs=alloc,input_logical=N,input_allocated=t.st_blocks*512,whole_allocated=b,open_unlinked=unlinked,duplicate_names_or_fds=aliases)

def main(d,end):
    need(d==d.resolve() and d.parent==S and re.fullmatch('raw-prefix-windows-[A-Za-z0-9]{6}',d.name) and d.stat().st_uid==os.getuid() and stat.S_IMODE(d.stat().st_mode)==0o700 and time.time()<end<=time.time()+180,'private fixed real-window study')
    cg=Path('/sys/fs/cgroup'+Path('/proc/self/cgroup').read_text().strip().split('::')[1]);get=lambda k:(cg/k).read_text().strip()
    need((get('cpu.max'),get('memory.max'),get('memory.swap.max'))==('100000 100000',str(256<<20),'0') and len(os.sched_getaffinity(0))==1,'window review grant')
    def guard():need(time.time()<end and not (d/'stop.requested').exists(),'real-window STOP/deadline')
    begin=time.monotonic();io_start=optional_io_stat(cg);p=tool();starts=inputs(d,guard);before={name:sig((d/name).stat()) for name in ('text','starts.u64')};ready=time.monotonic()
    own=SimpleNamespace(pid=os.getpid(),returncode=None,poll=lambda:None);jobs={'supervisor':own};peaks={};maxmem={}
    def sample():
        for k,x in footprint(d,jobs).items():peaks[k]=max(peaks.get(k,0),x)
        cur=int(get('memory.current'))
        if cur>maxmem.get('current',0):
            stats=dict(line.split() for line in get('memory.stat').splitlines());maxmem.update(current=cur,**{k:int(stats[k]) for k in ('anon','file','pagetables','slab')})
    with (d/'worker.log').open('xb',buffering=0) as out:
        os.posix_fallocate(out.fileno(),0,256<<10);sample();q=subprocess.Popen([p,'observe',str(end)],stdin=subprocess.DEVNULL,stdout=out,stderr=subprocess.STDOUT,cwd=d,preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_FSIZE,(256<<10,256<<10)));jobs['probe']=q
        try:
            while q.poll() is None:
                guard();sample()
                try:q.wait(timeout=.5)
                except subprocess.TimeoutExpired:pass
        except BaseException:
            (d/'stop.requested').touch()
            try:q.wait(timeout=3)
            except subprocess.TimeoutExpired:q.kill();q.wait()
            raise
        finally:
            with (d/'worker-exit.log').open('x') as f:f.write(f'worker={q.returncode}\n')
            out.truncate(out.tell());os.fsync(out.fileno())
    done=time.monotonic();sample()
    with (d/'exits.log').open('a') as f:f.write(f'worker={q.returncode}\n')
    need(not q.returncode and before=={name:sig((d/name).stat()) for name in before},'worker refused or input identity changed');guard();raw=(d/'worker.log').read_bytes();result=score(raw.decode(),starts,member(11));usage=resource.getrusage(resource.RUSAGE_SELF);events=get('memory.events')
    need({'oom 0','oom_kill 0'}<=set(events.splitlines()) and usage.ru_inblock*512<=180<<20 and usage.ru_oublock*512<=105<<20,'parent IO or OOM refusal')
    sample();result.update(io_stat_before=io_start,io_stat_after=optional_io_stat(cg),sampled_footprint_before_result=peaks,sampled_memory_breakdown_at_max_current=maxmem,aux_allocated_limit_before_result=4<<20,final_controls_reserve=1<<20)
    result.update(utc=time.strftime('%FT%TZ',time.gmtime()),raw_sha256=RAW_SHA,worker_log_sha256=digest(raw),m12_rank_input_bytes=K*8,private_input_bytes=N,private_input_mincore_pages_before_worker=0,prepared_s=ready-begin,observe_subprocess_s=done-ready,additional_review_s=time.monotonic()-done,peak_shared=int(get('memory.peak')),memory_events=events,core=sorted(os.sched_getaffinity(0)),cpu_max=get('cpu.max'),memory_max=get('memory.max'),swap_max=get('memory.swap.max'),parent_attributed_io_before_result=[usage.ru_inblock*512,usage.ru_oublock*512],full_100mb_prior=dict(rows=N+1,prefix_bytes=3993670228,escapes=1129147,max_lcp=34067),scope='Uninterrupted partial rank windows on the certified 100 MB input. No all-row pass, full SA/LCP, final rank, output index, target sample, isolated verifier speed or physical IO comparison.')
    body=(json.dumps(result,sort_keys=True,indent=2)+'\n').encode();need(len(body)<=128<<10,'receipt size')
    with (d/'result.json').open('xb') as f:os.posix_fallocate(f.fileno(),0,len(body));f.write(body);f.flush();os.fsync(f.fileno())
    print(json.dumps({k:result[k] for k in ('all','warm','signal','warm_windows_ge_10pct','anchor_complete','anchor_truncated')},sort_keys=True))
if __name__=='__main__':
    need(__debug__ and len(sys.argv)==3,'private rank window review only');main(Path(sys.argv[1]),int(sys.argv[2]))
