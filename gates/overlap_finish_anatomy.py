"""Small post-clock decomposition of the frozen finish on certified 10 GB arms.
Reads only an original hash-pinned diagnostic log, never a product/column/corpus.
"""
import hashlib
import json
from pathlib import Path
import re
import sys
from overlap_resident_gate import ROOT,save
from overlap_ten_score import get,verified_arm

TAGS=('XSA_FINISH_OVERLAP','XSA_FINISH_SWEEP_ANATOMY','SWEEP_PHASE_WALLS','SWEEP_STATS','LCP_SLICE_CONSUMER','XSA_FINISH_M1_STREAM')
PHASES=('readiness-pfck','fingerprints','seam-classes','sweep','m5-ef','m1-runs','publish')

def numeric(fields):
    r={}
    for col in fields:
        k,v=col.split('=',1);assert k not in r
        if re.fullmatch('[0-9]+',v):r[k]=int(v)
        elif re.fullmatch(r'[0-9]+\.[0-9]+',v):r[k]=float(v)
        else:r[k]=v
    return r

def extract(bank):
    assert __debug__;b,_,i=verified_arm(bank);s=get(bank,'source.json');orig=Path(s['source']);assert s['identity'] is s['complete_arm'] is True and orig.parent==ROOT/'sandbox' and orig.resolve()==orig
    ref=s['selected_logs']['finish.log'];p=orig/'finish.log';assert p.is_file() and not p.is_symlink() and p.stat().st_size==ref['original_bytes']<32<<20
    h=hashlib.sha256();n=0;sync={};ph={};single={};progress=0
    with p.open('rb') as f:
        for l in f:
            h.update(l);n+=1;tag,_,rest=l.decode().strip().partition(' ')
            if tag=='CONTAINER_SYNC':
                v=numeric(rest.split());assert v['member']=='m1' and type(v['unsynced_bytes']) is int and v['unsynced_bytes']>0 and v['why'] in ('piece-release','pre-close')
                g=sync.setdefault(v['why'],{'count':0,'logical_unsynced_bytes':0});g['count']+=1;g['logical_unsynced_bytes']+=v['unsynced_bytes']
            elif tag=='XSA_FINISH_PHASE':
                name,*kv=rest.split();assert name not in ph;ph[name]=numeric(kv)
            elif tag in TAGS:
                assert tag not in single;single[tag]=numeric(rest.split())
            elif tag=='XSA_FINISH_PROGRESS':progress+=1
    assert h.hexdigest()==ref['sha256'] and n==ref['original_lines'];assert tuple(ph)==PHASES and set(single)==set(TAGS)
    prev=0.
    for phase in ph.values():
        assert phase['wall']>=0 and abs(phase['cumulative']-prev-phase['wall'])<.03;prev=phase['cumulative']
    runs=i['members']['1'][3];m1=i['members']['1'][2];chi=i['members']['5'][3]
    assert runs==single['XSA_FINISH_SWEEP_ANATOMY']['raw_runs']==single['XSA_FINISH_M1_STREAM']['runs'] and single['SWEEP_STATS']['emits']==chi
    assert sum(v['logical_unsynced_bytes'] for v in sync.values())==single['XSA_FINISH_M1_STREAM']['m1_bytes']==m1
    assert single['LCP_SLICE_CONSUMER']['slices']==single['LCP_SLICE_CONSUMER']['retired']==38147
    proc=b['process']['finish'];return dict(profile=b['profile'],source_log_sha256=h.hexdigest(),finish_phase_seconds=ph,native_measurements=single,m1_sync=sync,
            progress_lines=progress,finish_process={k:proc[k] for k in ('cpu_s','read_bytes','write_bytes','cancelled_write_bytes','wchar','syscw')},
            interpretation='Native spans overlap. Sum of logical unsynced m1 bytes is payload coverage, not physical traffic/seek count. Process write_bytes is used unchanged by the promotion gate; cancellation is separate, not a discount.')

if __name__=='__main__':
    for name in sys.argv[1:]:
        p=Path(name);j=extract(p);dest=p/'finish-phases.json'
        if dest.exists():assert get(p,'finish-phases.json')==j
        else:save(dest,j)
        print('FINISH_ANATOMY_REPLAYED',p,j['profile'],j['source_log_sha256'])
