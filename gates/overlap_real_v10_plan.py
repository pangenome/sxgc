"""Admission for two sequential arms on ONE pinned 100 MB Pile slice (never a size switch)."""
import hashlib
import json
from pathlib import Path
import struct
import zlib

from overlap_v10_product_compare import FIN,FRONT,RANK,EMIT,TAP,OLD,JOIN,PINS as GENERATED_TOOLS
from overlap_real_v10_map import N,DOCS,S,RAW
G=Path(__file__).resolve().parent;SRC=S/'real-v10-map100-S4bZm0';T=Path('/tmp/real-v10-tools-Fch4DR')
REF=Path('/tmp/overlap-radix-Pymxu1/overlap_resident');LEX=Path('/tmp/resident-bin-DmG99k/lexparse');MON=LEX.parent/'monitor.so';RELAY=OLD/'relay';SINK=T/'sink';APPEND=T/'xsa';CHECK=T/'witness'
PINS={p:GENERATED_TOOLS[p] for p in (FIN,FRONT,RANK,EMIT,TAP,JOIN,RELAY)}|{SINK:'a17594d5a5b621271200b5a961c05f5652a98d3f0ef3a8a9df44f3d841489acd',APPEND:'da42625259d77cb4b053b0faae124cc83327f2aedf0937eeca32d331ce87706d',CHECK:'9b5918dc0326c910fae37eedaf0a9bfae13910e9bdcd7c59e0a070b0e6c1ff5f',REF:'088e99eb5e5a583b912adf4c6906a6db9feaeda651c23d319f5ae805a114bab7',LEX:'b3e06429708567a17f6ede3e0e6dac1891d252a3a495eb2177da025101f62544',MON:'fe5173c7d32215bdc7d41e59fd70edf059621ce83b38fb87b2e4d73ebbcb960a'}
IN={'raw':(N,RAW),'checker':(N,RAW),'remap':(256,'37b01d647470df1b5770e277427021b37465ea2c6d75e08d17ef4dda517b49d3'),'map/seps.bin':(68344,'63bb635e128e00d06037eaef5ce9e83d343cc622509b155d010789e4a20654ac')}
OLD_INDEX=S/'ext-product-QhQZxU/product.sxi2';OLD_SHA='6325c843c0d9aa9be00d69260aafdd794936e2e79f16174efb00c43db831f341';ROW_SHA='82747f11a9925bb4d89cc812410d98e411ce80b1767ee1ff4dc22e94e2cd874e';INV_SHA='afc938c0a1e96a62a2a6159fa119546a919b3042f8820847c207890bc9cafe67'
M,K,U=3_170_531,2_689_467,95_434_516;ROWS=N+1;RUNS=38_647_521;SLOTS=192_001;POST=N//2;M20=16<<20;PRODUCT=128<<20;CONTROL=4<<20;TRANSPORT=1<<20;STAGE=4<<20
MEM=2<<30;ARRAY=320<<20;RES_ARRAY=(14<<30)//10;ORACLE=140<<20
IDS=[1,4,5,7,10,11,12,13,15,20];SAME=[1,4,5,10,11,12,13]

def need(ok,msg):
    if not ok:raise ValueError(msg)
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def ceil(x,y):return (x+y-1)//y

def model():
    rows=ROWS;prc=78*(44+5*65536+16*ceil(rows.bit_length()*65536,64));slices=5*2_410_736
    base=dict(pfck=40+8*(rows//64+1),bitmap=ceil(rows,8),sparse=8*ceil(RUNS,1024),m20=M20,control=CONTROL,kernel_pipes=TRANSPORT)
    # At BPT1, both private pools have retired. The unchanged v10 streaming
    # finish consumes/unlinks the last PRC1s before opening either m5 BitsOut.
    # No m14, bridge, spool or fallback; still reserve bitmap and PFCK throughout.
    ef=ceil(3*rows+2,8)+2;finish=sum(base.values())+max(prc+slices,ef+slices)
    candidate=dict(front=U+16*K-8+8*M+CONTROL,rank=16*M+CONTROL,stream=finish+8*M+POST+STAGE)
    packed=U+12*K-8;queue=6<<20
    ref=dict(front=N+20*M-8+CONTROL,prepared=packed+4*M+CONTROL+queue,stream=finish+4*M+8*32768+queue)
    need(prc==60_067_176 and ef<prc and max(*ref.values(),*candidate.values())<=2*N,'joint reservation > 2N')
    return dict(reference=ref,candidate=candidate,finish_shared=base,prc_payload_slots=prc,sxls_payload_slots=slices,ef_bytes_upper=ef,finish_upper=finish,stream_posting=POST,stream_inverse=8*M,candidate_stage_margin=STAGE,resident_row_queue=queue,position_band=262144,product_cap_separate=PRODUCT,input_bytes_separate=sum(n for n,_ in IN.values()),scratch_limit=2*N)

def pins():
    need(sha(SRC/'source.json')==sha(G/'receipts/real-v10-map100/source.json')=='038ea10a124fc46821ce37f8395f2453a017ca9db6c04186c36728c7e2ce6f1b' and (SRC/'exits.log').read_text()=='source=0\nlauncher=0\n' and not (SRC/'stop.requested').exists(),'input receipt')
    need((T/'exits.log').read_text()=='compile=0\nlauncher=0\n' and sha(T/'tools.sha')==sha(G/'receipts/real-v10-100m-tools/tools.sha'),'tool receipt')
    for p,h in PINS.items():need(p==p.resolve() and p.is_file() and sha(p)==h,'tool pin: '+str(p))
    return {str(p):h for p,h in PINS.items()}

def inventory(p,version):
    n=p.stat().st_size;ids=IDS if version==10 else [1,4,5,7,10,11,12,13,14] if version==8 else IDS[:-2]
    need(64+40*len(ids)<n<=PRODUCT,'container size')
    with p.open('rb') as f:
        h=f.read(64);need(h[:4]==b'SXI2' and struct.unpack_from('<I',h,4)[0]==version and struct.unpack_from('<Q',h,8)[0]==ROWS and struct.unpack_from('<I',h,32)[0]==len(ids) and struct.unpack_from('<Q',h,40)[0]==n,'container head');f.seek(-40*len(ids),2);d=f.read();need(zlib.crc32(h[:56]+bytes(4)+h[60:]+d)==struct.unpack_from('<I',h,56)[0],'directory CRC')
        rec=list(struct.iter_unpack('<IIQQQQ',d));need([r[0] for r in rec]==ids,'wrong members');stop=n-len(d);spans=sorted((o,o+b) for _,_,o,b,_,_ in rec if b);need(all(64<=o<=e<=stop and (not i or spans[i-1][1]<=o) for i,(o,e) in enumerate(spans)),'overlapping member geometry')
        table={}
        for i,c,o,b,k,crc in rec:
            need(64<=o<=stop and b<=stop-o,'range');f.seek(o);left=b;v=0;s=hashlib.sha256()
            while left:
                buf=f.read(min(left,1<<20));need(buf,'short member');s.update(buf);v=zlib.crc32(buf,v);left-=len(buf)
            need(v==crc,'member CRC');table[i]=dict(codec=c,offset=o,bytes=b,count=k,crc=crc,sha256=s.hexdigest())
        need(table[1]['count']==RUNS and table[5]['count']==29_716_347,'wrong run/chi population')
        if version!=8:need(table[7]['sha256']==IN['remap'][1],'wrong full v10 remap member')
        if version==10:
            f.seek(table[20]['offset']);v=struct.unpack('<22Q',f.read(176));need((v[0],v[1],v[2],v[3],v[4],v[7],v[19:22])==(3,8,1000,ROWS,N,1000,(3,0,1)) and table[15]['count']==DOCS,'wrong trie/map target')
        return table

if __name__=='__main__':
    print(json.dumps(dict(files=model(),tools=pins()),indent=2,sort_keys=True))
