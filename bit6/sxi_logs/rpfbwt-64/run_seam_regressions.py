#!/usr/bin/env python3
"""VALIDATION-ONLY exact byte gates against accepted yeast235/k10 artifacts."""
import concurrent.futures, json, struct
from pathlib import Path
from seam_job import LOG, OUT, BIN, run, record
TOOLS=Path('/tmp/sxgc-dist-final')
YEAST=Path('/tmp/sxgc-distribution-yeast/xsa-build-wj3k_ccs')
K10=Path('/tmp/k10-endpoints-debug/retained')
PILOT=Path('/mnt/nvme3n1/erikg/sxgc-pilot/k10/h10ss_pfp')
K10PUB=Path('/mnt/nvme3n1/erikg/sxgc-k10sep/from-zero-p_fwd37s/k10.sxi')

def compare(stage,a,b):
    run(stage,['cmp','--',a,b])

def slim(tool,prefix,ri,heads,agg):
    cmd=[tool,'--slim','--resolve-ri4','--dict-stream','--ri4',ri,'--parse',prefix,'-t',24,'-o',agg]
    if heads:cmd+=['--head-sa',heads]
    return cmd

def main():
    run('seam-policy-primitive',[BIN/'slim_lce_test'])
    run('seam-policy-dense',['python3',LOG.parents[1]/'test_seam_repair.py','--tools',BIN,'--xsa',TOOLS/'xsa','--work',OUT/'dense'])
    results=[]
    for name,base,count,delta in [('yeast235',YEAST,85404336,96),('k10',K10,1627067257,4074)]:
        dst=OUT/name;dst.mkdir()
        if name=='k10':
            for ext in ['.dict','.parse','.rlebwt','.rlebwt.meta','.ssa']:
                compare('gate-k10-pilot'+ext,Path(str(PILOT)+ext),K10/('parse'+ext))
        ri,heads,agg,chi=[dst/('fresh.'+x) for x in ['ri4','head_sa','agg','sA']]
        run('gate-'+name+'-endpoints',[BIN/'rpfbwt_endpoints',base/'parse',ri,heads,'1e'])
        for ext,path in [('ri4',ri),('head_sa',heads)]:compare('gate-'+name+'-'+ext,path,base/('fresh.'+ext))
        if name=='yeast235':
            run('gate-'+name+'-slim',slim(BIN/'slim_dump',base/'parse',ri,heads,agg))
            compare('gate-'+name+'-agg',agg,base/'fresh.agg')
        else:
            # Accepted k10 publication stores runs/heads/chi, not its aggregate.
            # Reconstruct its aggregate with the accepted unmodified slim tool,
            # then compare every byte to the patched consumer's aggregate.
            baseline=dst/'accepted.agg'
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                jobs=[pool.submit(run,'gate-k10-accepted-slim',slim(TOOLS/'slim_dump',base/'parse',K10PUB,None,baseline)),
                      pool.submit(run,'gate-k10-slim',slim(BIN/'slim_dump',base/'parse',ri,heads,agg))]
                for job in jobs:job.result()
            compare('gate-k10-agg',agg,baseline)
        run('gate-'+name+'-sweep',[TOOLS/'xsa','chi-rspace','--stream-agg','--ri4',ri,'--agg',agg,'-o',chi])
        assert chi.stat().st_size==8*count
        if name=='yeast235':compare('gate-yeast235-chi',chi,base/'fresh.sA')
        else:
            publication=dst/'candidate.sxi'
            run('gate-k10-write',[TOOLS/'sxi_write','--ri4',ri,'--heads',heads,'--chi',chi,'--output',publication,'--mode','text','--orientation','forward'])
            compare('gate-k10-publication',publication,K10PUB)
        result=dict(dataset=name,status='PASS',chi=count,seam_delta=delta,historical_chi=count-delta,endpoint_bytes='identical',aggregate_bytes='identical',chi_unchanged=True)
        record(stage='regression-gate',**result);results.append(result)
    (LOG/'seam-policy-regression-results.json').write_text(json.dumps(dict(status='PASS',validation_only=True,results=results),indent=2)+'\n')

if __name__=='__main__':
    try:main()
    except Exception as error:
        record(stage='regression-gates',status='FAIL',error=str(error));raise
