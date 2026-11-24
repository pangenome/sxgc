"""Fixed 10 GB prepared P1 -> real 64-bit keys -> certified inverse.u64. No product or next-rung dispatch."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess as sp
import sys
import time
from overlap_byte_fixture import lease
from overlap_defaults import decision
from overlap_published import published_main
from overlap_resident_gate import CG,REPO,ROOT,cg_info,finished,proc_info,save,sha
from overlap_scanplan_gate import Phase
from overlap_segmented_gate import CUTOFF,RECEIPTS,cold,current_io,device_values,exclusive,identity,io_delta,load,memory_available,need,stat_io

S=ROOT/'sandbox';SRC=S/'packed-front-10g-YebiCY/columns';PREFIX='parse64-10g-';SCRIPT='overlap_parse64_study.sh'
N=10_000_000_000;M=318164488;K=234472723;T=11942962780;RAM=24<<30;ARRAYS=16<<30;ORACLE=14000<<20;MARGIN=256<<20
IN={'parse.u32':(4*M,'a56741994089e273fe11d27dd51ef09b68c04194f23c1bd70a1845a78e677092'),'dictionary.packed':(T,'2e68e3f5729b3c2d5c6665e5798af447194cb55514c2c9f2f6cfb71ec774fadd')}
BOUND=max(T+12*M,36*M)+(4<<20)

def private(d):need(d.resolve()==d and d.parent==S and re.fullmatch(PREFIX+'[A-Za-z0-9]{6}',d.name) and d.stat().st_uid==os.getuid() and d.stat().st_mode&0o077==0,'private study')
def guard(d,t,s=0):need(time.time()+s<t<=CUTOFF and not (d/'stop.requested').exists(),'headroom/stop')
def cores():
    seen=set();v=[]
    for i in sorted(os.sched_getaffinity(0)):
        p=Path(f'/sys/devices/system/cpu/cpu{i}/topology');x=tuple((p/k).read_text() for k in ('physical_package_id','core_id'))
        if x not in seen:v.append(i);seen.add(x)
        if len(v)==4:return v
    raise RuntimeError('four distinct available cores required')
def admit(d,t):
    private(d);guard(d,t,1100);lease(os.getppid(),t,SCRIPT);exclusive();rev=published_main(Path.home()/'sxgc');need(sp.check_output(['git','-C',REPO,'rev-parse','HEAD'],text=True).strip()==rev and not sp.check_output(['git','-C',REPO,'status','--porcelain']),'wrong pinned tree')
    need(set(S.glob(PREFIX+'*'))=={d} and not any(d.iterdir()),'do not repeat a started study');bank=RECEIPTS/'segmented-front-10g';f=load(bank,'segmented.json');ref=load(bank,'decode.json');need(hashlib.sha256((bank/'segmented.json').read_bytes()+(bank/'decode.json').read_bytes()).hexdigest()=='da231c6cee479c098e2f224de2fafcb9291400506d79e8bce72dd47e1d3436c6' and ref['decode_verified'] and f['equal_column_sha256']=={k:h for k,(_,h)in IN.items()} and f['trusted_columns']==str(SRC) and (f['census']['n'],f['census']['m'],f['census']['k'])==(N,M,K),'wrong column evidence')
    dec=decision('10g');need(dec['profile']=='packed-10g-l2-b1-t4-r1' and BOUND<2*N and shutil.disk_usage(S).free>2*N+(2<<30) and memory_available()>RAM+(2<<30),'control or headroom');stats={k:list(identity(SRC/k)) for k in IN};need(stats==f['bank_identity'],'changed column inodes')
    save(d/'admission.json',dict(rev=rev,deadline=t,parent=os.getppid(),cores=cores(),originals=stats,immutable_column_sha256=f['equal_column_sha256'],counted_upper=BOUND,profile='all-u64 prepared parse, four then one worker; 10 GB rung only'))

def copy_file(src,dest,n,phase):
    h=hashlib.sha256();orig=identity(src);need(src.resolve()==src and orig[2]==n,'input extent/path')
    with src.open('rb') as f,dest.open('xb') as g:
        left=n
        while left:
            phase.guard();b=f.read(min(left,2<<20));need(b,'short input');g.write(b);h.update(b);left-=len(b)
        need(not f.read(1) and identity(src)==orig,'input changed')
    dest.chmod(0o400);cold(dest);return h.hexdigest()

def grant(mem,cpu):
    g=cg_info();need((g['memory.max'],g['cpu.max'],g['memory.swap.max'])==(str(mem),f'{cpu*100000} 100000','0') and (CG/'cgroup.procs').read_text().split()==[str(os.getpid())] and {'oom 0','oom_kill 0'}<=set(g['memory.events'].splitlines()),'dedicated resource grant');return g

def measure(d,label,args,end,capr,capw,affinity,mem,cpus):
    before=grant(mem,cpus);r=dict(before=before,argv=list(map(str,args)),affinity=affinity,io_caps=[capr,capw],status='INCOMPLETE');p=None
    env=dict(PATH=os.defpath,LC_ALL='C',OMP_DYNAMIC='FALSE',OMP_THREAD_LIMIT='4',OMP_STACKSIZE='1M',OMP_PROC_BIND='FALSE');expected=load(d,'tools.json');need(sha(Path(args[0]))==expected[Path(args[0]).name],'tool drift');stop=d/'stop.requested';rss=whole=0;reason=None
    try:
        guard(d,end);t0=time.monotonic()
        with (d/(label+'.log')).open('xb') as log:
            p=sp.Popen(['taskset','-c',','.join(map(str,affinity)),*map(str,args)],env=env,stdout=log,stderr=log)
            with open(f'/proc/{p.pid}/io') as io:
                while True:
                    done=finished(p);x=proc_info(p.pid,io);rss=max(rss,x['VmHWM']);stats={}
                    for path in d.iterdir():
                        try:s=path.stat();stats[(s.st_dev,s.st_ino)]=s
                        except FileNotFoundError:pass
                    logical=sum(s.st_size for s in stats.values());whole=max(whole,sum(512*s.st_blocks for s in stats.values()));di=io_delta(stat_io(before['io.stat']),current_io(CG));bounds=[(x['read_bytes'],x['write_bytes'])]+[(a['rbytes'],a['wbytes']) for a in (di or {}).values()]
                    if time.time()>=end or stop.exists() or logical>2*N or shutil.disk_usage(d).free<MARGIN or any(a>capr or b>capw for a,b in bounds):reason='time/IO/scratch/stop';stop.touch(exist_ok=True)
                    if done:break
                    time.sleep(.2)
            rc=p.wait();r.update(wall_s=time.monotonic()-t0,rc=rc,stop=reason,process=x,sampled_rss=rss,whole_dir_sampled_allocated=whole)
        after=grant(mem,cpus);r.update(after=after,per_device_io=io_delta(stat_io(before['io.stat']),stat_io(after['io.stat'])));need(rc==0 and reason is None and int(after['memory.peak'])<=mem,label+' failed');r['status']='COMPLETED'
    finally:
        if p is not None and p.poll() is None:stop.touch(exist_ok=True);p.wait()
        save(d/(label+'.json'),r)

def worker(d,t,mode):
    private(d);p=load(d,'admission.json');need(p['deadline']==t,'deadline drift');lease(p['parent'],t,SCRIPT);guard(d,t);exclusive();a=p['cores'];stop=d/'stop.requested';tools=d/'parse64';keys=lambda arm:d/f'keys-{arm}.u64';ranks=lambda arm:d/f'inverse-{arm}.u64';legacy=d/'parse.u32';tape=d/'dictionary.packed'
    if mode=='copy':
        g=grant(192<<20,1);r=dict(before=g);phase=Phase(d,r,'authenticated_copy_and_cold',t,T+4*M+MARGIN,T+4*M+MARGIN)
        for name,(n,h) in IN.items():need(list(identity(SRC/name))==p['originals'][name] and copy_file(SRC/name,d/name,n,phase)==h,'wrong private column')
        phase.guard(True);r.update(after=grant(192<<20,1),cold_resident_pages=0,private_stats={k:identity(d/k) for k in IN});save(d/'copy.json',r)
    elif mode=='lift':
        guard(d,t,900);c=load(d,'copy.json');need(all(list(identity(d/k))==v for k,v in c['private_stats'].items()),'private copy changed');end=min(t,int(time.time())+240)
        measure(d,mode,[tools,'lift',tape,legacy,keys(4),K,4,ARRAYS,stop,end],end,T+4*M+MARGIN,8*M+MARGIN,a,RAM,4)
    elif mode=='keys':
        r=dict(before=grant(192<<20,1));need(load(d,'lift.json')['status']=='COMPLETED' and list(identity(tape))==load(d,'copy.json')['private_stats']['dictionary.packed'] and tape.stat().st_nlink==1,'retire only closed generated copy');tape.unlink();keys(4).chmod(0o400);ph=Phase(d,r,'second_copy_hash_cold',t,8*M+MARGIN,8*M+MARGIN);h=copy_file(keys(4),keys(1),8*M,ph);cold(keys(4));ph.guard(True);r.update(sha256=h,private_stats={str(i):identity(keys(i)) for i in [4,1]},cold_resident_pages=0,after=grant(192<<20,1));need(r['private_stats']['1'][:2]!=r['private_stats']['4'][:2],'aliased inputs');save(d/'keys.json',r)
    elif mode in ('rank4','rank1'):
        i=int(mode[-1]);guard(d,t,400 if i==4 else 300);need(list(identity(keys(i)))==load(d,'keys.json')['private_stats'][str(i)],'prepared file changed');cold(keys(i));end=min(t,int(time.time())+580)
        measure(d,mode,[tools,'rank',keys(i),ranks(i),i,ARRAYS,stop,end],end,8*M+MARGIN,8*M+MARGIN,a,RAM,4);need(ranks(i).stat().st_size==8*M,'rank file size');ranks(i).chmod(0o400)
    elif mode in ('verify4','verify1'):
        i=int(mode[-1]);r=dict(before=grant(ORACLE,1));need(load(d,f'rank{i}.json')['status']=='COMPLETED' and list(identity(legacy))==load(d,'copy.json')['private_stats']['parse.u32'],'original lexical ID witness changed');cold(legacy);cold(ranks(i));end=min(t,int(time.time())+180)
        measure(d,mode,[d/'verify',legacy,4,ranks(i),ORACLE,stop,end],end,12*M+MARGIN,MARGIN,a[:1],ORACLE,1);ph=Phase(d,r,'post_verify_sha256',t,8*M+MARGIN,MARGIN);r.update(sha256=sha(ranks(i)),bytes=ranks(i).stat().st_size);ph.guard(True);r.update(after=grant(ORACLE,1));save(d/f'identity{i}.json',r);print(f'PARSE64_RANK_CERTIFIED threads={i} rows={M} sha256={r["sha256"]}',flush=True)
    else:raise ValueError(mode)

def score(d):
    r={};p=load(d,'admission.json');k=load(d,'keys.json');need(p['counted_upper']==BOUND and k['cold_resident_pages']==0,'scratch/cold');geometries=[]
    for i in [4,1]:
        a=load(d,f'rank{i}.json');o=load(d,f'verify{i}.json');h=load(d,f'identity{i}.json');log=(d/f'rank{i}.log').read_text();head=dict((k,int(v))for k,v in re.findall(r'(\w+)=(\d+)',next(s for s in log.splitlines() if s.startswith('HEAD '))));geometries.append({k:v for k,v in head.items() if k not in ('team','requested','tiles')});need(head['requested']==head['team']==i and head['rows']==M and head['distinct']==K and head['keys_above_u32']>0 and 'key_bits=64 rank_bits=64' in log and f'array_bound={40*M+(4<<20)+i*(2<<20)}' in log and 'permutation_recurrence=1' in log,'native width/team');need(o['rc']==a['rc']==0 and a['stop'] is o['stop'] is None and a['affinity']==p['cores'] and a['after']['memory.swap.max']=='0' and int(a['after']['memory.peak'])<=RAM and a['process']['read_bytes']>0 and f'VERIFIED_PARSE64 rows={M} source_token_width=4 ranks=uint64 permutation=1 strict_recurrence=1' in (d/f'verify{i}.log').read_text() and h['bytes']==8*M,'rank verification')
        r[str(i)]=dict(prepared_s=a['wall_s'],cpu_s=a['process']['cpu_s'],read_bytes=a['process']['read_bytes'],write_bytes=a['process']['write_bytes'],peak=int(a['after']['memory.peak']),verifier_s=o['wall_s'],sha256=h['sha256'],per_device_io=a['per_device_io'])
    need(geometries[0]==geometries[1] and r['1']['sha256']==r['4']['sha256'],'unmatched rank columns');ratio={f:r['4'][f]/r['1'][f] for f in ('prepared_s','cpu_s','read_bytes','write_bytes')};return dict(arms=r,geometry=geometries[0],four_over_one=ratio,parallel_goal_met=ratio['prepared_s']<=.8 and max(ratio['read_bytes'],ratio['write_bytes'])<=1.05,certified_parse=True,scope='prepared 64-bit token keys -> fsynced inverse.u64 only. Verifier and adapter separate; no raw product clock, 100 GB fit or launch.')

if __name__=='__main__':
    need(__debug__,'assertions required');mode=sys.argv[1];d=Path(sys.argv[2]);
    if mode=='score':import json;print(json.dumps(score(d),sort_keys=True,indent=2))
    elif mode=='admit':admit(d,int(sys.argv[3]))
    else:worker(d,int(sys.argv[3]),mode)
