#!/usr/bin/env python3
"""Attest completed pricing evidence; never substitutes proxies for observations."""
from pathlib import Path
import hashlib, json, math, re, subprocess
root=Path(__file__).resolve().parents[2]; evidence=root/'tools/sol8b'
hist={r['d']:r for r in map(json.loads,(evidence/'histogram-1b.jsonl').read_text().splitlines())}
boundary={r['d']:r for r in map(json.loads,(evidence/'boundary-1b.jsonl').read_text().splitlines())}
conditional={r['d']:r for r in map(json.loads,(evidence/'conditional-1b.jsonl').read_text().splitlines())}
assert set(hist)==set(boundary)==set(conditional)=={4,6,8}
for d,h in hist.items():
    c=conditional[d]; b=boundary[d]
    assert h['n']==c['n']==1000000665 and h['chunks']==c['chunks']==b['chunks']==8
    assert sum(h['contributor_mass'])==h['n']
    assert sum(h['contributor_buckets'])==h['buckets']
    assert h['one_chunk_mass']==h['contributor_mass'][1]
    assert h['all_chunks_mass']==h['contributor_mass'][8]
    lm=sum(m*math.log2(k) for k,m in enumerate(h['contributor_mass']) if k)
    assert abs(lm-h['mass_log2_contributors'])<.001
    assert h['independent_binary_searches_k8']==8*h['buckets']
    assert h['independent_binary_searches_pile']==1310*h['buckets']
    assert c['observed_merge_mass']+c['unobserved_merge_mass']+c['one_chunk_mass']==c['n']
    assert c['pile_unobserved_tail_mass']==c['n']-c['observed_merge_mass']
    assert b['searches']==b['sample_unique_keys']*8 and b['position_probes']>b['searches']
    assert b['prefix_bytes']==d*b['position_probes']
    assert b['seconds']>0
log=(evidence/'conditional-work.log').read_text()
baseline=re.search(r'HYBRID_BASELINE comparisons=(\d+) symbols=(\d+) rows=(\d+)',log)
assert baseline and tuple(map(int,baseline.groups()))==(3674629435,60485597061,1000000665)
assert not subprocess.check_output(['git','diff','f74a5e8','--','bit6/cross_lcp_merge.cpp','xsa/runtime/bit6/cross_lcp_merge.cpp'],cwd=root)
print('HYBRID_PRICING_ATTEST_PASS d=4,6,8 census=exact mass_conserved=1 conditional_coverage=accounted baseline_counters=exact repaired_boundary_samples=1 production_sources=unchanged product_gates=NOT_RUN pricing_stop=1')
