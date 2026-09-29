import json,pathlib,numpy as np
h=pathlib.Path(__file__).resolve().parent
out=[]
for line in (h/'measurements.jsonl').read_text().splitlines():
    row=json.loads(line)
    if row.get('stage')!='measurement':continue
    prefix=h/'work'/row['tag']
    data=np.memmap(str(prefix)+'.dict',dtype='u1',mode='r')
    lengths=np.diff(np.r_[-1,np.flatnonzero(data==1)])-1
    occ=np.memmap(str(prefix)+'.occ',dtype='<u4',mode='r')
    assert len(lengths)==len(occ)
    total=int(occ.sum(dtype=np.uint64))
    assert total==row['parse_entries'],(row['tag'],total,row['parse_entries'])
    order=np.argsort(lengths);cdf=occ[order].cumsum(dtype=np.uint64)
    quantiles={str(p):int(lengths[order[np.searchsorted(cdf,max(1,total*p),side='left')]]) for p in [.5,.9,.99,1.]}
    out.append(dict(tag=row['tag'],occurrences=total,weighted_mean=float(np.dot(lengths.astype(np.float64),occ)/total),weighted_quantiles=quantiles))
(h/'phrase-statistics.json').write_text(json.dumps(out,indent=2)+'\n')
