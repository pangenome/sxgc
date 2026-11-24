"""Small fixed book-window/seeded in-RAM finite-order crossover. NO Pile payload, product or dispatch from scores."""
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
from overlap_bounded_shortest import archived
from overlap_published import published_main
from overlap_resident_gate import CG,ROOT,REPO,cg_info,save,sha,proc_info
from overlap_segmented_gate import io_delta,stat_io,memory_available
from overlap_timing_idle import exclusive

S=ROOT/'sandbox'; PREFIX='byte-local-fixture-'; BOOK=Path.home()/'text/pg64317.txt'; SIZE=306317; WIDTH=3072; WINDOWS=64
DIGEST='43b101cc636c0214414fb994b71b2180074f3ec0b11d5699e7ce81450688dbd3'; CUTOFF=1795531169
BASE=REPO/'gates/receipts/bounded-shortest-100m'; BASE_SCORE='0827609e682d72d1e3e2ae4ce2ea5f564414f7691edc38f99072c0c0f5a2b8c4'; MODES=('Bits','Bytes','Bytes','Bits');SHAPES=('book','wide','families')

def need(ok,s):
    if not ok:raise RuntimeError(s)

def private(d,prefix=PREFIX):need(d.resolve()==d and d.parent==S and re.fullmatch(prefix+'[A-Za-z0-9]{6}',d.name) and d.stat().st_uid==os.getuid() and d.stat().st_mode&0o077==0,'private fixture receipt')
def load(p):return json.loads(p.read_bytes())
def guard(d,t,reserve=0):need(time.time()+reserve<t<=CUTOFF and not (d/'stop.requested').exists(),'deadline/stop')
def lease(pid,t,script='overlap_byte_fixture.sh'):
    coord=Path('/proc')/str(pid);lock=Path(os.environ['XDG_RUNTIME_DIR'])/'overlap-ten-timing.lock'
    need((coord/'fd/9').samefile(lock) and any(Path(os.fsdecode(arg)).name==script for arg in (coord/'cmdline').read_bytes().split(b'\0') if arg) and (coord/'cmdline').read_bytes().endswith(str(t).encode()+b'\0'),'wrong coordinator')
    with lock.open('a') as f:
        try:fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:return
        raise RuntimeError('coordinator lacks lease')

def admit(d,t):
    private(d);guard(d,t,120);lease(os.getppid(),t);exclusive();rev=published_main(Path.home()/'sxgc')
    need(set(S.glob(PREFIX+'*'))=={d} and not any(d.iterdir()) and sp.check_output(['git','-C',REPO,'rev-parse','HEAD'],text=True).strip()==rev and not sp.check_output(['git','-C',REPO,'status','--porcelain']),'prior attempt, dirty or unpublished source')
    r=archived(BASE);need(sha(BASE/'comparison.json')==BASE_SCORE and r['emitting_order_counter_prediction_matched'] and not r['compared_to_archived']['control']['both_under_2x'],'missing certified hotspot')
    need(memory_available()>=1<<30 and shutil.disk_usage(S).free>=256<<20,'host headroom');need(BOOK.resolve()==BOOK and stat.S_ISREG(BOOK.lstat().st_mode) and BOOK.stat().st_size==SIZE,'wrong tiny public sample')
    save(d/'inputs.json',dict(revision=rev,parent=os.getppid(),deadline=t,profile='book+2 seeded in-RAM finite-group crossover; no Pile input',baseline_score_sha256=BASE_SCORE,book=str(BOOK),book_bytes=SIZE,book_sha256=DIGEST,windows=WINDOWS,width=WIDTH))

def grant():
    c=cg_info();need(c['memory.max']==str(192<<20) and c['memory.swap.max']=='0' and c['cpu.max']=='100000 100000' and (CG/'cgroup.procs').read_text().split()==[str(os.getpid())],'dedicated 192 MiB one CPU no swap required');return c

def measure(d,t):
    return measured_book_fixture(d,t,PREFIX,'overlap_byte_fixture.sh','--fixed-small-windows',score,'CHECKED_FINITE_ORDER_ONLY')

def measured_book_fixture(d,t,prefix,launcher,flag,scorer,completed):
    """Both small studies use exactly the same authenticated book, window policy and service grant."""
    private(d,prefix);p=load(d/'inputs.json');need(p['deadline']==t,'changed deadline');lease(p['parent'],t,launcher);guard(d,t,60);exclusive();before=grant();r=dict(status='NOT_VERIFIED',before=before);child=None
    try:
        exe=d/'fixture';need(exe.is_file() and not exe.is_symlink() and sha(exe)==(d/'fixture.sha').read_text().split()[0],'fixture binary changed')
        with open(BOOK,'rb',opener=lambda p,f:os.open(p,f|os.O_NOFOLLOW)) as f:
            data=f.read(SIZE+1);need(len(data)==SIZE and hashlib.sha256(data).hexdigest()==DIGEST and stat.S_ISREG(os.fstat(f.fileno()).st_mode),'wrong book bytes')
        offsets=[i*(SIZE-WIDTH)//(WINDOWS-1) for i in range(WINDOWS)];sample=b''.join(data[i:i+WIDTH] for i in offsets);del data
        need(len(sample)==WINDOWS*WIDTH and all(j-i>=WIDTH for i,j in zip(offsets,offsets[1:])),'overlapping/short sample');r.update(sample_sha256=hashlib.sha256(sample).hexdigest(),sample_bytes=len(sample),offsets=offsets)
        with os.fdopen(os.memfd_create('byte-order-book-windows'),'w+b') as f, (d/'fixture.log').open('xb') as out:
            f.write(sample);f.seek(0);del sample;guard(d,t,60);child=sp.Popen([exe,flag,d/'stop.requested',str(t)],stdin=f,stdout=out,stderr=sp.STDOUT,env=dict(PATH=os.defpath,LC_ALL='C'))
            with open(f'/proc/{child.pid}/io') as io:
                os.waitid(os.P_PID,child.pid,os.WEXITED|os.WNOWAIT);r['process']=proc_info(child.pid,io)
            r['rc']=child.wait()
        after=grant();r.update(after=after,per_device_io=io_delta(stat_io(before['io.stat']),stat_io(after['io.stat'])))
        need(r['rc']==0 and 0<int(after['memory.peak'])<=192<<20 and {'oom 0','oom_kill 0'}<=set(after['memory.events'].splitlines()) and (d/'fixture.log').stat().st_size<=32<<10,'fixture failed/log/OOM')
        v=scorer(d/'fixture.log');guard(d,t);save(d/'score.json',v);r['status']=completed
    finally:
        if child is not None and child.poll() is None:(d/'stop.requested').touch();child.wait()
        save(d/'meter.json',r)

def score(log):
    need(log.is_file() and not log.is_symlink() and log.stat().st_size<=32<<10,'finite log size');lines=log.read_text().splitlines();need(len(lines)==12,'incomplete crossover');out={}
    for shape in SHAPES:
        v=[]
        for i,how in enumerate(MODES):
            tag,*fields=lines.pop(0).split();x=dict(f.split('=') for f in fields);need(tag=='CHECKED' and (x.pop('shape'),x.pop('order'),int(x.pop('rep')))==(shape,how,1+i//2),'order/profile')
            x={k:(float(t) if k.endswith('_s') else int(t)) for k,t in x.items()};need(all(math.isfinite(t) and t>=0 for t in x.values()) and x['cpu_s']>0 and x['wall_s']>0,'negative or absent clock')
            k,u=x['words'],x['descriptors'];fixed=13056;slots100=((64<<20)-20*8192-fixed)//8;budget=fixed+8*((u*slots100+95434515)//95434516)
            need(0<k<=u<=x['rows'] and u<=1<<20 and 0<x['groups']<=u and 0<x['largest']<=min(x['peak_slots'],k) and x['scans']>0 and x['distributions']>0 and x['distributions']+x['scans']<=32 and x['workspace']==budget and 8*x['peak_slots']+x['frame_reserve']+fixed<=budget and 4096*x['frames_peak']<=x['frame_reserve'],'work/shape/budget')
            need((how=='Bits')==(x['frame_reserve']==0) and (how!='Bits' or all(x[k]==0 for k in ('byte_partitions','frames_peak','leaf_comparisons','shifted_slots'))),'unexpected algorithm counters');v.append(x)
        common=('dictionary_bytes','words','descriptors','rows','groups','largest','workspace');need(all(all(x[k]==v[0][k] for k in common) for x in v),'unmatched shapes')
        need(all(all(x[k]==y[k] for k in x if not k.endswith('_s')) for x,y in ((v[0],v[3]),(v[1],v[2]))),'nondeterministic work')
        out[shape]=dict(observations=v,bytes_over_bits_wall=[v[1]['wall_s']/v[0]['wall_s'],v[2]['wall_s']/v[3]['wall_s']],bytes_over_bits_cpu=[v[1]['cpu_s']/v[0]['cpu_s'],v[2]['cpu_s']/v[3]['cpu_s']])
    worst={k:max(v['bytes_over_bits_wall']) for k,v in out.items()};good=worst['book']<=.65 and min(worst['wide'],worst['families'])<=.8 and max(worst.values())<=1.1
    return dict(shapes=out,worst_wall_ratio=worst,fixture_hypothesis_met=good,product_promotion=False,scope='in-RAM finite groups and same checker; not full suffix rows, raw preparation, or a Pile product; no dispatch')

def checked_fixture_bank(d):
    s=load(d/'source.json');p=load(d/'inputs.json');m=load(d/'meter.json');names=set('build-and-fixtures.log exits.log fixture.log fixture.sha inputs.json meter.json score.json'.split());original=Path(s['original_directory'])
    need(s['artifacts'].keys()==names and all(not (d/n).is_symlink() and (d/n).is_file() and (d/n).stat().st_size<=32768 and sha(d/n)==s['artifacts'][n] for n in names),'archive digests')
    need(s['exit_code']==0 and p['revision']==s['revision']=='261de0723152a5681bbed9dfc427845bccaf1279' and original.parent==S and re.fullmatch(PREFIX+'[A-Za-z0-9]{6}',original.name) and (d/'fixture.sha').read_text().split()==[s['native_sha256'],str(original/'fixture')] and (d/'exits.log').read_text().strip()=='fixture=0','launch pin/exit')
    need(p['book_bytes']==SIZE and p['book_sha256']==DIGEST and p['baseline_score_sha256']==BASE_SCORE and (p['width'],p['windows'])==(WIDTH,WINDOWS) and m['offsets']==[i*(SIZE-WIDTH)//(WINDOWS-1) for i in range(WINDOWS)] and m['sample_bytes']==WIDTH*WINDOWS and m['sample_sha256']=='09840d8046cb448eb6d9a04d60e85bbe053cfd397b676a9f188c7a925309034c','sample identity')
    need(m['status']=='CHECKED_FINITE_ORDER_ONLY' and m['rc']==0 and m['process']['cpu_s']>0 and m['per_device_io']==io_delta(stat_io(m['before']['io.stat']),stat_io(m['after']['io.stat'])),'fixture meter')
    for cg in (m['before'],m['after']):need((cg['memory.max'],cg['cpu.max'],cg['memory.swap.max'])==(str(192<<20),'100000 100000','0') and 0<int(cg['memory.peak'])<=192<<20 and {'oom 0','oom_kill 0'}<=set(cg['memory.events'].splitlines()),'fixture grant/OOM')
    z=score(d/'fixture.log');need(z==load(d/'score.json'),'score mismatch');return z

if __name__=='__main__':
    if len(sys.argv)==3 and sys.argv[1]=='archived':print(json.dumps(checked_fixture_bank(Path(sys.argv[2])),sort_keys=True,indent=2))
    elif len(sys.argv)==3 and sys.argv[1]=='score':print(json.dumps(score(Path(sys.argv[2])),sort_keys=True,indent=2))
    elif len(sys.argv)==4 and sys.argv[1] in ('admit','measure') and re.fullmatch(r'[1-9][0-9]{9}',sys.argv[3]):{'admit':admit,'measure':measure}[sys.argv[1]](Path(sys.argv[2]),int(sys.argv[3]))
    else:raise SystemExit('expected score FIXTURE_LOG or fixed launcher mode')
