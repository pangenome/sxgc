"""One new fixed 100 MB direct-range arm: change only expansion into whole-key tiles and ordered packet service."""
import hashlib
import json
from pathlib import Path
import sys
import overlap_finite_direct64_study as g
from overlap_scanplan_gate import Phase

s=g.s;need=g.need;load=s.load;BANK=g.OUT;CONTROL='ce28e5e539ecdb47757f37cfb2d1e4afda00e93a7ac11fb51c0269884368debd'
P=g.Profile('finite-tiles64-100m-','overlap_finite_tiles64_study.sh',s.RECEIPTS/'finite-tiles64-100m',Path(__file__).resolve(),'tiled')
def control():
    z=g.score(BANK);a=z['phases'];need(z==load(BANK,'score.json') and s.sha(BANK/'score.json')==CONTROL and z['component_goal_met'] and a['decoded']==g.U and a['queue_full_lane_s']>a['publish_wall_s'] and z['verified_rows']==g.N+1,'missing matched direct-range hotspot');return z
def original(bank=BANK,profile=g.DIRECT,revision='ca74354eb2cd73e85e79dda5f9baa80a3d80cf98',pin=CONTROL):
    x=load(bank,'source.json');d=Path(x['original_directory']);g.private(d,profile);f=d/'route4.ef';need(x['rc']==0 and x['revision']==revision and x['artifacts']['score.json']==pin and f.resolve()==f and list(s.identity(f))==load(bank,'emit4.json')['route_stat'],'earlier posting inode changed');return f
def admit(d,end):
    control();original();g.admit(d,end,P,CONTROL)
def identity(d,end,profile=P,pin=CONTROL,baseline=control,witness=original):
    p=g.authority(d,end,profile);g.old.checked_pair(d,4);g.old.guard(d,end,10);need(p['control_score']==pin,'control drift');c=baseline()['phases'];a=witness();b=d/'route4.ef';expected=load(d,'emit4.json')['route_stat'];need(list(s.identity(b))==expected and a.stat().st_size==b.stat().st_size==c['wire'] and b.stat().st_nlink==1 and not a.samefile(b),'different file extent/identity');r=dict(before=s.grant(192<<20,1));q=Phase(d,r,'separate_posting_byte_comparison',end,2*c['wire']+(16<<20),4<<20);h=hashlib.sha256()
    with a.open('rb') as f,b.open('rb') as v:
        left=c['wire']
        while left:
            q.guard();x=f.read(min(left,1<<20));need(x and v.read(len(x))==x,'posting payload mismatch');h.update(x);left-=len(x)
        need(not f.read(1) and not v.read(1),'posting payload grew')
    need(witness()==a and list(s.identity(b))==expected,'posting inode changed during compare');q.guard(True);r.update(after=s.grant(192<<20,1),equal_bytes=c['wire'],sha256=h.hexdigest(),control_stat=s.identity(a),candidate_stat=expected);s.save(d/'identity.json',r)
def treatment(d,b,bank,profile,pin):
    z=g.score(d);p=load(d,'admission.json');i=load(d,'identity.json');old=b['phases'];a=z['phases'];q=g.fields(d,'EMIT_TILES');er=load(d,'emit4.json');need(p['control_score']==pin and p['emit_mode']==profile.mode and er['argv'][1]==profile.mode and er['route_stat']==i['candidate_stat'] and i['control_stat']==load(bank,'emit4.json')['route_stat'] and i['candidate_stat'][:2]!=i['control_stat'][:2] and i['equal_bytes']==a['wire']==old['wire'] and g.re.fullmatch('[0-9a-f]{64}',i['sha256']) and i['after']['memory.max']==str(192<<20) and i['after']['memory.swap.max']=='0' and i['after']['cpu.max']=='100000 100000','posting proof or treatment drift');need(all(a[k]==old[k] for k in ('decoded','leaves','bands','rows','groups','bitmap_groups','exact_bypass','local_scans','max_tie','max_mass','waves','populated','counted')) and all(z['direct'][k]==v for k,v in g.fields(bank,'DIRECT_RANGES').items() if not k.endswith('_s')),'workload changed')
    need(q['grain']==1536 and a['leaves']<q['tiles']<=g.old.ceil(g.U,1536)+a['leaves'] and q['tiles']<=a['packets']<=g.old.ceil(g.N,2048)+q['tiles'] and a['writes']>=a['packets']+1 and 0<q['max_tile_descriptors']<=q['max_tile_rows']<=g.N and q['tiles_above_ring']<=q['tiles'] and 0<=q['plan_s']<=a['publish_wall_s'] and 0<=a['queue_full_lane_s']<=a['produce_lane_s'],'tile protocol or clocks');rat={k:z['meters']['emit'][k]/b['meters']['emit'][k] for k in ('cpu_s','read_bytes','write_bytes','peak')};rat.update(publish_wall=a['publish_wall_s']/old['publish_wall_s'],wall=z['prepared_to_verified_EOF_s']/b['prepared_to_verified_EOF_s']);busy=(a['produce_lane_s']-a['queue_full_lane_s'])/a['publish_wall_s'];return z,q,i,rat,busy
def score(d):
    z,q,i,rat,busy=treatment(d,control(),BANK,P,CONTROL);a=z['phases'];mech=rat['publish_wall']<=.65 and 2<=busy<=4.001;met=mech and rat['wall']<=.90 and max(rat['read_bytes'],rat['write_bytes'])<=1.05
    return dict(verified_rows=z['verified_rows'],narrow_sha256=z['narrow_sha256'],control_score=CONTROL,prepared_to_verified_EOF_s=z['prepared_to_verified_EOF_s'],phases=a,tiles=q,unblocked_expansion_lanes=busy,tiled_over_direct=rat,publication_hypothesis=mech,component_goal_met=met,posting_sha256=i['sha256'],meters=z['meters'],scope='One 4-worker prepared 100 MB direct-range tiled publication vs archived full-range publication. No product/scale launch.')

if __name__=='__main__':
    need(__debug__,'assertions required');mode=sys.argv[1];d=Path(sys.argv[2])
    if mode=='score':print(json.dumps(score(d),sort_keys=True,indent=2))
    elif mode=='admit':admit(d,int(sys.argv[3]))
    elif mode=='setup':g.setup(d,int(sys.argv[3]),P)
    elif mode=='pair':g.pair(d,int(sys.argv[3]),P)
    elif mode=='meter':g.meter(d,int(sys.argv[3]),int(sys.argv[4]),sys.argv[5],int(sys.argv[6]),P)
    elif mode=='identity':identity(d,int(sys.argv[3]))
    else:raise ValueError(mode)
