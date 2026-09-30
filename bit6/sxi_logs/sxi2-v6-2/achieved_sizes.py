#!/usr/bin/env python3
"""Read published v4 directories and compare exact bytes to preflight/v5."""
import json
from pathlib import Path
from struct import unpack_from

here=Path(__file__).parent
dest=Path('/mnt/nvme3n1/erikg/sxi2-v6-2-b25eaecb')
pre={x['artifact']:x for x in json.loads((here/'space-preflight.json').read_text())}
def directory(path):
    with path.open('rb') as f:
        h=f.read(64);count=unpack_from('<I',h,32)[0];d=f.read(40*count)
    assert len(d)==40*count and path.stat().st_size==unpack_from('<Q',h,40)[0]
    return {'version':unpack_from('<I',h,4)[0], 'n':unpack_from('<Q',h,8)[0],
            'r':unpack_from('<Q',h,24)[0], 'bytes':path.stat().st_size,
            'members':{str(unpack_from('<I',d,40*i)[0]):{
                'codec':unpack_from('<I',d,40*i+4)[0],
                'bytes':unpack_from('<Q',d,40*i+16)[0],
                'count':unpack_from('<Q',d,40*i+24)[0]} for i in range(count)}}
out=[]
for name,row in pre.items():
    path=dest/(name+'.sxi2')
    if not path.exists(): continue
    d=directory(path)
    assert d['version']==4 and d['n']==row['n'] and d['r']==row['r']
    assert d['bytes']==row['projected_bytes'] and d['bytes']<row['v5_bytes']
    assert {k:v['bytes'] for k,v in d['members'].items()}==row['members']
    assert d['members']['5']['count']==row['chi'] and d['members']['10']['bytes']==0
    out.append({'artifact':name,'bytes':d['bytes'],'v5_bytes':row['v5_bytes'],
                'v5_ratio':d['bytes']/row['v5_bytes'],
                'phi_lf_bits_per_run':8*(d['members']['8']['bytes']+d['members']['10']['bytes'])/row['r'],
                'members':d['members'],'gate':'PASSED'})
print(json.dumps(out,indent=2))
