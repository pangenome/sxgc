"""One new 4-lane 100 MB raw-row candidate: direct balanced EF ranges vs frozen coarse/replay 4-lane arm."""
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess as sp
import sys
import time
import overlap_finite_emit64_study as old
from overlap_scanplan_gate import Phase

s=old.s;need=s.need;load=s.load;N,M,U=old.N,old.M,old.U;PREFIX='finite-direct64-100m-';SCRIPT='overlap_finite_direct64_study.sh';BANK=old.OUT;OUT=s.RECEIPTS/'finite-direct64-100m';PIN='45ceab98f853e3194280fb24a2fd7a9cdb5df8f2b6d47601251e233018f6c861';RANK='afc938c0a1e96a62a2a6159fa119546a919b3042f8820847c207890bc9cafe67';CHECKER='1d0f8261980a6932c39b91bfa7e0612bc914baa08cc2d19df2c52f5d90a9165e'
TARGET=old.SLOTS//5;BANDS=old.ceil(U,TARGET);FILE_UP=min(old.ceil(U*(l+1)+BANDS*((old.V-1)>>l),8) for l in range(64))+16*4*BANDS;BOUND=old.OTHER+FILE_UP

def prior():
    a=old.score(BANK);need(a==load(BANK,'score.json') and s.sha(BANK/'score.json')==PIN and not a['parallel_goal_met'] and a['arms']['4']['phases']['decoded']==1379799143 and load(BANK,'setup.json')['rank_sha256']==RANK and load(BANK,'tools.json')['oracle']==CHECKER,'different predecessor observation');return a['arms']['4']
def sources():
    prior();z=load(BANK,'source.json');d=Path(z['original_directory']);old.private(d);p=load(BANK,'setup.json');need(z['rc']==0 and z['revision']=='1563640503aac0fd2636170567a7b225c916c259' and z['artifacts']['score.json']==PIN,'wrong prior launch');raw=p['source_hashes']['raw'];paths={'raw':(d/'raw',N,raw),'oracle-raw':(d/'oracle-raw',N,raw),'inverse.u64':(d/'inverse.u64',8*M,RANK)}
    need(all(f.resolve()==f and list(s.identity(f))==p['private_stats'][k] and f.stat().st_size==n for k,(f,n,h) in paths.items()),'banked prepared file changed');return paths
def window(start,end):need(600<end-start<=1000,'fixed 601..1000 s single 100 MB arm')
def private(d):need(d.resolve()==d and d.parent==s.S and re.fullmatch(PREFIX+'[A-Za-z0-9]{6}',d.name) and d.stat().st_uid==os.getuid() and not d.stat().st_mode&0o077,'private candidate receipt')
def admit(d,end):
    start=int(time.time());window(start,end);private(d);s.lease(os.getppid(),end,SCRIPT);s.exclusive();old.guard(d,end,600);rev=s.published_main(Path.home()/'sxgc');paths=sources();p=load(BANK,'admission.json');need(old.cpus()==p['cores'] and BOUND==199532024<=2*N and s.memory_available()>3<<30 and shutil.disk_usage(d).free>2<<30,'grant/scratch headroom')
    need(set(s.S.glob(PREFIX+'*'))=={d} and not any(d.iterdir()) and not OUT.exists() and sp.check_output(['git','-C',s.REPO,'rev-parse','HEAD'],text=True).strip()==rev and not sp.check_output(['git','-C',s.REPO,'status','--porcelain']),'repeat or unpublished source');s.save(d/'admission.json',dict(rev=rev,parent=os.getppid(),start_unix=start,deadline=end,cores=p['cores'],source_stats={k:s.identity(f) for k,(f,n,h) in paths.items()},hashes={k:h for k,(f,n,h) in paths.items()},prior=PIN,counted_upper=BOUND,slots=old.SLOTS,desired_descriptors=TARGET,scope='one 4-worker direct candidate vs frozen 4-worker coarse replay only; same slot/grant/EOF checker. No rank/front/product rerun.'))
def authority(d,end):
    private(d);p=load(d,'admission.json');window(p['start_unix'],end);need(p['deadline']==end and time.time()>=p['start_unix'],'changed window');s.lease(p['parent'],end,SCRIPT);old.guard(d,end);return p
def tools(d):r=load(d,'tools.json');need(set(r)=={'emit','oracle'} and r['oracle']==CHECKER and all(s.sha(d/k)==h for k,h in r.items()),'binary drift')
def setup(d,end):
    p=authority(d,end);tools(d);s.exclusive();r=dict(before=s.grant(192<<20,1));ph=Phase(d,r,'private_authenticated_copy_cold',end,3*N,3*N);paths=sources()
    for k,(f,n,h) in paths.items():need(list(s.identity(f))==p['source_stats'][k] and s.copy_file(f,d/k,n,ph)==h,'prepared source changed: '+k)
    ph.guard(True);r.update(after=s.grant(192<<20,1),private_stats={k:s.identity(d/k) for k in paths},hashes=p['hashes'],resident_pages=0);s.save(d/'setup.json',r)
def inputs(d):
    x=load(d,'setup.json');need(x['private_stats']=={k:list(s.identity(d/k)) for k in ('raw','inverse.u64','oracle-raw')} and len({(d/k).stat().st_ino for k in x['private_stats']})==3 and not (d/'parse.u32').exists(),'private file drift')
def pair(d,end):
    authority(d,end);tools(d);inputs(d);s.exclusive();old.guard(d,end,420);need(not (d/'pair4.json').exists(),'do not repeat');t=time.monotonic()
    for k in ('raw','oracle-raw','inverse.u64'):s.cold(d/k)
    until=min(end-5,int(time.time())+405);s.save(d/'cold4.json',dict(wall_s=time.monotonic()-t,resident_pages=0,inputs=['raw','inverse.u64','oracle-raw'],arm_deadline=until));script=Path(__file__).resolve();old.pipeline(d,4,until,old.unit(d,end,4,'emit',until,script),old.unit(d,end,4,'oracle',until,script));old.checked_pair(d,4);print(f'DIRECT64_BYTE_ROWS_VERIFIED rows={N+1}',flush=True)
def meter(d,end,w,role,until):
    p=authority(d,end);tools(d);inputs(d);stop=d/'stop.requested';need(w==4 and role in ('emit','oracle') and load(d,'cold4.json')['arm_deadline']==until<=end and not (d/f'{role}4.json').exists(),'wrong candidate arm');cmd=[d/'emit','sampled',d/'raw',d/'inverse.u64',d/'route4.ef',w,old.ARRAYS,old.OTHER,TARGET,old.SLOTS,stop,until] if role=='emit' else [d/'oracle','--component-100m',d/'oracle-raw',stop];old.native(d,w,role,until,p['cores'],cmd,BOUND)
def fields(d,tag):
    line,=[l for l in (d/'emit4.log').read_text().splitlines() if l.startswith(tag+' ')];return {k:float(v) if k.endswith('_s') else int(v) for k,v in (kv.split('=',1) for kv in line.split()[1:])}
def score(d):
    base=prior();p=load(d,'admission.json');window(p['start_unix'],p['deadline']);q=old.checked_pair(d,4);c=load(d,'cold4.json');z=load(d,'setup.json');need(p['prior']==PIN and p['counted_upper']==BOUND<=2*N and p['slots']==old.SLOTS and p['desired_descriptors']==TARGET and p['cores']==load(BANK,'admission.json')['cores'] and q['pipe_bytes']==base['pipe_bytes'] and c['resident_pages']==z['resident_pages']==0 and c['inputs']==['raw','inverse.u64','oracle-raw'] and z['hashes']==p['hashes']=={'raw':load(BANK,'setup.json')['source_hashes']['raw'],'oracle-raw':load(BANK,'setup.json')['source_hashes']['raw'],'inverse.u64':RANK} and load(d,'tools.json')['oracle']==CHECKER,'different profile or inputs')
    a=fields(d,'FINITE_EMIT');f=fields(d,'DIRECT_RANGES');prev=base['phases'];need(all(a[k]==prev[k] for k in ('rows','m','k','n','team','multi','max_tie','max_mass','descriptors','groups','bitmap_groups','row_bytes')) and a['counted']==old.OTHER+a['wire']<=BOUND and a['arrays']<=old.ARRAYS and 0<a['populated']<=f['explicit_write']<=a['wire']<=f['guaranteed_wire_upper']==FILE_UP and f['samples']==64*f['target_bands']==64*BANDS and 0<a['bands']==f['bands']<=BANDS and 0<f['max_range_descriptors']<=f['max_range_rows']<=N and U<=a['decoded']<=16*U and a['leaves']>=a['bands'] and all(math.isfinite(v) and v>=0 for v in list(a.values())+list(f.values())),'range counts, file proof or canonical rows changed')
    arm={};wall=q['prepared_to_verified_EOF_s'];need(wall>0 and a['order_wall_s']>0 and abs(a['order_lane_s']/a['order_wall_s'])<=4.0001,'invalid clocks')
    for role in ('emit','oracle'):
        r=load(d,role+'4.json');prod=role=='emit';mem,cpu=(old.RAM,4) if prod else (old.ORACLE,1);g=r['after'];native=r['process'];caps=[12*N,BOUND+(8<<20)] if prod else [2*N,4<<20];need(r['status']=='COMPLETED' and r['rc']==0 and r['stop'] is None and r['affinity']==(p['cores'][:4] if prod else p['cores'][4:]) and (g['memory.max'],g['cpu.max'],g['memory.swap.max'])==(str(mem),f'{cpu*100000} 100000','0') and 0<int(g['memory.peak'])<=mem and {'oom 0','oom_kill 0'}<=set(g['memory.events'].splitlines()) and r['counted_sampled']<=BOUND and r['io_caps']==caps and 0<native['read_bytes']<=caps[0] and native['write_bytes']<=caps[1],'meter or grant')
        arm[role]=dict(wall_s=r['wall_s'],cpu_s=native['cpu_s'],read_bytes=native['read_bytes'],write_bytes=native['write_bytes'],peak=int(g['memory.peak']),sampled_rss=r['sampled_rss'],per_device_io=r['per_device_io'],sampled_counted=r['counted_sampled'],whole_allocated_including_raw=r['allocated_including_raw_copies'])
    ratio={k:arm['emit'][k]/base['emit'][k] for k in ('cpu_s','read_bytes','write_bytes')};ratio['wall']=wall/base['prepared_to_verified_EOF_s'];occ=a['order_lane_s']/a['order_wall_s'];routing=a['decoded']<=2*U and occ>=2.5;goal=routing and ratio['wall']<=.50 and max(ratio['read_bytes'],ratio['write_bytes'])<=1.05
    return dict(verified_rows=N+1,narrow_sha256=old.old.DIGEST,prepared_to_verified_EOF_s=wall,direct_vs_archived_four=ratio,routing_hypothesis=routing,order_active_lanes=occ,component_goal_met=goal,phases=a,direct=f,meters=arm,setup_s=z['private_authenticated_copy_cold']['wall_s'],cold_barrier_s=c['wall_s'],scope='One new 100 MB prepared raw+inverse -> independent byte EOF. Archived four-worker comparison. Not a raw or product clock or scale authorization.')

if __name__=='__main__':
    need(__debug__,'assertions required');mode=sys.argv[1];d=Path(sys.argv[2])
    if mode=='score':print(json.dumps(score(d),sort_keys=True,indent=2))
    elif mode=='admit':admit(d,int(sys.argv[3]))
    elif mode=='setup':setup(d,int(sys.argv[3]))
    elif mode=='pair':pair(d,int(sys.argv[3]))
    elif mode=='meter':meter(d,int(sys.argv[3]),int(sys.argv[4]),sys.argv[5],int(sys.argv[6]))
    else:raise ValueError(mode)
