"""Measured 1/3/10 GB overlap gates, from raw or prepared lexical parse.
Build stdout is reserved for the separate raw verifier. No Python per-row path.
"""
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import shutil
import tempfile
import subprocess as sp
import sys
import time

REPO = Path(__file__).resolve().parents[1]
@dataclass(frozen=True)
class Rung:
    n:int; sha:str; anchor:Path; finish:Path; finish_flags:tuple; shared_gib:int; arrays_gib:int; oracle_mib:int
ROOT=Path('/mnt/nvme2n1/erikg/TAPEREADER')
ONE=Rung(1_000_000_665,'b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3',ROOT/'climb/rung1/run_wide/product.sxi2',Path('/tmp/xsa_finish_cur'),('--with-chi','--container-stream'),20,14,1400)
THREE=Rung(3_000_000_000,'3935e8f1d220092d2cd12081938748e8a62f02e882bd021781ec7c3d0cabe7f3',ROOT/'bins-p1/run14e/reader/product.sxi2',ROOT/'bins-p1/bin/xsa_finish',(),60,42,4200)
TEN=Rung(10_000_000_000,'26cfb2de0cccab8a8712a332956d58693dcbbb34373a9031a7bbaaa08cc504c9',ROOT.parent/'lcpruns-gate/g3/scratch/pf-10gb.sxi2',Path('/tmp/xsa_finish_cur'),('--with-chi','--container-stream'),200,140,14000)
TEN_ARMS={f'packed-10g-l{l}-b{b}-t{t}-r{r}':(l,b,t,r) for l in (1,2,4) for b in (0,1) for t in (1,2,4) for r in (0,1)}
def config(profile):
    if profile in TEN_ARMS or profile=='packed-10g-bitmap':return TEN
    if profile in ('packed-3g','packed-3g-bitmap'):return THREE
    if profile in ('narrow','wide','wide-handoff','packed-raw','packed-radix-1','packed-radix-2','packed-radix-4','packed-radix-2-batch','packed-radix-4-batch'):return ONE
    raise ValueError('unknown rung: '+profile)

# Select only gate controls (never persist unrelated inherited environment).
CONTROL_KEYS=set('SS_LCP_SLICE_WINDOW CROSS_V2_PIECES CROSS_V2_PIECE_WINDOW PFCK_SPACING XSA_GATE_PACKED_10G XSA_GATE_PACKED_3G XSA_FINISH_SPOOL XSA_RESIDENT_RADIX_LANES XSA_RESIDENT_RADIX_STEP XSA_RESIDENT_DICT_THREADS OMP_NUM_THREADS OMP_THREAD_LIMIT OMP_DYNAMIC OMP_WAIT_POLICY GOMP_SPINCOUNT XSA_RESIDENT_PACKED XSA_RESIDENT_WIDE_DICT XSA_TEST_RESIDENT_TRIPLES XSA_BATCH_SXLS_ESCAPES XSA_PRIVATE_SXLS_RECLAIM'.split())

def dict_extra(D,t):
    return t*(64+(4*256+2*24576)*(8 if D>=2**31 else 4))+3*4097 if t>1 and D>1 else 0

def emit_extra(D,lanes):
    return 104*((D+16383)//16384)+lanes*2*65536*12+lanes*(3<<20)+(1<<20) if lanes>1 else 0

def expired(d,deadline):
    if deadline is not None and time.time()>=deadline:
        (d/'stop.requested').touch(exist_ok=True);return True
    return False

def release_unowned_pipe(workers,flag):
    # On a stop the sink may never be launched. A producer already inside
    # write_all cannot check the flag until its blocking write returns.
    if flag.exists() and 'sink' not in workers and not workers['resident'].stdout.closed:
        workers['resident'].stdout.close()

def omp_settings(t):
    cap=len(os.sched_getaffinity(0))
    for path in (CG,*CG.parents):
        if path==Path('/sys/fs'):break
        if (path/'cpu.max').is_file():
            quota,period=(path/'cpu.max').read_text().split()
            if quota!='max':cap=min(cap,int(quota)//int(period))
    if cap<4 or t>cap:raise RuntimeError(f'matched arm requires four available CPUs; effective grant {cap}')
    return dict(XSA_RESIDENT_DICT_THREADS=str(t),OMP_NUM_THREADS=str(t),OMP_THREAD_LIMIT=str(t),OMP_DYNAMIC='FALSE',OMP_WAIT_POLICY='PASSIVE',GOMP_SPINCOUNT='0')
GIB = 1 << 30
CG = Path('/sys/fs/cgroup')/Path('/proc/self/cgroup').read_text().strip().split('::/')[1]

def save(p, obj): p.write_text(json.dumps(obj, indent=2, sort_keys=True)+'\n')
def say(*args): print(*args, file=sys.stderr, flush=True)
def sha(p):
    with p.open('rb') as f: return hashlib.file_digest(f,'sha256').hexdigest()
def members(p):
    with p.open('rb') as f:
        header=f.read(64); assert header[:8]==b'SXI2\x08\0\0\0' and struct.unpack_from('<I',header,32)[0]==9
        end=f.seek(-9*40,2); tab={}
        for _ in range(9):
            mid,codec,off,size,ct=struct.unpack_from('<IIQQQ',f.read(40))
            assert mid not in tab and 64<=off<=off+size<=end; tab[mid]=(codec,off,size,ct)
    assert set(tab)=={1,4,5,7,10,11,12,13,14}; return tab

def cg_info():
    data={x:(CG/x).read_text().strip() for x in ('memory.max','memory.swap.max','memory.peak','memory.events','cpu.stat','cpu.max')}
    data['io.stat']=(CG/'io.stat').read_text().strip() if (CG/'io.stat').exists() else None
    return data

def proc_info(pid,iofile):
    p=Path('/proc')/str(pid); status=(p/'status').read_text()
    # Reopening /proc/<zombie>/io may be denied; an FD opened while live
    # still yields final counters before wait(). Never substitute zeros.
    iofile.seek(0); data={k:int(v) for k,v in (l.split(': ') for l in iofile.read().splitlines())}
    for k in ('VmHWM','VmRSS','RssAnon'):
        m=re.search(r'^'+k+r':\s+(\d+) kB',status,re.M); data[k]=int(m[1])*1024 if m else 0
    # comm can contain spaces. Fields 14/15 are user/system clock ticks.
    fields=(p/'stat').read_text().rsplit(')',1)[1].split(); hz=os.sysconf('SC_CLK_TCK')
    data['cpu_s']=(int(fields[11])+int(fields[12]))/hz
    return data

def finished(p):
    # Read final /proc/io and CPU counters *before* wait reaps the zombie.
    return os.waitid(os.P_PID,p.pid,os.WEXITED|os.WNOHANG|os.WNOWAIT) is not None

def footprint(d,retained,ram=False,markers=None):
    # Logical sizes count even if sparse. Sample allocated bytes separately,
    # including private copies, logs and the partial/final product.
    whole=0; parts={'prepared_refs_ids':retained}; stack=[d]
    while stack:
        try: entries=os.scandir(stack.pop())
        except FileNotFoundError: continue  # finish retired the pieces directory
        for e in entries:
            try: st=e.stat(follow_symlinks=False)
            except FileNotFoundError: continue
            whole+=st.st_blocks*512
            if e.is_dir(follow_symlinks=False):stack.append(e.path);continue
            rel=Path(e.path).relative_to(d); f=e.name
            if rel.parts[0]=='in' or len(rel.parts)==1 and (f in ('oracle-corpus','inverse.check','product.sxi2','product.sxi2.partial','stop.requested') or f.endswith(('.log','.json','.sha'))):continue
            if markers is not None and rel.parts[0]=='bands' and f.endswith(('.lcp.done','.lcp.consumed')):markers[0]+=1
            if ram: group={'pieces':'ram_pieces','bands':'ram_slices','bitmap':'ram_bitmap'}[rel.parts[0]]
            elif rel.parts[0]=='columns': group='generated_lexparse'
            else: group=rel.parts[0] if len(rel.parts)==1 else ('pieces' if '.pieces' in str(rel.parent) else 'slices' if rel.parts[0]=='bands' else f)
            parts[group]=parts.get(group,0)+st.st_size
    return whole,parts

def final_markers(bands,rows,reclaim):
    """Only after both workers exit. Unique directory names + exact count
    prove both empty markers for every expected tail ID, without a set."""
    slices=(rows+262143)//262144;first=max(0,slices-64) if reclaim else 0;count=0
    with os.scandir(bands) as entries:
        for e in entries:
            st=e.stat(follow_symlinks=False);assert stat.S_ISREG(st.st_mode) and st.st_size==0 and st.st_nlink==1
            if e.name=='producer.done':continue
            m=re.fullmatch(r'band_(0|[1-9][0-9]*)\.lcp\.(done|consumed)',e.name)
            assert m and first<=int(m[1])<slices;count+=1
    assert count==2*(slices-first) and (bands/'producer.done').is_file()
    return count

def oracle(d,tools,cfg,deadline=None):
    if expired(d,deadline):raise SystemExit(75)
    assert (CG/'memory.max').read_text().strip()==str(cfg.oracle_mib<<20) and (CG/'memory.swap.max').read_text().strip()=='0'
    start=time.monotonic(); peak=0
    with (d/'oracle.log').open('xb') as f:
        args=[tools/'oracle',d/'oracle-corpus'];
        if cfg==TEN:args.append(d/'stop.requested')
        p=sp.Popen(args,stdout=f,stderr=f)
        with open(f'/proc/{p.pid}/io') as io:
            while not finished(p):
                expired(d,deadline);peak=max(peak,proc_info(p.pid,io)['VmHWM']); time.sleep(.5)
            stats=proc_info(p.pid,io); rc=p.wait()
    save(d/'oracle.json',dict(rc=rc,process=stats,sampled_peak_rss=peak,cgroup=cg_info(),wall_including_wait_s=time.monotonic()-start))
    assert rc==0 and f'ORACLE rows={cfg.n+1} independent_final_rank={cfg.n+1} all_adjacent_sa_lcp_bwt_verified=1' in (d/'oracle.log').read_text()

def radix_lanes(profile):return {'packed-radix-1':1,'packed-radix-2':2,'packed-radix-4':4}.get(profile.removesuffix('-batch'),1)

def pack_front(d,tools,cfg,lanes=1,threads=1,deadline=None):
    """Produce *fresh* columns from private cold raw text. The packer itself
    charges each bucket, assignment and output before writing (limit 2N).
    No active sink/finish until the new dictionary tape has been retired.
    """
    from private_cold import cold
    cols=d/'columns'; assert not cols.exists() and not (d/'merged').exists()
    start=time.monotonic(); peak=allocated=0
    with (d/'pack.log').open('xb') as log:
        args=[tools/'lexparse',d/'in/corpus',cols]
        if cfg!=TEN:args.append('--packed-3g' if cfg==THREE else '--packed')
        if expired(d,deadline):raise SystemExit(75)
        p=sp.Popen(args,stdout=log,stderr=log)
        with open(f'/proc/{p.pid}/io') as io:
            while not finished(p):
                expired(d,deadline)
                peak=max(peak,proc_info(p.pid,io)['VmHWM']); wh,parts=footprint(d,0); allocated=max(allocated,wh)
                if sum(parts.values())>2*cfg.n or time.monotonic()-start>(1200 if cfg==TEN else 600):(d/'stop.requested').touch()
                time.sleep(.25)
            info=proc_info(p.pid,io); code=p.wait()
    front=dict(rc=code,process=info,sampled_peak_rss=peak,sampled_allocated=allocated,wall_s=time.monotonic()-start,cgroup_after=cg_info())
    save(d/'front-end.json',front); assert code==0 and not (d/'stop.requested').exists()
    log=(d/'pack.log').read_text()
    if cfg==TEN:
        v={s:int(x) for s,x in re.findall(r'(\w+)=(\d+)',next(l for l in log.splitlines() if l.startswith('LEX10 ')))}
        ref=json.loads((REPO/'gates/receipts/packed-front-10g/front-end.json').read_text())
        for key,value in ref['census'].items():
            if key not in ('read_bytes','write_bytes'):assert v[key]==value,key
        n,m,k,descriptors,peak_counted=(v[s] for s in ('n','m','k','descriptors','peak_counted'));total=v['dictionary_bytes']+4*m
    else:
        match=re.search(r'^LEXPARSE n=(\d+) phrases=(\d+) distinct=(\d+) descriptors=(\d+) .* peak_counted=(\d+) final_columns=(\d+) ',log,re.M)
        assert match; n,m,k,descriptors,peak_counted,total=map(int,match.groups())
    assert n==cfg.n
    if cfg==ONE: assert (m,k)==(31_784_026,25_336_659)
    sizes={p.name:p.stat().st_size for p in cols.iterdir()}; assert set(sizes)=={'parse.u32','dictionary.packed'}
    assert sizes['parse.u32']==4*m and sum(sizes.values())==total<=peak_counted<=2*n
    tape=sizes['dictionary.packed']; D=descriptors+9*k-7; assert tape==D+3*k-1
    resident=12*(k+1)+13*m+8*((m+15)//16)
    bounds=dict(parse=tape+8*m+32*k,aux=tape+D+25*(k+1)+32*m,sort=9*D+resident+(64<<20),
                emit=9*D+16*((D+63)//64)+resident+40*k+(8<<20))
    if cfg==TEN:
        resident=12*(k+1)+21*m+16*((m+15)//16)
        bounds=dict(parse=tape+8*m+40*k,aux=tape+D+33*(k+1)+56*(m+1),sort=9*D+resident+(64<<20),emit=9*D+16*((D+63)//64)+resident+40*k+(8<<20))
        assert bounds==ref['proposed_array_bytes'] and v['D']==D and v['alphabet']<=254 and max(n,tape,D)<2**43
    bounds['emit']+=emit_extra(D,lanes);bounds['sort']+=dict_extra(D,threads)
    front.update(n=n,m=m,k=k,descriptors=descriptors,D=D,array_bounds=bounds,counted_front_peak=peak_counted,columns=sizes); save(d/'front-end.json',front)
    assert 0<k<=m and m+6*k<2**31 and max(bounds.values())<=cfg.arrays_gib*GIB
    if cfg!=TEN:assert m<=100_000_000 and k<=80_000_000 and tape<2**32
    if cfg==THREE: assert 2**31<=D<2**63 and n+1<2**32
    # Conservative reproducible cut: evict only this gate's freshly generated
    # columns and private corpus (even the finish must pay for cold text).
    t=time.monotonic()
    for path in (cols/'dictionary.packed',cols/'parse.u32',d/'in/corpus'): cold(path)
    front.update(counted_front_peak=peak_counted,columns=sizes,cold_barrier_s=time.monotonic()-t)
    save(d/'front-end.json',front); say('PACKED_FRONT',m,k,peak_counted,round(front['wall_s'],3)); return front

def build(d,tools,profile,deadline=None):
    cfg=config(profile); N=cfg.n; matched=profile in TEN_ARMS
    if matched!=(deadline is not None):raise ValueError('deadline required only for matched profiles')
    if matched and expired(d,deadline):raise SystemExit(75)
    lanes,batch,threads,reclaim=TEN_ARMS.get(profile,(radix_lanes(profile),int(profile.endswith('-batch')),1,0))
    omp=omp_settings(threads) if matched else {};cpu_start=time.process_time()
    row_queue=lanes*2*65536*12 if lanes>1 else 0
    bitmap=matched or profile in ('packed-3g-bitmap','packed-10g-bitmap') or profile.startswith('packed-radix-')
    assert (CG/'memory.max').read_text().strip()==str(cfg.shared_gib*GIB) and (CG/'memory.swap.max').read_text().strip()=='0'
    src=d/'in'; corpus=src/'corpus'; assert corpus.stat().st_size==N
    packed=profile.startswith('packed-'); raw_start=time.monotonic(); cg0=cg_info()
    if packed:
        front=pack_front(d,tools,cfg,lanes,threads,deadline); producer_in=d/'columns'; retained=front['columns']['parse.u32'] # through finish
    else:
        producer_in=src; retained=sum((src/f).stat().st_size for f in ('parse.u32','dictionary.refs'))
    retired=False
    refs=members(cfg.anchor); rows=N+1
    # Same finish profile; reserve its full-sized named spools conservatively
    # even though some do not coexist. 64 slices (all escapes worst case).
    # 32 outstanding pieces + 64-release batch, plus 8 for reader/frontier.
    per_piece=44+5*65536+16*((rows.bit_length()*65536+63)//64)
    plan=dict(retained=retained,pfck=64+8*(rows//22+2),bitmap=(rows+7)//8,sparse=refs[11][2],
              lcp_runs=refs[14][2],ef_spools=refs[5][2],pieces=104*(per_piece+40),
              slices=64*(48+17*262144),control_margin=8<<20)
    if matched or profile.startswith('packed-radix-'):plan['row_queue']=row_queue
    bound=sum(plan.values()); assert bound<2*N
    before_emit=sum(front['columns'].values())+plan['control_margin']+row_queue if packed else bound
    post_bound=max(before_emit,bound); assert post_bound<=2*N
    save(d/'scratch-plan.json',dict(parts=plan,counted_bound=post_bound,limit=2*N,before_emit_bound=before_emit,finish_bound=bound,
                                   front_limit=2*N if packed else None,rule='retire fresh packed tape before starting consumers' if packed else 'prepared refs/IDs stay live'))
    (d/'merged').mkdir(); flag=d/'stop.requested'; ram=None; temp=None; extra_env={}; bands=d/'bands'
    # Bitmap already charged once in plan/bound. A tmpfs copy does not
    # coexist with a disk copy. Additionally reserve its RAM until unlink.
    reserve=plan['pieces']+plan['slices']+plan['control_margin']+(plan['bitmap'] if bitmap else 0); rampeak=shmempeak=marker_peak=0
    if profile=='wide-handoff' or packed:
        # PFCK, m14, EF spools, retained parse and product always on NVMe.
        # Bitmap stays there too except in the explicit one-file A/B.
        assert any(line.split()[4]=='/dev/shm' and ' - tmpfs ' in line for line in Path('/proc/self/mountinfo').read_text().splitlines())
        assert shutil.disk_usage('/dev/shm').free>=reserve and cfg.arrays_gib*GIB+reserve+wit_budget(cfg)<cfg.shared_gib*GIB
        temp=tempfile.TemporaryDirectory(prefix='xsa-volatile-',dir='/dev/shm'); ram=Path(temp.name)
        bands=ram/'bands'; extra_env={'XSA_GATE_VOLATILE_PIECES':str(ram/'pieces')}
        if bitmap:(ram/'bitmap').mkdir(mode=0o700)
    bands.mkdir();prepared_cg=cg_info() if matched else None; start=time.monotonic()
    env={k:v for k,v in os.environ.items() if not k.startswith(('XSA_','SS_','CROSS_','PFCK_','TRIE_','LD_PRELOAD'))}
    env.update(SS_LCP_SLICE_WINDOW='64',CROSS_V2_PIECES='1',CROSS_V2_PIECE_WINDOW='32',PFCK_SPACING='22')
    common=['--corpus',corpus,'--remap',src/'remap','--threads','2']; pref=d/'merged/frag'
    w={}; proc={}; iofiles={}; exitcode={}; times={}; peaks={}; logs={}; started={}; tail=None
    controls={}
    def launch(name,cmd,**kwargs):
        if matched:
            settings=kwargs['env'];assert 'XSA_FINISH_SPOOL' not in settings
            assert settings.get('XSA_PRIVATE_SXLS_RECLAIM')==('1' if name=='sink' and reclaim else None)
        logs[name]=(d/(name+'.log')).open('xb'); started[name]=time.monotonic()
        kwargs.setdefault('stdout',logs[name]); w[name]=sp.Popen(cmd,stderr=logs[name],**kwargs); peaks[name]=0
        if matched:
            from overlap_ten_controls import stable_read
            requested={k:v for k,v in kwargs['env'].items() if k in CONTROL_KEYS}
            controls[name]=stable_read(w[name].pid,requested)
            if controls[name]!=requested:request('child environment mismatch or unavailable: '+name)
        iofiles[name]=open(f'/proc/{w[name].pid}/io')
    def request(reason):
        if not flag.exists():say('STOP_REQUEST',reason); flag.write_text(reason+'\n')
        md=d/'merged/modeb';
        if md.exists():(md/'stop.requested').touch(exist_ok=True)
    audit=os.dup(1)
    if cfg==THREE: env['XSA_GATE_PACKED_3G']='1'
    if cfg==TEN: env['XSA_GATE_PACKED_10G']='1'
    if matched:env={k:v for k,v in env.items() if not k.startswith(('OMP_','GOMP_'))}
    if matched or profile.startswith('packed-radix-'):env.update(XSA_RESIDENT_RADIX_LANES=str(lanes),XSA_RESIDENT_RADIX_STEP='16384')
    rank_input=[d/'inverse.check'] if cfg==ONE else []
    binary_dir=tools if (tools/'overlap_resident').exists() else REPO/'xsa/target/release'
    if expired(d,deadline):raise SystemExit(75)
    launch('resident',[binary_dir/'overlap_resident',producer_in,flag,str(cfg.arrays_gib*GIB),*rank_input],
           env=dict(env,**omp,XSA_RESIDENT_PACKED=str(int(packed)),XSA_RESIDENT_WIDE_DICT=str(int(profile!='narrow')),XSA_RESIDENT_AUDIT_FD=str(audit)),stdout=sp.PIPE,pass_fds=(audit,)); os.close(audit)
    counted_peak=0; disk_peak=0; at_peak={}; each={}; last_disk=0; active='start'; samples={}
    with (d/'resident.log').open() as tail:
        while True:
            for l in tail.readlines():
                if l.startswith('RESIDENT_'):say(l.rstrip())
                if l.startswith('RESIDENT_BEGIN'):active=l.strip().split('=')[-1]
            if expired(d,deadline):
                request('deadline')
            release_unowned_pipe(w,flag)
            if active=='emit' and 'sink' not in w and not flag.exists():
                # Only this invocation's newly created packed tape. The binary
                # has closed it and starts emitting only from anonymous arrays.
                if packed:
                    tape=producer_in/'dictionary.packed'; assert tape.stat().st_size==front['columns'][tape.name]
                    tape.unlink(); retired=True; say('PACKED_RETIRE bytes='+str(front['columns'][tape.name]))
                # Don't charge a sort's idle wall to the finish. Frozen data path.
                if reclaim:extra_env['XSA_PRIVATE_SXLS_RECLAIM']='1'
                launch('sink',[binary_dir/'pfp_sort_r',*common,'--out-prefix',pref,'--bands',bands,'--scratch-budget-bytes',str(2*N-retained)],
                       env=dict(env,**extra_env,XSA_TEST_RESIDENT_TRIPLES='1',XSA_BATCH_SXLS_ESCAPES=str(batch)),stdin=w['resident'].stdout,stdout=sp.DEVNULL); w['resident'].stdout.close()
                finish_env=dict(env,LD_PRELOAD=str(tools/'monitor.so'),XSA_GATE_PRIVATE_STOP=str(flag)); fds=()
                if bitmap:
                    # Only this finish child can create the one mapped file in
                    # its private tmpfs directory. No changed binary or flags.
                    fds=(os.open(ram/'bitmap',os.O_DIRECTORY|os.O_NOFOLLOW),)
                    finish_env.update(LD_PRELOAD=finish_env['LD_PRELOAD']+':'+str(tools/'bitmap.so'),
                                      XSA_GATE_BITMAP_PATH=str(pref)+'.chi-bitmap',XSA_GATE_BITMAP_DIRFD=str(fds[0]))
                try:
                    launch('finish',[cfg.finish,*common,'--prefix',pref,'--lcp-bands',bands,'--output',d/'product.sxi2','--poll-timeout','0','--wit-ram-budget',str(wit_budget(cfg)),*cfg.finish_flags],env=finish_env,pass_fds=fds)
                finally:
                    for fd in fds:os.close(fd)
            for name,p in w.items():
                if name in exitcode: continue
                done=finished(p); info=proc_info(p.pid,iofiles[name]);proc[name]=info;peaks[name]=max(peaks[name],info['VmHWM'])
                if done:
                    exitcode[name]=p.wait();times[name]=time.monotonic()-started[name];say('EXIT',name,exitcode[name],round(times[name],3))
                    if exitcode[name]: request(f'{name} rc={exitcode[name]}')
            if time.monotonic()-last_disk>1 or len(exitcode)==len(w):
                last_disk=time.monotonic(); whole,parts=footprint(d,0 if packed else retained); disk_peak=max(disk_peak,whole)
                if ram is not None:
                    markers=[0] if matched else None
                    allocated,rparts=footprint(ram,0,ram=True,markers=markers); rparts.pop('prepared_refs_ids'); parts.update(rparts)
                    if matched:marker_peak=max(marker_peak,markers[0])
                    rampeak=max(rampeak,allocated); shmempeak=max(shmempeak,int(re.search(r'^shmem (\d+)$',(CG/'memory.stat').read_text(),re.M)[1]))
                    if max(allocated,sum(rparts.values()))>reserve:request('volatile scratch exceeded reserved RAM')
                    if bitmap and ('frag.chi-bitmap' in parts or rparts.get('ram_bitmap',0)>plan['bitmap']):request('wrong bitmap placement/extent')
                # Anonymous reordering buffers: charge the entire reservation
                # throughout the post-front arm, not just observable tmpfs.
                if row_queue:parts['row_queue_reserved']=row_queue
                counted=sum(parts.values());
                for k,v in parts.items(): each[k]=max(each.get(k,0),v)
                if counted>counted_peak: counted_peak=counted;at_peak=parts
                if counted>(before_emit if packed and not retired else bound):request('counted scratch crossed reservation')
            mem=int((CG/'memory.current').read_text()); samples[active]=max(samples.get(active,0),mem)
            if flag.exists():request('private flag')
            if sum(x['read_bytes'] for x in proc.values())>20*N or time.monotonic()-start>(12822 if cfg==TEN else 3600):request('runaway read/time guard')
            if len(exitcode)==len(w):break
            time.sleep(.2)
    for f in (*logs.values(),*iofiles.values()):f.close()
    rc=dict(profile=profile,rc=exitcode,sampled_wall_s=times,phase_sampled_shared=samples,process=proc,sampled_peak_rss=peaks,
            counted_sampled_peak=counted_peak,counted_at_peak=at_peak,individual_sampled_peaks=each,whole_dir_sampled_allocated=disk_peak,
            wall_s=time.monotonic()-start,shared_before=cg0,shared_after=cg_info(),stop=flag.exists())
    if profile.startswith('packed-radix-'):rc['sxls_batched']=profile.endswith('-batch')
    if matched:rc.update(prepared_cgroup_before=prepared_cg,worker_controls=controls,sxls_batched=bool(batch),omp_settings=omp,cpu_affinity=sorted(os.sched_getaffinity(0)),harness_cpu_s=time.process_time()-cpu_start,deadline_unix=deadline)
    if packed:
        rc.update(raw_to_product_s=time.monotonic()-raw_start,raw_front_wall_s=front['wall_s'],packed_tape_retired=retired,
                  raw_counted_highwater=max(front['counted_front_peak'],counted_peak),raw_dir_sampled_allocated=max(front['sampled_allocated'],disk_peak),
                  raw_worker_io={k:front['process'][k]+sum(x[k] for x in proc.values()) for k in ('read_bytes','write_bytes')})
    if ram is not None:
        rc['volatile_handoff']=dict(reserved_bytes=reserve,sampled_tmpfs_allocated=rampeak,sampled_cgroup_shmem=shmempeak,swap_max=0,mode='replay from raw input' if packed else 'restart from prepared inputs, no handoff recovery')
        if matched and not rc['stop'] and exitcode==dict(resident=0,sink=0,finish=0):
            assert bands==ram/'bands' and not (d/'bands').exists() and not any(each.get(k,0) for k in ('slices','pieces','frag.chi-bitmap'))
            rc['sxls_markers']=dict(reclaim=bool(reclaim),sampled_peak=marker_peak,exact_final=final_markers(bands,rows,reclaim),bands_on_tmpfs=True)
        if bitmap:
            marks=[l for l in (d/'finish.log').read_text().splitlines() if l.startswith('GATE_BITMAP ')] if (d/'finish.log').exists() else []
            rc['bitmap_placement']=dict(bytes_reserved=plan['bitmap'],sampled_bytes=each.get('ram_bitmap',0),events=marks,
                                        empty_after_finish=not any((ram/'bitmap').iterdir()))
        link=Path(str(pref)+'.pieces')
        if link.is_symlink() and link.readlink()==ram/'pieces':link.unlink()
        temp.cleanup()  # only this gate's fresh volatile directory
    save(d/'build.json',rc)
    assert exitcode==dict(resident=0,sink=0,finish=0) and not flag.exists()
    assert 'oom_kill 0' in rc['shared_after']['memory.events']

def wit_budget(cfg): return (2 if cfg==ONE else 20 if cfg==TEN else 6)*GIB

def certify(d,cfg):
    actual=sha(d/'product.sxi2'); table=members(d/'product.sxi2'); same=table==members(cfg.anchor)
    check='inverse_ranks=31784026' if cfg==ONE else f"recurrence_ranks={json.loads((d/'front-end.json').read_text())['m']}"
    rank_ok=f'RESIDENT_CHECK {check} virtual_end=0' in (d/'resident.log').read_text()
    good=actual==cfg.sha and same and rank_ok and json.loads((d/'oracle.json').read_text())['rc']==0
    save(d/'identity.json',dict(verified=good,sha256=actual,expected=cfg.sha,bytes=(d/'product.sxi2').stat().st_size,members=table,reference_table_identical=same))
    assert good; print('PASS resident ranks, all raw SA/LCP/BWT rows, exact nine-member SHA',actual,flush=True)

if __name__=='__main__':
    mode=sys.argv[1]; d=Path(sys.argv[2]);
    if mode=='certify' and len(sys.argv) in (3,4):certify(d,config(sys.argv[3] if len(sys.argv)>3 else 'narrow'))
    elif mode=='oracle' and len(sys.argv) in (4,5,6):
        profile=sys.argv[4] if len(sys.argv)>4 else 'narrow'
        if (profile in TEN_ARMS)!=(len(sys.argv)==6):raise ValueError('deadline required only for matched profiles')
        oracle(d,Path(sys.argv[3]),config(profile),int(sys.argv[5]) if len(sys.argv)==6 else None)
    elif mode=='build' and len(sys.argv) in (4,5,6):build(d,Path(sys.argv[3]),sys.argv[4] if len(sys.argv)>4 else 'narrow',int(sys.argv[5]) if len(sys.argv)==6 else None)
    else:raise ValueError(mode)
