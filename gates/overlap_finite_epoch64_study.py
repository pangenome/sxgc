"""Space treatment only, one 100 MB fit-only rolling EF stage (no concurrent finish/product)."""
import hashlib
import json
from pathlib import Path
import re
import sys
import overlap_finite_tiles64_study as t
from overlap_scanplan_gate import Phase

g=t.g;s=t.s;load=s.load;need=s.need;N=g.N;U=g.U;BANK=t.P.bank;CONTROL='b61006c8d8af7eb2b90ae9fe38005e3ddc2185aa16d1414af7948dd301ebe825'
P=g.Profile('finite-epoch64-100m-','overlap_finite_epoch64_study.sh',s.RECEIPTS/'finite-epoch64-100m',Path(__file__).resolve(),'epochs')
CAP=N//2;HELD=2*N-CAP;LIVE=g.old.OTHER+CAP

def control():
    z=t.score(BANK);need(z==load(BANK,'score.json') and s.sha(BANK/'score.json')==CONTROL and z['component_goal_met'] and z['phases']['decoded']==U and z['phases']['counted']==195533144 and z['phases']['wire']==165974592 and z['phases']['waves']==622,'unmatched tiled control');return z

def original():return t.original(BANK,t.P,'59ec7cd0354f7e5cae11227d61d03031c7780a0a',CONTROL)

def upper(d,v,b):return min(g.old.ceil(d*(l+1)+b*((v-1)>>l),8) for l in range(64))+16*4*b

def model(n,m,k,u,slots,wire=None):
    v=32*(m+(u-k)//32);b=min(16384,g.old.ceil(u,slots//5));w=upper(min(u,slots),v,min(b,4));cap=n//2;size=wire or upper(u,v,b)
    # Every nonlast greedy epoch has more than cap-max_wave bytes: it refused the following whole wave.
    need(w<cap,'no positive guaranteed fill bound');return dict(n=n,m=m,k_assumed=k,u_assumed=u,slots=slots,desired_bands=b,whole_wave_upper=w,total_file_upper=size,posting_cap=cap,actual_counted_upper_if_fit=8*m+(4<<20)+cap,unspent_finish_reserve=2*n-cap-8*m-(4<<20),epoch_upper_if_every_range_fits=g.old.ceil(size,cap-w))

def prediction():
    b=control();p=model(N,g.M,g.old.K,U,g.old.SLOTS,b['phases']['wire']);need(p['epoch_upper_if_every_range_fits']==g.old.ceil(p['total_file_upper'],CAP)==4 and g.fields(BANK,'DIRECT_RANGES')['max_range_descriptors']<=g.old.SLOTS and p['actual_counted_upper_if_fit']==LIVE==79558803 and HELD-g.old.OTHER==120442203,'missing four-epoch admission proof');return p

def parts(d):
    out=[]
    for line in (d/'emit4.log').read_text().splitlines():
        if line.startswith('EPOCH_PART '):
            row=dict(x.split('=',1) for x in line.split()[1:]);need(set(row)=={'i','lo','hi','waves','desc','rows','bytes','interior','populated','sha256'} and re.fullmatch('[0-9a-f]{64}',row['sha256']),'invalid epoch receipt');out.append({k:v if k=='sha256' else int(v) for k,v in row.items()})
    return out

def layout(d):
    b=control()['phases'];p=prediction();rows=parts(d);need(len(rows)==4,'not predicted four epochs');lo=0;sums=dict(bytes=0,desc=0,rows=0,waves=0,populated=0)
    for idx,r in enumerate(rows):
        need(r['i']==idx and r['lo']==lo<r['hi']<=b['bands'] and 0<r['desc']<=r['rows'] and g.old.ceil(r['hi']-lo,4)<=r['waves']<=r['hi']-lo and r['desc']<=r['waves']*g.old.SLOTS and 0<r['populated']<=r['bytes']<=CAP and 0<=r['interior']<=r['bytes'] and r['interior']%4096==r['bytes']%8==0 and (idx==3 or r['bytes']>CAP-p['whole_wave_upper']),'bad wave/range/extent');lo=r['hi']
        for k in sums:sums[k]+=r[k]
    need(lo==b['bands'] and sums==dict(bytes=b['wire'],desc=U,rows=N,waves=b['waves'],populated=b['populated']),'incomplete epoch coverage');return rows

def proof(d,end):
    p=g.authority(d,end,P);g.old.checked_pair(d,4);g.old.guard(d,end,10);need(p['control_score']==CONTROL,'wrong control');a=original();b=d/'route4.ef';er=load(d,'emit4.json');rows=layout(d);need(list(s.identity(b))==er['route_stat'] and b.stat().st_size==rows[-1]['bytes'] and b.stat().st_nlink==1 and not a.samefile(b),'wrong final slot');r=dict(before=s.grant(192<<20,1));ph=Phase(d,r,'separate_all_epoch_image_proof',end,a.stat().st_size+CAP+(16<<20),4<<20);h=[hashlib.sha256() for _ in rows];whole=hashlib.sha256()
    # The old file has ALL interiors then ALL tails. Feed each closed slot digest from its
    # two disjoint slices of the one immutable control. Byte-compare the still available last slot too.
    with a.open('rb') as ctrl,b.open('rb') as last:
        for interior in (True,False):
            for j,z in enumerate(rows):
                left=z['interior'] if interior else z['bytes']-z['interior']
                while left:
                    ph.guard();x=ctrl.read(min(left,1<<20));need(x and (j!=len(rows)-1 or last.read(len(x))==x),'short/different retained image');whole.update(x);h[j].update(x);left-=len(x)
        need(not ctrl.read(1) and not last.read(1),'trailing posting data')
    dig=[x.hexdigest() for x in h];need(original()==a and list(s.identity(b))==er['route_stat'] and whole.hexdigest()==control()['posting_sha256'] and dig==[z['sha256'] for z in rows],'epoch digests/control/slot mismatch');ph.guard(True);r.update(after=s.grant(192<<20,1),control_sha256=whole.hexdigest(),slot_sha256=dig,verified_bytes=sum(z['bytes'] for z in rows),last_byte_compared=rows[-1]['bytes'],control_stat=s.identity(a),candidate_stat=er['route_stat']);s.save(d/'identity.json',r)

def score(d):
    b=control();old=b['phases'];r=layout(d);peak=max(x['bytes'] for x in r);z=g.score(d,held=HELD,live=LIVE,posting_peak=peak);a=z['phases'];e=g.fields(d,'EPOCH_INDEX');q=g.fields(d,'EMIT_TILES');i=load(d,'identity.json');p=load(d,'admission.json');er=load(d,'emit4.json')
    need(p['control_score']==CONTROL and p['emit_mode']=='epochs' and p['nonposting_reserved']==HELD and er['argv'][1]=='epochs' and int(er['argv'][7])==HELD and i['control_stat']==load(BANK,'emit4.json')['route_stat'] and i['candidate_stat']==er['route_stat'] and i['control_stat'][:2]!=i['candidate_stat'][:2] and i['candidate_stat'][2]==i['last_byte_compared']==r[-1]['bytes'] and i['control_sha256']==b['posting_sha256'] and i['slot_sha256']==[v['sha256'] for v in r] and i['verified_bytes']==a['wire'] and (i['after']['cpu.max'],i['after']['memory.max'],i['after']['memory.swap.max'])==('100000 100000',str(192<<20),'0'),'posting witness, profile or grant mismatch');need(all(a[k]==old[k] for k in ('decoded','leaves','bands','rows','groups','bitmap_groups','exact_bypass','local_scans','max_tie','max_mass','waves','populated','wire','packets')) and all(z['direct'][k]==v for k,v in g.fields(BANK,'DIRECT_RANGES').items() if not k.endswith('_s')) and all(q[k]==v for k,v in b['tiles'].items() if not k.endswith('_s')),'routing, geometry or tiles changed');need(e['epochs']==len(r)==4 and e['encode_offset_visits']==4*U and e['hash_readback']==e['total_extent']==old['wire'] and e['file_cap']==CAP and e['peak_extent']==peak and a['counted']==HELD+peak<=2*N and e['nonposting_reserved']==HELD and 0<e['hash_s']<z['prepared_to_verified_EOF_s'],'live vs traffic bounds')
    ratio={k:z['meters']['emit'][k]/b['meters']['emit'][k] for k in ('cpu_s','peak','read_bytes','write_bytes')};ratio.update(wall=z['prepared_to_verified_EOF_s']/b['prepared_to_verified_EOF_s'],counted_upper=(peak+g.old.OTHER)/old['counted']);space=100*(peak+g.old.OTHER)<=41*old['counted'] and 5*(HELD-g.old.OTHER)>=6*N;met=space and ratio['wall']<=1.30 and ratio['cpu_s']<=1.25 and max(ratio['read_bytes'],ratio['write_bytes'])<=1.05
    return dict(verified_rows=z['verified_rows'],narrow_sha256=z['narrow_sha256'],control_score=CONTROL,prepared_to_verified_EOF_s=z['prepared_to_verified_EOF_s'],phases=a,epoch=e,parts=r,remaining_reserved_for_other_stages=HELD-g.old.OTHER,actual_stage_files_upper=peak+g.old.OTHER,preflight_stage_files_upper=LIVE,slot_digests_verified_against_complete_control=i['verified_bytes'],separate_posting_proof_s=i['separate_all_epoch_image_proof']['wall_s'],epoch_over_tiled=ratio,space_goal_met=space,component_goal_met=met,meters=z['meters'],scope='Fixed 100 MB rolled-posting component, one live inode and held-out scratch. No finish ran; no end-to-end fit, product or speed promotion.')

if __name__=='__main__':
    need(__debug__,'assertions required');mode=sys.argv[1]
    if mode=='bounds' and len(sys.argv)==2:
        print(json.dumps(dict(measured_100m_geometry=prediction(),conditional_100g_upper=model(10**11,3181334488,1,10**11,192000000),not_product_admission=True),sort_keys=True,indent=2));sys.exit(0)
    d=Path(sys.argv[2])
    if mode=='score':print(json.dumps(score(d),sort_keys=True,indent=2))
    elif mode=='admit':prediction();original();g.admit(d,int(sys.argv[3]),P,CONTROL,held=HELD,live=LIVE)
    elif mode=='setup':g.setup(d,int(sys.argv[3]),P)
    elif mode=='pair':g.pair(d,int(sys.argv[3]),P)
    elif mode=='meter':g.meter(d,int(sys.argv[3]),int(sys.argv[4]),sys.argv[5],int(sys.argv[6]),P,held=HELD,live=LIVE)
    elif mode=='identity':proof(d,int(sys.argv[3]))
    else:raise ValueError(mode)
