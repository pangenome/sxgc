"""No corpus read: fixed inputs, file bound, unchanged predecessor and synthetic scorer rejection paths."""
from pathlib import Path
import shutil
import tempfile
import overlap_finite_direct64_study as g
from overlap_finite_emit64_policy_test import rejects

def run():
    for d in (600,1001):rejects(lambda:g.window(1000,1000+d))
    for d in (601,1000):g.window(1000,1000+d)
    assert g.BANDS==2486 and g.BOUND==199532024<=2*g.N and set(g.sources())=={'raw','inverse.u64','oracle-raw'};prior=g.prior()
    with tempfile.TemporaryDirectory() as td:
        d=Path(td);save=lambda k,v:g.s.save(d/(k+'.json'),v)
        for k in ('tools.json','pair4.json','oracle4.log','oracle4.json','emit4.json','cold4.json'):shutil.copyfile(g.BANK/k,d/k)
        p=g.load(g.BANK,'admission.json');p.update(prior=g.PIN,counted_upper=g.BOUND,desired_descriptors=g.TARGET,start_unix=p['deadline']-700);hs={k:h for k,(_,_,h) in g.sources().items()};p['hashes']=hs;save('admission',p);save('setup',dict(resident_pages=0,hashes=hs,private_authenticated_copy_cold={'wall_s':.5}));s=g.load(d,'emit4.json');s['io_caps']=[12*g.N,g.BOUND+(8<<20)];save('emit4',s);pair=g.load(d,'pair4.json');pair['prepared_to_verified_EOF_s']=.40*prior['prepared_to_verified_EOF_s'];save('pair4',pair)
        a=dict(prior['phases']);a.update(decoded=g.U,bands=g.BANDS,leaves=g.BANDS,order_wall_s=30,order_lane_s=95);f=dict(explicit_write=a['wire'],samples=64*g.BANDS,target_bands=g.BANDS,bands=g.BANDS,guaranteed_wire_upper=g.FILE_UP,max_range_descriptors=45000,max_range_rows=55000)
        def write():
            (d/'emit4.log').write_text('DIRECT_RANGES '+' '.join(f'{k}={v}' for k,v in f.items())+'\nFINITE_EMIT '+' '.join(f'{k}={v}' for k,v in a.items())+'\n')
        write();assert g.score(d)['component_goal_met'];a['decoded']=3*g.U;write();assert not g.score(d)['routing_hypothesis'];f['guaranteed_wire_upper']+=1;write();rejects(lambda:g.score(d));print('PASS one-shot direct gate bounds, prior score unchanged, mechanism miss retained, invalid wire bound refused')
if __name__=='__main__':run()
