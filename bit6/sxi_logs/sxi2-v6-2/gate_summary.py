#!/usr/bin/env python3
"""Independent recheck of retained v6-2 space, chi, MEM, and HTTP evidence."""
import hashlib
import json
from pathlib import Path

here=Path(__file__).parent
v5=here.parent/'sxi2-v5'
pre={x['artifact']:x for x in json.loads((here/'space-preflight.json').read_text())}
sizes={x['artifact']:x for x in json.loads((here/'achieved-sizes-final.json').read_text())}
labels={'yeast235':'yeast','pile-frag':'pile','k10':'k10'}
out=[]
for corpus,label in labels.items():
    p=pre[corpus];s=sizes[corpus]
    native_old=(v5/(label+'-native-old.jsonl')).read_bytes()
    native_new=(here/(label+'-native-new.jsonl')).read_bytes()
    h=json.loads((here/(corpus+'-http-gate.json')).read_text())
    reference=corpus+'-bench-http-gate.json' if corpus=='pile-frag' else corpus+'-http-gate.json'
    v5h=json.loads((v5/reference).read_text())
    artifact=Path('/mnt/nvme3n1/erikg/sxi2-v6-2-b25eaecb')/(corpus+'.sxi2')
    with artifact.open('rb') as f: version4=f.read(8)==b'SXI2\x04\x00\x00\x00'
    row={'corpus':corpus,'preflight_below_v5':p['gate']=='PASSED',
         'version_4':version4,
         'exact_projected_bytes':s['bytes']==p['projected_bytes'],
         'below_v5':s['bytes']<p['v5_bytes'],
         'chi_exact':s['members']['5']['count']==p['chi'],
         'derived_lf_empty':s['members']['10']['bytes']==0,
         'raw_sample_members_absent':not ({'2','3'}&s['members'].keys()),
         'native_byte_parity':native_old==native_new,
         'native_sha256':hashlib.sha256(native_new).hexdigest(),
         'native_records':len(native_new.splitlines()),
         'http_byte_parity':h['byte_parity'] and h['new']['sha256']==h['old']['sha256'],
         'http_median_ms':h['new']['median_ms'],
         'v5_http_median_ms':v5h['new']['median_ms']}
    if corpus!='k10':
        old=v5/(label+'-mems-old.tsv')
        if not old.exists():old=v5/(label+'-mems2-old.tsv')
        row['bounded_byte_parity']=old.read_bytes()==(here/(label+'-bounded-new.tsv')).read_bytes()
    out.append(row)
print(json.dumps(out,indent=2))
keys=('preflight_below_v5','version_4','exact_projected_bytes','below_v5','chi_exact',
      'derived_lf_empty','raw_sample_members_absent','native_byte_parity','http_byte_parity',
      'bounded_byte_parity')
if not all(all(x.get(k,True) for k in keys) for x in out):raise SystemExit(1)
