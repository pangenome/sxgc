"""Continue only the missing lane arms after the 0769803 post-certificate capture race.
Timed code, tools and private inputs stay on the original schedule. The controller observes
a second, stable live /proc environment sample outside timed services; no new row pass.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess as sp
import sys
import threading
import time

from overlap_bank_arm import bank
from overlap_published import published_main
from overlap_resident_gate import ROOT,REPO,TEN_ARMS,CONTROL_KEYS,save
from overlap_scaling_policy import experiment_order,evaluate
from overlap_ten_controls import AUDITED,PIN,WORKER,FIRST,STABLE,environ,sha
from overlap_ten_score import measured_goals
from overlap_timing_idle import exclusive

S=ROOT/'sandbox';CUTOFF=1795531169;SERIES=S/'scale-full-10g-RZ25xc';H=Path('/tmp/overlap-restart-jceQI2');BANK=REPO/'gates/receipts'
GRID='dict-grid-10g-Yy3QxU';STUDY=BANK/'scale-10g';TOOLS=Path('/tmp/overlap-ten-series-2TmYG9/tools');SRC=TOOLS.parent/'source'

def need(ok,msg):
    if not ok:raise RuntimeError(msg)

def git(p,*args):return sp.check_output(['git','-C',p,*args],text=True).strip()

def load(p):return json.loads(p.read_bytes())

def confirm_pin():
    need(git(H,'rev-parse','HEAD')==PIN and git(SRC,'rev-parse','HEAD')==WORKER and not git(H,'status','--porcelain') and not git(SRC,'status','--porcelain'),'timed source drift')
    need(all(sha(H/'gates'/k)==v for k,v in AUDITED.items()) and (SERIES/'tool-paths.txt').read_text().splitlines()==list(map(str,(SRC,TOOLS))),'schedule or launch drift')
    need((SERIES/'schedule.txt').read_text().splitlines()==[str(S/'packed-10g-bitmap-3eqCFk'),str(H/'gates/receipts/radix-full-1g'),str(CUTOFF),PIN],'pinned schedule required')
    first=STUDY/FIRST
    for l in (first/'binaries.sha').read_text().splitlines()[1:]:
        digest,name=l.split('  ',1);p=Path(name)
        if p.parent==TOOLS or p.name=='xsa_finish_cur':need(sha(p)==digest,'changed tool: '+name)
    need(load(BANK/'native-grid-10g/result.json')==load(S/GRID/'result.json'),'wrong grid')

def wanted(profile):
    lanes,batch,t,reclaim=TEN_ARMS[profile];base=dict(SS_LCP_SLICE_WINDOW='64',CROSS_V2_PIECES='1',CROSS_V2_PIECE_WINDOW='32',PFCK_SPACING='22',XSA_GATE_PACKED_10G='1',XSA_RESIDENT_RADIX_LANES=str(lanes),XSA_RESIDENT_RADIX_STEP='16384')
    sink=dict(base,XSA_TEST_RESIDENT_TRIPLES='1',XSA_BATCH_SXLS_ESCAPES=str(batch))
    if reclaim:sink['XSA_PRIVATE_SXLS_RECLAIM']='1'
    return dict(resident=dict(base,XSA_RESIDENT_PACKED='1',XSA_RESIDENT_WIDE_DICT='1',XSA_RESIDENT_DICT_THREADS=str(t),OMP_NUM_THREADS=str(t),OMP_THREAD_LIMIT=str(t),OMP_DYNAMIC='FALSE',OMP_WAIT_POLICY='PASSIVE',GOMP_SPINCOUNT='0'),sink=sink,finish=base)

def args(pid):return [os.fsdecode(b) for b in (Path('/proc')/str(pid)/'cmdline').read_bytes().split(b'\0') if b]

def controller(d,script):
    found=[]
    for p in Path('/proc').glob('[0-9]*/cmdline'):
        try:a=args(p.parent.name)
        except (FileNotFoundError,ProcessLookupError,PermissionError):continue
        if len(a)>=5 and a[1:4]==[str(script),'build',str(d)]:found.append(int(p.parent.name))
    need(len(found)<=1,'ambiguous controller');return next(iter(found),None)

def sample(parent,d,name,exe,expect):
    proc=Path('/proc')/str(parent);group=(proc/'cgroup').read_text().strip()
    need(group.startswith('0::/'),'unexpected cgroup');cg=Path('/sys/fs/cgroup')/group[4:]
    child=(proc/'task'/str(parent)/'children').read_text().split()
    for item in child:
        p=Path('/proc')/item
        try:
            a=args(item);ticks=int((p/'stat').read_text().rsplit(')',1)[1].split()[19]);ppid=int((p/'stat').read_text().rsplit(')',1)[1].split()[1]);
            if not a or a[0]!=str(exe) or ppid!=parent:continue
            need(any(x.startswith(str(d)+'/') for x in a) and (p/'cgroup').read_text().strip()==group and (p/'exe').samefile(exe),'worker identity mismatch')
            v=environ((p/'environ').read_bytes());
            if not v:continue
            need(v==expect and args(item)==a and int((p/'stat').read_text().rsplit(')',1)[1].split()[19])==ticks,'stable child environment mismatch: '+name)
            return cg,dict(controls=v,argv=a,exe=str(exe),same_executable_inode=True,controller_pid=parent,pid=int(item),start_ticks=ticks,cgroup=group)
        except (ProcessLookupError,FileNotFoundError):continue
    return None

def observe(d,profile,out,done,script,tools,deadline):
    obs={};parent=None;err=None
    try:
        for name,binary in [('resident',tools/'overlap_resident'),('sink',tools/'pfp_sort_r'),('finish',Path('/tmp/xsa_finish_cur'))]:
            while not (d/(name+'.log')).exists():need(not done.wait(.4) and time.time()<deadline and not (d/'stop.requested').exists(),'worker not started')
            end=time.monotonic()+12
            while True:
                parent=parent or controller(d,script);r=sample(parent,d,name,binary,wanted(profile)[name]) if parent else None
                if r:
                    cg,obs[name]=r;break
                need(not done.wait(.05) and time.monotonic()<end,'cannot observe stable '+name)
        grant={k:(cg/k).read_text().strip() for k in ('cpu.max','memory.max','memory.swap.max')};need(grant=={'cpu.max':'400000 100000','memory.max':str(200<<30),'memory.swap.max':'0'},'incorrect service grant')
        save(out/STABLE,dict(profile=profile,binaries_sha256=sha(d/'binaries.sha'),original=str(d),cgroup=obs['resident']['cgroup'],controller_pid=parent,observed=obs,**grant))
        print('STABLE_CONTROLS_OBSERVED',profile,flush=True)
    except Exception as e:
        err=repr(e);save(out/'observer-error.json',dict(error=err));print('STOP_REQUEST external control observer:',err,flush=True)
        # Never signal a timed worker or fabricate evidence; this is the private cooperative boundary.
        (d/'stop.requested').touch(exist_ok=True)

def original_postscore(code,log,d,harness=H):
    if code==0:return ('certified receipt='+str(d)) in log.splitlines()
    before,tag,trace=log.rpartition('Traceback (most recent call last):');lines=[l for l in trace.strip().splitlines() if not re.fullmatch(r'\s*[\^~]+\s*',l)]
    return (code==1 and bool(tag) and len(lines)>=3 and lines[-1]=='AssertionError' and
        lines[-2].strip()=="assert b['worker_controls']==dict(resident=dict(base,**b['omp_settings'],XSA_RESIDENT_PACKED='1',XSA_RESIDENT_WIDE_DICT='1'),sink=sink_controls,finish=base)" and
        lines[-3].strip()==f'File "{harness}/gates/overlap_ten_score.py", line 58, in verified_arm' and
        'PASS resident ranks, all raw SA/LCP/BWT rows, exact nine-member SHA ' in before)

def run(profile,out,prior):
    confirm_pin();exclusive();need(not list(S.glob(profile+'-??????')),'an arm already exists; no duplicate attempts')
    secs=[load(x/'build.json')['raw_to_product_s'] for x in [BANK/'packed-serial-10g',*prior]]
    reserve=min(secs[0],1.4*max(secs[1:]))*1.08+480;need(time.time()+reserve<CUTOFF,'insufficient measured ETA headroom');print('TEN_ADMISSION remaining_s=',int(CUTOFF-time.time()),'need_s=',round(reserve),flush=True)
    return observed_profile(H,SRC,TOOLS,profile,CUTOFF,out,reserve)

def observed_profile(harness,src,tools,profile,deadline,out,reserve,env=None):
    """Same frozen executable launch. Observer is outside the timed services. Not a rung/CPU selector."""
    need(all(sha(harness/'gates'/k)==v for k,v in AUDITED.items()) and git(harness,'rev-parse','HEAD')==PIN and
         git(src,'rev-parse','HEAD')==WORKER and not git(harness,'status','--porcelain'),'unsupported measured source')
    need(__debug__ and profile in TEN_ARMS and reserve>0 and time.time()+reserve<deadline and not list(out.iterdir()),'insufficient time/fresh observer directory required')
    cmd=['bash','gates/overlap_ten_product.sh','--profile',profile,str(src),str(tools),str(deadline)];save(out/'launch.json',dict(argv=cmd,cwd=str(harness),harness=PIN,worker=WORKER,required_s=reserve,deadline=deadline))
    d=t=None;done=threading.Event();print('TEN_ARM_START profile='+profile,flush=True)
    with (out/'launcher.log').open('x') as log,sp.Popen(cmd,cwd=harness,env=env,stdout=sp.PIPE,stderr=sp.STDOUT,text=True,bufsize=1) as p:
        try:
            for l in p.stdout:
                log.write(l);log.flush()
                if l.startswith('receipt='):
                    m=re.fullmatch('receipt=(.*) tools=(.*)\n',l);need(m and Path(m[2])==tools and d is None,'ambiguous launcher receipt')
                    d=Path(m[1]);need(d.resolve()==d and d.parent==S and re.fullmatch(profile+r'-[a-zA-Z0-9]{6}',d.name),'unexpected receipt path');print('RUN_DIRECTORY',d,flush=True)
                    t=threading.Thread(target=observe,args=(d,profile,out,done,harness/'gates/overlap_resident_gate.py',tools,deadline));t.start()
                if l.startswith(('STOP','RESIDENT_RADIX_JOIN','RESIDENT_STREAM','TEN_PRODUCT_SCORE','PASS resident ranks','Traceback','AssertionError')):print(l,end='',flush=True)
        finally:
            rc=p.wait();done.set()
            if t:t.join()
    save(out/'exit.json',dict(original_launcher_rc=rc));need(d is not None and original_postscore(rc,(out/'launcher.log').read_text(),d,harness),'unexpected launcher exit or score failure')
    need((out/STABLE).is_file() and not (d/'stop.requested').exists(),'missing actual env or stopped')
    b=out/profile;bank(d,b);(b/STABLE).write_bytes((out/STABLE).read_bytes());save(b/'postcheck.json',dict(original_launcher_rc=rc,original_launcher_log_sha256=sha(out/'launcher.log'),recovery_revision=git(REPO,'rev-parse','HEAD')))
    g=measured_goals(b)
    if (b/'goals.json').exists():need(g==load(b/'goals.json'),'pinned goals changed')
    else:save(b/'goals.json',g)
    return b

def main():
    need(__debug__ and not sys.argv[1:],'no arbitrary inputs or profiles');rev=published_main(Path.home()/'sxgc');need(git(REPO,'rev-parse','HEAD')==rev and not git(REPO,'status','--porcelain'),'private recovery snapshot not published')
    confirm_pin();pair=BANK/'radix-full-1g';grid=BANK/'native-grid-10g';old=BANK/'packed-serial-10g';order=experiment_order(pair,grid);need(order==[FIRST,'packed-10g-l2-b1-t4-r1','packed-10g-l1-b1-t4-r1'],'wrong order')
    first=STUDY/FIRST;prior=[first];z=evaluate(old,pair,grid,prior);need(not z['complete_lane_sweep'] and len(z['rows'])==1,'partial sweep');need(not any(list(S.glob(p+'-??????')) for p in order[1:]),'a remaining arm exists; review, do not restart')
    import tempfile
    target=Path(tempfile.mkdtemp(prefix='ten-continue-',dir=S));print('TEN_RECOVERY',target,'postcheck_revision='+rev,flush=True)
    for p in order[1:]:
        out=target/('study-'+p);out.mkdir();b=run(p,out,prior);prior.append(b);result=evaluate(old,pair,grid,prior);save(target/f'score-{len(prior)}.json',result)
        print('TEN_ARM_CERTIFIED profile='+p+' bank='+str(b)+' original='+load(b/'source.json')['source'],flush=True)
    print('TEN_SERIES_COMPLETE receipt='+str(target)+' selected='+str(result['selected']),flush=True)
if __name__=='__main__':main()
