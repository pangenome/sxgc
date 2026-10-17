#!/usr/bin/env python3
import json,re
from pathlib import Path
base=Path('/mnt/nvme2n1/erikg');dest=Path(__file__).parent
hist=[json.loads(line) for line in (dest/'histogram-1b.jsonl').read_text().splitlines()]
for h in hist:
    assert h['singleton_count']+h['residue_suffixes']==h['suffixes']
    bins=h['multiplicity_log2_bucket_counts']
    assert bins[0]==h['singleton_count'] and sum(bins[1:])==h['residue_buckets']
rows=[]
for size,suffix,chi in [('100m','-final',29716349),('1b','',283296933)]:
    root=base/f'sol8a-gate-{size}{suffix}';result=json.loads((root/'result.json').read_text())
    assert result['producer_rc']==result['finish_rc']==result['cmp_rc']==0
    output=(root/'finish.log').read_text();assert 'SXI2_PASS' in output
    assert re.search(r'PILE_FINISH_DONE chi='+str(chi)+r'\b',output)
    lines=(root/'producer.log').read_text().splitlines();workline=next(line for line in lines if line.startswith('RADIX_WORK '))
    work={key:int(value) for key,value in re.findall(r'(\w+)=(\d+)',workline)}
    n=(base/'extcols-scratch'/f'mergperf-{size}-snap.txt').stat().st_size
    assert work['key_ops']==work['singleton']+work['residue']==n
    assert work['key_bytes']==8*n-28 and work['selection_passes']==0
    assert work['sweeps']==work['segments']+1 and work['sweep_bytes']==work['prefix_ops']==n*work['sweeps']
    assert work['peak_position_slots']<=2*work['position_cap']
    assert work['peak_position_record_bytes']==16*work['peak_position_slots']
    work['derived_radix_digit_ops']=2*9*work['key_ops']
    work['derived_radix_offset_ops']=255*9*work['segments']
    work['certified_prefix_symbols_elided']=8*work['comparisons']
    if size=='1b':
        h=next(h for h in hist if h['N']==8)
        assert work['singleton']==h['singleton_count'] and work['residue']==h['residue_suffixes']
        work['retired_merge_symbols']=60485597061
        work['symbols_ratio_new_over_retired']=work['symbols']/60485597061
        work['symbol_reduction_fraction']=1-work['symbols']/60485597061
    rows.append(dict(size=size,work=work,gate=result,workline=workline))
(dest/'counter-summary.json').write_text(json.dumps(rows,indent=2)+'\n')
(dest/'gates.log').write_text('\n'.join(f"{r['size']}: chi={29716349 if r['size']=='100m' else 283296933} producer_rc=0 finish_rc=0 cmp_rc=0\n{r['workline']}" for r in rows)+'\n')
print('SOL8A_ATTEST_PASS gates=100m,1b exact_chi=1 byte_identity=1 histogram_count_identity=1 position_bound=1 counters=1')
