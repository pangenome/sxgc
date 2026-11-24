"""Score one new fixed 100 MB shortest-tail producer vs two archived arms. Never dispatch a control."""
import json
import math
import re
import sys
from pathlib import Path
from overlap_bounded_review import RUNG, N, K, U, arm_observation, load, need, review, sha, small
from overlap_segmented_gate import io_delta, stat_io

PREFIX = 'bounded-shortest-100m-'
SOURCE_RECORD = 'b8c5abfce4bffec96cc14d3abc2a8d3eb4f4a6a078bc85ec2387c000d1fb9b7b'
CLOCKS = ('prepared_to_verified_EOF_s','raw_to_verified_EOF_s')


def reference():
    need(sha(RUNG/'source.json')==SOURCE_RECORD,'different 100 MB pair')
    r=review(RUNG)
    need(r==load(RUNG/'review.json') and not r['under_3x_component_wall'] and r['observed_oversized_groups']==0 and 1<=r['largest_finite_group']<=8192,'missing measured baseline and admission target')
    return dict(pair_source_sha256=SOURCE_RECORD,pair_review_sha256=sha(RUNG/'review.json'),pair_revision=load(RUNG/'inputs.json')['revision'],
                columns=r['columns'],expected_peak=r['largest_finite_group'],expected_shortest_payload=9*(K-1),
                prior_classifications=r['classifications'],prior_digit_reads=r['radix_digit_reads'],profile='--component-shortest-100m',
                gates='exact rows; measure both endpoint ratios <3 and preferred <2 vs archived resident. No product or scale promotion.')


def checked_tools(d):
    old=load(RUNG/'binaries.json'); new=load(d/'binaries.json')
    need(set(old)==set(new) and all(re.fullmatch('[0-9a-f]{64}',v) for v in new.values()) and all(new[k]==old[k] for k in ('lexparse','oracle')) and new['overlap_bounded_packed']!=old['overlap_bounded_packed'],'prep/verifier drift or no new producer')


def compare(d):
    r=reference(); need(load(d/'reference.json')==r and not (d/'control').exists() and not (d/'stop.requested').exists(),'not exactly one new unstopped arm')
    checked_tools(d); inp=load(d/'inputs.json'); old=load(RUNG/'inputs.json')
    need(inp.keys()==old.keys() and all(inp[k]==v for k,v in old.items() if k not in ('revision','deadline','mem_available')) and re.fullmatch('[a-f0-9]{40}',inp['revision']) and inp['revision']!=old['revision'],'input/limit drift')
    a=d/'candidate'; v,c=arm_observation(a,'candidate',inp)
    need(load(d/'result.json')['arms']=={'candidate':v} and c['sha256']==r['columns'],'incomplete arm/columns')
    io={}
    for role in ('prep','candidate','oracle'):
        m=load(a/(role+'.json')); base=load(RUNG/'candidate'/(role+'.json'))
        need(m['effective']['affinity']==base['effective']['affinity'] and m['text_pipe_bytes']==base['text_pipe_bytes'] and v['row_pipe_bytes']==load(RUNG/'candidate/result.json')['row_pipe_bytes'],'different cpu set or pipe sizes')
        argv=m['argv']; control=base['argv']; path=Path(argv[0]).parent.parent; prior=Path(control[0]).parent.parent
        expected=[z.replace(str(prior)+'/',str(path)+'/') for z in control]
        if role=='candidate':expected[1]=r['profile']
        need(argv==expected and path.name.startswith(PREFIX),'different native workload')
        io[role]=io_delta(stat_io(m['before']['io.stat']),stat_io(m['after']['io.stat']))
    lines=small(a/'producer.log').decode().splitlines(); prior=small(RUNG/'candidate/producer.log').decode().splitlines()
    need(len(lines)==4 and lines[0]==prior[0],'different array plan or unexpected dry order')
    emit=re.fullmatch(r'compact passes=(\d+)\+(\d+) digit_reads=(\d+) heap_cap=8192',lines[2]); done=re.fullmatch(r'bounded rows=(\d+) max_ties=(\d+) ranges=(\d+)',lines[3]); need(emit and done and int(done[1])==N+1 and int(done[2])==r['expected_peak'] and int(done[3])==int(emit[2]),'emitted group/row witness')
    passes=int(emit[1])+int(emit[2]); need(0<int(emit[1])<passes<=32,'emission pass cap')
    s=re.fullmatch(r'shortest entries=(\d+) payload_bytes=(\d+) exact_peak=(\d+) heap_cap=8192 dry_order_needed=false seconds=([0-9.]+)',lines[1]); need(s and tuple(map(int,s.groups()[:3]))==(K-1,r['expected_shortest_payload'],r['expected_peak']) and 0<float(s[4])<v[CLOCKS[0]],'missing actual shortest-key proof/clock')
    z=v['native_by_stage']; comparisons={}
    for a0 in ('control','candidate'):
        ref=load(RUNG/a0/'result.json'); native=ref['native_by_stage'][a0]; w={k:v[k]/ref[k] for k in CLOCKS}
        need(all(math.isfinite(x) and x>0 for x in w.values()),'wall ratio')
        comparisons[a0]=dict(wall=w,both_under_3x=max(w.values())<3,both_under_2x=max(w.values())<2,producer_cpu_ratio=z['candidate']['cpu_s']/native['cpu_s'],
            producer_peak_ratio=z['candidate']['cgroup_peak']/native['cgroup_peak'],prepared_producer_io_delta={k:z['candidate'][k]-native[k] for k in ('read_bytes','write_bytes')})
    return dict(scope='one new 100 MB stream; separately timed archived controls, unequal RAM vs resident; no reruns or product promotion',
        rows=N+1,narrow_sha256=inp['pin']['narrow_row_sha256'],actual_largest_class=int(s[3]),shortest_census_s=float(s[4]),shortest_payload_bytes=int(s[2]),
        avoided_full_classifications=r['prior_classifications']-passes*U,avoided_full_passes=r['prior_classifications']//U-passes,avoided_radix_digit_reads=r['prior_digit_reads']-int(emit[3]),emitting_order_counter_prediction_matched=lines[2:]==prior[2:],
        wall={k:v[k] for k in (*CLOCKS,'prep_s','raw_phase_sum_s')},native_by_stage=z,per_device_io=io,compared_to_archived=comparisons,product_promotion=False)

if __name__=='__main__':
    if len(sys.argv)==2 and sys.argv[1]=='--reference': value=reference()
    elif len(sys.argv)==2:value=compare(Path(sys.argv[1]))
    else:raise SystemExit('expected --reference or a measured single arm parent directory')
    print(json.dumps(value,indent=2,sort_keys=True))
