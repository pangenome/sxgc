"""One fixed 100 MB four-lane span-cached order against the frozen, tiled-publish endpoint."""
import json
from pathlib import Path
import sys
import overlap_finite_tiles64_study as t

g=t.g;s=t.s;need=s.need;load=s.load
BANK=t.P.bank;CONTROL='b61006c8d8af7eb2b90ae9fe38005e3ddc2185aa16d1414af7948dd301ebe825'
P=g.Profile('finite-cache64-100m-','overlap_finite_cache64_study.sh',s.RECEIPTS/'finite-cache64-100m',Path(__file__).resolve(),'cached')

def control():
    z=t.score(BANK);a=z['phases'];need(z==load(BANK,'score.json') and s.sha(BANK/'score.json')==CONTROL and z['component_goal_met'] and a['order_wall_s']>.7*z['prepared_to_verified_EOF_s'] and a['order_lane_s']-a['gather_lane_s']>150 and a['decoded']==g.U,'different matched sort hotspot');return z

def original():return t.original(BANK,t.P,'59ec7cd0354f7e5cae11227d61d03031c7780a0a',CONTROL)

def score(d):
    b=control();z,q,i,rat,busy=t.treatment(d,b,BANK,P,CONTROL);old=b['phases'];a=z['phases'];c=g.fields(d,'CACHE_ORDER');need(i['sha256']==b['posting_sha256'] and all(q[k]==v for k,v in b['tiles'].items() if not k.endswith('_s')) and a['packets']==old['packets'] and a['arrays']==old['arrays'] and a['order_lane_s']>a['gather_lane_s']>=0,'geometry, emitted tiles or reservation changed');need(all(type(x) is int and x>=0 for x in c.values()) and 0<c['payload']<=64 and c['cache_capacity']==512 and c['per_lane_bytes']==24576 and c['prefix_sorts']>0 and 2*c['cache_runs']<=c['cache_desc']<=min(512,1<<(64-c['payload']))*c['cache_runs'] and (1+min(512,1<<(64-c['payload'])))*c['slow_runs']<=c['slow_desc'] and bool(c['slow_desc'])==bool(c['slow_comps']) and c['cache_desc']+c['whole_equal']+c['slow_desc']<=g.U and c['reprobes']<=2*g.U,'cache width, leaf or probe accounting')
    probes=c['cache_desc']+c['reprobes']+2*c['slow_comps'];rat.update(order_wall=a['order_wall_s']/old['order_wall_s'],sort_only_lane_s=(a['order_lane_s']-a['gather_lane_s'])/(old['order_lane_s']-old['gather_lane_s']));mech=2*probes<=3*g.U and rat['sort_only_lane_s']<=.60 and rat['order_wall']<=.65;met=mech and rat['wall']<=.80 and max(rat['read_bytes'],rat['write_bytes'])<=1.05
    return dict(verified_rows=z['verified_rows'],narrow_sha256=z['narrow_sha256'],control_score=CONTROL,prepared_to_verified_EOF_s=z['prepared_to_verified_EOF_s'],phases=a,cache=c,post_gather_sort_span_loads=probes,sort_span_loads_per_distinct_offset=probes/g.U,cached_over_tiled=rat,order_hypothesis=mech,component_goal_met=met,posting_sha256=i['sha256'],tiles=q,unblocked_expansion_lanes=busy,meters=z['meters'],scope='One matched prepared 100 MB finite-key sorter treatment; independently verified EOF and separate posting identity, no raw/product/scale promotion.')

if __name__=='__main__':
    need(__debug__,'assertions required');mode=sys.argv[1];d=Path(sys.argv[2])
    if mode=='score':print(json.dumps(score(d),sort_keys=True,indent=2))
    elif mode=='admit':control();original();g.admit(d,int(sys.argv[3]),P,CONTROL)
    elif mode=='setup':g.setup(d,int(sys.argv[3]),P)
    elif mode=='pair':g.pair(d,int(sys.argv[3]),P)
    elif mode=='meter':g.meter(d,int(sys.argv[3]),int(sys.argv[4]),sys.argv[5],int(sys.argv[6]),P)
    elif mode=='identity':t.identity(d,int(sys.argv[3]),P,CONTROL,control,original)
    else:raise ValueError(mode)
