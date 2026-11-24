"""Offline upper on a two-code routing index of DISTINCT owned offsets. No input corpus opened."""
import hashlib
import json
from pathlib import Path
import sys
BANK=Path(__file__).resolve().parent/'receipts'
P=32
CELLS=256*257
PIN={'scanplan-10g/scan.json':'fea28d298533a0096945441477b2c9778b0dff471bbe260796f45be43922b049','scanplan-100g/scan.json':'49370600ae37702965c4a736a2de118566e87c00f70cf487484366051094be69','vocab-10g/census.out':'016f755ff8cc72863dac5daa182a502c70a830af1df5828f3d20bec6691e542d'}
def bank(name):
    b=(BANK/name).read_bytes();assert hashlib.sha256(b).hexdigest()==PIN[name];return b
def ceil(a,b):return -(-a//b)
def bits(v,c):
    assert 0<=c<=v
    if not c:return (0,0)
    return min((c*(l+1)+((v-1)>>l),l) for l in range(64))
def bands(rows,target):
    assert target>0 and len(rows)==CELLS
    out=[];begin=0;load=0
    for i,value in enumerate(rows):
        if load and value>target-load and value:out.append((begin,i));begin=i;load=0
        load+=value
    out.append((begin,CELLS));return out
def model(n,m,k_lower,u_upper,rows,desc_upper,workers,target):
    assert 0<k_lower<=m<=n and 0<u_upper<=n and len(rows)==len(desc_upper)==CELLS and sum(rows)==n and sum(desc_upper)==u_upper and all(0<=d<=r for d,r in zip(desc_upper,rows))
    pages=m+(u_upper-k_lower)//P;v=P*pages;groups=bands(rows,target);loads=[(sum(rows[i:j]),sum(desc_upper[i:j])) for i,j in groups];nb=len(groups);assert v>=u_upper and all(r>=d>0 for r,d in loads)
    # Each worker has a disjoint sub-universe. Even using the SAME l in all stripes,
    # the sum of their high-bitmap lengths cannot exceed the whole-universe bound.
    wire=ceil(sum(bits(v,c)[0] for _,c in loads),8)+16*workers*nb
    directory=8*ceil(pages,64)+16*ceil(pages,512)
    # Bounds private inverse + .route payload (all slices, incl. padding) + 4 MiB margin.
    # Raw copy, old originals, view arrays, and future local subpartition/finish are not this ledger.
    return dict(n=n,m=m,upper_u=u_upper,token_pages_upper=pages,token_universe_upper=v,page_directory_upper_bytes=directory,workers=workers,raw_rows_per_band_target=target,bands=nb,largest_single_band_rows=max(r for r,_ in loads),bands_over_row_target=sum(r>target for r,_ in loads),rows_in_oversize_cells=sum(r for r,_ in loads if r>target),descriptor_index_file_upper_bytes=wire,with_private_inverse_and_margin=8*m+wire+(4<<20),scratch_limit=2*n)
def reports():
    result={}
    for rung in ('10g','100g'):
        c=json.loads(bank(f'scanplan-{rung}/scan.json'))['census'];n,m=c['n'],c['m'];rows=[0]*CELLS
        for i,f in c['cells']:
            assert 0<=i<CELLS and rows[i]==0 and f>0;rows[i]=f
        target=max(128,ceil(n,1024));result[rung+'_occurrence_upper']=model(n,m,1,n,rows,rows,4,target)
        if rung=='10g':
            head,*cells=bank('vocab-10g/census.out').decode().splitlines();v=dict(s.split('=') for s in head.split());desc=[0]*CELLS
            for s in cells:
                i,a,b=map(int,s.split());assert not desc[i] and 0<a<=b==rows[i];desc[i]=a
            result['10g_distinct_upper']=model(n,m,int(v['k']),int(v['U']),rows,desc,4,target)
    assert all(x['with_private_inverse_and_margin']<x['scratch_limit'] for x in result.values());return dict(results=result,scope='Banked histogram upper for rank-page / Elias-Fano routing. Not a newly measured front, all-byte row order, joint emit fit or 100 GB admission.',sha256=PIN)
if __name__=='__main__':
    if len(sys.argv)!=1:raise SystemExit('archived-input model only')
    print(json.dumps(reports(),sort_keys=True,indent=2))
