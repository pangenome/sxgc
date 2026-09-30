#!/usr/bin/env python3
"""Read published SXI directories and compare exact member and file bytes."""
import json
from pathlib import Path
from struct import unpack_from

here=Path(__file__).parent
sources={
    'yeast235':Path('/home/erikg/worktrees/sxgc/pi-worktree-e7286239-b261-424c-8585-f2604e5d83e0-s0-0/vendor/byte-remap-gates/yeast235.sxi'),
    'pile-frag':Path('/home/erikg/worktrees/sxgc/pi-worktree-e7286239-b261-424c-8585-f2604e5d83e0-s0-0/vendor/byte-remap-gates/pile-frag.sxi'),
    'k10':Path('/mnt/nvme3n1/erikg/sxgc-k10sep/from-zero-p_fwd37s/k10.sxi'),
}
def directory(path):
    with path.open('rb') as f:
        h=f.read(64)
        count=unpack_from('<I',h,32)[0]
        d=f.read(40*count)
    assert len(d)==40*count and path.stat().st_size==unpack_from('<Q',h,40)[0]
    return {'magic':h[:4].decode(),'version':unpack_from('<I',h,4)[0],
            'n':unpack_from('<Q',h,8)[0],'r':unpack_from('<Q',h,24)[0],
            'bytes':path.stat().st_size,
            'members':{str(unpack_from('<I',d,40*i)[0]):{
                'codec':unpack_from('<I',d,40*i+4)[0],
                'bytes':unpack_from('<Q',d,40*i+16)[0],
                'count':unpack_from('<Q',d,40*i+24)[0]}
                for i in range(count)}}
out=[]
for label,old_path in sources.items():
    new_path=here/f'{label}.sxi2'
    if not new_path.exists():continue
    old,new=directory(old_path),directory(new_path)
    assert old['magic']=='SXI1' and new['magic']=='SXI2' and new['version']==3
    assert old['n']==new['n'] and old['r']==new['r']
    assert '2' not in new['members'] and '3' not in new['members']
    out.append({'artifact':label,'sxi1_bytes':old['bytes'],'sxi2_bytes':new['bytes'],
                'ratio':new['bytes']/old['bytes'],'new_members':new['members'],
                'space_gate':'PASSED' if new['bytes']<old['bytes'] else 'FAILED'})
print(json.dumps(out,indent=2))
