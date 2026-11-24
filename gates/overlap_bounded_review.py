"""Offline review of the fixed 100 MB control/compact pair. No worker, payload, or product reads."""
import hashlib
import json
import math
from pathlib import Path
import re
import sys

BANK = Path(__file__).resolve().parent / 'receipts'
RUNG = BANK / 'bounded-component-100m'
N, M, K, T, U = 100000503, 3170531, 2689467, 127708112, 95434516
Q = 4 << 20
ARMS = ('control', 'candidate')
CAP = dict(control=2048, candidate=512, oracle=140, prep=192)
STAGES = ('cold-raw.json', 'cold-columns.json', 'prep.json', 'prep.log', 'producer.log', 'oracle.json', 'oracle.log', 'exits.json', 'result.json')


def need(cond, why):
    if not cond:
        raise ValueError(why)


def small(p):
    need(not p.is_symlink() and p.is_file() and p.stat().st_size <= 131072, 'not a small receipt: '+str(p))
    return p.read_bytes()


def sha(p):
    return hashlib.sha256(small(p)).hexdigest()


def load(p):
    return json.loads(small(p))


def arm_observation(p, a, inp):
    """Common endpoint/grant/identity checks; standalone meter files, no live input."""
    v = load(p/'result.json'); e = load(p/'exits.json'); pin = inp['pin']
    need(e == dict(producer=0,oracle=0,wall_s=v['prepared_to_verified_EOF_s']), 'peer exits')
    need(load(p/'cold-raw.json') == dict(raw_bytes=N,oracle_bytes=N,raw_sha256=pin['raw_sha256'],mincore_resident_pages=0), 'raw pin/cold')
    c = load(p/'cold-columns.json'); need(c['mincore_resident_pages'] == 0 and c['sha256'].keys() == {'dictionary.packed','parse.u32'} and all(re.fullmatch('[0-9a-f]{64}',h) for h in c['sha256'].values()), 'column shape/cold')
    prep_bound = N+20*M-8+Q; prepared_bound=T+4*M+Q
    prep = small(p/'prep.log').decode(); need(f'dictionary_bytes={T} parse_bytes={4*M} refs_written=0' in prep and f'n={N} phrases={M} distinct={K} descriptors={U} ' in prep and f'peak_counted={prep_bound-Q} final_columns={prepared_bound-Q} ' in prep and v['prep_counted_peak']==prep_bound and v['prepared_bound']==prepared_bound, 'prep shape or scratch')
    need(small(p/'oracle.log').decode().strip() == f'VERIFIED rows={N+1} final_rank={N+1} narrow_sha256={pin["narrow_row_sha256"]}', 'raw-row oracle')
    secs = (v['prep_s'], e['wall_s'], v['raw_to_verified_EOF_s']); need(all(math.isfinite(s) and s>0 for s in secs) and v['raw_phase_sum_s']==sum(secs[:2])<=secs[2] and 0<v['row_pipe_bytes']<=1<<20, 'component clocks or pipe')
    for role in ('prep',a,'oracle'):
        m=load(p/(role+'.json')); stats=v['native_by_stage'][role]; proc=m['process']; bound=CAP[role]<<20
        need(m['rc']==0 and 'error' not in m and 0 < m['wall_s'] <= (secs[0] if role=='prep' else secs[1]) and 0<m['text_pipe_bytes']<=1<<20, 'worker exit/clock')
        for cg in (m['before'],m['after']):
            need(cg['cpu.max']=='100000 100000' and cg['memory.max']==str(bound) and cg['memory.swap.max']=='0' and 0<int(cg['memory.peak'])<=bound and {'oom 0','oom_kill 0'}<=set(cg['memory.events'].splitlines()), 'grant/event')
            need(cg['io.stat'] is None or isinstance(cg['io.stat'],str), 'unrecognized device evidence')
        need(stats==dict(cgroup_peak=int(m['after']['memory.peak']),sampled_hwm=m['sampled_hwm'],**{k:proc[k] for k in ('read_bytes','write_bytes','cpu_s')}) and 0<stats['sampled_hwm']<=bound and proc['cpu_s']>0 and 0<=proc['read_bytes']<=inp['read_limits'][role] and 0<=proc['write_bytes']<=inp['write_limits'][role], 'process attribution or cap')
    return v,c


def review(d):
    src = load(d/'source.json'); rev = src['revision']; inp = load(d/'inputs.json'); result = load(d/'result.json')
    names = set('status.json inputs.json result.json binaries.json build.log lease.json'.split())
    names.update(a+'/'+f for a in ARMS for f in (*STAGES, a+'.json'))
    need(names == src['artifacts'].keys() and all(sha(d/f) == h for f,h in src['artifacts'].items()), 'archive hashes')
    need(src['exit_code'] == 0 and inp['revision'] == rev == 'e2f0c54aa364e71056366400f6fbaebb4d38e5a5' and load(d/'status.json') == dict(deadline=inp['deadline'], qualified=True), 'admission or exit')
    need(load(d/'binaries.json') == src['worker_sha256'] and sha(d/'source-pin.json') == '51b6d8552601cc7e67e985614663fcc65453e13a1552a5b36469c5584487b01d' and sha(d/'provenance.json') == '871c6d432e1570f93ddb45ec753f1cc6171cbb6ba605ce1aefbaf13426b15702', 'source/worker pin')
    pin = load(d/'source-pin.json'); digest = pin['narrow_row_sha256']; ref = load(BANK/'radix-emit-100m/result.json')
    prep_bound = N + 20*M - 8 + Q; prepared_bound = T + 4*M + Q
    need(inp['pin'] == pin and pin['n'] == N and (ref['rows'],ref['digest']) == (N+1,digest) and inp['cgroup_MiB'] == CAP and inp['array_grants'] == dict(candidate=320<<20, control=1536<<20), 'rung contract')
    need((inp['prep_scratch'],inp['prepared_scratch'],inp['limit']) == (prep_bound,prepared_bound,2*N) and max(prep_bound,prepared_bound)<=2*N, 'scratch bound')
    arms = {}; columns = None
    for a in ARMS:
        v,c=arm_observation(d/a, a, inp)
        need(result['arms'][a] == v and (not columns or columns==c), 'different results or columns')
        columns=c; arms[a]=v
    control, candidate = (arms[a] for a in ARMS)
    ratios={k:candidate[k]/control[k] for k in ('prep_s','prepared_to_verified_EOF_s','raw_to_verified_EOF_s')}
    need(ratios==result['candidate_over_control_wall'], 'wall comparison')
    native=small(d/'candidate/producer.log').decode().splitlines(); m=re.fullmatch(r'bounded arrays parse=(\d+) PhaseBytes \{ aligned: (\d+), lists: (\d+), emit: (\d+) \} arena=(\d+)',native[0]); need(m and max(map(int,m.groups()[:4]))<=inp['array_grants']['candidate'] and int(m[5])==64<<20,'candidate phase plan')
    pre=re.fullmatch(r'compact admission_passes=(\d+)\+(\d+) admission_digit_reads=(\d+) window_probes_bound=(\d+)',native[1]); emit=re.fullmatch(r'compact passes=(\d+)\+(\d+) digit_reads=(\d+) heap_cap=(\d+)',native[2]); g=re.fullmatch(r'bounded rows=(\d+) max_ties=(\d+) ranges=(\d+)',native[3]); need(pre and emit and g and pre.groups()[:3]==emit.groups()[:3] and int(g[1])==N+1 and int(g[3])==int(emit[2]) and int(pre[4])==0 and int(g[2])<=int(emit[4])==8192,'order work witness')
    cl=small(d/'control/producer.log').decode(); stream=re.search(r'RESIDENT_STREAM rows=(\d+) descriptors=(\d+) groups=(\d+) max_group=(\d+) ',cl); need(stream and (int(stream[1]),int(stream[2]),int(stream[4]))==(N+1,U,int(g[2])) and f'RESIDENT_CHECK recurrence_ranks={M} virtual_end=0' in cl,'reference stream/recurrence')
    passes=2*(int(emit[1])+int(emit[2])); cap=int(emit[4]); cost=((M+cap)//cap)*2*(M.bit_length()+1)
    p,q=candidate['native_by_stage']['candidate'],control['native_by_stage']['control']; performance=max(ratios[k] for k in ('raw_to_verified_EOF_s','prepared_to_verified_EOF_s'))
    return dict(scope='100 MB raw-to-verified stream only, unequal producer memory; no product or larger-rung clearance',rows=N+1,narrow_sha256=digest,columns=columns['sha256'],wall_ratios=ratios,producer_cpu_ratio=p['cpu_s']/q['cpu_s'],producer_cgroup_peak_ratio=p['cgroup_peak']/q['cgroup_peak'],producer_read_byte_difference=p['read_bytes']-q['read_bytes'],producer_write_byte_difference=p['write_bytes']-q['write_bytes'],counted_preparation_peak=prep_bound,counted_prepared_reservation=prepared_bound,
        finite_groups=int(stream[3]),largest_finite_group=int(g[2]),classifications=U*passes,full_descriptor_passes=passes,radix_digit_reads=2*int(emit[3]),
        window_probe_upper_per_oversized_descriptor=cost,minimum_oversized_group_probe_upper=(cap+1)*cost,observed_oversized_groups=0,under_3x_component_wall=performance<3,under_2x_component_wall=performance<2,product_promotion=False)

if __name__=='__main__':
    print(json.dumps(review(Path(sys.argv[1]) if len(sys.argv)>1 else RUNG),indent=2,sort_keys=True))
