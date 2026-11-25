"""Authenticate the whole-document 100 MB Pile prefix and emit its exact XSEP.
No row generator, index conversion, source deletion, delimiter-inferred start, or larger dispatch.
"""
import hashlib
import json
import mmap
import os
from pathlib import Path
import shutil
import stat
import struct
import sys
import time

ROOT=Path('/mnt/nvme2n1/erikg'); S=ROOT/'TAPEREADER/sandbox'; MAP=ROOT/'pilemap100'
N=100_000_503; DOCS=16_556; TOTAL=100_000_000_000; TOTAL_DOCS=16_119_767
MANIFEST='65cce20598b1dacbe4a6301d390c323646391ca7e42c43d91b400364e341761e'
RAW='3c965f581b46ba72abfd7fee7a0ce2efac5a61cd4dc5725e9253dcebcccc2383'
SEPS='daab7cf7ab5772b3ef56ca32fe6428a8df5759ec962069aefb8325cda283f4d4'
META_PREFIX='7c00fbb72b7bad965629c94091bd539380ef126a3c8a6ac7de52a69185ed7882'
REMAP_V8='ec59055b1b82e8bae4710ddcc64d9daabb8b87234c4bbf575b96f3c45bd592a7'

def sha(b):return hashlib.sha256(b).hexdigest()
def need(v,msg):
    if not v:raise ValueError(msg)
def state(s):return [s.st_dev,s.st_ino,s.st_size,s.st_mtime_ns,s.st_ctime_ns]
def pinfile(p,n):
    s=p.lstat();need(stat.S_ISREG(s.st_mode) and p==p.resolve() and s.st_size==n,str(p));return state(s)
def save(p,b):
    with p.open('xb') as f:f.write(b);f.flush();os.fsync(f.fileno())
def table(used):
    seq=sorted(used);need(0<len(seq)<256,'unrepresentable alphabet');rv=bytearray(256)
    for i,b in enumerate(seq+sorted(set(range(256))-used),1):rv[b]=i%256
    return bytes(rv)

def prefix_records(meta,deltas,anchors,n,count):
    rec=list(struct.iter_unpack('<QQHIQI',meta));need(len(rec)==count+1 and len(deltas)==4*len(rec),'metadata prefix+following row')
    starts=[];off=0;want=0
    for i,((d,),row) in enumerate(zip(struct.iter_unpack('<I',deltas),rec)):
        off+=d;start,length,shard,set_id,record_id,_=row
        need(off==start==want and (i!=0 or d==0) and (i==0 or d>0) and shard==0 and set_id<22 and record_id==i,'XSEP/XPM1 start, shard or ordinal')
        if i%64==0:need(struct.unpack_from('<Q',anchors,8*(i//64))[0]==off,'XSEP anchor')
        want=start+length+1;starts.append(start)
    need(starts[-1]==n,'cut is not an exact document end')
    a=starts[:-1:64];wire=struct.pack('<4sIIIQQQQ',b'XSEP',1,1,64,count,n,len(a),0)+deltas[:-4]+struct.pack(f'<{len(a)}Q',*a)
    return rec[:-1],wire

def source(d,end):
    need(d.parent==S and d.resolve()==d and d.name.startswith('real-v10-map100-') and stat.S_IMODE(d.stat().st_mode)==0o700 and not (d/'inputs').exists(),'fresh private fixed source job')
    cg=Path('/sys/fs/cgroup'+Path('/proc/self/cgroup').read_text().strip().split('::')[1]);cv=lambda k:(cg/k).read_text().strip()
    need((cv('memory.max'),cv('memory.swap.max'),cv('cpu.max'))==(str(256<<20),'0','100000 100000') and len(os.sched_getaffinity(0))==1,'source grants')
    def io():return {k:int(v) for k,v in (l.split(': ') for l in Path('/proc/self/io').read_text().splitlines()) if k in ('read_bytes','write_bytes')}
    def guard():
        meter=io();need(time.time()<end and not (d/'stop.requested').exists() and shutil.disk_usage(d).free>=1<<30,'stop, deadline or space');need(meter['read_bytes']<=800<<20 and meter['write_bytes']<=320<<20,'attributed source IO exceeded')
    guard();start=time.monotonic();inp=d/'inputs';inp.mkdir(mode=0o700);mp=inp/'map';mp.mkdir()
    manifest=(MAP/'MANIFEST.json').read_bytes();need(sha(manifest)==MANIFEST,'banked manifest');facts=json.loads(manifest)
    need((facts['corpus_bytes'],facts['doc_count'],facts['verification']['mismatches'],facts['verification']['documents_compared'])==(TOTAL,TOTAL_DOCS,0,TOTAL_DOCS),'source map provenance')
    need(facts['sidecars']['seps.bin']['sha256']==SEPS,'XSEP manifest digest')
    src={S/'pile-slice100.bin':N,ROOT/'pile-head100.txt':TOTAL,MAP/'seps.bin':66_494_092,MAP/'meta.bin':548_073_068,S/'pilemap100-slice':563_894,S/'remap-256.bin':256}
    ids={str(p):pinfile(p,z) for p,z in src.items()}
    tmp=hashlib.sha256();left=N;used=set()
    with (S/'pile-slice100.bin').open('rb') as a,(ROOT/'pile-head100.txt').open('rb') as b,(inp/'raw').open('xb') as o,(inp/'checker').open('xb') as w:
        while left:
            guard();take=min(1<<20,left);x=a.read(take);y=b.read(take);need(len(x)==take and x==y,'slice differs from actual Pile prefix');tmp.update(x);used.update(x);o.write(x);w.write(y);left-=take
        need(a.read(1)==b'' and tmp.hexdigest()==RAW,'source length or SHA');o.flush();w.flush();os.fsync(o.fileno());os.fsync(w.fileno())
    need(not (inp/'raw').samefile(inp/'checker'),'independent private checker file')
    with (MAP/'seps.bin').open('rb') as f:
        h=hashlib.sha256();left=src[MAP/'seps.bin']
        while left:
            guard();x=f.read(min(1<<20,left));need(x,'short XSEP');h.update(x);left-=len(x)
        need(f.read(1)==b'' and h.hexdigest()==SEPS,'full source XSEP SHA');f.seek(0)
        head=struct.unpack('<4sIIIQQQQ',f.read(48));need(head==(b'XSEP',1,1,64,TOTAL_DOCS,TOTAL,(TOTAL_DOCS+63)//64,0),'source XSEP shape')
        delta=f.read(4*(DOCS+1));f.seek(48+4*TOTAL_DOCS);anchors=f.read(8*(DOCS//64+1))
    with (MAP/'meta.bin').open('rb') as f:blob=f.read(563894);following=f.read(34)
    need(sha(blob)==META_PREFIX and blob==(S/'pilemap100-slice').read_bytes(),'XPM1 row prefix vs archived slice')
    magic,version,nd,n,ns,nn,np,sp,width,_=struct.unpack('<4sI6QIQ',blob[:68]);need((magic,version,nd,n,width)==(b'XPM1',1,TOTAL_DOCS,TOTAL,34),'XPM1 head');off=68+np+6*nn+sp+6*ns
    rec,wire=prefix_records(blob[off:]+following,delta,anchors,N,DOCS);save(mp/'seps.bin',wire)
    internal=0;sample=None
    with (inp/'raw').open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as text:
        need(sha(text)==RAW,'stored raw SHA')
        for i,(off,length,_,_,_,cnt) in enumerate(rec):
            if i%256==0:guard()
            stop=off+length;need(text[stop]==30 and text[off:stop].count(b'\x1e')==cnt,'framing or internal separator disagrees with record metadata');internal+=cnt
            if sample is None and i and off>=18:
                pat=text[off-17:off+16]
                if len(pat)==33 and pat.count(30)==1 and all(32<=v<127 or v==30 for v in pat) and text.find(pat)==off-17 and text.find(pat,off-16)<0:sample=dict(pos=off-17,cut=off,pattern_hex=pat.hex())
        need(sample is not None,'unique boundary query')
    with (inp/'checker').open('rb') as f:need(hashlib.file_digest(f,'sha256').hexdigest()==RAW,'stored independent checker SHA')
    remap=table(used);save(inp/'remap',remap);old=(S/'remap-256.bin').read_bytes();need(sha(old)==REMAP_V8,'historical remap')
    for p in (inp/'raw',inp/'checker',mp/'seps.bin',inp/'remap'):p.chmod(0o400)
    for p in (mp,inp):fd=os.open(p,os.O_RDONLY|os.O_DIRECTORY);os.fsync(fd);os.close(fd)
    # Inspect 176 bytes of the m20 head, never assume that all v10 files are boundary aware.
    other=Path('/tmp/sw-fx/t150m-v10.sxi2');pinfile(other,128266224)
    with other.open('rb') as f:
        f.seek(-40,2);i,c,off,size,count,crc=struct.unpack('<IIQQQQ',f.read(40));need(i==c==20 and off+size<=other.stat().st_size-400,'historical 150 MB entry');f.seek(off);head=struct.unpack('<22Q',f.read(176))
    need((head[0],head[1],head[2],head[4],head[19],head[21])==(3,8,1000,150000001,3,0),'150 MB source/awareness profile')
    for p,z in src.items():need(pinfile(p,z)==ids[str(p)],'input changed during comparison')
    guard();report=dict(scope='Exact real-data inputs and canonical map only. No product or row clocks.',n=N,rows=N+1,docs=DOCS,source_manifest_sha256=MANIFEST,raw_sha256=RAW,source_xsep_sha256=SEPS,meta_prefix_sha256=META_PREFIX,source_files=ids,attributed_io=io(),raw_prefix_bytes_compared=N,source_xsep_bytes_hashed=66_494_092,private_raw_copies=2,private_copy_bytes=2*N,map_bytes=len(wire),map_sha256=sha(wire),slice_remap_sha256=sha(remap),old_v8_remap_matches=remap==old,present_symbols=len(used),owned_framing_bytes=DOCS,internal_separators=internal,tail_partial=False,last_start=rec[-1][0],first_excluded_document_start=N,boundary_query=sample,other_150m_v10_boundary_aware=head[21],wall_s=time.monotonic()-start,shared_peak=int(cv('memory.peak')),cpu_max=cv('cpu.max'),memory_max=int(cv('memory.max')),swap_max=int(cv('memory.swap.max')),utc=time.strftime('%FT%TZ',time.gmtime()))
    save(d/'source.json',(json.dumps(report,indent=2,sort_keys=True)+'\n').encode());print(f'Authenticated {DOCS} complete documents, {N} raw bytes, XSEP {sha(wire)}, canonical remap matches v8={remap==old}',flush=True)

if __name__=='__main__':source(Path(sys.argv[1]),int(sys.argv[2]))
