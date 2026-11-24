"""Exact 100 MB component gate. Public entry: overlap_bounded_component.sh DEADLINE."""
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import subprocess as sp
import sys
import tempfile
import time
import threading

import overlap_bounded_shortest as shortest
from overlap_published import published_main
from overlap_resident_gate import ROOT, REPO, CG, cg_info, finished, proc_info, save, sha
from overlap_scaling_policy import one_series
from overlap_timing_idle import exclusive, WORKERS
from private_cold import cold

S = ROOT/'sandbox'
SOURCE = S/'pile-slice100.bin'
BANK = REPO/'gates/receipts'
PIN = BANK/'bounded-component-100m/source-pin.json'
DIGEST = '82747f11a9925bb4d89cc812410d98e411ce80b1767ee1ff4dc22e94e2cd874e'
N, M, K, T, U = 100000503, 3170531, 2689467, 127708112, 95434516
QUEUE = 4 << 20
MIB = 1 << 20
# Maximum packed bucket+assignment+tape logical live from the lexparse replacement
# invariant: each (12+raw) record pays for its assignment and new packed word.
PREP_BOUND = N + 8*(M-1) + 12*M
CAP = dict(prep=192, candidate=512, control=2048, oracle=140)
READ = dict(prep=6*N, candidate=8*N, control=8*N, oracle=8*N)
WRITE = dict(prep=8*N, candidate=QUEUE, control=QUEUE, oracle=QUEUE)
WORKERS.update({'overlap_ten_dict_grid.py','overlap_ten_dict_grid.sh','overlap_bounded_packed',
                'overlap_lexparse','overlap_ten_front.py','overlap_ten_series.sh'})


def check(ok, reason):
    if not ok: raise ValueError(reason)


def pin():
    p = json.loads(PIN.read_text())
    check(set(p) == {'source','n','raw_sha256','narrow_row_sha256'} and p['source']==str(SOURCE)
          and p['n']==N and p['narrow_row_sha256']==DIGEST and re.fullmatch('[0-9a-f]{64}',p['raw_sha256']), 'invalid source pin')
    # Closed fixed-rung study. Bind the reviewed *three distinct raw hashes* and
    # their verified row source before opening any payload; a local n/digest alone isn't provenance.
    proof=PIN.with_name('provenance.json')
    check(sha(PIN)=='51b6d8552601cc7e67e985614663fcc65453e13a1552a5b36469c5584487b01d' and
          sha(proof)=='871c6d432e1570f93ddb45ec753f1cc6171cbb6ba605ce1aefbaf13426b15702','different/unreviewed 100 MB source evidence')
    sp.run(['git','-C',REPO,'ls-files','--error-unmatch',PIN.relative_to(REPO),proof.relative_to(REPO)],check=True,stdout=sp.DEVNULL)
    return p


def before(d, deadline, reserve=0):
    check(not (d/'stop.requested').exists() and time.time()+reserve < deadline, 'stop or insufficient remaining time')


def preflight(d, deadline):
    before(d,deadline,900); exclusive()
    p = pin(); rev = published_main(Path.home()/'sxgc')
    check(sp.check_output(['git','-C',REPO,'rev-parse','HEAD'],text=True).strip()==rev and not sp.check_output(['git','-C',REPO,'status','--porcelain']),'pin drift')
    check(deadline<=1795531169,'outside experiment window')
    emit,batch,_ = one_series(BANK/'radix-full-1g')
    check(emit['matched_speed_io_pass'] and batch['matched_batch_pass'], '1 GB comparison did not pass')
    ref=json.loads((BANK/'radix-emit-100m/result.json').read_text())
    check((ref['rows'],ref['digest'])==(N+1,DIGEST), 'wrong legacy row bank')
    check(SOURCE.resolve()==SOURCE and stat.S_ISREG(SOURCE.lstat().st_mode) and SOURCE.stat().st_size==N,'wrong raw bank')
    check(os.getuid()!=0,'non-root user service required')
    check(shutil.disk_usage(S).free >= 2<<30, 'scratch headroom')
    available=int(re.search(r'^MemAvailable:\s+(\d+) kB',Path('/proc/meminfo').read_text(),re.M)[1])*1024
    check(available>=3<<30,'host memory headroom')
    save(d/'inputs.json',dict(pin=p,revision=rev,deadline=deadline,cgroup_MiB=CAP,array_grants={'candidate':320*MIB,'control':1536*MIB},
         mem_available=available,read_limits=READ,write_limits=WRITE,prep_scratch=PREP_BOUND+QUEUE,prepared_scratch=T+4*M+QUEUE,limit=2*N))
    check(max(PREP_BOUND,T+4*M)+QUEUE <= 2*N,'scratch bound')
    return p


def columns(col):
    check({f.name:f.lstat().st_size for f in col.iterdir() if stat.S_ISREG(f.lstat().st_mode)} ==
          {'parse.u32':4*M,'dictionary.packed':T} and len(list(col.iterdir()))==2,'column shape')


def shape(log):
    match=re.search(r'^LEXPARSE n=(\d+) phrases=(\d+) distinct=(\d+) descriptors=(\d+) max_bucket=\d+ peak_counted=(\d+) final_columns=(\d+) reads=(\d+) decode_verified=0$',log,re.M)
    check(match,'missing prep receipt'); n,m,k,u,peak,total,read=map(int,match.groups())
    check((n,m,k,u,total)==(N,M,K,U,T+4*M) and 0<peak<=PREP_BOUND and read<=READ['prep'],'wrong prep shape/traffic')
    return peak


def private_raw(dst, digest, run, deadline):
    from hashlib import sha256
    before(run,deadline);h=sha256();left=N
    with open(SOURCE,'rb',opener=lambda p,f:os.open(p,f|os.O_NOFOLLOW)) as src, dst.open('xb') as out:
        while left:
            before(run,deadline);block=src.read(min(left,1<<20));check(block,'short source');left-=len(block);out.write(block);h.update(block)
        check(not src.read(1),'long source')
    dst.chmod(0o400);check(dst.stat().st_size==N and h.hexdigest()==digest,'private raw pin mismatch'); cold(dst)


def grant(cg, role):
    check(cg['memory.max']==str(CAP[role]*MIB) and cg['cpu.max']=='100000 100000' and cg['memory.swap.max']=='0','wrong actual grant')


def hierarchy(role):
    affinity=sorted(os.sched_getaffinity(0)); check(affinity,'empty CPU set')
    levels={}
    for path in (CG,*CG.parents):
        if path==Path('/sys/fs/cgroup'): break
        mem=(path/'memory.max').read_text().strip(); cpu=(path/'cpu.max').read_text().split()
        check(mem=='max' or int(mem)>=CAP[role]*MIB,'ancestor memory limit')
        check(cpu[0]=='max' or int(cpu[0])>=int(cpu[1]),'ancestor CPU limit')
        levels[str(path)]={'memory.max':mem,'cpu.max':' '.join(cpu)}
    return dict(affinity=affinity,levels=levels)


def traffic(role, info):
    check(info['read_bytes']<=READ[role] and info['write_bytes']<=WRITE[role],'attributed IO runaway: '+role)


def lease(d,deadline):
    owner=json.loads((d/'lease.json').read_text()); pid,fd=owner['pid'],owner['fd']
    proc=Path('/proc')/str(pid)
    lock=Path(f'/run/user/{os.getuid()}/overlap-ten-timing.lock')
    check(owner['deadline']==deadline and (proc/'fd'/str(fd)).samefile(lock) and
          re.search(r'^lock:\s+\d+: FLOCK\s+ADVISORY\s+WRITE '+str(pid)+r' ',(proc/'fdinfo'/str(fd)).read_text(),re.M),'no live coordinator lease')
    argv=[os.fsencode(Path(__file__).resolve())]+([b'--shortest-100m'] if d.name.startswith(shortest.PREFIX) else [])+[str(deadline).encode(),b'']
    check(owner['directory']==str(d) and (proc/'cmdline').read_bytes().split(b'\0')[-len(argv):]==argv,'not this fixed component coordinator')


def worker(d, arm, role, deadline):
    # This internal meter has no file, command, grant or profile arguments.
    follow=d.name.startswith(shortest.PREFIX)
    check(d.parent==S and d.resolve()==d and (follow or d.name.startswith('bounded-component-100m-')) and arm in (('candidate',) if follow else ('candidate','control')) and role in ('prep',arm,'oracle'),'private stage')
    a=d/arm; col=a/'columns'; flag=d/'stop.requested'; rec={}; child=None; start=time.monotonic()
    try:
        lease(d,deadline); before(d,deadline); cg=cg_info(); grant(cg,role); rec['before']=cg; rec['effective']=hierarchy(role)
        if follow:
            base=shortest.load(shortest.RUNG/'candidate'/(role+'.json')); check(rec['effective']['affinity']==base['effective']['affinity'],'different CPU set from measured arm')
        env=dict(PATH=os.defpath,LC_ALL='C',OMP_NUM_THREADS='1',OMP_THREAD_LIMIT='1',RAYON_NUM_THREADS='1')
        if role=='prep': cmd=['lexparse',a/'raw',col,'--packed']
        elif role=='oracle': cmd=['oracle','--component-100m',a/'oracle-raw',flag]
        elif role=='candidate': cmd=['overlap_bounded_packed','--component-shortest-100m' if follow else '--component-100m',col,flag]
        else:
            cmd=['overlap_resident',col,flag,str(1536*MIB)]
            env.update(XSA_TEST_ROW43_FIXTURE='1',XSA_RESIDENT_PACKED='1',XSA_RESIDENT_WIDE_DICT='1')
        cmd[0]=d/'tools'/cmd[0]; rec['argv']=list(map(str,cmd)); peak=0
        if role in ('candidate','control'): columns(col)
        if role=='prep':
            # Legacy --packed polls the flag in the column directory's parent.
            (a/'stop.requested').symlink_to(flag)
        # Bound text even on unexpected stderr. Do not insert a row relay.
        writer = sys.stderr.buffer if role in ('candidate','control') else sys.stdout.buffer
        child=sp.Popen(cmd,env=env, stdout=None if role in ('candidate','control') else sp.PIPE,
                       stderr=sp.PIPE if role in ('candidate','control') else sp.STDOUT)
        output=child.stderr if role in ('candidate','control') else child.stdout
        rec['text_pipe_bytes']=fcntl.fcntl(output,fcntl.F_GETPIPE_SZ)
        def drain():
            remaining=65536
            while block:=output.read(8192):
                writer.write(block[:remaining]); writer.flush()
                if len(block)>remaining: flag.touch()
                remaining=max(0,remaining-len(block))
        reader=threading.Thread(target=drain); reader.start()
        try:
            check(rec['text_pipe_bytes']<=MIB,'text pipe exceeds reservation')
            if follow: check(rec['text_pipe_bytes']==base['text_pipe_bytes'],'text pipe capacity differs from baseline')
            with open(f'/proc/{child.pid}/io') as io:
                try:
                    idle_check=0
                    while not finished(child):
                        if role!='oracle' and time.monotonic()>idle_check:
                            exclusive(child.pid); idle_check=time.monotonic()+5
                        info=proc_info(child.pid,io); peak=max(peak,info['VmHWM']); traffic(role,info)
                        before(d,deadline); time.sleep(.12)
                finally:
                    if not finished(child): flag.touch()
                    os.waitid(os.P_PID,child.pid,os.WEXITED|os.WNOWAIT)
                    rec['process']=proc_info(child.pid,io); rec['sampled_hwm']=peak
                traffic(role,rec['process'])
        finally:
            if not finished(child): flag.touch()
            reader.join()
        check(child.wait()==0,'native stage failed'); before(d,deadline)
    except BaseException as e:
        rec['error']=repr(e); flag.touch(); raise
    finally:
        if child is not None: rec['rc']=child.wait()
        rec['wall_s']=time.monotonic()-start; rec['after']=cg_info(); save(a/(role+'.json'),rec)
    check({'oom 0','oom_kill 0'}<=set(rec['after']['memory.events'].splitlines()),'oom')


def unit(d,arm,role,deadline):
    return ['systemd-run','--user','--quiet','--collect','--wait','--pipe','-p','Nice=10','-p','LimitCORE=0','-p','MemorySwapMax=0','-p','CPUQuota=100%',
            '-p',f'MemoryMax={CAP[role]}M','/usr/bin/python3','-E',Path(__file__).resolve(),'_meter',d,arm,role,str(deadline)]


def study(d,deadline,p):
    follow=d.name.startswith(shortest.PREFIX)
    results={}; identity=shortest.load(d/'reference.json')['columns'] if follow else None
    for arm in (('candidate',) if follow else ('control','candidate')):
        before(d,deadline,420); exclusive(); a=d/arm; a.mkdir(); col=a/'columns'
        private_raw(a/'oracle-raw',p['raw_sha256'],d,deadline); private_raw(a/'raw',p['raw_sha256'],d,deadline); before(d,deadline,420); exclusive()
        save(a/'cold-raw.json',dict(raw_bytes=N,oracle_bytes=N,raw_sha256=p['raw_sha256'],mincore_resident_pages=0))
        with (a/'prep.log').open('xb') as log:
            t=time.monotonic(); sp.run(unit(d,arm,'prep',deadline),stdout=log,stderr=sp.STDOUT,check=True); prep=time.monotonic()-t
        peak=shape((a/'prep.log').read_text()); columns(col); (a/'raw').unlink()
        digests={f.name:sha(f) for f in col.iterdir()}
        check(identity is None or identity==digests,'nonidentical prepared columns'); identity=digests
        for f in col.iterdir():
            check(f.stat().st_nlink==1,'shared column'); cold(f)
        cold(a/'oracle-raw'); save(a/'cold-columns.json',dict(sha256=digests,mincore_resident_pages=0))
        before(d,deadline,360); exclusive()
        with (a/'producer.log').open('xb') as pl,(a/'oracle.log').open('xb') as ol:
            start=time.monotonic(); prod=sp.Popen(unit(d,arm,arm,deadline),stdout=sp.PIPE,stderr=pl); oracle=None
            try:
                pipe_bytes=fcntl.fcntl(prod.stdout,fcntl.F_GETPIPE_SZ); check(pipe_bytes<=MIB,'row pipe exceeds reservation')
                if follow: check(pipe_bytes==shortest.load(shortest.RUNG/'candidate/result.json')['row_pipe_bytes'],'row pipe capacity differs from baseline')
                oracle=sp.Popen(unit(d,arm,'oracle',deadline),stdin=prod.stdout,stdout=ol,stderr=sp.STDOUT)
                prod.stdout.close()
                while prod.poll() is None or oracle.poll() is None:
                    if oracle.poll() not in (0,None) or prod.poll() not in (0,None): (d/'stop.requested').touch()
                    before(d,deadline); time.sleep(.1)
            finally:
                prod.stdout.close()
                if sys.exc_info()[0]: (d/'stop.requested').touch()
                rc=prod.wait(); oc=oracle.wait() if oracle else None
                wall=time.monotonic()-start; save(a/'exits.json',dict(producer=rc,oracle=oc,wall_s=wall))
        check(rc==oc==0,'pipeline failed'); before(d,deadline)
        check((a/'oracle.log').read_text().strip()==f'VERIFIED rows={N+1} final_rank={N+1} narrow_sha256={DIGEST}','missing oracle proof')
        if arm=='control': check(f'RESIDENT_CHECK recurrence_ranks={M} virtual_end=0' in (a/'producer.log').read_text(),'control rank proof')
        results[arm]=dict(prep_s=prep,prepared_to_verified_EOF_s=wall,raw_to_verified_EOF_s=start+wall-t,raw_phase_sum_s=prep+wall,prep_counted_peak=peak+QUEUE,prepared_bound=T+4*M+QUEUE)
        meter={name:json.loads((a/(name+'.json')).read_text()) for name in ('prep',arm,'oracle')}
        results[arm]['native_by_stage']={name:{'read_bytes':r['process']['read_bytes'],'write_bytes':r['process']['write_bytes'],
            'cpu_s':r['process']['cpu_s'],'sampled_hwm':r['sampled_hwm'],'cgroup_peak':int(r['after']['memory.peak'])} for name,r in meter.items()}
        results[arm]['row_pipe_bytes']=pipe_bytes
        save(a/'result.json',results[arm]); shutil.rmtree(col); (a/'oracle-raw').unlink()
    if follow:
        save(d/'result.json',dict(scope='new shortest-tail candidate only; compare to archived component, not to a product',arms=results))
        return
    ratios={k:results['candidate'][k]/results['control'][k] for k in ('prep_s','prepared_to_verified_EOF_s','raw_to_verified_EOF_s')}
    save(d/'result.json',dict(scope='component only, unequal producer RAM; not construction or scale clearance',arms=results,
                              candidate_over_control_wall=ratios))


def build(d,deadline):
    tools=d/'tools'; tools.mkdir()
    # Compilation is outside the construction scratch and scored clocks.
    with tempfile.TemporaryDirectory(prefix='bounded-component-build-') as tmp, (d/'build.log').open('xb') as log:
        def run(args):
            before(d,deadline); env={k:v for k,v in os.environ.items() if not k.startswith(('XSA_','OMP_','GOMP_')) and k not in ('CFLAGS','CC','RUSTFLAGS','CARGO_ENCODED_RUSTFLAGS','LD_PRELOAD')}
            sp.run(args,check=True,cwd=REPO,stdout=log,stderr=sp.STDOUT,timeout=deadline-time.time(),env=env)
        run(['cargo','build','-j1','--locked','--offline','--release','--manifest-path','xsa/pfp/Cargo.toml','--target-dir',tmp,
             '--bin','overlap_resident','--bin','overlap_bounded_packed'])
        for name in ('overlap_resident','overlap_bounded_packed'): shutil.copy2(Path(tmp)/'release'/name,tools/name)
        for src,name,libs in [('overlap_lexparse.cpp','lexparse',[]),('overlap_bounded_component_oracle.cpp','oracle',['-lcrypto'])]:
            run(['g++','-O2','-std=c++17','-Wall','-Wextra','-Werror','gates/'+src,'-o',tools/name,*libs])
    check((d/'build.log').stat().st_size<=128*1024,'oversized build log')
    save(d/'binaries.json',{f.name:sha(f) for f in tools.iterdir()})


def main(deadline,follow=False):
    prefix=shortest.PREFIX if follow else 'bounded-component-100m-'
    d=Path(tempfile.mkdtemp(prefix=prefix,dir=S)); print('component receipt:',d,flush=True)
    flag=d/'stop.requested'
    for sig in (signal.SIGTERM,signal.SIGINT): signal.signal(sig,lambda *_:flag.touch())
    rec={'deadline':deadline,'qualified':False}
    try:
        with (Path(os.environ['XDG_RUNTIME_DIR'])/'overlap-ten-timing.lock').open('a') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            p=preflight(d,deadline)
            if follow:
                check(set(S.glob(prefix+'*'))=={d},'earlier shortest candidate attempt; do not repeat')
                save(d/'reference.json',shortest.reference())
            build(d,deadline)
            if follow: shortest.checked_tools(d)
            save(d/'lease.json',dict(pid=os.getpid(),fd=lock.fileno(),deadline=deadline,directory=str(d)))
            study(d,deadline,p); before(d,deadline)
            if follow:
                save(d/'comparison.json',shortest.compare(d)); print('SHORTEST_100M_STREAM_VERIFIED',flush=True)
            rec['qualified']=True
    except BaseException as e:
        rec['reason']=repr(e); flag.touch(); raise
    finally: save(d/'status.json',rec)


if __name__=='__main__':
    check(not sys.flags.optimize,'optimization disables bank replay checks')
    if len(sys.argv)==6 and sys.argv[1]=='_meter': worker(Path(sys.argv[2]),*sys.argv[3:5],int(sys.argv[5]))
    elif len(sys.argv)==3 and sys.argv[1]=='--shortest-100m' and re.fullmatch(r'[1-9][0-9]{0,10}',sys.argv[2]): main(int(sys.argv[2]),follow=True)
    elif len(sys.argv)==2 and re.fullmatch(r'[1-9][0-9]{0,10}',sys.argv[1]): main(int(sys.argv[1]))
    else: raise SystemExit('expected a single absolute Unix deadline')
