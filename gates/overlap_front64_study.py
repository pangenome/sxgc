"""Fixed direct-u64 raw front through certified P1 inverse on the 10 GB rung. No larger dispatch."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess as sp
import sys
import time
import overlap_parse64_study as s
from overlap_scanplan_gate import Phase
from overlap_front64_balance import balance

PREFIX='raw64-p1-10g-';SCRIPT='overlap_front64_study.sh';RAW=s.ROOT.parent/'pf-10gb.txt';BANK=s.RECEIPTS/'parse64-10g';PIN='3e05977d201c7afda0487314df9a22bf736154759da687a2c40869660776b37d'
N,M,K=s.N,s.M,s.K;D64=s.T+4*K;U=9129290112;BOUND=D64+16*M+(4<<20);RAM=32<<30;ARRAYS=20<<30
RANK='b2adba88d6bd0bfee923928ba84cb4c9d7bc53eef17a84472fd6965bec8ec89f';RAW_SHA='178f411b196e6741c473b4bbf442086bc64e8c4ae102359fc3c0e3f17a12085d';need=s.need;load=s.load

# Only this unstarted endpoint was renewed by the operator. Other gates retain their old cutoff.
MIN_REMAINING=900
MAX_WINDOW=1800

def window(start,end):
    need(start+MIN_REMAINING < end <= start+MAX_WINDOW, 'front64 needs a fresh 901..1800 second lease')

def private(d):need(d.resolve()==d and d.parent==s.S and re.fullmatch(PREFIX+'[A-Za-z0-9]{6}',d.name) and d.stat().st_uid==os.getuid() and d.stat().st_mode&0o077==0,'private raw64 study')
def admit(d,end):
    start=int(time.time());window(start,end)
    private(d);s.guard(d,end,MIN_REMAINING,cutoff=start+MAX_WINDOW);s.lease(os.getppid(),end,SCRIPT);s.exclusive();rev=s.published_main(Path.home()/'sxgc');need(sp.check_output(['git','-C',s.REPO,'rev-parse','HEAD'],text=True).strip()==rev and not sp.check_output(['git','-C',s.REPO,'status','--porcelain']) and set(s.S.glob(PREFIX+'*'))=={d} and not any(d.iterdir()),'prior attempt or source pin')
    base=s.score(BANK);need(base==load(BANK,'score.json') and s.sha(BANK/'score.json')==PIN and base['arms']['4']['sha256']==RANK and base['parallel_goal_met'],'certified downstream handoff required');f=load(s.RECEIPTS/'segmented-front-10g','segmented.json');need(f['raw_sha256']==RAW_SHA and list(s.identity(RAW))==f['source_identity'] and f['bank_identity']=={k:list(s.identity(s.SRC/k)) for k in s.IN} and s.memory_available()>=RAM+(2<<30) and shutil.disk_usage(d).free>=N+2*N+(2<<30) and BOUND<=2*N and N+24*M+8+(12<<20)<=ARRAYS,'grants, columns or disk');s.save(d/'admission.json',dict(rev=rev,admitted_unix=start,deadline=end,parent=os.getppid(),cores=s.cores(),raw_stat=s.identity(RAW),reference_stats=f['bank_identity'],counted_upper=BOUND,prior_score=PIN,scope='one fixed raw-to-P1 stage; no byte suffix stream, product or scale launch'))

def files(d):
    # Never double charge a transient hard link; only the private immutable raw copy is excluded.
    keep={};total={};stack=[d]
    while stack:
        p=stack.pop()
        try:
            st=p.stat();key=st.st_dev,st.st_ino;total[key]=st.st_blocks*512
            if p.is_dir():stack.extend(p.iterdir())
            elif p!=d/'raw':keep[key]=st.st_size
        except FileNotFoundError:pass # the native publisher atomically renames its closed private directory
    return sum(keep.values()),sum(total.values())

def fields(d,name,tag):
    line,=[l for l in (d/(name+'.log')).read_text().splitlines() if l.startswith(tag+' ')];return dict(f.split('=',1) for f in line.split()[1:])

def worker(d,end,mode):
    private(d);p=load(d,'admission.json');window(p['admitted_unix'],p['deadline']);need(p['deadline']==end and p['admitted_unix']<=time.time(),'deadline drift');s.lease(p['parent'],end,SCRIPT);s.guard(d,end,cutoff=p['deadline']);s.exclusive();stop=d/'stop.requested';raw=d/'raw';col=d/'columns';tape=col/'dictionary.lex64';keys=col/'keys.u64';rank=d/'inverse.u64';cp=p['cores']
    def native(name,args,seconds,capr,capw,mem,workers):
        t=min(end,int(time.time())+seconds);s.measure(d,name,[*args,stop,t],t,capr+s.MARGIN,capw+s.MARGIN,cp[:workers],mem,workers,ledger=lambda:files(d),cutoff=end)
    if mode=='copy':
        r=dict(before=s.grant(192<<20,1));ph=Phase(d,r,'copy_hash_cold',end,N+s.MARGIN,N+s.MARGIN);need(list(s.identity(RAW))==p['raw_stat'] and s.copy_file(RAW,raw,N,ph)==RAW_SHA,'raw pin');ph.guard(True);s.save(d/'copy.json',dict(r,raw_sha256=RAW_SHA,private_stat=s.identity(raw),mincore_resident_pages=0,after=s.grant(192<<20,1)))
    elif mode=='build':
        s.guard(d,end,800,cutoff=end);need(list(s.identity(raw))==load(d,'copy.json')['private_stat'],'raw changed');s.grant(RAM,4);t0=time.monotonic();native('front',[d/'front',raw,col,4,ARRAYS,2*N-8*M-(4<<20)],420,4*N,D64+8*M,RAM,4);f=fields(d,'front','FRONT64');want={'n':N,'m':M,'k':K,'team':4,'dictionary_bytes':D64,'key_bytes':8*M,'descriptors':U,'arrays':N+24*M+8+(12<<20)};need(all(int(f[k])==v for k,v in want.items()) and int(f['keys_above_u32'])>0 and keys.stat().st_size==8*M and tape.stat().st_size==D64,'bad front dimensions');bar=time.monotonic()
        for path in (raw,tape,keys):path.chmod(0o400);s.cold(path)
        cold_s=time.monotonic()-bar;native('rank',[d/'parse64','rank',keys,rank,4,s.ARRAYS],140,8*M,8*M,RAM,4);elapsed=time.monotonic()-t0;rank.chmod(0o400);s.save(d/'build.json',dict(raw_to_rank_s=elapsed,cold_barrier_s=cold_s,cold_resident_pages=0,raw_to_columns_s=load(d,'front.json')['wall_s'],prepared_to_rank_s=load(d,'rank.json')['wall_s'],column_stats={x.name:s.identity(x) for x in (tape,keys,rank)},counted_upper=BOUND))
    elif mode=='verify':
        s.grant(s.ORACLE,1);b=load(d,'build.json');need(b['column_stats']=={x.name:list(s.identity(x)) for x in (tape,keys,rank)} and p['reference_stats']=={k:list(s.identity(s.SRC/k)) for k in s.IN},'changed output or reference');s.cold(tape);s.cold(keys);native('columns-check',[d/'front-verify',tape,keys,s.SRC/'dictionary.packed',s.SRC/'parse.u32',K,s.ORACLE],220,D64+s.T+12*M,0,s.ORACLE,1);v=fields(d,'columns-check','FRONT64_VERIFIED');need(v['legacy_tape_sha256']==s.IN['dictionary.packed'][1] and v['legacy_ids_sha256']==s.IN['parse.u32'][1] and (int(v['m']),int(v['k']),int(v['descriptors']))==(M,K,U),'legacy column identity');s.cold(keys);s.cold(rank);native('ranks-check',[d/'verify',keys,8,rank,s.ORACLE],150,16*M,0,s.ORACLE,1);r={};ph=Phase(d,r,'post_verify_hash',end,8*M+s.MARGIN,s.MARGIN);h=s.sha(rank);ph.guard(True);need(h==RANK,'not the certified numeric-order rank column');s.save(d/'identity.json',dict(r,rank_sha256=h,rank_bytes=8*M,columns=v,all_rank_and_column_checks=True));print(f'RAW64_P1_VERIFIED m={M} sha256={h}',flush=True)
    else:raise ValueError(mode)

def score(d):
    i=load(d,'identity.json');b=load(d,'build.json');f=fields(d,'front','FRONT64');h=fields(d,'rank','HEAD');cg=[];io={}
    for label,mem,cpu in [('front',RAM,4),('rank',RAM,4),('columns-check',s.ORACLE,1),('ranks-check',s.ORACLE,1)]:
        z=load(d,label+'.json');g=z['after'];need(z['rc']==0 and z['stop'] is None and z['status']=='COMPLETED' and z['counted_dir_sampled']<=BOUND<=2*N and (g['memory.max'],g['cpu.max'],g['memory.swap.max'])==(str(mem),f'{cpu*100000} 100000','0') and {'oom 0','oom_kill 0'}<=set(g['memory.events'].splitlines()) and int(g['memory.peak'])<=mem,'stage or grant');cg.append(int(g['memory.peak']));io[label]={k:z['process'][k] for k in ('cpu_s','read_bytes','write_bytes')};io[label]['per_device_io']=z['per_device_io']
    need(load(d,'copy.json')['raw_sha256']==RAW_SHA and b['cold_resident_pages']==0 and i['rank_sha256']==RANK and i['rank_bytes']==8*M and i['all_rank_and_column_checks'] and b['raw_to_rank_s']>=b['raw_to_columns_s']+b['prepared_to_rank_s'] and len(set(int(x) for x in [f['keys_above_u32'],h['keys_above_u32'],i['columns']['keys_above_u32']]))==1 and int(h['team'])==int(f['team'])==4,'endpoint evidence');need('source_token_width=8 ranks=uint64 permutation=1 strict_recurrence=1' in (d/'ranks-check.log').read_text(),'wide rank witness')
    return dict(scope='10000000000 raw bytes through fsynced inverse.u64 only; not the byte-level index or a 100 GB authorization.',raw_to_rank_s=b['raw_to_rank_s'],raw_to_columns_s=b['raw_to_columns_s'],prepared_to_rank_s=b['prepared_to_rank_s'],cold_barrier_s=b['cold_barrier_s'],bin_balance=balance((d/'front.log').read_text()),counted_upper=BOUND,worker_io_cpu=io,peaks=cg,front=f,rank_sha256=RANK,verified_intermediate=True)
if __name__=='__main__':
    need(__debug__,'assertions required');mode=sys.argv[1];d=Path(sys.argv[2]);
    if mode=='score':print(json.dumps(score(d),sort_keys=True,indent=2))
    elif mode=='admit':admit(d,int(sys.argv[3]))
    else:worker(d,int(sys.argv[3]),mode)
