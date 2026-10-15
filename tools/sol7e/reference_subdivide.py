#!/usr/bin/env python3
"""Derive a coarser reference from the same planner's nested targets.

No walk work: R(s,P) equals R(s*Q/P,Q). Production validates all derived
cursors and source identities. Serial transition costs add by range.
"""
import struct, sys
from pathlib import Path
source=Path(sys.argv[1]);p=int(sys.argv[2]);target=Path(sys.argv[3])
data=source.read_bytes();u=lambda b:struct.unpack('<Q',b)[0]
length=u(data[:8]);identity=data[8:8+length]
magic=b'sol7e-range-v1';assert identity.startswith(magic)
off=len(magic);n,k,q,dollar,spacing=struct.unpack('<5Q',identity[off:off+40]);assert q%p==0 and 1<p<q
stride=q//p;a=off+40;end=a+8*(q+1)*k
boundaries=[identity[a+8*i*k:a+8*(i+1)*k] for i in range(q+1)]
newid=magic+struct.pack('<5Q',n,k,p,dollar,spacing)+b''.join(boundaries[::stride])+identity[end:]
refs=struct.unpack('<'+str(3*q)+'Q',data[8+length:]);coarse=[]
for i in range(p):
    coarse.extend(sum(refs[3*j+m] for j in range(i*stride,(i+1)*stride)) for m in range(3))
assert not target.exists()
target.write_bytes(struct.pack('<Q',len(newid))+newid+struct.pack('<'+str(3*p)+'Q',*coarse))
print(f'REFERENCE_COARSEN_PASS from_P={q} to_P={p} serial_range_costs=additive split_identity=validated_on_use')
