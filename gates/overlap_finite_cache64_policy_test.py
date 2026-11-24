"""Metadata-only admission/scoring checks. No timings or reads of corpus/route content."""
import shutil
import tempfile
from pathlib import Path
import overlap_finite_cache64_study as c
from overlap_finite_emit64_policy_test import rejects
g=c.g

def run():
    base=c.control();assert c.original().stat().st_size==base['phases']['wire'] and c.P.mode=='cached' and c.P.bank!=c.BANK
    with tempfile.TemporaryDirectory() as td:
        d=Path(td);save=lambda k,v:g.s.save(d/(k+'.json'),v)
        for k in ('tools.json','pair4.json','oracle4.log','oracle4.json','emit4.json','cold4.json','admission.json','setup.json','identity.json'):shutil.copyfile(c.BANK/k,d/k)
        p=g.load(d,'admission.json');p.update(emit_mode='cached',control_score=c.CONTROL);save('admission',p);e=g.load(d,'emit4.json');i=g.load(d,'identity.json');i['control_stat']=e['route_stat'].copy();e['route_stat'][1]+=1;i['candidate_stat']=e['route_stat'];e['argv'][1]='cached';save('emit4',e);save('identity',i)
        q=g.load(d,'pair4.json');q['prepared_to_verified_EOF_s']*=.75;save('pair4',q);a=dict(base['phases'],order_wall_s=33,order_lane_s=105);tiles=dict(base['tiles']);f=g.fields(c.BANK,'DIRECT_RANGES');v=dict(payload=36,cache_capacity=512,per_lane_bytes=24576,prefix_sorts=2486,cache_runs=1_000_000,cache_desc=80_000_000,reprobes=10_000_000,whole_equal=0,slow_runs=100,slow_desc=150000,slow_comps=1500000)
        def dump():
            (d/'emit4.log').write_text(''.join(k+' '+' '.join(f'{a}={b}' for a,b in l.items())+'\n' for k,l in [('DIRECT_RANGES',f),('FINITE_EMIT',a),('EMIT_TILES',tiles),('CACHE_ORDER',v)]))
        dump();assert c.score(d)['component_goal_met'];v['slow_comps']=90_000_000;dump();assert not c.score(d)['order_hypothesis'];v['slow_comps']=1500000;q['prepared_to_verified_EOF_s']/=.75;save('pair4',q);dump();assert c.score(d)['order_hypothesis'] and not c.score(d)['component_goal_met'];tiles['extension_descriptors']+=1;dump();rejects(lambda:c.score(d));tiles['extension_descriptors']-=1;v['reprobes']=2*g.U+1;dump();rejects(lambda:c.score(d))
    print('PASS one sort treatment, unchanged tiled evidence/geometry, span-work and time misses retained, invalid bounded refill refused')
if __name__=='__main__':run()
