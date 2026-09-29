#!/usr/bin/env python3
"""VALIDATION-ONLY: resume after accepted front end; never runs rpfbwt.
Requires measured old-cap refusal and PASS yeast235/k10 byte gates first.
All outputs are new files in this workspace. Retained parse inputs stay read-only.
"""
import json, re, struct, time
from pathlib import Path
from seam_job import LOG, OUT, BIN, run, record
TOOLS=Path('/tmp/sxgc-dist-final')
INPUT=Path('/tmp/rpfbwt-64-466')

def snapshot():
    return {str(p):dict(size=p.stat().st_size,mtime_ns=p.stat().st_mtime_ns,inode=p.stat().st_ino)
            for p in INPUT.glob('parse.*')}

def main():
    gates=json.loads((LOG/'seam-policy-regression-results.json').read_text())
    assert gates['status']=='PASS' and len(gates['results'])==2
    measurement=(LOG/'seam466-measure.log').read_text()
    assert 'FATAL SLIM: CYCLIC_SEAM_REFUSED' in measurement
    proposed=re.search(r'requested_lce=(\d+)',measurement)
    assert proposed
    events=[json.loads(x) for x in (LOG/'validation.jsonl').read_text().splitlines()]
    assert any(x.get('stage')=='full466-rpfbwt' and x.get('status')=='PASS' for x in events)
    before=snapshot();(LOG/'seam466-inputs-before.json').write_text(json.dumps(before,indent=2)+'\n')
    record(stage='resume-preflight',status='PASS',front_end='SKIPPED_ALREADY_PASSED',requested_lce_at_old_refusal=int(proposed[1]),requested_lce_exact=False,regression_gates='PASS')
    dst=OUT/'full466';dst.mkdir()
    ri,heads,agg,chi=[dst/('validation.'+x) for x in ['ri4','head_sa','agg','sA']]
    prefix=INPUT/'parse'
    log=run('resume466-endpoints',[BIN/'rpfbwt_endpoints',prefix,ri,heads,'1e'])
    stats=re.search(r'CYCLIC_SEAM_WORK max_seam_lce=(\d+) total_work=(\d+) total_limit=(\d+) exact=1',log)
    assert stats
    fp=[]
    for line in log.splitlines():
        if line.startswith('SLIM_FP seam-'):
            fp.append(dict(re.findall(r'(\w+)=([\d.]+)',line)))
    record(stage='seam-work',status='PASS',max_seam_lce=int(stats[1]),total_work=int(stats[2]),total_limit=int(stats[3]),fingerprints=fp)
    with (INPUT/'parse.rlebwt.meta').open('rb') as f:n,r=struct.unpack('<QQ',f.read(16))
    assert (n,r)==(1403221068491,2739737289)
    for ext in ['ssa','ssa_t']:
        p=INPUT/('parse.'+ext)
        with p.open('rb') as f:assert struct.unpack('<Q',f.read(8))[0]==r
        assert p.stat().st_size==8+8*r
    record(stage='resume466-tap-count',status='PASS',raw_n=n,raw_r=r)
    run('resume466-tap-structure',[LOG/'check_tap',prefix])
    run('resume466-slim',[BIN/'slim_dump','--slim','--resolve-ri4','--dict-stream','--ri4',ri,'--head-sa',heads,'--parse',prefix,'-t',48,'-o',agg])
    sweep=run('resume466-sweep',[TOOLS/'xsa','chi-rspace','--stream-agg','--ri4',ri,'--agg',agg,'-o',chi])
    count=chi.stat().st_size//8
    assert chi.stat().st_size%8==0 and f'chi = {count}' in sweep
    record(stage='resume466-chi-count',status='PASS',chi=count)
    names=INPUT/'collection.txt.names.tsv'
    run('resume466-audit',[TOOLS/'sxi_text_audit',INPUT/'collection.txt',ri,agg,chi,1000,'--agc','/home/erikg/hprcv2/HPRC_r2_assemblies_0.6.1.agc','--names',names,'--agc2flat',TOOLS/'agc2flat'])
    publication=dst/'hprc.validation.sxi'
    run('resume466-write',[TOOLS/'sxi_write','--ri4',ri,'--heads',heads,'--chi',chi,'--output',publication,'--mode','dna','--orientation','reversed','--names',names])
    run('resume466-validate',[TOOLS/'xsa','sxi-info',publication])
    after=snapshot();assert before==after
    record(stage='resume466-input-preservation',status='PASS',method='size/mtime_ns/inode; tools open inputs read-only')
    result=dict(status='PASS',validation_only=True,chi=count,max_seam_lce=int(stats[1]),total_verification_work=sum(int(x['verification_work']) for x in fp),total_work=int(stats[2]),total_limit=int(stats[3]),output=str(publication),retained_inputs_unchanged=True)
    (LOG/'validation-resume-result.json').write_text(json.dumps(result,indent=2)+'\n');record(**result)

if __name__=='__main__':
    try:main()
    except Exception as error:
        record(stage='resume466',status='FAIL',error=str(error));raise
