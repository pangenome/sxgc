#!/usr/bin/env python3
"""Recheck retained v5 gate outputs without rerunning expensive queries."""
import collections
import hashlib
import json
from pathlib import Path

root=Path(__file__).parent
pre={x['artifact']:x for x in json.loads((root/'space-preflight.json').read_text())}
sizes={x['artifact']:x for x in json.loads((root/'achieved-sizes-final.json').read_text())}
expected_chi={'yeast235':85404336,'pile-frag':306164765,'k10':1627067257}
labels={'yeast235':'yeast','pile-frag':'pile','k10':'k10'}
def sha(b):return hashlib.sha256(b).hexdigest()
out=[]
for corpus,label in labels.items():
    p,s=pre[corpus],sizes[corpus]
    old=(root/f'{label}-native-old.jsonl').read_bytes()
    new=(root/f'{label}-native-new.jsonl').read_bytes()
    hits=collections.Counter(json.loads(line)['read'] for line in old.splitlines())
    http=json.loads((root/f'{corpus}-http-gate.json').read_text())
    row={'corpus':corpus,'space_preflight':p['gate'],
         'exact_projection':p['sxi2_projected_bytes']==s['sxi2_bytes'],
         'achieved_space':s['space_gate'],
         'raw_sample_members_absent':not ({'2','3'}&s['new_members'].keys()),
         'chi_count':s['new_members']['5']['count'],
         'chi_count_exact':s['new_members']['5']['count']==expected_chi[corpus],
         'native_mems_byte_parity':old==new,
         'native_mems_sha256':sha(new),
         'native_mems_hits':sum(hits.values()),
         'multi_occurrence':max(hits.values())>1,
         'http_byte_parity':http['byte_parity']
             and http['old']['sha256']==http['new']['sha256'],
         'http_hits':http['new']['hits']}
    if corpus!='k10':
        a=(root/f'{label}-mems-old.tsv').read_bytes() if (root/f'{label}-mems-old.tsv').exists() else (root/f'{label}-mems2-old.tsv').read_bytes()
        b=(root/f'{label}-mems-new.tsv').read_bytes() if (root/f'{label}-mems-new.tsv').exists() else (root/f'{label}-mems2-new.tsv').read_bytes()
        row['bounded_mems_byte_parity']=a==b
    out.append(row)
print(json.dumps(out,indent=2))
if not all(all(v for k,v in x.items() if k in ('exact_projection','raw_sample_members_absent',
        'chi_count_exact','native_mems_byte_parity','multi_occurrence','http_byte_parity',
        'bounded_mems_byte_parity')) and x['space_preflight']=='PASSED'
        and x['achieved_space']=='PASSED' for x in out):raise SystemExit(1)
