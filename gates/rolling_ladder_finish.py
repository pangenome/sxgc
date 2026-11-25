"""Consume the actual prepared inverse exactly once: rolling postings -> trie
row pipe -> PRC1/SXLS -> frozen finish -> in-place v10 joins. No byte SA spool.
"""
import hashlib, json, os, signal, struct, sys, time, zlib
from rolling_ladder import *
from overlap_v10_target_credits import bounds,geometry,words,ceil,PIECE,BLOCK,SLICE
from private_cold import cold
T=Path('/tmp/rolling-ladder-tools-xOvDDi');SINK=T/'sink';APPEND=T/'join'
ANCHOR={'1g':(BASE/'TAPEREADER/climb/rung1/run_wide/product.sxi2','b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3'),
        '10g':(BASE/'lcpruns-gate/g3/scratch/pf-10gb.sxi2','26cfb2de0cccab8a8712a332956d58693dcbbb34373a9031a7bbaaa08cc504c9'),
        '100g':(BASE/'PILE100-REHEARSAL/pile-head100.sxi2','15419c427b5bdc9fb63548901ca6cf83d54f486da890f66faa55021f0a0dc053')}
STABLE=(1,4,5,10,11,12,13)

def scan(path,version,n,guard):
    # One read of the finalized bytes for both SHA256 and member identity.
    pin=statid(path);ids=list(STABLE)+[7,14] if version==8 else list(STABLE)+[7,15,20];full=hashlib.sha256();members={};size=pin[2];need(64<size<=2*n,'product size')
    with path.open('rb') as f:
        head=f.read(64);magic,v,rows,sets,runs,k,_,length,_,crc=struct.unpack('<4sIQQQIIQQQ',head);need((magic,v,rows,sets,k,length)==(b'SXI2',version,n+1,1,len(ids),size),'container head');f.seek(-40*k,2);d=f.read(40*k);need(zlib.crc32(head[:56]+bytes(4)+head[60:]+d)==crc,'container CRC');arr=list(struct.iter_unpack('<IIQQQQ',d));need([row[0] for row in arr]==sorted(ids),'profile members');f.seek(0)
        def take(left,md=None):
            c=0
            while left:
                guard();b=f.read(min(left,4<<20));need(b,'short index');full.update(b);left-=len(b)
                if md is not None:md.update(b);c=zlib.crc32(b,c)
            return c
        for i,codec,off,nb,count,crc in sorted(arr,key=lambda x:(x[2],x[3])):
            need(64<=off<=size-40*k and nb<=size-40*k-off,'member extent');md=hashlib.sha256()
            if nb:
                need(f.tell()<=off,'overlap');take(off-f.tell());need(take(nb,md)==crc,'member CRC')
            else:need(crc==0,'empty CRC')
            members[i]=dict(codec=codec,bytes=nb,count=count,offset=off,sha256=md.hexdigest())
        take(size-f.tell());need(f.tell()==size and statid(path)==pin,'index changed')
    return dict(sha256=full.hexdigest(),bytes=size,rows=rows,runs=runs,table=members)

def plan(h,p):
    anchor,want=ANCHOR[h.r];reference=None;r=h.r;n=h.n
    if r=='100g':pr,sw,runs=6,400,31381597694
    else:
        reference=scan(anchor,8,n,h.guard);need(reference['sha256']==want,'wrong reference SHA');runs=reference['runs'];a=reference['table'][12]
        with anchor.open('rb') as f:f.seek(a['offset']);data=f.read(a['bytes'])
        need(hashlib.sha256(data).hexdigest()==a['sha256'],'m12 changed');k=ceil(runs,PIECE);hh=ceil(runs,BLOCK);need(struct.unpack_from('<8Q',data)==(BLOCK,hh,runs,n+1,PIECE,k,64,64+8*k),'m12 dimensions');ends=words(data[64:64+8*k]);ends.append(n+1);lo,hi=bounds(n+1,runs,PIECE,BLOCK,ends,(words(data[64+8*k:]),),h.guard);c=geometry(n+1,ends,lo,hi,SLICE,h.guard);sw=c['slice_sufficient']+2;pr=max(6,c['piece_sufficient']+2)
    slots=ceil(192*n,100000);post=n//2;rows=n+1;pfck=40+8*(rows//22+1);ef=ceil(3*rows+2,8)+2;sx=sw*(48+17*SLICE);pieces=(64+pr+8)*(44+5*PIECE+16*ceil(rows.bit_length()*PIECE,64));m20cap=(2333923624 if r=='100g' else n//8)
    # At EF emission the piece pool is drained. Keep even the last LCP window
    # reserved; guest capacity also covers accumulated BPG1/marker blocks.
    active=post+p['inverse_bytes']+pfck+ceil(rows,8)+8*ceil(runs,BLOCK)+max(ef,pieces)+sx+m20cap+36*ceil(runs,PIECE)+(8<<20)
    need(pr<=128 and sw<=512 and active+h.reserve//2<=2*n,'live logical scratch envelope');return reference,dict(runs=runs,slots=slots,posting=post,piece_window=pr,slice_window=sw,stream_logical_upper_including_transport=active,m20_cap=m20cap)

def finish(h):
    d=h.d;q=h.q;raw=d/'in/raw';mp=d/'in/map';remap=d/'in/remap';product=d/'work.sxi2';pref=q/'merged/frag';iv=q/'inverse.u64';end=h.end;n=h.n;r=h.r;stop=h.stop;p=load(d/'prepared.json')
    need((d/'exits.log').read_text()=='prepare=0\nprepare_launcher=0\n' and not load(d/'prepare-meter.json')['stop'] and not product.exists() and not (d/'work.sxi2.partial').exists() and p['rung']==r and p['raw']==h.rawsha and statid(h.img)==p['image_stat'],'not a successful, unchanged prepared input');toolpins((EMIT,TAP,FIN,MON,JOIN,FUSE,FSCK,UMOUNT));need(sha(SINK)=='3158df0248cd41c40e98a1ac5f39a66bfbd08a73981e4744628bfd56f9e4da5e' and sha(APPEND)=='49901282e1d2dc5e32081dcdc401a42a6631256a5074156b170af8978b8bf7cb' and (T/'exits.log').read_text()=='compile=0\nlauncher=0\n','ladder tools pin')
    need(sha(raw)==h.rawsha and sha(remap)==p['input']['remap_sha256'] and sha(mp/'seps.bin')==p['input']['map_sha256'],'prepared input digest');ref,pl=plan(h,p);save(d/'stream-plan.json',pl);cold(h.img);h.volume(False);need(iv.stat().st_size==8*h.m and sha(iv)==p['inverse_sha256'],'retained inverse SHA');cold(raw)
    common=['--corpus',raw,'--remap',remap,'--threads',2];start=time.monotonic();print(f'ROLLING_STREAM_START {r} slots={pl["slots"]} slices={pl["slice_window"]} pieces={pl["piece_window"]}',flush=True);h.step='stream'
    rows=h.launch('emit',[EMIT,'staged-epochs',raw,iv,q/'posting',4,h.array,2*n-pl['posting'],pl['slots']//5,pl['slots'],stop,end],pipe=True,fsize=pl['posting'])
    tapped=h.launch('tap',[TAP,'ctx-tap','--row43-pipe',stop,'--deadline',end,'--corpus',raw,'--n',n+1,'--doc-starts',mp,'--depth',8,'--min-support',1000,'--docs-min-support',1000,'--threads',1,'--out',q/'m20'],src=rows,pipe=True,fsize=pl['m20_cap'])
    h.launch('sink',[SINK,*common,'--out-prefix',pref,'--bands',q/'bands','--scratch-budget-bytes',2*n],src=tapped,env=dict(XSA_ROLLING_V10_RUNG=r,CROSS_V2_PIECES='1',CROSS_V2_PIECE_WINDOW=str(pl['piece_window']),SS_LCP_SLICE_WINDOW=str(pl['slice_window']),XSA_BATCH_SXLS_ESCAPES='1',PFCK_SPACING='22'))
    while not ((q/'merged/frag.pieces/g-0').exists() and (q/'bands/band_0.lcp.done').exists()):
        h.tick();need(h.work['sink'].returncode is None,'sink ended without initial handoff');time.sleep(.15)
    h.launch('finish',[FIN,*common,'--prefix',pref,'--lcp-bands',q/'bands','--output',product,'--container-stream','--with-chi','--no-lcp-runs','--wit-ram-budget',min(h.ram//4,24<<30),'--poll-timeout',1800],env=dict(XSA_WORK_ROOT='driver',PFCK_SPACING='22',LD_PRELOAD=str(MON),XSA_GATE_PRIVATE_STOP=str(stop)),kib=max(4096,n//(256*1024)),fsize=2*n)
    for k in ('emit','tap','sink','finish'):h.wait(k)
    need('EPOCH_INDEX epochs=' in (d/'product-emit.log').read_text() and 'position_file_bytes=0' in (d/'product-tap.log').read_text() and f'ROLLING_V10_SINK rung={r} rows={n+1} ' in (d/'product-sink.log').read_text() and f'runs={pl["runs"]} ' in (d/'product-sink.log').read_text(),'unfinished rolling stream')
    h.run('join',[APPEND,'rolling-join'],fsize=2*n);joined=time.monotonic();h.step='check';idx=scan(product,10,n,h.guard);tt=idx['table'];need(idx['runs']==pl['runs'] and tt[7]['sha256']==p['input']['remap_sha256'] and tt[15]['count']==p['input']['docs'] and tt[20]['sha256']==sha(q/'m20'),'wrong runs or new members')
    if ref:
        for i in STABLE:need(all(ref['table'][i][k]==tt[i][k] for k in ('codec','count','bytes','sha256')),'pinned v8 stable payload mismatch: '+str(i))
    else:need(idx['sha256']==ANCHOR[r][1],'historical complete v10 SHA mismatch')
    if ref: # no extra raw scan of an already-pinned identical 100 GB product
        h.run('docs',[JOIN,'docstarts','verify',product,'--docs-map',mp,'--samples',1024]);need('DOCSTARTS_PARITY result=PASS' in (d/'product-docs.log').read_text(),'doc check')
        h.run('ctx',[JOIN,'ctx-verify','--sxi',product,'--text',raw,'--context','the','--context','and','--context','hex:1e']);need('mismatches=0' in (d/'product-ctx.log').read_text(),'context check')
    with product.open('rb') as f:os.fsync(f.fileno())
    h.close();need(not h.mountinfo() and h.img.stat().st_nlink==1,'image still live');os.link(product,d/'index.sxi2');product.unlink();syncdir(d);h.img.unlink();h.q.rmdir();h.sample();v=load(d/'prepare-meter.json');met=h.report();elapsed=joined-start;rawjoin=p['raw_to_closed_prepared_s']+elapsed
    # Two separately leased stages: report service clocks and do not disguise
    # the real gap (tools/preflight) as a contiguous benchmark.
    result=dict(rung=r,input_bytes=n,product=str(d/'index.sxi2'),product_sha256=idx['sha256'],product_bytes=idx['bytes'],rows=idx['rows'],runs=idx['runs'],members=tt,known_full_v10_pin_checked=r=='100g',prior_stable_payloads_checked=list(STABLE) if ref else None,
                prepare_front_rank_s=p['raw_to_closed_prepared_s'],prepared_stream_to_join_s=elapsed,front_rank_plus_stream_join_s_not_contiguous=rawjoin,input_setup_s=p['input_setup_s'],postcheck_and_retire_s=time.monotonic()-joined,whole_two_service_s=v['service_s']+time.monotonic()-h.start,
                peak_shared=max(int(v['after']['memory.peak']),int(met['after']['memory.peak'])),peak_scratch_logical=max(x['peaks']['scratch_logical_with_8MiB_transport_stage'] for x in (v,met)),peak_scratch_allocated_with_reservations=max(x['peaks']['scratch_allocated_with_8MiB_transport_stage'] for x in (v,met)),scratch_limit=2*n,private_input_bytes=n,scope='Requested rolling v10 ladder. Stage service times, not a matched resident clock. Rung release is correctness/capacity, not performance promotion.',promote_next=True,utc=time.strftime('%FT%TZ',time.gmtime()));save(d/'complete.json',result);print(f'BUILT {r} {idx["bytes"]} bytes {idx["sha256"]}',flush=True)
if __name__=='__main__':
    r,d,end=sys.argv[1:];h=Build(r,Path(d),int(end),'product');signal.signal(signal.SIGTERM,lambda *_:h.stop.touch())
    try:finish(h)
    except BaseException:h.abort();raise
    finally:save(h.d/'product-meter.json',h.report())
