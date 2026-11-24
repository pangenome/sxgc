"""No corpus. Price native type sizes against the frozen 100 MB cuts, not an RSS measurement."""
import hashlib
import json
from pathlib import Path
import subprocess as sp
import sys
from overlap_raw_view64_test import fields

BANK=Path(__file__).resolve().parent/'receipts/finite-epoch64-100m';PIN='fc87f879608719e016221b6a383b2079e09d158885be494e09f90a2cafbc7cab'

def plan(exe,n,m,k,u,target,slots,team,held):
    args=['plan',*map(str,(n,m,k,u,target,slots,team,held))]
    def run(mode):return sp.check_output([exe,mode,*args],text=True)
    z=run('staged-epochs');baseline=fields(run('epochs'),'EPOCH_PLAN');assert fields(z,'EPOCH_PLAN')==baseline
    a,b=({key:int(s) for key,s in row.items()} for row in (fields(z,'EPOCH_STAGE_BUDGET'),baseline));nb=b['desired_bands']
    # x86-64: two 512-word OutBuf (two counters each), five u64, unsigned + padding.
    assert a['per_active_band']==team*8*(2*(512+2)+5+1) and a['all_encoder_route']==a['pre_census']+nb*a['per_active_band'] and b['array_upper']==max(a['all_encoder_route'],a['emission_upper'])
    return a|dict(desired_bands=nb,unconditional_upper=b['array_upper'])

def review(exe):
    data=(BANK/'score.json').read_bytes();assert hashlib.sha256(data).hexdigest()==PIN;r=json.loads(data);a=r['phases'];parts=r['parts'];p=json.loads((BANK/'admission.json').read_bytes());assert r['component_goal_met'] and len(parts)==4 and parts[0]['lo']==0 and all(x['hi']==y['lo'] for x,y in zip(parts,parts[1:])) and parts[-1]['hi']==a['bands']
    b=plan(exe,a['n'],a['m'],a['k'],a['descriptors'],p['desired_descriptors'],p['slots'],a['team'],p['nonposting_reserved']);widths=[z['hi']-z['lo'] for z in parts];assert widths==[748,744,748,246] and b['desired_bands']==a['bands'] and a['arrays']==b['unconditional_upper']==251389144
    route=b['pre_census']+max(widths)*b['per_active_band'];conditional=max(route,b['emission_upper']);delta=b['unconditional_upper']-conditional
    assert delta>0 and route==b['all_encoder_route']-(a['bands']-max(widths))*b['per_active_band'];hundred=plan(exe,10**11,3181334488,1,10**11,38400000,192000000,4,150000000000)
    assert hundred['unconditional_upper']==149898703072
    return dict(archived_score=PIN,archived_widths=widths,native_plan_100m=b,route_if_archived_cuts=route,conditional_worker_array_upper=conditional,charge_removed=delta,remaining_of_320_MiB_if_archived_cuts=(320<<20)-conditional,dimensions_only_100g=hundred,scope='Array reservations from exact archived 100 MB epoch cuts and unchanged emitter type sizes; not a new 100 MB stream, measured allocation, lower RSS/time, shared finish grant or scale admission.')
if __name__=='__main__':
    assert __debug__ and len(sys.argv)==2;print(json.dumps(review(Path(sys.argv[1]).resolve()),indent=2,sort_keys=True))
