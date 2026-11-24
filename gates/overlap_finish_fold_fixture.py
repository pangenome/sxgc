"""Fold RAM component only: archived counters, generated rows or pinned reader source. No Pile row producer or finish launch."""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess as sp
import sys
import threading
import time
from overlap_finish_credits import D, LOG, N, R, ROW, RUN, ceil, pin
from overlap_finish_row_fence_test import byte_reference,geometry
from overlap_raw_view64_test import fields
from overlap_v8_sxls_refuse import BASE

G=Path(__file__).resolve().parent
SRC=G.parent/'xsa/runtime/bit6/xsa_finish.cpp'
BLOB='8a04d79a0dc80cd813c3a561cb75f1910fd57777'
CLASS='48fe0cf632d43eb96d93dd979e6338bedd214fdb2d0068e32f5c152767536096'
sha=lambda b:hashlib.sha256(b).hexdigest()

def source():
    b=SRC.read_bytes();assert hashlib.sha1(f'blob {len(b)}\0'.encode()+b).hexdigest()==BLOB
    a=b.index(b'struct LcpColumn {');c=b[a:b.index(b'\nstatic void fp_self_check',a)];assert sha(c)==CLASS and len(c)==17473;return c

def model():
    source();f=D/'finish.log';pin(f,66989,LOG);s=f.read_text();assert sha(f.read_bytes())==LOG
    counter=fields(s,'XSA_FINISH_LCP_BANDS_DONE');stat={k:int(counter[k]) for k in ('loads','preads','slices','retired','live','empty','staged_bytes','escapes','max_slice_bytes','max_live_lcp_slice_bytes')};n=N+1;q=ceil(R,RUN);H=stat['max_live_lcp_slice_bytes'];e=134284
    assert stat==dict(loads=q+1,preads=972,slices=382,retired=382,live=1,empty=0,staged_bytes=n+q,escapes=1129147,max_slice_bytes=ROW+16*e,max_live_lcp_slice_bytes=3958321)
    assert fields(s,'ENDPOINT_STREAM_PASS')==dict(n=str(n),k='1',r=str(R),sentinel_rows='1',seam_classes='0')
    # F >= len + source_rows + 16*source_esc >= 2*len+16*deep >= 18*deep.
    # The final short slice cannot contain more escapes than there are rows.
    assert n%ROW<e;deep=H//18;cap=1<<(deep-1).bit_length();vector_peak=16*(cap+cap//2);ranks=16*RUN
    copy=H+16*e;r=H+vector_peak
    return dict(source_blob=BLOB,reader_source_sha256=CLASS,finish_log_sha256=LOG,original=stat,fold_rows=n,max_copied_deep_entries=deep,fresh_pair_capacity_upper=cap,deep_reallocation_peak_bytes=vector_peak,load_copy_payload_upper=copy,load_deep_payload_upper=r,head_and_tail_rank_bytes_upper=ranks,conditional_reader_plus_two_rank_arrays_upper=max(copy,r)+ranks,conditions='Fresh per-job band, exact seam-free maximal run cuts, v8 non-spooling loader; GCC 13 libstdc++ fresh-vector capacities and old+new geometric realloc. Logical vector payloads only; excludes BandSlots, allocator retention, control objects, stack, corpus/PFCK, bitmap, directories, IO/cache and concurrent stages. Not RSS, disk scratch, timing or admission.')

def relay(d):
    import random
    rng=random.Random(309552);ok=refused=0
    for j,(text,B,S) in enumerate([(b'!',1,1),(b'banana\0banana\xff',2,1),(rng.randbytes(24575),91,101),(b'x'*14710+b'y'*401,3,177),(rng.randbytes(32)*700,4,150)]):
        file=d/f'gen-raw-{j}';file.write_bytes(text);_,wide=byte_reference(text);nar,(runs,sl,m,_,_)=geometry(wide);h,b=zip(*[(v,b) for _,v,b in struct.iter_unpack('<III',nar)]);n=len(b);starts=[0]+[x for x in range(1,n) if b[x]!=b[x-1]];assert len(starts)==runs
        front=peak=reads=staged=loads=slices=0
        ends=[starts[max(0,min(runs,r+B)-2)] for r in range(0,runs,B)]+[n]
        for end in ends:
            if end==front:continue
            lo=max(0,front-1);front=end;length=end-lo;left=lo//S;right=(end-1)//S;count=right-left+1;reads+=count;slices=max(slices,count);staged+=length;loads+=1
            a=left*S;z=min(n,(right+1)*S);peak=max(peak,length+z-a+16*sum(v>=255 for v in h[a:z]))
        before=[d/'relay-fixture','--fixture-fold',file,d/'stop.requested',str(int(time.time())+20),*map(str,(runs,sl,m)),sha(nar),str(B),str(S)]
        for cap in [peak,peak-1]:
            z=sp.run([*before,str(cap)],input=wide,capture_output=True,timeout=6)
            if cap==peak:
                assert z.returncode==0 and z.stdout==nar and {k:int(v) for k,v in fields(z.stderr.decode(),'FOLD_GEOMETRY').items()}==dict(loads=loads,reads=reads,staged=staged,peak=peak,max_slices=slices),(j,z.stderr);ok+=1
            else:
                assert z.returncode==70 and b'fold live slice geometry' in z.stderr and z.stdout==nar[:len(z.stdout)] and len(z.stdout)<=12*((n-1)//8192)*8192,(j,z.stderr);refused+=1
    print(f'PASS {ok} independent complete raw row streams with fold fence, {refused} one-byte-short fold caps withheld tail',flush=True)

def partial(d):
    # Exercise the actual pinned Rust slot writer, but only a generated PREFIX.
    # A reader-only C++ probe supplies explicit fold spans (NOT a PRC1 consumer).
    # No Pile bytes, valid EOF product claim, 100 MB sweep, or row file.
    p=json.loads((G/'receipts/v8-sxls-slots-fixtures/source.json').read_text());exe=Path(p['original_directory'])/'build/debug/pfp_sort_r';assert exe.resolve()==exe and sha(exe.read_bytes())==p['fixture_native_sha256']=='2ca46019392adc87b1cfb360c054deaf7c8a09685580ebe0f287922cbf606015'
    n=7*ROW;cut=6*ROW+13;arm=d/'arm';arm.mkdir(mode=0o700);banks=arm/'bands';raw=d/'shape-only';remap=d/'test.map';remap.write_bytes(bytes(range(256)))
    with raw.open('xb') as f:f.truncate(N)
    cmd=[exe,'--corpus',raw,'--remap',remap,'--out-prefix',arm/'merged/frag','--bands',banks,'--threads','2'];step=threading.Event();errors=[];goal=time.monotonic()+24;stop=arm/'merged/modeb/stop.requested'
    def guard():assert time.monotonic()<goal and not (d/'stop.requested').exists() and not stop.exists()
    with (d/'sink.log').open('xb') as log:
        writer=sp.Popen(cmd,stdin=sp.PIPE,stdout=log,stderr=sp.STDOUT,env=BASE,bufsize=0)
        def supply():
            try:
                pack=struct.Struct('<III').pack_into;buf=bytearray(8192*12)
                for off in range(0,n,8192):
                    guard()
                    for j in range(8192):
                        i=off+j;h=0 if not i else 34067 if i%1709==0 else 255 if i%17==0 else i%255;b=1 if i<6*ROW else 2+(i-6*ROW)%2 if i<cut else 1 if i<cut+1000 else 2;pack(buf,12*j,1 if i else N,h,b)
                    left=memoryview(buf)
                    while left:
                        wrote=writer.stdin.write(left);assert wrote;left=left[wrote:]
                    if off+8192==6*ROW:step.set()
            except BaseException as err:errors.append(str(err) or repr(err))
            finally:writer.stdin.close()
        thread=threading.Thread(target=supply);thread.start();reader_rc=None
        try:
            while not step.wait(.02):guard();assert writer.poll() is None and not errors,errors
            guard();done=list(banks.glob('*.lcp.done'));assert len(done)==5 and not (banks/'band_5.lcp').exists() and not list(banks.glob('*.consumed'));under=banks/'slice-slots';ids=[]
            for i in range(5):
                f=under/str(i);assert f.samefile(banks/f'band_{i}.lcp') and f.stat().st_nlink==2;ids.append(f.stat().st_ino)
            z=sp.run([d/'probe','generated-consume'],cwd=d,stdout=sp.PIPE,stderr=sp.PIPE,timeout=10,check=False);reader_rc=z.returncode;(d/'reader.log').write_bytes(z.stderr+z.stdout);assert reader_rc==0,z.stderr
            rc=writer.wait(timeout=4);thread.join(timeout=2);assert not errors and not thread.is_alive() and rc==2 and b'truncated fixed SXLS narrow input' in (d/'sink.log').read_bytes(),errors
            assert not list(banks.glob('*.lcp')) and len(list(banks.glob('*.lcp.consumed')))==7 and len(list(banks.glob('*.lcp.done')))==7 and not (banks/'producer.done').exists() and not (arm/'merged/frag.pieces/terminal').exists()
            assert sorted(x.stat().st_ino for x in under.iterdir())==sorted(ids) and all(x.stat().st_nlink==1 for x in under.iterdir());print(z.stdout.decode().strip());print('PASS five-slot Rust writer -> seven copied/unlinked SXLS, all generated LCP values and overlap checked; sink deliberately truncated, no terminal/finish/product')
        finally:
            if writer.poll() is None:
                if stop.parent.is_dir():stop.touch(exist_ok=True)
                try:writer.wait(timeout=3)
                except sp.TimeoutExpired:writer.kill();writer.wait()
            thread.join(timeout=2);(d/'partial-exits.json').write_text(json.dumps(dict(reader=reader_rc,sink=writer.returncode,feeder_errors=errors,stop=stop.exists()))+'\n')

if __name__=='__main__':
    assert __debug__
    if sys.argv[1:]==['model']:print(json.dumps(model(),indent=2,sort_keys=True));sys.exit(0)
    mode,d=sys.argv[1],Path(sys.argv[2]);assert d.resolve()==d and d.parent==Path('/tmp') and re.fullmatch(r'v8-fold-geometry-[A-Za-z0-9]{6}',d.name) and d.stat().st_uid==os.getuid() and d.stat().st_mode&0o777==0o700
    if mode=='class':
        with (d/'pinned-lcp-column.inc').open('xb') as f:f.write(source())
    elif mode=='relay':relay(d)
    elif mode=='partial':partial(d)
    else:raise ValueError(mode)
