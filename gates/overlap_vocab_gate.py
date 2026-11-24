"""Optional 10 GB component only: independent post-clock descriptor/row census checks."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess as sp
import sys
import time
from overlap_scanplan_gate import Phase,MARGIN
from overlap_scanplan_hundred import CUTOFF,CALIBRATION_SHA,calibration_clock
from overlap_segmented_gate import (CG,RECEIPTS,SANDBOX,RUNGS,cg_info,identity,io_delta,stat_io,current_io,need,cold)
from overlap_resident_gate import TEN,REPO,finished,proc_info,save,sha
from overlap_ten_matched_score import metrics
from overlap_scaling_policy import one_series
from overlap_published import published_main
from overlap_timing_idle import WORKERS,exclusive
from overlap_vocab_plan import CELLS,counts,digest,lexical_block_model,study

WORKERS.update(('overlap_ten_series.sh','overlap_ten_dict_grid.py','overlap_ten_dict_grid.sh','overlap_bounded_packed'))
SOURCE=RUNGS['10g'][1];TAPE=SANDBOX/'packed-front-10g-YebiCY/columns/dictionary.packed'
RESERVE=4<<20;COMPONENT_CAP=1200;HEADROOM=1500

def load(p):return json.loads(p.read_text())

def prospective(n,m,maxword):
    """Count bytes once per original word except at most fan partial input segments in an active merge."""
    h=32<<20;fan=8;ceil=lambda a,b:(a+b-1)//b
    need(0<m<=n and 0<maxword<=1<<20,'shape refusal')
    B=n+12*m-8;A=B+8*m;raw_bucket_max=ceil(B,h-maxword-4)+1;raw_parts=raw_bucket_max+255
    forms=ceil(A,h-maxword-12)+raw_parts;created=raw_parts+forms;nr=raw_parts
    level=0;r=raw_bucket_max
    while r>1:r=ceil(r,fan);level+=1
    writes=B+(1+max(0,level-1))*A
    for _ in range(max(0,level-1)):
        nr=ceil(nr,fan)+256;created+=ceil(A,h-maxword-12)+nr
    out=dict(first_pass_bytes=B,one_count_run_pass_upper=A,max_raw_segments_per_bucket=raw_bucket_max,
             created_segments_upper=created,merge_levels_upper=level,logical_scratch_traffic_upper=2*writes,
             logical_peak_upper=A+fan*h,small_control_reserve=RESERVE,counted_bound=A+fan*h+RESERVE,counted_limit=2*n)
    need(out['counted_bound']<=2*n and 2*writes<=24*n and level<=6 and created<=65536 and raw_bucket_max<=8192,'prospective scratch/IO/directory cap')
    return out

def banks():
    raw=(RECEIPTS/'scanplan-10g/scan.json').read_bytes();need(hashlib.sha256(raw).hexdigest()==CALIBRATION_SHA,'reviewed scan calibration changed')
    ref=load(RECEIPTS/'packed-front-10g/front-end.json');s=json.loads(raw);calibration_clock(s,ref['census'])
    need(metrics(RECEIPTS/'packed-serial-10g')['profile']=='packed-10g-bitmap','missing exact full 10 GB control');one_series(RECEIPTS/'radix-full-1g')
    return s['census'],ref['census']

def admitted(deadline):
    need(__debug__ and time.time()+HEADROOM<deadline<=CUTOFF,'not enough time or outside study');exclusive()
    rev=published_main(Path.home()/'sxgc');need(sp.check_output(['git','-C',REPO,'rev-parse','HEAD'],text=True).strip()==rev and not sp.check_output(['git','-C',REPO,'status','--porcelain']),'study pin drift')
    c,f=banks();plan=prospective(c['n'],c['m'],c['max_phrase']);need(shutil.disk_usage(SANDBOX).free>=3*TEN.n+(1<<30),'disk headroom')
    need(identity(SOURCE)[2]==TEN.n and identity(TAPE)[2]==f['dictionary_bytes'] and not SOURCE.is_symlink() and not TAPE.is_symlink(),'incorrect raw/tape identity')
    return dict(revision=rev,deadline=deadline,raw=str(SOURCE),source_stat=identity(SOURCE),raw_sha256=c['sha256'],banked_tape=str(TAPE),tape_stat=identity(TAPE),plan=plan)

def grant():
    cg=cg_info();need(cg['memory.max']==str(192<<20) and cg['memory.swap.max']=='0' and cg['cpu.max']=='100000 100000' and
        (CG/'cgroup.procs').read_text().split()==[str(os.getpid())],'not private single-CPU 192 MiB service');return cg

def meter(d,cmd,role,end,report):
    n=TEN.n;flag=d/'stop.requested';before=grant();r=report[role]=dict(before=before,argv=list(map(str,cmd)))
    rcaps=(16*n,16*n) if role=='census' else (TAPE.stat().st_size+MARGIN,MARGIN)
    t=time.monotonic();peak=alloc=live=scans=0;next_scan=t;reason=None;bi=stat_io(before['io.stat']);info={}
    with (d/(role+'.out')).open('xb') as output,(d/(role+'.err')).open('xb') as err:
        p=sp.Popen(cmd,cwd=d,stdout=output,stderr=err,env=dict(PATH=os.defpath,LC_ALL='C'))
        try:
            with open(f'/proc/{p.pid}/io') as io:
                while True:
                    done=finished(p);info=proc_info(p.pid,io);peak=max(peak,info['VmHWM']);delta=io_delta(bi,current_io(CG));now=time.monotonic()
                    if role=='census' and now>=next_scan:
                        paths=d/'vocab.tmp';a=v=0
                        try:
                            with os.scandir(paths) as entries:
                                for entry in entries:
                                    try:st=entry.stat(follow_symlinks=False)
                                    except FileNotFoundError:continue
                                    a+=st.st_blocks*512;v+=st.st_size
                        except FileNotFoundError:pass
                        live=max(live,v);alloc=max(alloc,a);scans+=1;next_scan=time.monotonic()+max(4,40*(time.monotonic()-now))
                    traffic=[(info['read_bytes'],info['write_bytes']),*((x['rbytes'],x['wbytes']) for x in (delta or {}).values())]
                    if (time.time()>=end or flag.exists() or live+RESERVE>2*n or shutil.disk_usage(d).free<MARGIN or
                        any(not (0<=rd<=rcaps[0] and 0<=wr<=rcaps[1]) for rd,wr in traffic) or os.fstat(output.fileno()).st_size+os.fstat(err.fileno()).st_size>RESERVE):
                        reason='time/stop/traffic/scratch/log guard';flag.touch(exist_ok=True)
                    if done:break
                    time.sleep(.25)
        except BaseException:
            flag.touch(exist_ok=True);raise
        finally:
            code=p.wait();after=cg_info();r.update(rc=code,stop=reason,wall_s=time.monotonic()-t,process=info,peak_sampled_rss=peak,after=after,
                    device_io=io_delta(bi,stat_io(after['io.stat'])),rw_caps=rcaps,scans=scans,sampled_bucket_logical=live,sampled_bucket_allocated=alloc)
    need(code==0 and not reason and not flag.exists() and {'oom 0','oom_kill 0'}<=set(after['memory.events'].splitlines()),'component stopped/failed/OOM')

def readout(p):
    need(p.stat().st_size<=RESERVE,'oversize census output');lines=p.read_text().splitlines();tokens=lines[0].split();summary={k:int(z) for k,z in (x.split('=') for x in tokens)}
    need(len(tokens)==len(summary) and len(lines)<=1+CELLS,'malformed census output');rows=[tuple(map(int,l.split())) for l in lines[1:]]
    return summary,counts([(i,d) for i,d,_ in rows]),counts([(i,v) for i,_,v in rows])

def worker(d,deadline,report,verify):
    grant();need(d.resolve()==d and d.parent==SANDBOX and re.fullmatch(r'vocab-10g-[a-zA-Z0-9]{6}',d.name) and d.stat().st_mode&0o077==0,'not private study');c,f=banks();flag=d/'stop.requested'
    before=lambda:need(not flag.exists() and time.time()<deadline,'deadline/stop');before();inputs=load(d/'inputs.json');need(deadline==inputs['deadline']<=CUTOFF,'deadline changed')
    need(inputs['plan']==prospective(TEN.n,c['m'],c['max_phrase']) and inputs['raw_sha256']==c['sha256'],'plan/identity drift')
    if not verify:
        need(admitted(deadline)=={**inputs,'source_stat':tuple(inputs['source_stat']),'tape_stat':tuple(inputs['tape_stat'])},'changed admission')
        raw=d/'corpus';h=hashlib.sha256();copy=Phase(d,report,'copy_cold',deadline,TEN.n+MARGIN,TEN.n+MARGIN)
        with open(SOURCE,'rb',opener=lambda p,g:os.open(p,g|os.O_NOFOLLOW)) as src,raw.open('xb') as dest:
            left=TEN.n
            while left:
                copy.guard();b=src.read(min(1<<20,left));need(b,'short corpus');h.update(b);dest.write(b);left-=len(b)
            need(not src.read(1),'long corpus')
        need(tuple(inputs['source_stat'])==identity(SOURCE) and h.hexdigest()==c['sha256'],'raw pin');raw.chmod(0o400);cold(raw);copy.guard(True);ident=identity(raw)
        report.update(raw_sha256=h.hexdigest(),private_bytes=ident[2],mincore_resident_pages=0,private_stat=ident)
        end=min(deadline,int(time.time())+COMPONENT_CAP);meter(d,[d/'census','--internal-calibrate-10g',str(end)],'census',end,report);before()
        v,dc,oc=readout(d/'census.out');plan=inputs['plan'];need(not (d/'vocab.tmp').exists() and identity(raw)==ident,'unretired bucket or changed copy')
        need(all(v[k]==c[k] for k in ('n','m','starts_above_u32','max_phrase','max_owned_stride','eof_start','eof_raw_length','global_end_rows','alphabet')) and oc==counts(c['cells']),'raw occurrence witness mismatch')
        need((v['k'],v['D'],v['U'],v['tape'])==(f['k'],f['D'],f['descriptors'],f['dictionary_bytes']),'distinct census mismatch')
        need(v['buckets']==256 and v['segment']==32<<20 and v['raw_segments']<=v['files']<=plan['created_segments_upper'] and 0<v['merge_levels']<=plan['merge_levels_upper'] and
            v['scratch_read']==v['scratch_write'] and v['peak']<=plan['logical_peak_upper'] and v['scratch_write']*2<=plan['logical_scratch_traffic_upper'] and v['peak']+RESERVE<=2*TEN.n and
            0<report['census']['process']['read_bytes'] and v['sort_compare_calls']>0,'census bounds/IO')
        report.update(summary=v,counted_peak_plus_control=v['peak']+RESERVE,distinct_digest=digest(dc),row_digest=digest(oc),out_sha256=sha(d/'census.out'),status='RAW_CENSUS_COMPARED');return
    front=load(d/'front.json');need(front['status']=='RAW_CENSUS_COMPARED' and front['out_sha256']==sha(d/'census.out') and list(identity(TAPE))==inputs['tape_stat'],'changed reference or front')
    meter(d,[d/'reference','--ten-reference',TAPE,flag,str(deadline)],'reference',deadline,report);before()
    v,dc,oc=readout(d/'census.out');result=study(v['n'],v['m'],v['k'],v['U'],v['D'],dc,oc);line=(d/'reference.out').read_text().split();need(line[0]=='VOCAB_REFERENCE','missing bank witness');r=dict(x.split('=') for x in line[1:])
    need(tuple(inputs['tape_stat'])==identity(TAPE) and r['descriptor_cells_sha256']==result['distinct_cells_sha256']==front['distinct_digest'] and oc==counts(c['cells']) and
         all(int(r[a])==v[b] for a,b in [('k','k'),('descriptors','U'),('D','D'),('tape','tape'),('max_phrase','max_phrase')]),'packed descriptor witness mismatch')
    result['lexical_codec_and_navigation']=lexical_block_model(v['n'],v['m'],v['k'],v['U'],v['max_phrase'],r)
    save(d/'model.json',result);report.update(status='VERIFIED_10G_VOCABULARY_ONLY',packed_tape_sha256=r['tape_sha256'],reference_cold_claim=False,model_sha256=sha(d/'model.json'))

if __name__=='__main__':
    action=sys.argv[1];d=Path(sys.argv[2]);end=int(sys.argv[3]);report={'status':'REFUSED','product':False}
    if action=='preflight':save(d/'inputs.json',admitted(end))
    else:
        need(action in ('census','reference'),'unknown action');p=d/('verify.json' if action=='reference' else 'front.json');need(not p.exists(),'receipt reuse')
        try:worker(d,end,report,action=='reference')
        except Exception as e:report['error']=repr(e);raise
        finally:save(p,report)
