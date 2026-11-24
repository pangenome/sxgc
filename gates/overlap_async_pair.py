"""Optional fixed 1 GB raw-product A/B of the audit tee. No historical re-selection or next-rung dispatch."""
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

from overlap_defaults import BANK,decision
from overlap_published import published_main
from overlap_radix_product_score import metrics
from overlap_resident_gate import ASYNC_RESERVE,AUDIT_PROFILE,CG,ONE,REPO,ROOT,cg_info,save,sha
from overlap_scanplan_gate import Phase,MARGIN
from overlap_scanplan_hundred import CUTOFF,DEFAULT,ONE_DEFAULT,verify_default
from overlap_segmented_gate import ONE_RAW_SHA,cold,identity,io_delta,memory_available,need,stat_io
from overlap_three_score import binaries
from overlap_timing_idle import exclusive

S=ROOT/'sandbox';REF=BANK/'radix-full-1g/batch';N=ONE.n;SOURCE=ROOT/'climb/rung1/pile-slice1g.bin';RANKS=S/'parse-ranks-Ys27jk/work/inverse.u32';REMAP=ROOT/'climb/rung1/remap-256.bin';STEPS=('sync','async')
FILES={'in/corpus':(SOURCE,N),'oracle-corpus':(SOURCE,N),'in/remap':(REMAP,256),'inverse.check':(RANKS,127136104)}
NATIVE=('monitor.so','oracle','overlap_resident','pfp_sort_r','lexparse','bitmap.so')

def load(d,name):return json.loads((d/name).read_bytes())
def ensure_time(end,baseline,remaining):
    needed=math.ceil(remaining*(1.7*baseline+120)+90);need(0<remaining<=2 and baseline>0 and time.time()+needed<=end<=CUTOFF,'outside window/insufficient pair headroom');return needed

def published(end,remaining=2):
    rev=published_main(Path.home()/'sxgc');need(sp.check_output(['git','-C',REPO,'rev-parse','HEAD'],text=True).strip()==rev and not sp.check_output(['git','-C',REPO,'status','--porcelain']),'unpublished/dirty source');exclusive()
    docs=(REPO/'OVERLAP_DEFAULTS.md').read_text();dec={}
    for rung,path in [('1g',ONE_DEFAULT),('10g',DEFAULT)]:
        dec[rung]=decision(rung);verify_default((REPO/path).read_bytes(),docs,dec[rung],rung)
    need(dec['1g']['profile']==AUDIT_PROFILE and memory_available()>=(24<<30) and shutil.disk_usage(S).free>=(9*N+(2<<30) if remaining==2 else 6*N+(1<<30)),'profile or host headroom');sec=dec['1g']['raw_s'];return dict(rev=rev,expected_profile=AUDIT_PROFILE,source=str(SOURCE),deadline=end,baseline_raw_s=sec,required_s=ensure_time(end,sec,remaining),decisions={k:v['profile'] for k,v in dec.items()})

def private(d):
    need(d.parent==S and d==d.resolve() and re.fullmatch('async-pair-1g-[a-zA-Z0-9]{6}',d.name) and d.stat().st_uid==os.getuid() and d.stat().st_mode&0o077==0,'not private fixed pair')

def grant():
    cg=cg_info();need(cg['memory.max']==str(192<<20) and cg['memory.swap.max']=='0' and cg['cpu.max']=='100000 100000' and (CG/'cgroup.procs').read_text().split()==[str(os.getpid())],'prep requires dedicated 192 MiB, one CPU, zero swap');return cg

def prep_proof(d,arm,rev):
    v=load(d,'prep.json');p=v['private_stat'];o=v['source_stat'];dig=v['sha256'];need(v['status']=='PRIVATE_COLD' and v['arm']==arm and v['revision']==rev and re.fullmatch(r'[a-f0-9]{40}',rev),'private input attestation')
    need(set(dig)==set(p)==set(o)==set(FILES) and all(re.fullmatch('[a-f0-9]{64}',s) for s in dig.values()) and dig['in/corpus']==dig['oracle-corpus']==ONE_RAW_SHA and dig['in/remap']==binaries(REF)['remap'],'raw/remap/column identity')
    for name,(_,n) in FILES.items():
        for ident in (o[name],p[name]):need(len(ident)==5 and all(type(x) is int for x in ident) and ident[2]==n,'input stat')
    need(o['in/corpus']==o['oracle-corpus'] and len({tuple(x[:2]) for x in p.values()})==4 and not ({tuple(x[:2]) for x in p.values()} & {tuple(x[:2]) for x in o.values()}),'private inode alias')
    page=os.sysconf('SC_PAGE_SIZE');need(v['mincore_resident_pages']==0 and v['mincore_checked_pages']=={f:(n+page-1)//page for f,(_,n) in FILES.items()},'incomplete mincore')
    for cg in (v['cgroup_before'],v['cgroup_after']):need(cg['memory.max']==str(192<<20) and 0<int(cg['memory.peak'])<=192<<20 and cg['cpu.max']=='100000 100000' and cg['memory.swap.max']=='0' and {'oom 0','oom_kill 0'}<=set(cg['memory.events'].splitlines()),'cold phase grant/OOM')
    ph=v['private_copy_and_cold'];cap=3*N+MARGIN;need(ph['read_cap']==ph['write_cap']==cap and math.isfinite(ph['wall_s']) and ph['wall_s']>0 and sum(n for _,n in FILES.values())<=ph['worker_io']['write_bytes']<=cap and 0<=ph['worker_io']['read_bytes']<=cap,'private copy accounting')
    need(ph['per_device_io'] is None or bool(ph['per_device_io']),'missing device observation is null');
    for dev,counts in (ph['per_device_io'] or {}).items():need(re.fullmatch(r'\d+:\d+',dev) and all(0<=counts[k]<=cap for k in ('rbytes','wbytes')),'copy IO cap')
    return v

def validate_arm(d,t,arm,end):
    private(d.parent);p=load(d.parent,'admission.json');need(arm in STEPS and end==p['deadline'] and time.time()<end<=CUTOFF and d==d.resolve() and d.name==arm and t==d.parent/'tools' and not (d/'stop.requested').exists(),'unexpected audit pair worker')
    prep=prep_proof(d,arm,p['rev']);need(all(tuple(ident)==identity(d/name) and stat.S_ISREG((d/name).lstat().st_mode) and stat.S_IMODE((d/name).stat().st_mode)==0o400 for name,ident in prep['private_stat'].items()),'private inode changed after cold')

def prepare(d,t,arm,end):
    private(d.parent);need(d.name==arm in STEPS and t==d.parent/'tools' and not d.exists(),'fresh pair arm required');p=load(d.parent,'admission.json');need(p['deadline']==end,'deadline drift')
    remaining=2-STEPS.index(arm);cg=grant();now=published(end,remaining);need(all(v==p[k] for k,v in now.items() if k!='required_s'),'changed source/defaults before preparation')
    old=binaries(REF);files=[t/k for k in NATIVE]+[ONE.finish];new={f.name:sha(f) for f in files};need(set(new)==set(old)-{'remap'} and all(v==old[k] for k,v in new.items() if k not in ('overlap_resident','pfp_sort_r')),'frozen native tool drift');ensure_time(end,p['baseline_raw_s'],remaining)
    d.mkdir(mode=0o700);(d/'in').mkdir();originals={};copies={};hashes={};r=dict(status='REFUSED',revision=p['rev'],arm=arm,cgroup_before=cg);before=lambda:need(time.time()<end and not (d/'stop.requested').exists(),'time/stop')
    try:
        # Four isolated files, checked at the same preparation boundary on each arm. No cache eviction of originals.
        phase=Phase(d,r,'private_copy_and_cold',end,3*N+MARGIN,3*N+MARGIN)
        for name,(src,n) in FILES.items():
            ident=identity(src);need(src.resolve()==src and stat.S_ISREG(src.stat().st_mode) and ident[2]==n,'wrong input');originals[name]=ident;h=hashlib.sha256();dest=d/name
            with open(src,'rb',opener=lambda p,f:os.open(p,f|os.O_NOFOLLOW)) as f,dest.open('xb') as w:
                left=n
                while left:
                    phase.guard();data=f.read(min(left,1<<20));need(data,'short raw');w.write(data);h.update(data);left-=len(data)
                need(not f.read(1) and identity(src)==ident,'source changed')
            hashes[name]=h.hexdigest();dest.chmod(0o400);cold(dest);copies[name]=identity(dest)
        phase.guard(True);need(len(set(x[:2] for x in copies.values()))==4 and all(x[:2] not in [v[:2] for v in originals.values()] for x in copies.values()),'shared private file')
        need(hashes['in/corpus']==hashes['oracle-corpus']==ONE_RAW_SHA and hashes['in/remap']==old['remap'],'raw/remap pin');before()
        if arm=='async':
            a=load(d.parent/'sync','prep.json');need(a['sha256']==hashes and a['source_stat']=={k:list(v) for k,v in originals.items()},'pair inputs changed')
        (d/'binaries.sha').write_text(p['rev']+'\n'+''.join(f'{new[f.name]}  {f}\n' for f in files)+hashes['in/remap']+'  '+str(d/'in/remap')+'\n')
        (d/'cold.log').write_text('all four private inputs: fsync plus mincore zero resident pages\n');r.update(status='PRIVATE_COLD',mincore_resident_pages=0,mincore_checked_pages={k:math.ceil(n/os.sysconf('SC_PAGE_SIZE')) for k,(_,n) in FILES.items()},sha256=hashes,private_stat=copies,source_stat=originals,cgroup_after=grant());ensure_time(end,p['baseline_raw_s'],remaining);before()
    except BaseException as e:r.update(status='REFUSED',error=repr(e));raise
    finally:save(d/'prep.json',r)

def measurements(d,arm):
    b,f,i,core,wall=metrics(d);need(b['profile']==AUDIT_PROFILE and b['audit_pair_arm']==arm and b['verifier_transport']=='dedicated inherited FIFO' and not (d/'stop.requested').exists(),'wrong tee arm')
    rev=(d/'binaries.sha').read_text().splitlines()[0];p=prep_proof(d,arm,rev);tools=binaries(d);old=binaries(REF);need(set(tools)==set(old) and all(re.fullmatch(r'[a-f0-9]{64}',v) and (k in ('overlap_resident','pfp_sort_r') or old[k]==v) for k,v in tools.items()),'frozen tools')
    g=load(d,'scratch-plan.json');need(g==load(REF,'scratch-plan.json') and b['raw_counted_highwater']==load(REF,'build.json')['raw_counted_highwater'] and b['raw_counted_highwater']<=2*N,'different scratch contract')
    ext=load(REF,'front-end.json')['array_bounds'].copy();reserve=ASYNC_RESERVE if arm=='async' else 0;ext['emit']+=reserve
    ref=load(REF,'front-end.json');need(all(f[k]==ref[k] for k in ('columns','n','m','k','D','descriptors','counted_front_peak')),'packed input census')
    need(f['array_bounds']==ext and b['tee_reserve_array_bytes']==reserve and i==load(REF,'identity.json') and core==metrics(REF)[3],'resource, identity or row mismatch');need((d/'exits.log').read_text().strip()=='build=0 oracle=0','service exits')
    base=dict(SS_LCP_SLICE_WINDOW='64',CROSS_V2_PIECES='1',CROSS_V2_PIECE_WINDOW='32',PFCK_SPACING='22',XSA_RESIDENT_RADIX_LANES='4',XSA_RESIDENT_RADIX_STEP='16384')
    res=dict(base,XSA_RESIDENT_PACKED='1',XSA_RESIDENT_WIDE_DICT='1');sink=dict(base,XSA_TEST_RESIDENT_TRIPLES='1',XSA_BATCH_SXLS_ESCAPES='1');
    if arm=='async':res['XSA_RESIDENT_ASYNC_AUDIT']='1'
    need(b['worker_controls']==dict(resident=res,sink=sink,finish=base) and b['sxls_markers']['exact_final']==2*((N+1+262143)//262144) and not b['sxls_markers']['reclaim'],'actual allowlisted settings/marker mismatch')
    log=(d/'resident.log').read_text();need('RESIDENT_DICT_SORT kernel=wide_via_narrow wide=true selected_threads=1 ' in log,'wrong dictionary sort');lines=[x for x in log.splitlines() if x.startswith('RESIDENT_ASYNC_AUDIT ')]
    need(len(lines)==int(arm=='async'),'async code unexpectedly active/inactive');tee={}
    if lines:
        v=dict(s.split('=') for s in lines[0].split()[1:]);tee={k:(float(x) if k.endswith('_s') else int(x)) for k,x in v.items()}
        need(tee['data_bytes']==12*N and tee['sink_bytes']==tee['audit_bytes']==12*(N+1) and 1<=tee['peak_inflight_blocks']<=8 and all(math.isfinite(v) and v>=0 for v in tee.values()),'tee byte/credit accounting')
    cpu=lambda cg:int(dict(s.split() for s in cg['cpu.stat'].splitlines())['usage_usec'])/1e6
    o=load(d,'oracle.json');c=re.fullmatch(r'cert_process_wall_s=([.\d]+) cpu_user_s=([.\d]+) cpu_system_s=([.\d]+)\n',(d/'certify.log').read_text());need(c and float(c[1])>0,'separate certificate clock')
    prepared={k:sum(x[k] for x in b['process'].values()) for k in ('read_bytes','write_bytes')};need(all(0<prepared[k] and prepared[k]+f['process'][k]==b['raw_worker_io'][k] for k in prepared),'worker IO ledger')
    need(0<wall['emit']<=wall['prepared'] and wall['raw']+.001>=wall['prepared']+f['wall_s']+f['cold_barrier_s'] and len(b['cpu_affinity'])>=4 and b['packed_tape_retired'] is True,'product wall/grant')
    need(all('oom_kill 0' in cg['memory.events'].splitlines() for cg in (b['shared_after'],o['cgroup'])),'OOM event')
    return dict(arm=arm,wall=wall,tee=tee,build_cpu_s=cpu(b['shared_after'])-cpu(b['shared_before']),peak_shared=int(b['shared_after']['memory.peak']),
        cpu_by_worker_s={k:v['cpu_s'] for k,v in b['process'].items()},queue_reserved=g['parts']['row_queue'],raw_counted=b['raw_counted_highwater'],
        prepared_counted=b['counted_sampled_peak'],prepared_reserved=g['counted_bound'],whole_private_allocated=b['raw_dir_sampled_allocated'],
        private_copy_cold=p['private_copy_and_cold'],raw_io=b['raw_worker_io'],prepared_io=prepared,
        raw_device_io=io_delta(stat_io(b['shared_before']['io.stat']),stat_io(b['shared_after']['io.stat'])),
        prepared_device_io=io_delta(stat_io(b['prepared_cgroup_before']['io.stat']),stat_io(b['shared_after']['io.stat'])),oracle_s=o['wall_including_wait_s'],
        oracle_cpu_s=o['process']['cpu_s'],oracle_io={k:o['process'][k] for k in ('read_bytes','write_bytes')},certificate_wall_s=float(c[1]))

def score(d):
    a,b=[d/arm for arm in STEPS];va,vb=[measurements(x,x.name) for x in (a,b)];need(binaries(a)==binaries(b) and (a/'binaries.sha').read_text().splitlines()[0]==(b/'binaries.sha').read_text().splitlines()[0] and
        all(load(a,'prep.json')[k]==load(b,'prep.json')[k] for k in ('sha256','source_stat')) and not ({tuple(x[:2]) for x in load(a,'prep.json')['private_stat'].values()} & {tuple(x[:2]) for x in load(b,'prep.json')['private_stat'].values()}) and load(a,'build.json')['cpu_affinity']==load(b,'build.json')['cpu_affinity'],'unmatched pair')
    ratios={k:vb['wall'][k]/va['wall'][k] for k in ('raw','prepared','emit')};io={k:vb['prepared_io'][k]/va['prepared_io'][k] for k in va['prepared_io']};cpu=vb['build_cpu_s']/va['build_cpu_s']
    ok=max(ratios[k] for k in ('raw','prepared'))<=.95 and max(io.values())<=1.05 and cpu<=1.05
    return dict(sync=va,async_audit=vb,time_ratio=ratios,post_front_io_ratio=io,build_cpu_ratio=cpu,verified_full_products=True,matched_product_speed_io_cpu_pass=ok,
                scope='standalone fixed 1 GB A/B only. Async audit_write_s covers END; worker times overlap. No change to the banked rung policy, no 10 GB or 100 GB projection.')

if __name__=='__main__':
    need(__debug__,'assertions required');mode=sys.argv[1];d=Path(sys.argv[2]);
    if mode=='admit':private(d);need(not any(d.iterdir()),'not a fresh study');save(d/'admission.json',published(int(sys.argv[3])))
    elif mode=='prepare':prepare(d,Path(sys.argv[3]),sys.argv[4],int(sys.argv[5]))
    elif mode=='single':print('AUDIT_VERIFIED',json.dumps(measurements(d,d.name)))
    elif mode=='score':v=score(d);save(d/'comparison.json',v);print('AUDIT_PAIR',json.dumps(v))
    else:raise ValueError(mode)
