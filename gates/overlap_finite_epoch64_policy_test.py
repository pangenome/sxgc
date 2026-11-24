"""Synthetic receipts only. Full wire traffic differs from the live rolling-file cap."""
from pathlib import Path
import shutil
import tempfile
import overlap_finite_epoch64_study as e
from overlap_finite_emit64_policy_test import rejects
g=e.g

def run():
    base=e.control();predict=e.prediction();assert e.original().stat().st_size==predict['total_file_upper'] and predict['epoch_upper_if_every_range_fits']==4 and e.model(10**11,3181334488,1,10**11,192000000)['unspent_finish_reserve']==124545129792
    with tempfile.TemporaryDirectory() as td:
        d=Path(td);save=lambda k,v:e.s.save(d/(k+'.json'),v)
        for k in ('tools.json','pair4.json','oracle4.log','oracle4.json','emit4.json','cold4.json','admission.json','setup.json','identity.json'):shutil.copyfile(e.BANK/k,d/k)
        peak=49999872;a=dict(base['phases'],counted=e.HELD+peak);p=g.load(d,'admission.json');p.update(emit_mode='epochs',control_score=e.CONTROL,counted_upper=e.LIVE,nonposting_reserved=e.HELD);save('admission',p);er=g.load(d,'emit4.json');i=g.load(d,'identity.json');i['control_stat']=er['route_stat'].copy();er['route_stat'][1]+=1;last=a['wire']-3*peak;er['route_stat'][2]=last;i['candidate_stat']=er['route_stat'];er['argv'][1]='epochs';er['argv'][7]=str(e.HELD);er['counted_sampled']=peak+8*g.M+300000;save('emit4',er);o=g.load(d,'oracle4.json');o['counted_sampled']=er['counted_sampled'];save('oracle4',o);hs=[s*64 for s in '1234'];i.update(control_sha256=base['posting_sha256'],slot_sha256=hs.copy(),verified_bytes=a['wire'],last_byte_compared=last,separate_all_epoch_image_proof={'wall_s':.4});save('identity',i)
        b=base['phases'];rows=[];for_pop=b['wire']-b['populated']
        for idx in range(4):
            z=peak if idx<3 else last;lo=idx*748;hi=lo+748 if idx<3 else b['bands'];rows.append(dict(i=idx,lo=lo,hi=hi,bytes=z,interior=4096,desc=28000000 if idx<3 else g.U-84000000,rows=29500000 if idx<3 else g.N-88500000,waves=187 if idx<3 else 61,populated=z if idx<3 else z-for_pop,sha256=hs[idx]))
        q=dict(epochs=4,file_cap=e.CAP,peak_extent=peak,total_extent=a['wire'],nonposting_reserved=e.HELD,encode_offset_visits=4*g.U,hash_readback=a['wire'],hash_s=.2)
        def dump():
            fields=[('EPOCH_PART',r) for r in rows]+[('EPOCH_INDEX',q),('DIRECT_RANGES',g.fields(e.BANK,'DIRECT_RANGES')),('FINITE_EMIT',a),('EMIT_TILES',base['tiles'])];(d/'emit4.log').write_text(''.join(k+' '+' '.join(f'{x}={y}' for x,y in v.items())+'\n' for k,v in fields))
        dump();assert e.score(d)['component_goal_met'];pair=e.load(d,'pair4.json');pair['prepared_to_verified_EOF_s']*=1.4;save('pair4',pair);assert e.score(d)['space_goal_met'] and not e.score(d)['component_goal_met'];q['encode_offset_visits']-=g.U;dump();rejects(lambda:e.score(d));q['encode_offset_visits']+=g.U;rows[1]['lo']+=1;dump();rejects(lambda:e.score(d));rows[1]['lo']-=1;dump();i['slot_sha256'][0]='d'*64;save('identity',i);rejects(lambda:e.score(d))
    print('PASS four-epoch width/fit prediction, independent traffic/live guards, no hidden rewalk or seam gap, full posting witness required; slow arm retained')
if __name__=='__main__':run()
