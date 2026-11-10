#!/usr/bin/env python3
"""Nearest-rank percentiles; retains sample weighting instead of claiming node weights."""
import collections, csv, json, math, pathlib, statistics, sys
rows=[]
for r in csv.reader(pathlib.Path(sys.argv[1]).read_text().splitlines()):
    if r[:1]==['sample'] and r[1]!='id':
        rows.append(dict(zip(['id','len','width','degree','search_us','rank_hist_proxy_us','ladder1024_proxy_us','rank_ops','anchor_us','lf_steps','transition_hot_us','exact_rank_us'],map(float,r[1:]))))
assert len(rows)>=100, len(rows)
def stats(v):
    v=sorted(v)
    return dict(n=len(v),p50=v[math.ceil(.50*len(v))-1],p99=v[math.ceil(.99*len(v))-1],mean=statistics.mean(v))
metrics={k:stats([r[k] for r in rows if r[k]>=0 and (k!='lf_steps' or r['anchor_us']>=0)]) for k in ['search_us','rank_hist_proxy_us','ladder1024_proxy_us','anchor_us','lf_steps','transition_hot_us','exact_rank_us']}
metrics['reverse_proxy_position_us']=stats([r['rank_hist_proxy_us']+r['ladder1024_proxy_us'] for r in rows])
metrics['hybrid_position_us']=stats([r['search_us']+r['anchor_us'] for r in rows if r['anchor_us']>=0])
ladder={}
if len(sys.argv)>2:
    lr=[list(map(float,r[1:])) for r in csv.reader(pathlib.Path(sys.argv[2]).read_text().splitlines()) if r[0]=='ladder' and r[1]!='id']
    assert len(lr)>=100
    ladder={"samples":len(lr),"extend_us":stats([r[2] for r in lr]),"hist_us":stats([r[3] for r in lr]),"position_us":stats([r[2]+r[3] for r in lr]),"width_histogram":dict(collections.Counter(int(r[1]) for r in lr)),"rank_ops":dict(collections.Counter(int(r[4]) for r in lr))}
print(json.dumps(dict(ladder=ladder,samples=len(rows),anchor_lookups=sum(int(r['width']) for r in rows if r['anchor_us']>=0),anchor_lf_steps=sum(int(r['lf_steps']) for r in rows),metrics=metrics,degree_histogram=dict(sorted(collections.Counter(int(r['degree']) for r in rows).items())),by_length={str(n):dict(samples=len(v),degree_mean=statistics.mean(r['degree'] for r in v),degree_histogram=dict(sorted(collections.Counter(int(r['degree']) for r in v).items())),width=stats([r['width'] for r in v]),anchor_eligible=sum(r['anchor_us']>=0 for r in v)) for n in [8,32,128,1024] if (v:=[r for r in rows if r['len']==n])}),indent=2))
