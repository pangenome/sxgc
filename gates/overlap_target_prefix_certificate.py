"""Authenticate only the first 10 GB (+ the 16-byte EOF word) of the pinned
100 GB source. Transfer stable ordinary-phrase duplicates, not EOF words, raw
proportions, query maps or cached payloads. No construction or range admission.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shutil
import stat
import sys
import time
from types import SimpleNamespace
from overlap_prefix_vocab_bounds import cells, end_cells, inheritance, need, price
from overlap_vocab_plan import CELLS, counts, digest
from overlap_finite_epoch64_study import model as epoch_model
from overlap_v10_target_credits import BASE,G,S
from overlap_real_v10_meter import files
from overlap_v10_product_compare import optional_io_stat

R=G/'receipts';P=10**10;N=10**11;M=3181334488;SLOTS=192000000
SMALL='178f411b196e6741c473b4bbf442086bc64e8c4ae102359fc3c0e3f17a12085d'
BIG='de79fe96057b604a71513770111f246ab0e936d3cf56415bcc5a43bba60bbbfd'
PINS={'scanplan-100g/scan.json':'49370600ae37702965c4a736a2de118566e87c00f70cf487484366051094be69',
      'scanplan-10g/scan.json':'fea28d298533a0096945441477b2c9778b0dff471bbe260796f45be43922b049',
      'vocab-10g/census.out':'016f755ff8cc72863dac5daa182a502c70a830af1df5828f3d20bec6691e542d',
      'vocab-10g/reference.out':'8899714b2a16c4459f19acfc9f852d7c1ede32228f738851b60133dfab65185f',
      'vocab-10g/inputs.json':'926f81e2b5f97323500789fa5e4517cb114feeaba85ec0aa8c200706a604927b',
      'vocab-10g/review.json':'9789aaf3eb09a2772320a4567531b099957006a7d0f7849c14e7602a3ab9be68',
      'pos64-front-10g/identity.json':'76fcfa027dfb5289648cb4c50673377196f176b7b854ced784bb45711832f6e8'}

def sha(data):return hashlib.sha256(data).hexdigest()
def pin(key):
    p=R/key;need(p.resolve()==p and p.stat().st_size<1<<20,'receipt extent/path');b=p.read_bytes();need(sha(b)==PINS[key],'receipt digest: '+key);return b

def sparse(full):return {i:v for i,v in enumerate(full) if v}
def read_meta():
    a=json.loads(pin('scanplan-10g/scan.json'));b=json.loads(pin('scanplan-100g/scan.json'));ins=json.loads(pin('vocab-10g/inputs.json'));r=json.loads(pin('vocab-10g/review.json'))['actual_10g'];ident=json.loads(pin('pos64-front-10g/identity.json'))
    for z,n,s in ((a,P,SMALL),(b,N,BIG)):
        c=z['census'];need(c['n']==n and z['source_stat'][2]==n and z['copy_sha256']==c['sha256']==s and z['counted_column_scratch_bytes']==0 and c['global_end_rows']==1 and z['mincore_resident_pages']==0,'wrong scan')
        need(c['eof_raw_length']==n-c['eof_start'] and 0<c['eof_raw_length']<=c['max_phrase'],'scan EOF extent')
    need(a['source']==ins['raw']==str(BASE/'pf-10gb.txt') and a['source_stat']==ins['source_stat'] and ins['raw_sha256']==SMALL and b['source']==str(BASE/'pile-head100.txt') and a['kernel_sha256']==b['kernel_sha256'],'input lineage')
    p,s=a['census'],b['census'];del a
    lines=pin('vocab-10g/census.out').decode().splitlines();z={k:int(v) for k,v in (t.split('=') for t in lines[0].split())};rows=[tuple(map(int,line.split())) for line in lines[1:]]
    dc=counts([(i,d) for i,d,_ in rows]);oc=counts([(i,o) for i,_,o in rows]);ref=dict(t.split('=') for t in pin('vocab-10g/reference.out').decode().split()[1:]);tape=r['U']+16*r['k']-8
    need(all(r[x]==z[x] for x in ('n','m','k','U','D')) and all(z[x]==p[x] for x in ('n','m','eof_start','eof_raw_length','max_phrase')) and (r['n'],r['m'],r['k'],r['U'])==(P,318164488,234472723,9129290112),'calibrated prefix dimensions')
    need(oc==counts(p['cells']) and sum(dc)==r['U'] and digest(dc)==ref['descriptor_cells_sha256'] and int(ref['k'])==r['k'] and int(ref['descriptors'])==r['U'],'prefix descriptor/occurrence witness')
    w=ident['columns'];need(tape==12880853672 and ident['all_records_and_occurrences'] is True and w['lex64_sha256']=='5d8008a9d84aa9113a62a84ec92abc045409d4ae5f1d71860cbc51d0de112e07' and w['legacy_tape_sha256']==ref['tape_sha256'] and (int(w['m']),int(w['k']),int(w['descriptors']))==(r['m'],r['k'],r['U']),'wide 10 GB tape witness')
    need(s['m']==M and p['eof_raw_length']==80 and s['eof_raw_length']==16,'target parse / fixed hash extents');ot=sparse(counts(s['cells']));need(sum(ot.values())==N,'target occurrence mass')
    # No reason to read even the prefix if it cannot change the retained-tape branch.
    upper=N-(P-r['U'])+16*(M-p['m']+r['k'])-8+16*M;need(upper+(4<<20)+48<2*N<N+32*M-8,'no discriminating ledger test')
    return (P,r['m'],r['k'],r['U']),sparse(oc),sparse(dc),ot,b['source_stat'],r

def reduce(meta,small_tail,target_tail):
    dims,oc,dc,ot,_,r=meta;need(sum(small_tail.values())==80 and sum(target_tail.values())==16,'audited EOF shapes')
    bounds,lo,hi=inheritance(*dims,N,M,oc,dc,ot,small_tail,target_tail);ledger=price(N,M,bounds);need(ledger['remaining_vs_2N']>0 and ledger['prior_triple_upper']>2*N,'not the fixed target phase decision')
    sched=epoch_model(N,M,bounds['k_lower'],bounds['U_upper'],SLOTS)
    schedule={k:sched[k] for k in ('desired_bands','whole_wave_upper','total_file_upper','posting_cap','epoch_upper_if_every_range_fits')}
    return dict(vocabulary_interval=bounds,logical_column_ledger=ledger,two_code_cells=cells(lo,hi,ot,SLOTS),
                lower_cell_sha256=digest([lo.get(c,0) for c in range(CELLS)]),upper_cell_sha256=digest([hi.get(c,0) for c in range(CELLS)]),conditional_epoch_envelope=schedule,
                finite_key_group_lower=r['finite_groups']-sum(small_tail.values()),largest_full_finite_tie_lower=r['largest_finite_group_descriptors']-1)

def sig(st):return [st.st_dev,st.st_ino,st.st_size,st.st_mtime_ns,st.st_ctime_ns]
def certify(f,length,tail_size,expected,guard,chunk=4<<20):
    # A single forward-only read of EXACTLY `length` bytes. No speculative read
    # into the remainder or byte-string evidence taken from a different file.
    need(0<tail_size<=length and f.tell()==0 and chunk>0,'prefix range');md=hashlib.sha256();total=0;tail=bytearray()
    while total<length:
        guard();part=f.read(min(length-total,chunk));need(0<len(part)<=length-total,'short/long raw prefix read');md.update(part)
        if total+len(part)>length-tail_size:tail.extend(part[max(0,length-tail_size-total):])
        total+=len(part)
    got=md.hexdigest();need(total==f.tell()==length and len(tail)==tail_size and got==expected,f'prefix SHA mismatch bytes={total} got={got} expected={expected}')
    guard();return bytes(tail)

def stamp(path,z):
    data=(json.dumps(z,sort_keys=True,indent=2)+'\n').encode();need(len(data)<128<<10,'receipt cap')
    with path.open('xb') as f:os.posix_fallocate(f.fileno(),0,len(data));f.write(data);f.flush();os.fsync(f.fileno())

def main(d,end):
    need(d.resolve()==d and d.parent==S and re.fullmatch(r'prefix-vocab-cert-[A-Za-z0-9]{6}',d.name) and d.stat().st_uid==os.getuid() and stat.S_IMODE(d.stat().st_mode)==0o700 and time.time()<end<=time.time()+150 and shutil.disk_usage(d).free>256<<20,'private prefix-only admission')
    cg=Path('/sys/fs/cgroup'+Path('/proc/self/cgroup').read_text().strip().split('::')[1]);get=lambda k:(cg/k).read_text().strip();need((get('cpu.max'),get('memory.max'),get('memory.swap.max'))==('100000 100000',str(192<<20),'0') and len(os.sched_getaffinity(0))==1,'prefix certificate grant')
    usage=lambda:resource.getrusage(resource.RUSAGE_SELF);own={'review':SimpleNamespace(pid=os.getpid(),returncode=None,poll=lambda:None)}
    def guard():
        need(time.time()<end and not (d/'stop.requested').exists(),'prefix certificate STOP/deadline')
        ru=usage();need(512*ru.ru_inblock<=P+(192<<20) and 512*ru.ru_oublock<2<<20,'prefix service IO cap')
    def allocation():
        a,u,_=files(d,own,lambda _,p:p.poll());need(not u and max(a['scratch'])<=1<<20,'prefix controls allocation or unlink');return a['scratch']
    start=time.monotonic();barrier=optional_io_stat(cg);meta=read_meta();record=meta[4];path=BASE/'pile-head100.txt';need(path==path.resolve() and sig(path.stat())==record,'not the unchanged previously hashed target');guard();allocation()
    # Identity only: no source eviction, mode change or write. Not a cold clock.
    with os.fdopen(os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC),'rb',buffering=0) as f:
        st=os.fstat(f.fileno());need(stat.S_ISREG(st.st_mode) and sig(st)==record,'changed source at open');t=time.monotonic()
        small=certify(f,P,80,SMALL,guard);f.seek(N-16);big=f.read(16);need(len(big)==16 and f.tell()==N,'short target EOF')
        guard();need(sig(os.fstat(f.fileno()))==sig(path.stat())==record,'changed target during prefix audit');ts=time.monotonic()
    def tail(b):return dict(length=len(b),sha256=sha(b),cells=sorted(end_cells(b).items()))
    auth=dict(prefix_sha256=SMALL,prefix_bytes_hashed=P,target_tail_bytes_read=16,prefix_eof=tail(small),target_eof=tail(big),target_stat=record,target_prior_full_sha256_not_recomputed=BIG)
    stamp(d/'authentication.json',auth);result=reduce(meta,end_cells(small),end_cells(big));guard();events=get('memory.events');need({'oom 0','oom_kill 0'}<=set(events.splitlines()),'certificate OOM');ru=usage()
    z=dict(scope='Direct prefix authentication plus inherited bounds; not an exact target vocabulary, large sort, physical fit, rate or launch permission.',auth=auth,metadata_pins=PINS,result=result,utc=time.strftime('%FT%TZ',time.gmtime()),
           shared_limit=192<<20,swap_limit=0,cpu_max=get('cpu.max'),cores=sorted(os.sched_getaffinity(0)),peak_shared=int(get('memory.peak')),memory_events=events,io_stat_before=barrier,io_stat_after=optional_io_stat(cg),
           attributed_read=512*ru.ru_inblock,attributed_write_before_result=512*ru.ru_oublock,raw_bytes_requested=P+16,prefix_and_eof_s=ts-t,service_body_s=time.monotonic()-start,private_raw_copy_bytes=0,column_scratch_bytes=0,logical_allocated_controls_before_result=allocation())
    stamp(d/'result.json',z);allocation();print(json.dumps({k:result[k] for k in ('vocabulary_interval','logical_column_ledger','conditional_epoch_envelope')}|{'cell_classes':result['two_code_cells']['by_class']},sort_keys=True))
if __name__=='__main__':
    need(__debug__ and len(sys.argv)==3,'private one-shot prefix certificate only');main(Path(sys.argv[1]),int(sys.argv[2]))
