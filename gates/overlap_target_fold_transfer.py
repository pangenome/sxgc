"""Historical logs + small v10 run-piece directory only. No raw, full m1/m12, corpus or row reads.
Test whether re-slicing LCP WITHOUT changing the folded run intervals can bound data lifetimes.
"""
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import stat
import struct
import sys
import time
from overlap_v10_target_credits import BASE,G,S,SLICE,PIECE,BLOCK,PROFILES,ceil,need,words

BANK=BASE/'PILE100-REHEARSAL';N=10**11;ROWS=N+1;RUNS=31381597694;PIECES=ceil(RUNS,PIECE);SLICES=ceil(ROWS,SLICE);M=3181334488;MAX_SLICE=48+17*SLICE
LOGS={'checkpoint.journal':(76695466,'cfc197d9720b6e438e331e05542ec9be0fc68eabdc445e8684cb76db86dbcbb1'), 'finish-v2.log':(49980974,'0ff4d5c6492b29b16f1b48b0d30e36853a3ae43592742f24e1040f85c9691a80'), 'emit-tape.log':(2333698,'7eecf3fdd1fa646223af032272127681cec767adb05e2d29cabd5694166ea9b9')}
CREDIT_SHA='bc7ece0bfcbd1da70407ab77b4431021813aca94995acefed1ed97eebdc55698'

def sig(st):return st.st_dev,st.st_ino,st.st_size,st.st_ctime_ns
@contextmanager
def original(p,extent):
    need(p.resolve()==p,'substituted path');fd=os.open(p,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC)
    with os.fdopen(fd,'rb') as f:
        st=os.fstat(fd);need(stat.S_ISREG(st.st_mode) and st.st_size==extent,'original extent');yield f;need(sig(st)==sig(os.fstat(fd))==sig(p.stat()),'original changed')

def lines(name,guard):
    size,digest=LOGS[name];md=hashlib.sha256();left=size
    with original(BANK/name,size) as f:
        batch=0
        while left:
            if batch%4096==0:guard()
            line=f.readline(65537);need(line.endswith(b'\n') and len(line)<=min(left,65536),'bad log record');left-=len(line);batch+=1;md.update(line);yield line.rstrip(b'\n')
        need(md.hexdigest()==digest and not f.read(1),'log SHA/EOF')

def packet(runs):return 44+5*runs+16*ceil(ROWS.bit_length()*runs,64)

def ledger(guard):
    _,path,size,_,_,pin,member_bytes,_=PROFILES[1];starts=None
    with original(path,size) as f:
        head=f.read(64);f.seek(-400,2);directory=f.read(400);need(hashlib.sha256(head+directory).hexdigest()==pin,'target directory');table={i:(c,o,b,h,crc) for i,c,o,b,h,crc in struct.iter_unpack('<IIQQQQ',directory)};c,o,b,h,_=table[12];need((c,b,h)==(112,member_bytes,ceil(RUNS,BLOCK)) and table[1][3]==RUNS,'m12/m1 target');f.seek(o);need(struct.unpack('<8Q',f.read(64))==(BLOCK,h,RUNS,ROWS,PIECE,PIECES,64,64+8*PIECES),'super samples');blob=f.read(8*PIECES);need(len(blob)==8*PIECES,'short super directory');l2sha=hashlib.sha256(blob).hexdigest();starts=words(blob);del blob;starts.append(ROWS)
    total=0;j=0;plan=False;spool=False;historic_sha=None
    for line in lines('checkpoint.journal',guard):
        if line.startswith(b'PLAN '):
            need(not plan and j==0 and all(x in line.split() for x in (b'n=100000000000',b'corpus_sha256=de79fe96057b604a71513770111f246ab0e936d3cf56415bcc5a43bba60bbbfd',b'finish_sha=bb929a7092cfeba7faa7c37ca999b71563e3af782d73fe693d1a35f5ecdbaaa9',b'with_chi=true')),'journal plan');plan=True;continue
        if line.startswith(b'SPOOL_INVENTORY '):
            need(j==PIECES and not spool and all(x in line.split() for x in (b'scheme=1',b'bands=30838',b'pieces=478846',b'work_rows=100000000001',b'work_runs=31381597694')),'historical spool inventory');spool=True;continue
        if line.startswith(b'BUILD_DONE '):
            z=re.fullmatch(rb'BUILD_DONE container=([0-9a-f]{64})',line);need(z and spool and historic_sha is None,'historical final record');historic_sha=z[1].decode();continue
        z=re.fullmatch(rb'PIECE_DURABLE idx=(\d+) shard=0 lo_key=([0-9a-f]+) hi_key=([0-9a-f]+) rows=(\d+) bytes=(\d+) sha=[0-9a-f]{64}',line)
        need(z and plan and j<PIECES,'journal piece');lo,hi=j*PIECE,min(RUNS,(j+1)*PIECE);nr=int(z[4]);nb=int(z[5]);need((int(z[1]),int(z[2],16),int(z[3],16),nr,nb)==(j,lo,hi,starts[j+1]-starts[j],packet(hi-lo)) and starts[j]==total and nr>=hi-lo,'durable piece disagrees with canonical sample/cut');total+=nr;j+=1
    need(j==PIECES and total==ROWS and historic_sha is not None,'piece coverage');del starts
    emit=False
    for line in lines('emit-tape.log',guard):
        if line.startswith(b'TAPE_EMIT_DONE '):need(not emit and line.startswith(b'TAPE_EMIT_DONE rows=100000000001 runs=31381597694 seam_pass=none '),'not seam-free full emitter');emit=True
    j=batch=high=commit=syncs=0;prc_bytes=0;summary={};matched=set()
    for line in lines('finish-v2.log',guard):
        if line.startswith(b'CONTAINER_SYNC '):
            z=re.fullmatch(rb'CONTAINER_SYNC why=(piece-release|pre-close) member=m1 member_bytes=(\d+) unsynced_bytes=(\d+)',line);need(z and int(z[2])-commit==int(z[3])>0,'durable m1 frontier');high=max(high,batch);batch=0;commit=int(z[2]);syncs+=1
        elif line.startswith(b'PIECE_ABSORBED '):
            nb=packet(min(PIECE,RUNS-j*PIECE));need(j<PIECES and line==f'PIECE_ABSORBED idx={j} container_off={commit} bytes={nb}'.encode() and commit>0,'unexpected release');j+=1;batch+=1;prc_bytes+=nb
        elif line.startswith(b'XSA_FINISH_LCP_BANDS_DONE '):
            need(not summary,'duplicate LCP summary');fields=dict(x.split(b'=',1) for x in line.split()[1:]);summary={k:int(fields[k.encode()]) for k in ('slices','retired','empty','loads','preads','staged_bytes','escapes','max_lcp','max_slice_bytes','max_live_lcp_slice_bytes','live')}
        elif line==b'ENDPOINT_STREAM_PASS n=100000000001 k=1 r=31381597694 sentinel_rows=1 seam_classes=0':matched.add('no_seams')
        elif line==b'LCP_RUNS member=absent reason=disabled-by-flag':matched.add('no_m14')
        elif line.startswith(b'XSA_FINISH_PFCK ') and b' n=100000000001 tau=22 count=4545454546 bytes=36363636408 ' in line:matched.add('pfck22')
        elif line==b'CONTAINER_FINAL bytes=63163489696 sha256=f39bf4d7e8ffdb47001c9e6c8ecb1e16dc15304dc78044bbc29527f414b35a6a members=8':matched.add('base')
    need(emit and j==PIECES and commit==table[1][2] and matched=={'no_seams','no_m14','pfck22','base'} and max(batch,high)==64,'incomplete finish');need(summary['live']==1 and summary['loads']==PIECES+1 and summary['staged_bytes']==ROWS+PIECES and summary['slices']==summary['retired']==30838 and summary['empty']==0,'not the same fold convention');return dict(historical=summary,recorded_final_digest_not_recomputed=historic_sha,compared_durable_piece_spans=j,m1_sync_records=syncs,max_pieces_per_m1_release=max(batch,high),m12_super_sha256=l2sha,container_requested_bytes=64+400+64+8*PIECES,prc_total_wire=prc_bytes,m20_bytes=table[20][2],log_pins=LOGS)

def raw_work(rows,esc,maxh):
    need(rows>0 and 0<=esc<=rows and 0<=maxh<rows and (esc>0)==(maxh>=255) and (not esc or esc>=maxh-254),'impossible maxima/escape count')
    # At length d, each repeated d-gram starting at a later text position
    # contributes one to (# starts - # distinct) = # adjacent LCP >= d.
    # The equal pair of length M supplies M-d+1 DISTINCT later starts, even
    # when the two occurrences overlap. Combine by max per depth, not by sum.
    tail=max(0,maxh-255);excess=max(0,maxh-esc);lower=(255*esc+tail*(tail+1)//2+excess*(excess+1)//2) if esc else maxh*(maxh+1)//2
    return dict(prefix_bytes_lower=lower,prefix_bytes_upper=(rows-esc)*min(254,maxh)+esc*maxh,escape_count=esc,max_lcp=maxh)

def transfer(archive,cred):
    old=archive['historical'];g=cred['credit_schedule'];p,s=6,400;need(g['slice_sufficient']<=s and g['piece_sufficient']<=p,'new credit proposal does not fit');H=old['max_live_lcp_slice_bytes'];extra=34*(SLICE-1);h=min(g['fold_worst_case_bytes_upper'],H+extra);deep=H//18;size=1<<(deep-1).bit_length();vec_growth=16*(size+size//2);buffer=16*SLICE
    reader=dict(old_reported_bytes=H,extra_source_padding_upper=extra,new_logical_upper=h,deep_records_upper=deep,deep_growth_bytes=vec_growth,copy_buffer_bytes=buffer,conditional_arrays_including_two_rank_arrays=h+max(vec_growth,buffer)+16*PIECE)
    # Deliberately charge both EF and ALL slots, even though a non-spooling
    # streaming finish releases closed pieces before encoding m5. These are
    # accounted terms, NOT an outer allocated-total certificate or a grant.
    parts=dict(inverse=8*M,posting_cap=N//2,producer_margin=4<<20,pfck=40+8*(ROWS//22+1),chi_bitmap=ceil(ROWS,8),sparse=8*ceil(RUNS,BLOCK),m20=archive['m20_bytes'],piece_payload=(p+64+8)*packet(PIECE),slice_payload=s*MAX_SLICE,ef_spools=ceil(3*ROWS+2,8)+2,bpg1_bytes=36*PIECES)
    return dict(credit_proposal=dict(piece=p,slice=s),widths=dict(run_length_upper=g['largest_piece_rows'],row_bits=ROWS.bit_length(),reported_lcp_bits=old['max_lcp'].bit_length(),u16_sxls_impossible=old['max_lcp']>65535,u32_lcp_array_plus_page=4*SLICE+65536),reader=reader,modeled_live_stream_terms=parts,modeled_stream_subtotal=sum(parts.values()),not_yet_allocated_or_granted=2*N-sum(parts.values()),bpg1_4k_blocks_if_retained=4096*PIECES,marker_and_progress_names_if_retained=PIECES+2*SLICES,logical_total_sxls_wire=ROWS+16*old['escapes']+48*SLICES,logical_raw_verifier_work=raw_work(ROWS,old['escapes'],old['max_lcp']))

def main(d,end):
    need(d.parent==S and d.resolve()==d and re.fullmatch('v10-target-fold-[A-Za-z0-9]{6}',d.name) and d.stat().st_uid==os.getuid() and stat.S_IMODE(d.stat().st_mode)==0o700 and time.time()<end<=time.time()+120,'private fold review');g=Path('/sys/fs/cgroup'+Path('/proc/self/cgroup').read_text().strip().split('::')[1]);cg=lambda name:(g/name).read_text().strip();need((cg('memory.max'),cg('cpu.max'),cg('memory.swap.max'))==(str(128<<20),'100000 100000','0') and len(os.sched_getaffinity(0))==1,'review grant');t=time.monotonic()
    def guard():need(time.time()<end and not (d/'stop.requested').exists(),'review STOP/deadline')
    raw=(G/'receipts/v10-target-credits/result.json').read_bytes();need(hashlib.sha256(raw).hexdigest()==CREDIT_SHA,'credit receipt');cred=json.loads(raw)['arms']['target100g'];a=ledger(guard);guard();v=transfer(a,cred);ru=resource.getrusage(resource.RUSAGE_SELF);events=cg('memory.events');need(512*ru.ru_inblock<=160<<20 and 512*ru.ru_oublock<=1<<20 and {'oom 0','oom_kill 0'}<=set(events.splitlines()),'review resource refusal')
    z=dict(archive=a,conditional=v,credit_receipt=CREDIT_SHA,utc=time.strftime('%FT%TZ',time.gmtime()),body_s=time.monotonic()-t,attributed_input_bytes=512*ru.ru_inblock,attributed_write_bytes_before_receipt=512*ru.ru_oublock,memory_peak=int(cg('memory.peak')),memory_events=events,scope='Historical piece/finish-log cross-check, conditional same-row re-slicing and byte-work bounds. No raw input, new construction, full index hash, physical fit, RSS or speed promotion.')
    with (d/'result.json').open('x') as f:json.dump(z,f,sort_keys=True,indent=2);f.write('\n');f.flush();os.fsync(f.fileno())
    print(json.dumps(v,sort_keys=True))
if __name__=='__main__':need(__debug__ and len(sys.argv)==3,'private metadata review only');main(Path(sys.argv[1]),int(sys.argv[2]))
