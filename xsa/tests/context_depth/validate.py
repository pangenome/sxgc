#!/usr/bin/env python3
"""Recheck threshold endpoints and measure per-depth round-trip latency."""
import json, pathlib, struct, sys
from study import Server, quant
exe,index,corpus,out=sys.argv[1:]; out=pathlib.Path(out)
rows=[json.loads(s) for s in (out/'observations.jsonl').read_text().splitlines()]
assert len(rows)==1000
(out/'validation').mkdir(exist_ok=True)
s=Server(exe,index,'branches',[],out/'validation'); f=open(corpus,'rb')
def ask(pos,d):
    f.seek(pos-d); r=s.ask(f.read(d))
    _,l,z,w,eof,other=struct.unpack_from('<6Q',r)
    n,=struct.unpack_from('<H',r,48)
    counts={r[50+9*j]:struct.unpack_from('<Q',r,51+9*j)[0] for j in range(n)}
    assert sum(counts.values())+eof+other==w and other==0
    return w,eof,counts
checks=0
for row in rows:
    for k,d in row['support'].items():
        if d: assert ask(row['position'],d)[0]>=int(k)+1
        if d<row['position']: assert ask(row['position'],d+1)[0]<int(k)+1
        checks+=1
oracle=[]
for line in (out/'oracle.tsv').read_text().splitlines():
    idx,d,w,eof,pairs=line.split('\t'); counts={int(b):int(c) for b,c in (p.split(':') for p in pairs.split(','))}
    assert ask(rows[0]['position'],int(d))==(int(w),int(eof),counts)
    oracle.append({'depth':int(d),'count':int(w),'distinct_next_bytes':len(counts),'status':'exact histogram parity'})
assert len(oracle)==2
cost={}
for d in [2**i for i in range(11)]:
    values=[]
    for row in rows:
        ask(row['position'],d); values.append(s.times[-1])
    cost[str(d)]=quant(values)
s.close()
(out/'validation.json').write_text(json.dumps({'threshold_checks':checks,'oracle':oracle,'branches_roundtrip_us_by_depth':cost},indent=2)+'\n')
print('PASS',checks,'threshold endpoints; oracle full histogram parity;',len(rows)*11,'depth latency queries')
