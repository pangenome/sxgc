"""One fixed 100 MB prepared raw+inverse -> independently checked byte rows, four then one. No product."""
import array
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess as sp
import sys
import time
import overlap_bounded_component as old
import overlap_parse64_study as s
from overlap_finite_route64_plan import ceil
from overlap_scanplan_gate import Phase

need=s.need;load=s.load;N=old.N;M=old.M;K=old.K;U=old.U;S=s.S
PREFIX='finite-emit64-100m-';SCRIPT='overlap_finite_emit64_study.sh';OUT=s.RECEIPTS/'finite-emit64-100m';PREV=s.RECEIPTS/'raw-view64-10g/score.json';PREV_SHA='27d05a8912317bfb29f6b8892886e44717b843b5005284aebb38bfeb4a39101a'
LEG=S/'parse-ranks-WjSNyk';INV=LEG/'work/inverse.u32';PARSE=LEG/'parse.u32';P_SHA='603e39e31da8fb51ad8838a114ee432dc03bb483e3e9f95f523b072d82e7f418';I_SHA='700c61993bf5e8c979c8fb6c1bb42332453a7c73c0b4699cb9f6ed2a06c7ee37'
TARGET=ceil(N,1024);SLOTS=ceil(192*N,100000);OTHER=8*M+(4<<20);NB=2*ceil(N,TARGET)+1;V=32*(M+(U-K)//32)
# Adjacent greedy bands have combined mass above target. Single l across every stripe and band suffices for the upper.
FILE_UP=min(ceil(U*(l+1)+NB*((V-1)>>l),8) for l in range(64))+16*4*NB
BOUND=OTHER+FILE_UP;RAM=512<<20;ARRAYS=320<<20;ORACLE=140<<20

def window(start,end):need(900<end-start<=1500,'fixed 901..1500 s 100 MB lease required')
def private(d):need(d.resolve()==d and d.parent==S and re.fullmatch(PREFIX+'[A-Za-z0-9]{6}',d.name) and d.stat().st_uid==os.getuid() and not d.stat().st_mode&0o077,'private fixed rung')
def cpus():
    out=[];seen=set()
    for i in sorted(os.sched_getaffinity(0)):
        d=Path(f'/sys/devices/system/cpu/cpu{i}/topology');key=tuple((d/k).read_text() for k in ('core_id','physical_package_id'))
        if key not in seen:out.append(i);seen.add(key)
        if len(out)==5:return out
    raise RuntimeError('four producer cores plus separate physical verifier core required')
def sources():
    p=old.pin();bank=s.RECEIPTS/'bounded-component-100m/candidate/cold-columns.json';r=s.RECEIPTS/'parse-ranks/100m.log'
    need(load(bank.parent,bank.name)['sha256']['parse.u32']==P_SHA and s.sha(LEG/'run.log')==s.sha(r)=='8d1a13614af3896abf450f4847b352223fbfd542f16be30f2aef2ca6e99cda48' and s.sha(PREV)==PREV_SHA and load(PREV.parent,PREV.name)['verified_navigation'],'changed input evidence')
    return {'raw':(old.SOURCE,N,p['raw_sha256']),'parse.u32':(PARSE,4*M,P_SHA),'old-inverse':(INV,4*M,I_SHA)}
def guard(d,end,reserve=0):s.guard(d,end,reserve,cutoff=end)
def use(d):
    logical=allocated=0
    for f in d.iterdir():
        try:z=f.lstat()
        except FileNotFoundError:continue
        need(stat.S_ISREG(z.st_mode) and z.st_nlink==1,'unexpected inode in study');allocated+=512*z.st_blocks
        if f.name not in ('raw','oracle-raw'):logical+=z.st_size
    return logical,allocated
def admit(d,end):
    start=int(time.time());window(start,end);private(d);s.lease(os.getppid(),end,SCRIPT);s.exclusive();guard(d,end,900);rev=s.published_main(Path.home()/'sxgc');paths=sources()
    need(set(S.glob(PREFIX+'*'))=={d} and not any(d.iterdir()) and not OUT.exists() and sp.check_output(['git','-C',s.REPO,'rev-parse','HEAD'],text=True).strip()==rev and not sp.check_output(['git','-C',s.REPO,'status','--porcelain']),'nonfresh study or source');need(BOUND==196914175<=2*N and shutil.disk_usage(S).free>=2<<30 and s.memory_available()>=3<<30,'headroom')
    ids={}
    for k,(f,n,h) in paths.items():need(f.resolve()==f and stat.S_ISREG(f.lstat().st_mode) and f.stat().st_size==n,'source path/extent');ids[k]=s.identity(f)
    s.save(d/'admission.json',dict(rev=rev,parent=os.getppid(),start_unix=start,deadline=end,cores=cpus(),source_stats=ids,prior=PREV_SHA,source_hashes={k:h for k,(_,_,h) in paths.items()},counted_upper=BOUND,slots=SLOTS,row_target=TARGET,profile='100 MB prepared raw + widened certified legacy inverse -> row43 -> concurrent independent raw checker. No raw-only clock or index.'))
def authority(d,end):
    private(d);p=load(d,'admission.json');window(p['start_unix'],end);need(p['deadline']==end and time.time()>=p['start_unix'],'changed window');s.lease(p['parent'],end,SCRIPT);guard(d,end);return p
def tools(d):
    r=load(d,'tools.json');need(set(r)=={'emit','verify','oracle'} and all(s.sha(d/k)==h for k,h in r.items()),'tool drift')
def setup(d,end):
    p=authority(d,end);tools(d);s.exclusive();r=dict(before=s.grant(192<<20,1));ph=Phase(d,r,'private_copies_and_adapter',end,4*N,4*N);src=sources()
    def checksrc(k):f,n,h=src[k];need(list(s.identity(f))==p['source_stats'][k],'input changed');return f,n,h
    for dst,orig in [('raw','raw'),('oracle-raw','raw'),('parse.u32','parse.u32')]:
        f,n,h=checksrc(orig);need(s.copy_file(f,d/dst,n,ph)==h,'private copy SHA: '+dst)
    need(sys.byteorder=='little' and array.array('I').itemsize==4 and array.array('Q').itemsize==8,'adapter ABI');f,n,h=checksrc('old-inverse');hin=hashlib.sha256();hout=hashlib.sha256();wide=d/'inverse.u64'
    with f.open('rb') as z,wide.open('xb') as out:
        while block:=z.read(256<<10):
            ph.guard();hin.update(block);a=array.array('I');a.frombytes(block);b=array.array('Q',a).tobytes();out.write(b);hout.update(b)
    need(hin.hexdigest()==h and wide.stat().st_size==8*M and checksrc('old-inverse')[0]==f,'inverse input drift');wide.chmod(0o400);s.cold(wide);ph.guard(True)
    pr=Phase(d,r,'separate_rank_proof_and_readback',end,20*M+(8<<20),8<<20);t=min(end,int(time.time())+90);cmd=[d/'verify',d/'parse.u32','4',wide,str(128<<20),d/'stop.requested',str(t)]
    with (d/'rank-proof.log').open('xb') as log:sp.run(cmd,check=True,timeout=max(1,t-time.time()),stdout=log,stderr=sp.STDOUT)
    need(f'VERIFIED_PARSE64 rows={M} source_token_width=4 ranks=uint64 permutation=1 strict_recurrence=1 virtual_end_rank=0'==(d/'rank-proof.log').read_text().strip() and s.sha(wide)==hout.hexdigest(),'written rank proof');pr.guard(True)
    # No verifier or mapping remains. This inode is the generated private witness, not the original.
    w=d/'parse.u32';need(w.stat().st_nlink==1 and not w.samefile(PARSE) and s.sha(w)==P_SHA,'witness retirement');w.unlink();r.update(after=s.grant(192<<20,1),rank_sha256=hout.hexdigest(),private_stats={k:list(s.identity(d/k)) for k in ('raw','oracle-raw','inverse.u64')},source_hashes=p['source_hashes']);s.save(d/'setup.json',r)
def checked_inputs(d):
    x=load(d,'setup.json');need(not (d/'parse.u32').exists() and x['private_stats']=={k:list(s.identity(d/k)) for k in ('raw','oracle-raw','inverse.u64')} and not (d/'raw').samefile(d/'oracle-raw'),'private inputs drift')
def unit(d,end,w,role,until):
    mem,cpu=(RAM,4) if role=='emit' else (ORACLE,1)
    return ['systemd-run','--user','--quiet','--wait','--pipe','--collect','-p',f'MemoryMax={mem}','-p','MemorySwapMax=0','-p',f'CPUQuota={100*cpu}%','-p','Nice=10','-p','IOAccounting=yes','-p','LimitCORE=0','-p','LimitSTACK=1M','-p',f'RuntimeMaxSec={max(1,until-int(time.time())+5)}','-p','TimeoutStopSec=3','/usr/bin/python3','-BE',str(Path(__file__).resolve()),'meter',str(d),str(end),str(w),role,str(until)]
def meter(d,end,w,role,until):
    p=authority(d,end);tools(d);checked_inputs(d);need(w in (4,1) and role in ('emit','oracle') and load(d,f'cold{w}.json')['arm_deadline']==until<=end and not (d/f'{role}{w}.json').exists(),'arm identity');prod=role=='emit';mem,cpu=(RAM,4) if prod else (ORACLE,1);aff=p['cores'][:4] if prod else p['cores'][4:];caps=(12*N,BOUND+(8<<20)) if prod else (2*N,4<<20);before=s.grant(mem,cpu);stop=d/'stop.requested';name=f'{role}{w}';ph=None;reason=None;rss=count=alloc=0
    for path in (s.CG,*s.CG.parents):
        if path==Path('/sys/fs/cgroup'):break
        lim=(path/'memory.max').read_text().strip();quota,period=(path/'cpu.max').read_text().split();need((lim=='max' or int(lim)>=mem) and (quota=='max' or int(quota)>=cpu*int(period)),'ancestor resource limit')
    cmd=[d/'emit',d/'raw',d/'inverse.u64',d/f'route{w}.ef',w,ARRAYS,OTHER,TARGET,SLOTS,stop,until] if prod else [d/'oracle','--component-100m',d/'oracle-raw',stop];r=dict(before=before,argv=list(map(str,cmd)),affinity=aff,io_caps=caps,status='INCOMPLETE');env=dict(PATH=os.defpath,LC_ALL='C',OMP_THREAD_LIMIT='4',OMP_DYNAMIC='FALSE',OMP_STACKSIZE='1M',OMP_PROC_BIND='FALSE',OMP_WAIT_POLICY='PASSIVE',GOMP_SPINCOUNT='0');start=time.monotonic()
    try:
        with (d/(name+'.log')).open('xb') as log:
            ph=sp.Popen(list(map(str,cmd)),stdin=sp.DEVNULL if prod else None,stdout=None if prod else log,stderr=log,env=env,preexec_fn=lambda:os.sched_setaffinity(0,aff));need(os.sched_getaffinity(ph.pid)==set(aff),'native affinity')
            with open(f'/proc/{ph.pid}/io') as io:
                while True:
                    finished=s.finished(ph);x=s.proc_info(ph.pid,io);rss=max(rss,x['VmHWM']);logical,physical=use(d);count=max(count,logical);alloc=max(alloc,physical);di=s.io_delta(s.stat_io(before['io.stat']),s.current_io(s.CG));traffic=[(x['read_bytes'],x['write_bytes'])]+[(z['rbytes'],z['wbytes']) for z in (di or {}).values()]
                    if time.time()>=until or stop.exists() or logical>BOUND or shutil.disk_usage(d).free<256<<20 or any(a>caps[0] or b>caps[1] for a,b in traffic):reason='scratch/IO/stop/deadline';stop.touch(exist_ok=True)
                    if finished:break
                    time.sleep(.12)
            rc=ph.wait();r.update(rc=rc,wall_s=time.monotonic()-start,process=x,sampled_rss=rss,counted_sampled=count,allocated_including_raw_copies=alloc,stop=reason)
        after=s.grant(mem,cpu);r.update(after=after,per_device_io=s.io_delta(s.stat_io(before['io.stat']),s.stat_io(after['io.stat'])));need(rc==0 and reason is None and int(after['memory.peak'])<=mem,name+' failed');r['status']='COMPLETED'
        if prod:r['route_stat']=s.identity(d/f'route{w}.ef')
    finally:
        if ph is not None and ph.poll() is None:stop.touch(exist_ok=True);ph.wait()
        s.save(d/(name+'.json'),r)
def pair(d,end,w):
    authority(d,end);tools(d);checked_inputs(d);s.exclusive();need(w in (4,1) and not (d/f'pair{w}.json').exists(),'wrong or repeated arm');guard(d,end,640 if w==4 else 360)
    if w==1:
        checked_pair(d,4);f=d/'route4.ef';need(list(s.identity(f))==load(d,'emit4.json')['route_stat'] and f.stat().st_nlink==1,'closed route payload changed');f.unlink()
    t=time.monotonic()
    for k in ('raw','oracle-raw','inverse.u64'):s.cold(d/k)
    until=min(end-(340 if w==4 else 10),int(time.time())+(390 if w==4 else 560));s.save(d/f'cold{w}.json',dict(wall_s=time.monotonic()-t,resident_pages=0,arm_deadline=until,inputs=['raw','inverse.u64','oracle-raw']));stop=d/'stop.requested';rc=oc=None;size=None
    with (d/f'pair{w}.log').open('xb') as log:
        start=time.monotonic();pp=sp.Popen(unit(d,end,w,'emit',until),stdout=sp.PIPE,stderr=log);oo=None
        try:
            size=fcntl.fcntl(pp.stdout,fcntl.F_GETPIPE_SZ);need(size<=1<<20,'row pipe reservation');oo=sp.Popen(unit(d,end,w,'oracle',until),stdin=pp.stdout,stdout=log,stderr=sp.STDOUT);pp.stdout.close();idle=0
            while pp.poll() is None or oo.poll() is None:
                if pp.poll() not in (0,None) or oo.poll() not in (0,None):stop.touch(exist_ok=True)
                guard(d,until)
                if time.monotonic()>idle:s.exclusive();idle=time.monotonic()+5
                time.sleep(.12)
        finally:
            pp.stdout.close()
            if sys.exc_info()[0]:stop.touch(exist_ok=True)
            rc=pp.wait();oc=oo.wait() if oo else None;s.save(d/f'pair{w}.json',dict(emitter=rc,oracle=oc,prepared_to_verified_EOF_s=time.monotonic()-start,pipe_bytes=size))
    checked_pair(d,w);print(f'FINITE_EMIT64_CHECKED w={w} rows={N+1}',flush=True)
def checked_pair(d,w):
    p=load(d,f'pair{w}.json');need(p['emitter']==p['oracle']==0 and (d/f'oracle{w}.log').read_text().strip()==f'VERIFIED rows={N+1} final_rank={N+1} narrow_sha256={old.DIGEST}','independent row proof missing')
    return p
def score(d):
    p=load(d,'admission.json');window(p['start_unix'],p['deadline']);need(p['counted_upper']==BOUND<=2*N and p['prior']==PREV_SHA and p['slots']==SLOTS and p['row_target']==TARGET and len(set(p['cores']))==5 and load(d,'setup.json')['source_hashes']==p['source_hashes']=={'old-inverse':I_SHA,'parse.u32':P_SHA,'raw':'3c965f581b46ba72abfd7fee7a0ce2efac5a61cd4dc5725e9253dcebcccc2383'},'admission/inputs');arms={}
    setup=load(d,'setup.json');need((d/'rank-proof.log').read_text().strip()==f'VERIFIED_PARSE64 rows={M} source_token_width=4 ranks=uint64 permutation=1 strict_recurrence=1 virtual_end_rank=0' and re.fullmatch('[0-9a-f]{64}',setup['rank_sha256']) and (setup['after']['memory.max'],setup['after']['memory.swap.max'],setup['after']['cpu.max'])==(str(192<<20),'0','100000 100000') and set(setup['private_stats'])=={'raw','oracle-raw','inverse.u64'},'missing adapter proof or setup grant')
    for w in (4,1):
        q=checked_pair(d,w);cold=load(d,f'cold{w}.json');need(cold['resident_pages']==0 and cold['inputs']==['raw','inverse.u64','oracle-raw'] and 0<q['pipe_bytes']<=1<<20,'cold/pipe');arm=dict(cold_barrier_s=cold['wall_s'],prepared_to_verified_EOF_s=q['prepared_to_verified_EOF_s'],pipe_bytes=q['pipe_bytes'])
        line,=[s for s in (d/f'emit{w}.log').read_text().splitlines() if s.startswith('FINITE_EMIT ')];f={k:float(v) if k.endswith('_s') else int(v) for k,v in (kv.split('=',1) for kv in line.split()[1:])};need((f['n'],f['m'],f['k'],f['descriptors'],f['rows'],f['max_tie'],f['row_bytes'],f['team'])==(N,M,K,U,N+1,6545,12*(N+1),w) and U<=f['decoded']<=16*U and 0<f['populated']<=f['wire']<=FILE_UP and f['counted']==OTHER+f['wire'] and f['arrays']<=ARRAYS and f['leaves']>=f['bands']>1 and f['bitmap_groups']>0 and q['prepared_to_verified_EOF_s']>0 and all(math.isfinite(v) and v>=0 for v in f.values()),'bad native coverage/fit');arm['phases']=f
        for role in ('emit','oracle'):
            r=load(d,f'{role}{w}.json');g=r['after'];mem,cpu=(RAM,4) if role=='emit' else (ORACLE,1);a=p['cores'][:4] if role=='emit' else p['cores'][4:];z=r['process'];need(r['io_caps']==([12*N,BOUND+(8<<20)] if role=='emit' else [2*N,4<<20]) and (g['memory.max'],g['cpu.max'],g['memory.swap.max'])==(str(mem),f'{cpu*100000} 100000','0') and {'oom 0','oom_kill 0'}<=set(g['memory.events'].splitlines()) and 0<int(g['memory.peak'])<=mem and r['affinity']==a and r['rc']==0 and r['status']=='COMPLETED' and r['stop'] is None and r['counted_sampled']<=BOUND and 0<z['read_bytes']<=r['io_caps'][0] and z['write_bytes']<=r['io_caps'][1],'meter/grant/IO')
            arm[role]=dict(native_service_s=r['wall_s'],cpu_s=z['cpu_s'],read_bytes=z['read_bytes'],write_bytes=z['write_bytes'],peak=int(g['memory.peak']),sampled_rss=r['sampled_rss'],per_device_io=r['per_device_io'],sampled_counted=r['counted_sampled'],sampled_whole_allocated=r['allocated_including_raw_copies'])
        arms[str(w)]=arm
    a,b=arms['4'],arms['1'];need(a['pipe_bytes']==b['pipe_bytes'] and all(a['phases'][k]==b['phases'][k] for k in ('bands','leaves','exact_bypass','groups','decoded','bitmap_groups','max_mass','local_scans')),'different workload');ratio={k:a['emit'][k]/b['emit'][k] for k in ('cpu_s','read_bytes','write_bytes')};ratio['wall']=a['prepared_to_verified_EOF_s']/b['prepared_to_verified_EOF_s'];return dict(verified_rows=N+1,narrow_sha256=old.DIGEST,arms=arms,four_over_one=ratio,parallel_goal_met=ratio['wall']<=.80 and max(ratio['read_bytes'],ratio['write_bytes'])<=1.05,counted_file_upper=BOUND,scope='Prepared raw + certified inverse to independently verified EOF only. Setup/adapter and rank proof separate. No raw-only/product clock or scale admission.')

if __name__=='__main__':
    need(__debug__,'assertions required');mode=sys.argv[1];d=Path(sys.argv[2]);
    if mode=='score':print(json.dumps(score(d),sort_keys=True,indent=2))
    elif mode=='admit':admit(d,int(sys.argv[3]))
    elif mode=='setup':setup(d,int(sys.argv[3]))
    elif mode=='pair':pair(d,int(sys.argv[3]),int(sys.argv[4]))
    elif mode=='meter':meter(d,int(sys.argv[3]),int(sys.argv[4]),sys.argv[5],int(sys.argv[6]))
    else:raise ValueError(mode)
