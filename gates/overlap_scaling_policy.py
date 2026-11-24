"""Predeclared experiment order and measured default policy; no 100 GB dispatch.
No component timing is substituted for construction wall. Score only finished
raw builds with the independent concurrent row check and exact v8 product.
"""
import argparse
import contextlib
import io
import json
import tempfile
from pathlib import Path

from overlap_radix_product_score import metrics as one_metrics, score as score_lanes, score_batch
from overlap_resident_gate import ONE, TEN, TEN_ARMS, save
from overlap_ten_matched_score import compare
from overlap_ten_score import get, verified_arm


def one_series(pair):
    emit=get(pair,'comparison.json');batch=get(pair,'batch.json')
    assert emit['parallel']==batch['fields'] and emit['lanes']==batch['lanes'] and emit['same_binary_and_product'] and batch['same_binary_and_product']
    paths=list(map(Path,(emit['control'],emit['parallel'],batch['block'])))
    # A portable receipt bank can stand in for the original private arm dirs.
    # Require all three mappings or none. Never silently mix unrelated arms.
    local=[pair/label for label in ('serial','parallel','batch')]
    if any(p.exists() for p in local):
        for p,original in zip(local,paths):
            source=get(p,'source.json');assert source['source']==str(original) and source['complete_arm'] is True
        paths=local
    # Recompute the actual score, not just the boolean in an input JSON file.
    with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
        dest=Path(tmp)/'score.json'
        for func,ar,reported,provenance in [(score_lanes,paths[:2],emit,('control','parallel')),(score_batch,paths[1:],batch,('fields','block'))]:
            try:func(*ar,dest)
            except SystemExit as exc:
                if exc.code!=75:raise
            actual=json.loads(dest.read_text());omit=lambda r:{k:v for k,v in r.items() if k not in provenance}
            assert omit(actual)==omit(reported),'one-GB score does not replay'
    # Reopen the actual three arm receipts. A verified MISS does not invalidate
    # the subsequent same-scale diagnostic but cannot count as a performance pass.
    stats=[one_metrics(p) for p in paths]; l=emit['lanes']; assert l in (2,4)
    assert [v[0]['profile'] for v in stats]==['packed-radix-1',f'packed-radix-{l}',f'packed-radix-{l}-batch']
    assert stats[0][2]==stats[1][2]==stats[2][2]
    return emit,batch,stats


def experiment_order(pair,grid):
    emit,batch,_=one_series(pair);g=get(grid,'result.json');t=g['selected_threads']; assert t in (1,2,4)
    assert [a['threads'] for a in g['arms']]==[1,4,2] and len({a['sha256'] for a in g['arms']})==1
    # Test parallelism at the actual 10 GB shared CPU quota. Favor two for the
    # FIRST experiment if four did not win the 1 GB end-to-end comparison.
    # Test all lane counts: publication and finish are concurrent CPU users.
    first=emit['lanes'] if emit['matched_speed_io_pass'] else 2
    b=int(batch['matched_batch_pass']);return [f'packed-10g-l{l}-b{b}-t{t}-r1' for l in (first,6-first,1)]


def select(rows):
    good=[r for r in rows if r['eligible']]
    if not good:return None
    best=min(r['raw_s'] for r in good)
    # Within 3% of fastest raw wall, avoid buying immaterial wall differences
    # with markedly more CPU. Within 3% of the least CPU there, choose the
    # smallest reorder queue. No optimality claim for untested machines/grants.
    near=[r for r in good if r['raw_s']<=best*1.03];cpu=min(r['build_cpu_s'] for r in near)
    return min((r for r in near if r['build_cpu_s']<=1.03*cpu),key=lambda r:(r['lanes'],r['raw_s']))


def evaluate(control,pair,grid,arms):
    verified_arm(control);order=experiment_order(pair,grid);_,_,small=one_series(pair)
    one=min(small,key=lambda x:x[-1]['raw'])[-1];rows=[];cross=[];single=[]
    for p in arms:
        b,_,_=verified_arm(p);assert b['profile'] in order
        c=compare('historical',control,p);cross.append(c)
        if len(arms)>1 and p!=arms[0]:single.append(compare('single-knob',arms[0],p))
        goals=get(p,'goals.json');norm={k:(b[name]/TEN.n)/(one[k]/ONE.n) for k,name in [('raw','raw_to_product_s'),('prepared','wall_s')]}
        def cpu(s):return int(dict(l.split() for l in s['cpu.stat'].splitlines())['usage_usec'])/1e6
        ratio=c['trial_over_control_wall']['raw']; post=c['trial_over_control_prepared_io']; high=max(norm.values())
        ok=(ratio<=0.95 and high<3 and max(post.values())<=1.05 and goals['promotion_under_3x'] and goals['normalized_io_pass'])
        rows.append(dict(profile=b['profile'],lanes=TEN_ARMS[b['profile']][0],path=str(p),raw_s=b['raw_to_product_s'],prepared_s=b['wall_s'],
                    build_cpu_s=cpu(b['shared_after'])-cpu(b['shared_before']),peak_shared_bytes=int(b['shared_after']['memory.peak']),
                    raw_scratch_bytes=b['raw_counted_highwater'],raw_ratio_to_historical_serial=ratio,normalized_to_fastest_verified_1g=norm,
                    post_io_ratio=post,eligible=ok,under_2x=high<2 and goals['target_under_2x']))
    assert len({r['profile'] for r in rows})==len(rows)
    winner=select(rows);complete=set(r['profile'] for r in rows)==set(order)
    return dict(kind='matched v8, cold raw, 4 build CPUs + 1 verifier, no swap',predeclared_order=order,complete_lane_sweep=complete,
                rows=rows,historical_comparisons=cross,single_knob_comparisons=single,selected=(winner['profile'] if winner and complete else None),
                performance_target_met=bool(winner and complete and winner['under_2x']),
                note='selection applies only to tested 10 GB resident v8 grant. Not a v10/100 GB fit or launch verdict.')

if __name__=='__main__':
    p=argparse.ArgumentParser();sub=p.add_subparsers(dest='cmd',required=True)
    a=sub.add_parser('order');a.add_argument('pair',type=Path);a.add_argument('grid',type=Path)
    a=sub.add_parser('score')
    for k in ('control','pair','grid','out'):a.add_argument(k,type=Path)
    a.add_argument('arms',nargs='+',type=Path);v=p.parse_args()
    if v.cmd=='order':print(*experiment_order(v.pair,v.grid))
    else:
        report=evaluate(v.control,v.pair,v.grid,v.arms);save(v.out,report)
        print('SCALING_SCORE',json.dumps({k:report[k] for k in ('complete_lane_sweep','rows','selected','performance_target_met')}),flush=True)
