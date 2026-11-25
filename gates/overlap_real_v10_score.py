"""Fixed real pair admission and read-only postcomparison. Never dispatch from a score."""
from pathlib import Path
import os
import re
import shutil
import subprocess as sp
import sys
import time

from overlap_real_v10_product import private,same
from overlap_real_v10_plan import *
from overlap_real_v10_meter import save,load
from overlap_finite_emit64_study import cpus
from overlap_parse64_study import memory_available

def admit(d,end):
    private(d);need(d.parent==S and re.fullmatch('real-v10-block100-[A-Za-z0-9]{6}',d.name) and set(S.glob('real-v10-block100-*'))=={d} and not list(d.iterdir()),'prior arm or non-private root');t=int(time.time());need(900<end-t<=1500 and memory_available()>=4<<30 and shutil.disk_usage(d).free>3<<30,'time, memory or disk admission');p=pins();plan=model();cores=cpus();need(len(cores)==5 and cores==sorted(set(cores)),'core geometry');rev=sp.check_output(['git','-C',G.parent,'rev-parse','HEAD'],text=True).strip();need(os.environ['SXGC_PINNED_SOURCE']==str(G.parent)+':'+rev and not sp.check_output(['git','-C',G.parent,'status','--porcelain']),'unpublished source')
    credit=G/'receipts/finish-credits-100m/review.json';need(sha(credit)=='f721ca2f7b6177de8d57202f2a62bbabdc8ddbfb3dc6b97c86fc91a6ad863427' and load(credit)['credits']['slice_sufficient']==4,'credit evidence');need(sha(G/'receipts/finite-epoch64-100m/score.json')=='fc87f879608719e016221b6a383b2079e09d158885be494e09f90a2cafbc7cab','epoch evidence');sf=Path('/tmp/v10-fixture-1001-1206931/tools/xsa/runtime/bit6/xsa_finish.cpp');src=sf.read_bytes();need(hashlib.sha256(src).hexdigest()=='cfafb5340bfd8e56ea40772b68599256ac1f6cb9add06806c5ee25813fec871b','v10 source');a=src.index(b'struct LcpColumn {');cl=src[a:src.index(b'\nstatic void fp_self_check',a)];need(hashlib.sha256(cl).hexdigest()=='48fe0cf632d43eb96d93dd979e6338bedd214fdb2d0068e32f5c152767536096','different SXLS loader')
    save(d/'admit.json',dict(start=t,end=end,rev=rev,pins=p,files=plan,cores=cores,raw_inputs=IN,source=str(SRC),shared_memory_max=MEM,checker_memory_max=ORACLE,rolling_array_limit=ARRAY,reference_array_limit=RES_ARRAY,data_caps={'tap':160<<20,'finish':192<<20,'sink':64<<20},finish_stack=8<<20,swap_max=0,scope='Fixed real 100 MB reference then rolling candidate, four shared build CPUs + separate verifier CPU, no scale authority.'));print(','.join(map(str,cores[:4])),cores[-1])

def score(d):
    private(d);ad=load(d/'admit.json');need(ad['files']==model() and ad['pins']=={str(p):s for p,s in PINS.items()},'unmatched plan');arms={}
    for k in ('reference','candidate'):
        a=d/k;s=load(a/'status.json');v=load(a/'verified.json');need(not s['stop'] and not (a/'stop.requested').exists() and 'arm-'+k+'=0' in (d/'exits.log').read_text().splitlines(),'unverified arm');ver=v['verifier'];g=s['after'];req={'mkfs','volume1','unmount1','checkfs1','volume2','unmount2','checkfs2','front','tap','witness','sink','finish','join','docs','ctx','query0','query1'}|({'resident','relay'} if k=='reference' else {'rank','emit'});w=s['worker'];need(set(w)==req and all(x['rc']==0 for x in w.values()) and ver['worker']['rc']==0 and v['all_rows_raw_verified'] and v['rows']==ROWS and v['docs']==DOCS and v['runs']==RUNS and v['table']=={str(i):x for i,x in inventory(a/'index.sxi2',10).items()} and v['full_sha256']==sha(a/'index.sxi2'),'worker, product or proof');need(v['clocks']==s['clocks'] and s['clocks']['raw_to_join_s']>=s['clocks']['prepared_to_join_s']>0 and s['peaks']['outer_charged']<=model()['outer_cap'][k]<=2*N and s['peaks']['scratch_bytes']<=max(model()[k].values()),'phase wall or peak');need((g['memory.max'],g['memory.swap.max'],g['cores'])==(str(MEM),'0',ad['cores'][:4]) and (ver['after']['memory.max'],ver['after']['cores'])==(str(ORACLE),ad['cores'][-1:]),'changed grant')
        arms[k]=dict(raw_to_join_s=s['clocks']['raw_to_join_s'],prepared_to_join_s=s['clocks']['prepared_to_join_s'],certified_wall_s=s['clocks']['raw_to_certified_publication_s'],postcheck_s=s['clocks']['postcheck_to_publication_s'],service_body_s=s['service_body_s'],bytes=v['bytes'],sha256=v['full_sha256'],position_file_bytes_each_way=v['position_file_bytes_each_way'],shared_peak=int(g['memory.peak']),verifier_peak=int(ver['after']['memory.peak']),logical_scratch_reserved=max(model()[k].values()),scratch_sampled=s['peaks']['scratch_bytes'],outer_scratch_reserved=model()['outer_cap'][k],sampled_scratch_allocation=s['peaks']['scratch_allocated'],sampled_allocation_plus_transport=s['peaks']['outer_charged'],sampled_high_phase=s['sampled_high_phase']['scratch_bytes'],attributed_worker_io={n:(x['ru_inblock_bytes'],x['ru_oublock_bytes']) for n,x in w.items()},cgroup_io=g['io.stat'],verifier_attributed_io=[ver['worker']['ru_inblock_bytes'],ver['worker']['ru_oublock_bytes']])
    r,c=arms.values();need(c['sha256']==r['sha256'] and r['bytes']==c['bytes'] and same(d/'reference/index.sxi2',d/'candidate/index.sxi2'),'raw file mismatch');ratio=c['raw_to_join_s']/r['raw_to_join_s'];return dict(scope='One whole-document real Pile prefix. Product, not speed or grant for any larger input.',input_bytes=N,rows=ROWS,docs=DOCS,full_identity=True,byte_sha256=c['sha256'],arms=arms,normalized_raw_wall_ratio=ratio,raw_under_2x=ratio<2,raw_under_3x=ratio<3,scratch_limit=2*N,physical_io_note='wait4 attributed block counters by native child; cgroup io.stat is null where not delegated. Logical position-band and pipe traffic are not physical IO.')

if __name__=='__main__':
    if sys.argv[1]=='admit':admit(Path(sys.argv[2]),int(sys.argv[3]))
    else:need(sys.argv[1]=='score','score mode');print(json.dumps(score(Path(sys.argv[2])),indent=2,sort_keys=True))
