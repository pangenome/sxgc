"""Compare two certified 10 GB arms; labels describe what can be attributed."""
import argparse
from pathlib import Path
import re

from overlap_resident_gate import TEN_ARMS,save
from overlap_ten_score import FIXED,HISTORICAL,HISTORICAL_WORKERS,get,traffic,verified_arm
from overlap_three_score import binaries

def device_bytes(before,after):
    """Cgroup block-device counters; preserve per-device attribution and
    missing data (stacked devices need not represent disjoint traffic)."""
    def parse(snapshot):
        text=(snapshot or {}).get('io.stat')
        if not text:return None
        result={}
        for line in text.splitlines():
            dev,*fields=line.split();values=dict(x.split('=') for x in fields)
            result[dev]={k:int(values[k]) for k in ('rbytes','wbytes')}
        return result
    lo,hi=parse(before),parse(after)
    if lo is None or hi is None:return None
    assert lo.keys()<=hi.keys()
    delta={dev:{k:v-lo.get(dev,{}).get(k,0) for k,v in values.items()} for dev,values in hi.items()}
    assert all(n>=0 for values in delta.values() for n in values.values());return delta

def metrics(path):
    b,f,i=verified_arm(path); hashes=binaries(path)
    assert set(hashes)==FIXED.keys()|HISTORICAL_WORKERS.keys() and all(hashes[k]==v for k,v in FIXED.items())
    assert 'zero resident pages' in (path/'cold.log').read_text() and f['cold_barrier_s']>0
    log=(path/'resident.log').read_text();stream,=re.findall(r'^RESIDENT_STREAM (.*)$',log,re.M)
    core={k:int(v) for k,v in re.findall(r'(rows|descriptors|groups|max_group)=(\d+)',stream)}
    assert len(core)==4 and core['descriptors']==f['descriptors']
    phases=dict((k,float(v)) for k,v in re.findall(r'^RESIDENT_PHASE phase=(\w+) seconds=([0-9.]+)',log,re.M));assert len(phases)==8
    profile=b['profile']
    if profile not in TEN_ARMS:
        assert profile=='packed-10g-bitmap' and hashes=={**FIXED,**HISTORICAL_WORKERS}
        assert (path/'binaries.sha').read_text().splitlines()[0]==HISTORICAL and 'RESIDENT_RADIX_JOIN' not in log
    return dict(profile=profile,workers=hashes,identity=i,stream=core,affinity=b.get('cpu_affinity'),
                harness_revision=(path/'binaries.sha').read_text().splitlines()[0],
                wall_s=dict(raw=b['raw_to_product_s'],prepared=b['wall_s'],front=f['wall_s'],**phases),
                raw_worker_bytes=b['raw_worker_io'],prepared_worker_bytes={k:traffic(b,k) for k in ('read_bytes','write_bytes')},
                raw_cgroup_device_bytes=device_bytes(b.get('shared_before'),b['shared_after']),
                prepared_cgroup_device_bytes=device_bytes(b.get('prepared_cgroup_before'),b['shared_after']),
                cpu_s=dict(front=f['process']['cpu_s'],harness=b.get('harness_cpu_s'),oracle=get(path,'oracle.json')['process']['cpu_s'],**{k:v['cpu_s'] for k,v in b['process'].items()}),
                sxls_markers=b.get('sxls_markers'),sink_syscw=b['process']['sink']['syscw'])

def compare(mode,control,trial):
    a,b=metrics(control),metrics(trial);assert a['identity']==b['identity'] and a['stream']==b['stream']
    assert b['profile'] in TEN_ARMS
    if mode=='single-knob':
        assert a['workers']==b['workers'] and a['profile'] in TEN_ARMS and a['affinity']==b['affinity'] and a['harness_revision']==b['harness_revision']
        knobs=TEN_ARMS[a['profile']];label='same-binary single-knob'
    else:
        assert mode=='historical' and a['profile']=='packed-10g-bitmap';knobs=(1,0,1,0);label='cross-revision combined (not single-knob attribution)'
    changed=[k for k,x,y in zip(('lanes','batch','dict_threads','reclaim'),knobs,TEN_ARMS[b['profile']]) if x!=y]
    if mode=='single-knob':assert len(changed)==1
    def ratio(key):return {k:v/a[key][k] for k,v in b[key].items() if a[key][k]>0}
    return dict(comparison=label,changed_knobs=changed,identical_verified_product=True,control_path=str(control),trial_path=str(trial),control=a,trial=b,
                trial_over_control_wall=ratio('wall_s'),trial_over_control_raw_io=ratio('raw_worker_bytes'),trial_over_control_prepared_io=ratio('prepared_worker_bytes'))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('mode',choices=('single-knob','historical'))
    for arg in ('control','trial','dest'):parser.add_argument(arg,type=Path)
    args=parser.parse_args();save(args.dest,compare(args.mode,args.control,args.trial))
