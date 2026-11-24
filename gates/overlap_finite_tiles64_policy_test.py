"""No corpus read. Match exact work/controls, retain a failed speed target, reject inconsistent coverage."""
import shutil
import tempfile
from pathlib import Path
import overlap_finite_tiles64_study as t
from overlap_finite_emit64_policy_test import rejects
g=t.g

def run():
    base=t.control();assert t.original().is_file() and t.P.mode=='tiled' and t.P.prefix!=g.DIRECT.prefix and t.P.bank!=g.OUT
    with tempfile.TemporaryDirectory() as td:
        d=Path(td);save=lambda k,v:g.s.save(d/(k+'.json'),v)
        for k in ('tools.json','pair4.json','oracle4.log','oracle4.json','emit4.json','cold4.json','admission.json','setup.json'):shutil.copyfile(t.BANK/k,d/k)
        p=g.load(d,'admission.json');p.update(emit_mode='tiled',control_score=t.CONTROL);save('admission',p);e=g.load(d,'emit4.json');e['argv'][1]='tiled';control_stat=e['route_stat'].copy();e['route_stat'][1]+=1;save('emit4',e);save('identity',dict(candidate_stat=e['route_stat'],control_stat=control_stat,equal_bytes=base['phases']['wire'],sha256='f'*64,after=dict(e['after'],**{'memory.max':str(192<<20),'cpu.max':'100000 100000'})));q=g.load(d,'pair4.json');q['prepared_to_verified_EOF_s']*=.85;save('pair4',q)
        a=dict(base['phases'],publish_wall_s=20,produce_lane_s=70,queue_full_lane_s=18,packets=65536,writes=65600);tiles=dict(grain=1536,tiles=64500,max_tile_descriptors=9200,max_tile_rows=10000,extension_descriptors=311,tiles_above_ring=1,plan_s=.04)
        def dump():
            (d/'emit4.log').write_text(''.join(tag+' '+' '.join(f'{k}={v}' for k,v in vals.items())+'\n' for tag,vals in [('DIRECT_RANGES',base['direct']),('FINITE_EMIT',a),('EMIT_TILES',tiles)]))
        dump();assert t.score(d)['component_goal_met'];q['prepared_to_verified_EOF_s']/=.85;save('pair4',q);assert t.score(d)['publication_hypothesis'] and not t.score(d)['component_goal_met'];a['publish_wall_s']=30;dump();assert not t.score(d)['publication_hypothesis'];a['local_scans']+=1;dump();rejects(lambda:t.score(d));a['local_scans']-=1;tiles['tiles']=a['packets']+1;dump();rejects(lambda:t.score(d))
    print('PASS pinned whole-range control, one tile arm, mechanism and wall misses retained; changed routing and invalid packet coverage refused')
if __name__=='__main__':run()
