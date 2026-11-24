"""One 10 GB low-reservation raw→canonical-column challenge; no rank/old-front repeat or scale dispatch."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess as sp
import sys
import time
import overlap_front64_study as old
from overlap_front64_balance import balance
from overlap_scanplan_gate import Phase

s=old.s; need=s.need; load=s.load; N,M,K=old.N,old.M,old.K
PREFIX='pos64-front-10g-'; SCRIPT='overlap_front64_pos_study.sh'
BANK=s.RECEIPTS/'raw64-p1-10g'; PIN='518e22ba6052a15607e0b3961448881d67a3946cfccb51c7860f8066d9ccf25c'
DICT='5d8008a9d84aa9113a62a84ec92abc045409d4ae5f1d71860cbc51d0de112e07'; KEYS='fb6e41baeae24a0d0a253a1e647262229d5f5ba14c27bccdd1b2be60e44a2274'
BOUND=old.D64+8*M+(4<<20)

def arrays(n,m,t):
    div=lambda a,b:(a+b-1)//b
    return max(n,8*m)+8*div(n,64)+8*(div(n,512)+1)+8*m+8*div(m,64)+(4<<20)+(t<<21)

def baseline():
    r=old.score(BANK); src=load(BANK,'source.json'); v=load(BANK,'identity.json')['columns']
    need(s.sha(BANK/'score.json')==PIN and r==load(BANK,'score.json') and r['verified_intermediate'] and
         src['revision']=='f060b101d40b18a99f2dfdff4c8ef3ebf96b23a4' and src['rc']==0 and
         all(s.sha(BANK/k)==h for k,h in src['artifacts'].items()) and
         v['lex64_sha256']==DICT and v['keys_sha256']==KEYS and r['rank_sha256']==old.RANK,'unverified control')
    return r

def private(d):
    need(d.resolve()==d and d.parent==s.S and re.fullmatch(PREFIX+'[A-Za-z0-9]{6}',d.name) and
         d.stat().st_uid==os.getuid() and not d.stat().st_mode&0o077,'private position study')

def admit(d,end):
    start=int(time.time()); old.window(start,end); private(d); s.guard(d,end,900,cutoff=start+old.MAX_WINDOW)
    s.lease(os.getppid(),end,SCRIPT);s.exclusive();p=load(BANK,'admission.json');baseline()
    rev=s.published_main(Path.home()/'sxgc');need(sp.check_output(['git','-C',s.REPO,'rev-parse','HEAD'],text=True).strip()==rev and not sp.check_output(['git','-C',s.REPO,'status','--porcelain']),'unpublished or dirty source')
    need(set(s.S.glob(PREFIX+'*'))=={d} and not any(d.iterdir()) and not (s.RECEIPTS/'pos64-front-10g/score.json').exists(),'prior challenge')
    need(list(s.identity(old.RAW))==p['raw_stat'] and {k:list(s.identity(s.SRC/k)) for k in s.IN}==p['reference_stats'] and s.cores()==p['cores'],'source/core drift')
    need(s.memory_available()>=old.RAM+(2<<30) and shutil.disk_usage(d).free>=N+2*N+(2<<30) and BOUND<=2*N and arrays(N,M,4)<=14<<30,'capacity guard')
    s.save(d/'admission.json',dict(rev=rev,deadline=end,admitted_unix=start,parent=os.getppid(),cores=p['cores'],raw_stat=p['raw_stat'],reference_stats=p['reference_stats'],prior_score=PIN,counted_upper=BOUND,scope='single 10 GB position-front raw→wide columns; no rank rerun or 100 GB dispatch'))

def worker(d,end,mode):
    private(d);p=load(d,'admission.json');old.window(p['admitted_unix'],p['deadline']);need(p['deadline']==end and p['admitted_unix']<=time.time(),'deadline drift');s.lease(p['parent'],end,SCRIPT);s.guard(d,end,cutoff=end);s.exclusive()
    raw=d/'raw';out=d/'columns';tape=out/'dictionary.lex64';keys=out/'keys.u64';cp=p['cores']
    def native(label,argv,limit,r,w,mem,workers):
        t=min(end,int(time.time())+limit)
        s.measure(d,label,[*argv,d/'stop.requested',t],t,r+s.MARGIN,w+s.MARGIN,cp[:workers],mem,workers,ledger=lambda:old.files(d),cutoff=end)
    if mode=='copy':
        r=dict(before=s.grant(192<<20,1));ph=Phase(d,r,'copy_hash_cold',end,N+s.MARGIN,N+s.MARGIN)
        need(list(s.identity(old.RAW))==p['raw_stat'] and s.copy_file(old.RAW,raw,N,ph)==old.RAW_SHA,'raw identity');ph.guard(True)
        s.save(d/'copy.json',dict(r,raw_sha256=old.RAW_SHA,private_stat=s.identity(raw),mincore_resident_pages=0,after=s.grant(192<<20,1)))
    elif mode=='build':
        s.guard(d,end,800,cutoff=end);need(list(s.identity(raw))==load(d,'copy.json')['private_stat'],'changed input')
        native('front',[d/'front','pos',raw,out,4,old.ARRAYS,2*N-8*M-(4<<20)],420,4*N,old.D64+8*M,old.RAM,4)
        need(tape.stat().st_size==old.D64 and keys.stat().st_size==8*M,'incorrect column extent')
        for f in (tape,keys):f.chmod(0o400)
        s.save(d/'outputs.json',{f.name:s.identity(f) for f in (tape,keys)})
    elif mode=='verify':
        s.grant(s.ORACLE,1);need(load(d,'outputs.json')=={f.name:list(s.identity(f)) for f in (tape,keys)} and p['reference_stats']=={k:list(s.identity(s.SRC/k)) for k in s.IN},'changed output/reference')
        s.cold(tape);s.cold(keys);native('columns-check',[d/'front-verify',tape,keys,s.SRC/'dictionary.packed',s.SRC/'parse.u32',K,s.ORACLE],220,old.D64+s.T+12*M,0,s.ORACLE,1)
        v=old.fields(d,'columns-check','FRONT64_VERIFIED');need(v==load(BANK,'identity.json')['columns'],'not byte-identical verified columns')
        s.save(d/'identity.json',dict(columns=v,all_records_and_occurrences=True,private_column_cold_resident_pages=0))
    else:raise ValueError(mode)

def score(d):
    base=baseline();z=load(d,'front.json');control=load(BANK,'front.json');oracle=load(d,'columns-check.json');f=old.fields(d,'front','FRONT64');geom=old.fields(d,'front','FRONT64_LAYOUT');p=load(d,'admission.json');old.window(p['admitted_unix'],p['deadline'])
    need(p['prior_score']==PIN and p['cores']==load(BANK,'admission.json')['cores'] and p['counted_upper']==BOUND and load(d,'copy.json')['raw_sha256']==old.RAW_SHA and load(d,'copy.json')['mincore_resident_pages']==0,'unmatched input')
    v=load(d,'identity.json');need(v['all_records_and_occurrences'] and v['columns']==load(BANK,'identity.json')['columns'] and v['columns']==old.fields(d,'columns-check','FRONT64_VERIFIED'),'not verified')
    for phase,mem,workers in [(z,old.RAM,4),(oracle,s.ORACLE,1)]:
        g=phase['after'];need(phase['rc']==0 and phase['status']=='COMPLETED' and phase['stop'] is None and phase['affinity']==p['cores'][:workers] and 0<phase['counted_dir_sampled']<=BOUND<=2*N and
            (g['memory.max'],g['cpu.max'],g['memory.swap.max'])==(str(mem),f'{workers*100000} 100000','0') and {'oom 0','oom_kill 0'}<=set(g['memory.events'].splitlines()) and 0<int(g['memory.peak'])<=mem,'phase grant or stop')
    need(all(f[key]==base['front'][key] for key in ('n','m','k','descriptors','max_phrase','starts_above_u32','keys_above_u32','dictionary_bytes','key_bytes','team','max_bin','nonempty_bins')) and int(f['team'])==4 and int(f['arrays'])==arrays(N,M,4),'front geometry')
    div=lambda a,b:(a+b-1)//b
    need(int(geom['position_bits'])==(N-1).bit_length()==34 and int(geom['projected_prefix_bits'])==30 and int(geom['start_bytes'])==8*div(N,64) and int(geom['entry_bytes'])==8*M and int(geom['rank_anchor_bytes'])==8*(div(N,512)+1) and 8*div(M,64)<=int(geom['head_bytes'])<=8*(div(M,64)+4095),'reservation detail')
    ratio=dict(wall=z['wall_s']/control['wall_s'],cpu=z['process']['cpu_s']/control['process']['cpu_s'],array_bound=int(f['arrays'])/int(base['front']['arrays']),sampled_rss=z['sampled_rss']/control['sampled_rss'],cgroup_peak=int(z['after']['memory.peak'])/int(control['after']['memory.peak']))
    for key in ('read_bytes','write_bytes'):
        need(z['process'][key]>0,'missing attributed IO');ratio[key]=z['process'][key]/control['process'][key]
    met=(ratio['wall']<=1.50 and ratio['sampled_rss']<=.85 and max(ratio['read_bytes'],ratio['write_bytes'])<=1.05 and int(f['arrays'])<=14<<30)
    return dict(verified_columns=True,layout_goal_met=met,position_over_reference=ratio,raw_to_columns_s=z['wall_s'],reference_s=control['wall_s'],oracle_s=oracle['wall_s'],front=f,layout=geom,bin_balance=balance((d/'front.log').read_text()),modeled_front_arrays_100gb=arrays(100_000_000_000,3181334488,4),scope='one 10 GB raw→canonical columns comparison; no product, new rank time, or 100 GB grant')

if __name__=='__main__':
    need(__debug__,'assertions required');mode=sys.argv[1];d=Path(sys.argv[2])
    if mode=='score':print(json.dumps(score(d),sort_keys=True,indent=2))
    elif mode=='admit':admit(d,int(sys.argv[3]))
    else:worker(d,int(sys.argv[3]),mode)
