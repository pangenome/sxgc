"""Actual rolling product preparation: authenticated raw/XSEP, lexical keys and
certified inverse. One private raw input, no verifier copy, no prior columns.
"""
import hashlib, os, signal, struct, sys, time
from array import array
from rolling_ladder import *
from private_cold import cold

def inputs(h):
    src=BASE/'pile-head100.txt';before=statid(src);need(before==[66308,30071944370,TOTAL,1794328255140006266,1794328255140006266],'target source identity');d=h.d/'in';d.mkdir();mp=d/'map';mp.mkdir();md=hashlib.sha256();used=set();known=b'';left=h.n
    with src.open('rb',buffering=0) as f,(d/'raw').open('xb',buffering=0) as out:
        while left:
            h.guard();b=f.read(min(left,4<<20));need(b,'short target prefix');md.update(b);out.write(b);left-=len(b)
            new=b.translate(None,known)
            if new:used.update(new);known=bytes(sorted(used))
        os.fsync(out.fileno())
    need(md.hexdigest()==h.rawsha and statid(src)==before and (d/'raw').stat().st_size==h.n,'raw prefix SHA/extent');remap=table(used);(d/'remap').write_bytes(remap)
    need(sha(MAP/'MANIFEST.json')==MANIFEST,'source map manifest');source=(MAP/'seps.bin').read_bytes();need(hashlib.sha256(source).hexdigest()==SEPS and starthdr(source)==(b'XSEP',1,1,64,TOTAL_DOCS,TOTAL,(TOTAL_DOCS+63)//64,0),'source XSEP')
    s=memoryview(source);dv=s[48:48+4*TOTAL_DOCS].cast('I');av=s[48+4*TOTAL_DOCS:].cast('Q');need(sys.byteorder=='little' and len(dv)==TOTAL_DOCS and dv[0]==0,'delta order');anchors=array('Q');pos=0;docs=0;next_start=TOTAL
    for i,delta in enumerate(dv):
        if i%65536==0:h.guard()
        pos+=delta;need(i==0 or delta>0,'invalid source doc extent')
        if i%64==0:need(av[i//64]==pos,'source anchor')
        if pos>=h.n:next_start=pos;break
        if not i%64:anchors.append(pos)
        docs+=1
    wire=struct.pack('<4sIIIQQQQ',b'XSEP',1,1,64,docs,h.n,len(anchors),0)+s[48:48+4*docs].tobytes()+anchors.tobytes();(mp/'seps.bin').write_bytes(wire)
    if h.r=='1g':need(docs==165626 and next_start==h.n,'wrong complete 1 GB cutoff')
    for p in (d/'raw',d/'remap',mp/'seps.bin'):
        with p.open('rb') as f:os.fsync(f.fileno())
        p.chmod(0o400);cold(p)
    syncdir(mp);syncdir(d);return dict(bytes=h.n,raw_sha256=h.rawsha,map_sha256=hashlib.sha256(wire).hexdigest(),remap_sha256=hashlib.sha256(remap).hexdigest(),docs=docs,next_original_doc_start=next_start,tail_partial=next_start!=h.n,alphabet=len(used),source_stat=before)
def starthdr(b):return struct.unpack_from('<4sIIIQQQQ',b)

def prepare(h):
    toolpins((FRONT,RANK,MKFS,FUSE,UMOUNT,FSCK));t=time.monotonic();info=inputs(h);save(h.d/'input.json',info);t1=time.monotonic();h.volume(True);q=h.q;ready=q/'lex';cols=q/'lex.partial';iv=q/'inverse.u64';raw=h.d/'in/raw'
    h.run('front',[FRONT,'pos-manifest',raw,ready,4,h.array,h.image-(4<<20),h.stop,h.end]);magic,n,m,k,u,size=struct.unpack('<6Q',ready.read_bytes());need((magic,n,m)==(0x3143465058,h.n,h.m) and ready.samefile(cols/'manifest') and ready.stat().st_nlink==2 and (cols/'keys.u64').stat().st_size==8*m and (cols/'dictionary.lex64').stat().st_size==size==u+16*k-8,'front output/manifest');need(1<=k<=m and k<=u<=n,'front dimensions')
    if h.r=='1g':need((k,u)==(25_336_659,938_179_281),'1 GB vocabulary mismatch')
    if h.r=='10g':need((k,u)==(234_472_723,9_129_290_112),'10 GB vocabulary mismatch')
    if h.r=='100g':need(234472723<=k<=3097642723 and 9129295283<=u<=99129290112,'100 GB vocabulary contradicts certificate')
    def retire(p):
        before=os.statvfs(q);sz=p.stat().st_size;need(p.stat().st_nlink==1,'aliased column');p.unlink();syncdir(p.parent);after=os.statvfs(q);need((after.f_bfree-before.f_bfree)*after.f_frsize>=sz,'closed column blocks not returned')
    retire(cols/'dictionary.lex64');h.run('rank',[RANK,'rank',cols/'keys.u64',iv,4,h.array,h.stop,h.end]);need('permutation_recurrence=1' in (h.d/'prepare-rank.log').read_text() and iv.stat().st_size==8*m,'rank certificate');ivsha=sha(iv);retire(cols/'keys.u64');h.close();t2=time.monotonic()
    save(h.d/'prepared.json',dict(rung=h.r,raw=h.rawsha,input=info,n=n,m=m,k=k,U=u,inverse_bytes=8*m,inverse_sha256=ivsha,image_stat=statid(h.img),image_bytes=h.image,input_setup_s=t1-t,raw_to_closed_prepared_s=t2-t1,scope='Actual front and rank only. No row spool or product yet. Closed, fsck-checked scratch image retained for emission.'))
if __name__=='__main__':
    r,d,end=sys.argv[1:];h=Build(r,Path(d),int(end),'prepare');signal.signal(signal.SIGTERM,lambda *_:h.stop.touch())
    try:prepare(h)
    except BaseException:h.abort();raise
    finally:save(h.d/'prepare-meter.json',h.report())
