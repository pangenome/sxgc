"""10 GB raw+inverse → ranked aligned-start navigation, 4 vs 1 workers, separate raw oracle. No row spool."""
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess as sp
import sys
import time
import overlap_front64_pos_study as pos
from overlap_scanplan_gate import Phase

old=pos.old;s=pos.s;need=s.need;load=s.load;N=old.N;M=old.M;K=old.K
POS=s.RECEIPTS/'pos64-front-10g';RAW_BANK=pos.BANK;POS_PIN='c6b8b58ee87c0bbb3ce98c079358ce3f08034d78d59f792b4a625b9459474edd'
PREFIX='raw-view64-10g-';SCRIPT='overlap_raw_view64_study.sh';RAM=20<<30;ARRAYS=14<<30;BOUND=16*M+(4<<20)

def planned(n,m,w):
    need(0<m<=n and 0<w<=64,'plan dimensions');div=lambda v,k:(v+k-1)//k
    p=max(1,div(n.bit_length(),8));r=max(1,div(m.bit_length(),8));blocks=div(m,64)
    return n+(p+r)*m+8*blocks+max(8*div(n,64),p*m+16*blocks)+(4<<20)+(w<<21)

def sources():
    need(pos.score(POS)==load(POS,'score.json') and s.sha(POS/'score.json')==POS_PIN and load(POS,'score.json')['layout_goal_met'],'position-front evidence')
    r=load(RAW_BANK,'source.json');z=load(POS,'source.json');x=Path(r['original_directory']);y=Path(z['original_directory']);need(x.parent==y.parent==s.S and old.private(x) is None and pos.private(y) is None,'unexpected source directories')
    a=load(RAW_BANK,'admission.json');rank=x/'inverse.u64';keys=y/'columns/keys.u64';expected=load(RAW_BANK,'build.json')['column_stats']['inverse.u64'];k=load(POS,'outputs.json')['keys.u64']
    need(list(s.identity(old.RAW))==a['raw_stat'] and list(s.identity(rank))==expected and list(s.identity(keys))==k and a['cores']==s.cores(),'raw, rank, oracle witness or cores changed')
    return {'raw':old.RAW,'inverse.u64':rank,'oracle-keys.u64':keys}

def window(start,end):need(start+1600<end<=start+2400,'expected a fixed 1601..2400 s view-only lease')
def private(d):need(d.resolve()==d and d.parent==s.S and re.fullmatch(PREFIX+'[A-Za-z0-9]{6}',d.name) and d.stat().st_uid==os.getuid() and not d.stat().st_mode&0o077,'private view study')

def admit(d,end):
    start=int(time.time());window(start,end);private(d);s.guard(d,end,1600,cutoff=start+2400);s.lease(os.getppid(),end,SCRIPT);s.exclusive();paths=sources();rev=s.published_main(Path.home()/'sxgc')
    need(sp.check_output(['git','-C',s.REPO,'rev-parse','HEAD'],text=True).strip()==rev and not sp.check_output(['git','-C',s.REPO,'status','--porcelain']) and set(s.S.glob(PREFIX+'*'))=={d} and not any(d.iterdir()) and not (s.RECEIPTS/'raw-view64-10g/score.json').exists(),'dirty, unpublished or repeated study')
    need(planned(N,M,4)<=ARRAYS and max(N+8*M,24*M)+(2<<20)<=s.ORACLE and BOUND<=2*N and s.memory_available()>RAM+(2<<30) and shutil.disk_usage(d).free>N+BOUND+(2<<30),'resource admission')
    s.save(d/'admission.json',dict(rev=rev,parent=os.getppid(),admitted_unix=start,deadline=end,cores=s.cores(),paths={k:str(v) for k,v in paths.items()},stats={k:s.identity(v) for k,v in paths.items()},prior_scores=[pos.PIN,POS_PIN],counted_upper=BOUND,scope='raw+inverse prepared navigation only, 4 then 1 workers. Separate verifier is the sole key witness reader. No full-text rows or next launch.'))

def worker(d,end,mode):
    private(d);p=load(d,'admission.json');window(p['admitted_unix'],p['deadline']);need(p['deadline']==end and p['admitted_unix']<=time.time(),'deadline drift');s.lease(p['parent'],end,SCRIPT);s.guard(d,end,cutoff=end);s.exclusive();raw=d/'raw';inv=d/'inverse.u64';keys=d/'oracle-keys.u64';cp=p['cores']
    if mode in ('copy','witness'):
        if mode=='witness':need(all(load(d,k+'.json')['status']=='COMPLETED' for k in ('view4','view1')),'no witness before both arms close')
        names=('raw','inverse.u64') if mode=='copy' else ('oracle-keys.u64',);pins={'raw':(N,old.RAW_SHA),'inverse.u64':(8*M,old.RANK),'oracle-keys.u64':(8*M,pos.KEYS)};size=sum(pins[k][0] for k in names);r=dict(before=s.grant(192<<20,1));ph=Phase(d,r,'private_copy_hash_cold',end,size+s.MARGIN,size+s.MARGIN)
        for k in names:
            path=Path(p['paths'][k]);size,h=pins[k];need(list(s.identity(path))==p['stats'][k] and s.copy_file(path,d/k,size,ph)==h,'wrong private payload: '+k)
        ph.guard(True);s.save(d/(mode+'.json'),dict(r,sha256={k:pins[k][1] for k in names},private_stats={k:s.identity(d/k) for k in names},mincore_resident_pages=0,after=s.grant(192<<20,1)));return
    need(load(d,'copy.json')['private_stats']=={f.name:list(s.identity(f)) for f in (raw,inv)},'private producer inputs changed')
    if mode in ('view4','view1'):
        w=int(mode[-1]);s.grant(RAM,4);need(not keys.exists() and not any(q.suffix in ('.lex64','.packed') for q in d.iterdir()),'producer must not have a dictionary or original keys');s.guard(d,end,1000 if w==4 else 700,cutoff=end)
        t0=time.monotonic();s.cold(raw);s.cold(inv);bar=time.monotonic()-t0;t=min(end,int(time.time())+(480 if w==4 else 780))
        s.measure(d,mode,[d/'view',raw,inv,w,ARRAYS,d/'stop.requested',t],t,N+8*M+s.MARGIN,s.MARGIN,cp,RAM,4,cutoff=end,ledger=lambda:old.files(d));s.save(d/(mode+'-cold.json'),dict(wall_s=bar,resident_pages=0,only_inputs=[raw.name,inv.name]));print(f'VIEW64_ARM_FINISHED workers={w}',flush=True)
    elif mode=='oracle':
        s.grant(s.ORACLE,1);need(load(d,'witness.json')['private_stats']=={keys.name:list(s.identity(keys))},'independent key witness changed')
        for f in [raw,inv,keys]:s.cold(f)
        t=min(end,int(time.time())+680);s.measure(d,mode,[d/'oracle',raw,keys,inv,s.ORACLE,d/'stop.requested',t],t,3*N+32*M+s.MARGIN,s.MARGIN,cp[:1],s.ORACLE,1,cutoff=end,ledger=lambda:old.files(d))
    else:raise ValueError(mode)

def score(d):
    p=load(d,'admission.json');window(p['admitted_unix'],p['deadline']);need(p['prior_scores']==[pos.PIN,POS_PIN] and p['counted_upper']==BOUND<=2*N and p['cores']==load(RAW_BANK,'admission.json')['cores'] and load(d,'copy.json')['sha256']=={'raw':old.RAW_SHA,'inverse.u64':old.RANK} and load(d,'witness.json')['sha256']=={'oracle-keys.u64':pos.KEYS},'identity/controls')
    o=old.fields(d,'oracle','RAW_VIEW_ORACLE');base=load(RAW_BANK,'score.json')['front'];roots={k:o[k] for k in ('raw_root','nav_root','query_root')};need(all(re.fullmatch('[0-9a-f]{64}',h) for h in roots.values()) and all(o[k]==base[k] for k in ('n','m','k','starts_above_u32','keys_above_u32')),'oracle shape');out={}
    for name in ('view4','view1','oracle'):
        oracle=name=='oracle';w=int(name[-1]) if not oracle else 1;mem=s.ORACLE if oracle else RAM;cpu=1 if oracle else 4;a=load(d,name+'.json');g=a['after'];proc=a['process']
        need(a['rc']==0 and a['status']=='COMPLETED' and a['stop'] is None and a['affinity']==p['cores'][:cpu] and a['counted_dir_sampled']<=BOUND and (g['memory.max'],g['cpu.max'],g['memory.swap.max'])==(str(mem),f'{cpu*100000} 100000','0') and 0<int(g['memory.peak'])<=mem and {'oom 0','oom_kill 0'}<=set(g['memory.events'].splitlines()),'meter or grant')
        out[name]=dict(service_s=a['wall_s'],cpu_s=proc['cpu_s'],read_bytes=proc['read_bytes'],write_bytes=proc['write_bytes'],peak=int(g['memory.peak']),sampled_rss=a['sampled_rss'],per_device_io=a['per_device_io'])
        if oracle:continue
        b=old.fields(d,name,'RAW_VIEW');v=old.fields(d,name,'VIEW_PROOF');c=load(d,name+'-cold.json');need(c['resident_pages']==0 and c['only_inputs']==['raw','inverse.u64'] and all(v[k]==h for k,h in roots.items()) and (int(b['n']),int(b['m']),int(b['k']),int(b['owned']),int(b['tape']),int(b['team']),int(b['arrays']),int(b['pos_bytes']),int(b['rank_bytes']))==(N,M,K,old.U,old.D64,w,planned(N,M,w),5,4),'incorrect roots, width or geometry')
        need(int(v['rows'])==M and v['keys_above_u32']==o['keys_above_u32'] and b['starts_above_u32']==o['starts_above_u32'] and b['max_lcp']==o['max_lcp'] and int(b['extended'])+int(b['carried'])==int(o['sum_lcp']) and int(b['extended'])<=N*(w+1)+8*M,'bad carry proof');out[name].update({k:float(b[k]) for k in ('scan_s','scatter_s','groups_s','plcp_s','rmq_s','ready_s','ready_cpu_s')});out[name].update(proof_s=float(v['proof_s']),proof_cpu_s=float(v['proof_cpu_s']),extended=int(b['extended']),carried=int(b['carried']),cold_barrier_s=c['wall_s']);need(0<out[name]['ready_s']<a['wall_s'] and math.isfinite(out[name]['proof_s']) and proc['read_bytes']>0,'invalid service clock')
        stripes=[dict(kv.split('=',1) for kv in line.split()[1:]) for line in (d/(name+'.log')).read_text().splitlines() if line.startswith('VIEW_STRIPE ')]
        times=[float(s['plcp_s']) for s in stripes];need([int(s['lane']) for s in stripes]==list(range(w)) and sum(int(s['rows']) for s in stripes)==M and sum(int(s['extended']) for s in stripes)==int(b['extended']) and all(0<=t<=out[name]['plcp_s']+.000002 for t in times),'stripe coverage/clocks');out[name]['stripe_plcp_s']=times
    a=out['view4'];b=out['view1'];rat={k:a[k]/b[k] for k in ('ready_s','service_s','cpu_s','read_bytes','scan_s','scatter_s','groups_s','plcp_s','rmq_s','proof_s')};met=rat['ready_s']<=.80 and rat['service_s']<=.90 and rat['read_bytes']<=1.05 and max(a['write_bytes'],b['write_bytes'])<=2<<20
    return dict(verified_navigation=True,parallel_goal_met=met,arms=out,four_over_one=rat,roots=roots,sum_aligned_byte_lcp=int(o['sum_lcp']),max_aligned_byte_lcp=int(o['max_lcp']),array_bound_10gb=planned(N,M,4),modeled_bound_100gb=planned(100_000_000_000,3181334488,4),counted_upper=BOUND,scope='10 GB raw + previously certified inverse -> resident parse-start view, then full ordered-field attestation. Not raw-only, full byte suffix stream, product or 100 GB measurement.')

if __name__=='__main__':
    need(__debug__,'assertions required');mode=sys.argv[1];d=Path(sys.argv[2])
    if mode=='score':print(json.dumps(score(d),sort_keys=True,indent=2))
    elif mode=='admit':admit(d,int(sys.argv[3]))
    else:worker(d,int(sys.argv[3]),mode)
