"""One frozen v8 readiness-only probe. No row producer, sweep or reference product read."""
import json
import os
from pathlib import Path
import re
import resource
import shutil
import stat
import subprocess as sp
import sys
import time
import overlap_parse64_study as s
from overlap_scanplan_gate import Phase
from overlap_finish_credits import N, HERE

G=Path(__file__).resolve().parent; SCRIPT='overlap_pfck128_study.sh';PREFIX='pfck128-ready-100m-';OUT=s.RECEIPTS/'pfck128-ready-100m'
T=128;COUNT=(N+1)//T+1;EXT=40+8*COUNT;MARGIN=16<<20;BOUND=EXT+MARGIN;DATA_LIMIT=64<<20 # per-process RLIMIT_DATA, NOT a full heap+cache grant
SOURCES={'raw':(s.S/'pile-slice100.bin',N,'3c965f581b46ba72abfd7fee7a0ce2efac5a61cd4dc5725e9253dcebcccc2383'),
         'remap':(s.S/'remap-256.bin',256,'ec59055b1b82e8bae4710ddcc64d9daabb8b87234c4bbf575b96f3c45bd592a7'),
         'finish':(Path('/tmp/xsa_finish_cur'),12_224_128,'9a0bde6739ba1819f8d298beca2c8a9bf53480fab19f637b49cf6e3c8e200fa7')}
need=s.need

def private(d):need(d.resolve()==d and d.parent==s.S and re.fullmatch(PREFIX+'[A-Za-z0-9]{6}',d.name) and d.stat().st_uid==os.getuid() and d.stat().st_mode&0o777==0o700,'private readiness receipt')
def window(start,end):need(600<end-start<=1000,'601..1000 seconds; single 100 MB readiness arm only')
def guard(d,end,extra=0):s.guard(d,end,extra,cutoff=end)
def admit(d,end):
    t=int(time.time());window(t,end);private(d);guard(d,end,600);s.lease(os.getppid(),end,SCRIPT);s.exclusive();rev=s.published_main(Path.home()/'sxgc')
    need(set(s.S.glob(PREFIX+'*'))=={d} and not any(d.iterdir()) and not OUT.exists() and sp.check_output(['git','-C',s.REPO,'rev-parse','HEAD'],text=True).strip()==rev and not sp.check_output(['git','-C',s.REPO,'status','--porcelain']),'prior arm or unpublished pin')
    need(s.sha(HERE/'review.json')=='f721ca2f7b6177de8d57202f2a62bbabdc8ddbfb3dc6b97c86fc91a6ad863427' and s.load(HERE,'review.json')['models'][-1]['sweep']['pfck']==EXT==6250072 and BOUND<2*N and s.memory_available()>1<<30 and shutil.disk_usage(d).free>1<<30,'wrong reference or headroom')
    stats={}
    for k,(p,n,h) in SOURCES.items():
        need(p.resolve()==p and stat.S_ISREG(p.lstat().st_mode) and p.stat().st_size==n,'input type/extent: '+k);stats[k]=s.identity(p)
    s.save(d/'admission.json',dict(rev=rev,start_unix=t,deadline=end,parent=os.getppid(),affinity=s.cores()[:2],source_stats=stats,source_sha256={k:x[2] for k,x in SOURCES.items()},scope='One frozen v8 readiness/histogram, file spacing 128; no row stream or product.'))
def authority(d,end):
    private(d);a=s.load(d,'admission.json');window(a['start_unix'],end);need(end==a['deadline'],'changed lease');s.lease(a['parent'],end,SCRIPT);guard(d,end);return a

def ledger(d):
    logical=allocated=0
    for p in d.iterdir():
        st=p.lstat();need(stat.S_ISREG(st.st_mode) and st.st_nlink==1,'unexpected nested/aliased output');allocated+=st.st_blocks*512
        if p.name!='raw':logical+=st.st_size
        if p.name=='frag.pfck2':need(st.st_size<=EXT,'PFCK extent limit')
    need(logical<=BOUND,'readiness scratch reservation');return logical,allocated

def setup(d,end,a):
    g=s.grant(192<<20,1);r=dict(before=g);ph=Phase(d,r,'authenticated_private_copy_cold',end,2*N,2*N)
    for k,(src,n,h) in SOURCES.items():
        ident=a['source_stats'][k];need(list(s.identity(src))==ident and s.copy_file(src,d/k,ident[2],ph)==h and not src.samefile(d/k),'different '+k)
    (d/'finish').chmod(0o500);ph.guard(True);r.update(after=s.grant(192<<20,1),private_stats={k:s.identity(d/k) for k in SOURCES},resident_pages=0);s.save(d/'copy.json',r)
    tools={k:s.sha(d/k) for k in ('ready-run','proof','monitor.so','finish')};need(tools['ready-run']==s.sha(G/'overlap_pfck128_worker.sh') and tools['finish']==SOURCES['finish'][2],'tool drift');s.save(d/'tools.json',tools);ledger(d)

def check_inputs(d):
    ids=s.load(d,'copy.json')['private_stats'];need(ids=={k:list(s.identity(d/k)) for k in SOURCES} and not (d/'no-product-dir').exists() and not (d/'no-bands').exists(),'changed private source or unexpected product route')
    need(all(s.sha(d/k)==h for k,h in s.load(d,'tools.json').items()),'binary drift')

def worker(d,end,mode):
    a=authority(d,end);s.exclusive()
    if mode=='setup':setup(d,end,a);return
    check_inputs(d);p=d/'frag.pfck2'
    if mode=='ready':
        need(not p.exists() and not (d/'ready.json').exists() and not (d/'ready.log').exists() and all(resource.getrlimit(k)==(v,v) for k,v in ((resource.RLIMIT_DATA,DATA_LIMIT),(resource.RLIMIT_STACK,1<<20),(resource.RLIMIT_FSIZE,EXT))) and Path('/sys/module/kernel/parameters/ignore_rlimit_data').read_text().strip()=='N','readiness repeat or wrong kernel limits')
        until=min(end,int(time.time())+45);cmd=[d/'ready-run',d]
        s.measure(d,mode,cmd,until,4*N,3*EXT+(4<<20),a['affinity'],192<<20,2,ledger=lambda:ledger(d),cutoff=end)
        log=(d/'ready.log').read_text();need(p.stat().st_size==EXT and f'n={N+1} tau=128 count={COUNT} bytes={EXT} ' in log and 'workers=2 segments=8 worker_bytes=1572864 ' in log and 'stage=readiness-pfck-histogram-probe ' in log and 'XSA_FINISH_PROGRESS' not in log and 'CONTAINER_FINAL' not in log,'wrong frozen readiness endpoint');print('PFCK128_READY',flush=True)
    elif mode=='cold':
        r=dict(before=s.grant(192<<20,1));need(s.load(d,'ready.json')['status']=='COMPLETED' and p.stat().st_size==EXT,'not closed readiness output');p.chmod(0o400);ph=Phase(d,r,'private_postclose_cold',end,4<<20,3*EXT+(4<<20));
        for k in ('raw','remap','frag.pfck2'):s.cold(d/k)
        ph.guard(True);r.update(after=s.grant(192<<20,1),resident_pages=0,pfck=s.identity(p));s.save(d/'postclose.json',r)
    elif mode=='proof':
        need(s.load(d,'postclose.json')['pfck']==list(s.identity(p)) and not (d/'proof.json').exists(),'uncertified handoff or repeat');until=min(end,int(time.time())+45);cmd=[d/'proof','--v8-100m',d/'raw',d/'remap',p,d/'stop.requested',until]
        s.measure(d,mode,cmd,until,N+EXT+(8<<20),4<<20,a['affinity'][:1],128<<20,1,ledger=lambda:ledger(d),cutoff=end)
        need(re.fullmatch(rf'PFCK_PROVED raw_bytes={N} n={N+1} tau=128 checkpoints={COUNT} file_bytes={EXT} used_symbols=\d+ virtual_end=1 exact_mod_2pow64=1\n',(d/'proof.log').read_text()),'wrong proof endpoint');r={};ph=Phase(d,r,'separate_digest',end,EXT+(4<<20),4<<20);h=s.sha(p);ph.guard(True);need(s.load(d,'postclose.json')['pfck']==list(s.identity(p)),'sidecar identity changed');r.update(pfck_sha256=h,bytes=EXT,checkpoints=COUNT);s.save(d/'identity.json',r);print('PFCK128_VERIFIED',flush=True)
    else:raise ValueError(mode)

def score(d):
    a=s.load(d,'admission.json');window(a['start_unix'],a['deadline']);need(a['source_sha256']=={k:x[2] for k,x in SOURCES.items()} and s.load(d,'tools.json')['finish']==SOURCES['finish'][2] and s.load(d,'postclose.json')['resident_pages']==s.load(d,'copy.json')['resident_pages']==0,'input mismatch')
    out={}
    for k,mem,cpu in [('ready',192<<20,2),('proof',128<<20,1)]:
        r=s.load(d,k+'.json');g=r['after'];p=r['process'];need(r['status']=='COMPLETED' and r['rc']==0 and r['stop'] is None and (g['memory.max'],g['cpu.max'],g['memory.swap.max'])==(str(mem),f'{cpu*100000} 100000','0') and {'oom 0','oom_kill 0'}<=set(g['memory.events'].splitlines()) and 0<int(g['memory.peak'])<=mem and r['counted_dir_sampled']<=BOUND and N<=p['read_bytes']<=r['io_caps'][0] and p['write_bytes']<=r['io_caps'][1],'wrong meter: '+k)
        out[k]=dict(wall_s=r['wall_s'],cpu_s=p['cpu_s'],read_bytes=p['read_bytes'],write_bytes=p['write_bytes'],peak=int(g['memory.peak']),sampled_rss=r['sampled_rss'],counted_sampled=r['counted_dir_sampled'],per_device_io=r['per_device_io'])
    ident=s.load(d,'identity.json');need(ident['bytes']==EXT and ident['checkpoints']==COUNT and re.fullmatch(r'[0-9a-f]{64}',ident['pfck_sha256']),'bad proof identity');return dict(phases=out,pfck_sha256=ident['pfck_sha256'],n=N+1,spacing=T,checkpoints=COUNT,file_bytes=EXT,counted_upper_including_tools=BOUND,copy_cold_s=s.load(d,'copy.json')['authenticated_private_copy_cold']['wall_s'],postclose_cold_s=s.load(d,'postclose.json')['private_postclose_cold']['wall_s'],hash_s=ident['separate_digest']['wall_s'],scope='Cold private raw -> frozen v8 readiness-only exit. Separate independent full PFCK proof. No sweep, row stream, product, new joint grant or performance promotion.')

if __name__=='__main__':
    need(__debug__,'assertions required');mode=sys.argv[1];d=Path(sys.argv[2]);
    if mode=='score':print(json.dumps(score(d),sort_keys=True,indent=2))
    elif mode=='admit':admit(d,int(sys.argv[3]))
    else:worker(d,int(sys.argv[3]),mode)
