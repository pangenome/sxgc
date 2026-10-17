#!/usr/bin/env python3
"""Fixture-distribution projection; no suffix positions or text are persisted."""
import json,math
from pathlib import Path
import numpy as np
base=Path('/mnt/nvme2n1/erikg');path=base/'extcols-scratch/mergperf-1b-snap.txt'
mapping=np.frombuffer((base/'sol6c-gate100m/remap-256.bin').read_bytes(),dtype=np.uint8)
counts=np.zeros(257*257,dtype=np.uint64);n=path.stat().st_size
with path.open('rb') as f:
    pending=f.read(1<<20)
    while pending:
        following=f.read(1<<20);a=mapping[np.frombuffer(pending,dtype=np.uint8)].astype(np.uint32)+1
        b=np.empty_like(a);b[:-1]=a[1:];b[-1]=int(mapping[following[0]])+1 if following else 0
        counts+=np.bincount(a*257+b,minlength=257*257).astype(np.uint64);pending=following
assert int(sum(counts))==n
pile=1310000000000;cap=32000000000;scaled=[math.ceil(int(c)*pile/n) if c else 0 for c in counts]
segments=[];total=0;oversized=0
for c in scaled:
    if c>cap:
        if total:segments.append(total);total=0
        oversized+=1;segments.extend([cap]*(c//cap));total=c%cap
    elif total+c>cap:segments.append(total);total=c
    else:total+=c
if total:segments.append(total)
result=dict(source_suffixes=n,source_prefix_buckets=int(np.count_nonzero(counts)),pile_bytes=pile,position_cap=cap,position_bytes_per_segment=8*cap,projected_segments=len(segments),projected_sweeps=1+len(segments),projected_text_read_bytes=pile*(1+len(segments)),projected_text_read_seconds=pile*(1+len(segments))/2e9,oversized_prefix_buckets=oversized,max_projected_prefix_bucket=max(scaled),qualification='1GB prefix distribution extrapolation; not pile measurement; oversized selection scans excluded')
print(json.dumps(result,indent=2))
