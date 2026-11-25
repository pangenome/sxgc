"""Whole-product comparison, restricted to one authenticated slice. No scale parameters."""
import os
from pathlib import Path
import re
import signal
import stat
import subprocess as sp
import sys
import time

from overlap_real_v10_plan import *
from overlap_real_v10_meter import Group,clock,files,grant,load,save
from private_cold import cold
from overlap_real_v10_volume import Volume


def private(p):need(p==p.resolve() and p.is_dir() and p.stat().st_uid==os.getuid() and stat.S_IMODE(p.stat().st_mode)==0o700,'not a private directory: '+str(p))
def fsyncdir(p):
    fd=os.open(p,os.O_DIRECTORY|os.O_RDONLY);os.fsync(fd);os.close(fd)
def same(a,b):
    if a.stat().st_size!=b.stat().st_size:return False
    with a.open('rb') as x,b.open('rb') as y:
        while True:
            p=x.read(1<<20)
            if p!=y.read(1<<20):return False
            if not p:return True

def prepare(h):
    src=SRC/'inputs';d=h.d;inputs=d/'in';inputs.mkdir();(inputs/'map').mkdir();t=clock()
    for k,(n,digest) in IN.items():
        s=src/k;p=inputs/k;need(s==s.resolve() and s.stat().st_size==n,'bad source extent');left=n
        with s.open('rb') as f,p.open('xb') as g:
            while left:
                h.guard();buf=f.read(min(left,1<<20));need(buf,'short source');g.write(buf);left-=len(buf)
            need(not f.read(1),'source extent changed');g.flush();os.fsync(g.fileno())
        need(sha(p)==digest and not p.samefile(s) and p.stat().st_nlink==1,'private copy SHA');p.chmod(0o400);cold(p)
    need(not (inputs/'raw').samefile(inputs/'checker'),'raw/checker alias');fsyncdir(inputs/'map');fsyncdir(inputs);h.timing['input_setup_and_cold_s']=clock()-t

def verify_job(h,core):
    end=h.end;d=h.d;user='/run/user/'+str(os.getuid());need(Path(user).is_dir(),'user manager missing')
    cmd=['/usr/bin/systemd-run','--user','--quiet','--wait','--pipe','--collect','--expand-environment=no','-p','MemoryMax=140M','-p','MemorySwapMax=0','-p','CPUQuota=100%','-p','Nice=10','-p','IOAccounting=yes','-p','LimitCORE=0','-p','LimitSTACK=8M','-p','LimitFSIZE=2M','-p','UMask=0077','-p',f'RuntimeMaxSec={max(3,end-int(time.time())+3)}s','-p','TimeoutStopSec=3s','/usr/bin/taskset','-c',core,'/usr/bin/python3','-BE',G/'overlap_real_v10_meter.py',d,h.name,end,core]
    return cmd,dict(XDG_RUNTIME_DIR=user,DBUS_SESSION_BUS_ADDRESS='unix:path='+user+'/bus')

def no_owner(path,proc):
    # The resident has copied, closed and reached emit. Never unlink somebody's
    # old columns or claim space for an unlinked but still-held private tape.
    need(path.is_file() and path.stat().st_nlink==1,'not private generated tape')
    if proc:
        need(str(path) not in Path(f'/proc/{proc.pid}/maps').read_text(),'still mapped')
        for fd in Path(f'/proc/{proc.pid}/fd').iterdir():
            try:need(os.readlink(fd)!=str(path),'still open')
            except FileNotFoundError:pass
    path.unlink();fsyncdir(path.parent)

def reference(root):
    r=root/'reference';out=r/'index.sxi2';proof=load(r/'verified.json');status=load(r/'status.json');need(not (r/'stop.requested').exists() and not status['stop'] and all(x['rc']==0 for x in status['worker'].values()) and proof['all_rows_raw_verified'] and proof['full_sha256']==sha(out) and 'arm-reference=0' in (root/'exits.log').read_text().splitlines(),'unchecked real reference');return out,proof

def arm(h,vcpu):
    d=h.d;q=d/'quota';ref=h.name=='reference';plan=model()[h.name];common=['--corpus',d/'in/raw','--remap',d/'in/remap','--threads',2];ctx=['--corpus',d/'in/raw','--n',ROWS,'--doc-starts',d/'in/map','--depth',8,'--min-support',1000,'--docs-min-support',1000,'--threads',1,'--out',q/'m20'];stop=h.stop;end=h.end;raw=d/'in/raw';ready=q/'front.ready';cols=q/'cols' if ref else q/'front.ready.partial';mapdir=d/'in/map';log=lambda name:(d/(name+'.log')).read_text();shared={'XSA_TEST_REAL_V10_100M_POOLS':'1','XSA_TEST_EXTERNAL_TRIPLES':'1','CROSS_V2_PIECES':'1','CROSS_V2_PIECE_WINDOW':'6','SS_LCP_SLICE_WINDOW':'5','XSA_BATCH_SXLS_ESCAPES':'1','PFCK_SPACING':'64'};pref=q/'merged/frag';product=d/'work.sxi2';tape=cols/('dictionary.packed' if ref else 'dictionary.lex64');key=cols/'keys.u64';inv=q/'inverse.u64';ids=cols/'parse.u32';other=None;m20cap=M20
    if not ref:
        other,prior=reference(h.root);m20cap=prior['table']['20']['bytes'];need(max(files(h.root,{},None)[0]['outside'])<(1<<20),'reference remnants outside controls')
    prepare(h);h.bound=plan['front'];h.phase='front';start=clock();front_files={};vol=h.volume=Volume(h,m20cap);vol.create()
    if ref:
        h.run('front',[LEX,raw,cols,'--packed']);need(ids.stat().st_size==4*M and tape.stat().st_size==U+12*K-8 and 'peak_counted='+str(N+20*M-8) in log('front'),'reference front');front_files.update(packed=tape.stat().st_size,ids=4*M);need(sha(ids)=='603e39e31da8fb51ad8838a114ee432dc03bb483e3e9f95f523b072d82e7f418','independent lexical ID SHA');need(set(cols.iterdir())=={ids,tape},'unexpected reference column')
    else:
        h.run('front',[FRONT,'pos-manifest',raw,ready,4,ARRAY,2*N-CONTROL,stop,end],fsize=U+16*K-8);need(ready.samefile(cols/'manifest') and ready.stat().st_nlink==2 and struct.unpack('<6Q',ready.read_bytes())==(0x3143465058,N,M,K,U,U+16*K-8) and key.stat().st_size==8*M and tape.stat().st_size==U+16*K-8 and set(cols.iterdir())=={key,tape,cols/'manifest'},'candidate manifest/columns');front_files.update(dict_bytes=tape.stat().st_size,keys=8*M);no_owner(tape,None);h.bound=plan['rank'];h.phase='rank';h.run('rank',[RANK,'rank',key,inv,4,ARRAY,stop,end]);need(inv.stat().st_size==8*M and sha(inv)==INV_SHA and 'permutation_recurrence=1' in log('rank'),'inverse proof or SHA');no_owner(key,None);front_files.update(inverse=8*M,inverse_sha256=INV_SHA)
    # Include private prepared-state cold barriers in the raw clock, not in
    # the first prepared timestamp. The two constructors have different columns.
    vol.reopen_cold()
    for f in (raw,d/'in/remap',mapdir/'seps.bin',d/'in/checker',*((tape,ids) if ref else (inv,))):cold(f)
    prepared=clock();h.timing['raw_to_prepared_s']=prepared-start;h.phase='prepared';h.bound=plan['prepared' if ref else 'stream'];p=None
    if ref:
        p=h.launch('resident',[REF,cols,stop,RES_ARRAY],env={'XSA_RESIDENT_RADIX_LANES':'4','XSA_RESIDENT_RADIX_STEP':'16384','XSA_RESIDENT_PACKED':'1','XSA_RESIDENT_WIDE_DICT':'1'},stream=True);h.until(lambda:(d/'resident.log').is_file() and 'RESIDENT_BEGIN phase=emit' in log('resident'));no_owner(tape,h.work['resident'])
    h.phase='stream';h.bound=plan['stream'];h.timing['ready_to_start_consumers_s']=clock()-prepared
    if ref:
        h.launch('tap',[JOIN,'ctx-tap','--bands',q/'tap-bands',*ctx],data=160<<20,fsize=m20cap)
        p=h.launch('relay',[RELAY,'narrow',raw,N,q/'tap-bands',stop],src=p,stream=True,fsize=262144)
    else:
        p=h.launch('emit',[EMIT,'staged-epochs',raw,inv,q/'posting',4,ARRAY,2*N-POST,SLOTS//5,SLOTS,stop,end],stream=True,fsize=POST)
        p=h.launch('tap',[TAP,'ctx-tap','--row43-pipe',stop,'--deadline',end,*ctx],src=p,stream=True,data=160<<20,fsize=m20cap)
    cmd,e=verify_job(h,vcpu);p=h.launch('witness',cmd,env=e,src=p,stream=True);h.launch('sink',[SINK,*common,'--out-prefix',pref,'--bands',q/'bands','--scratch-budget-bytes',2*N],src=p,env=shared,data=64<<20)
    h.until(lambda:(pref.parent/'frag.pieces/start').is_file() and (q/'bands/slice-slots').is_dir());h.launch('finish',[FIN,*common,'--prefix',pref,'--lcp-bands',q/'bands','--output',product,'--container-stream','--with-chi','--no-lcp-runs','--wit-ram-budget',128<<20,'--poll-timeout',120],env={'XSA_WORK_ROOT':'driver','PFCK_SPACING':'64','LD_PRELOAD':str(MON),'XSA_GATE_PRIVATE_STOP':str(stop)},data=192<<20);h.wait();ts=clock();h.timing.update(raw_to_base_finish_s=ts-start,prepared_to_base_finish_s=ts-prepared)
    need(f'FENCED rows={ROWS} final_rank={ROWS} runs={RUNS} ' in log('witness') and 'narrow_sha256='+ROW_SHA in log('witness') and 'FOLD_GEOMETRY loads=591 reads=972 staged=100001094 peak=3958321' in log('witness'),'full raw proof');ver=load(d/'checker-meter.json');need(ver['worker']['rc']==0 and ver['after']['memory.max']==str(ORACLE),'missing independent meter')
    for s in ['SS_V10_HANDOFF','SS_V8_PIECE_POOL_RETIRED slots=78','SS_V8_SLICE_POOL_RETIRED slots=5',f'SS_DONE_R overlap_stream rows={ROWS}']:need(s in log('sink'),s)
    for s in ['LCP_RUNS member=absent','tau=64 count=1562508 bytes=12500104',f'runs={RUNS} ',f'ENDPOINT_STREAM_PASS n={ROWS} k=1 r={RUNS}']:need(s in log('finish'),s)
    need(log('finish').count('PIECE_ABSORBED idx=')==590 and not (pref.parent/'frag.pieces/payload-slots').exists() and not (q/'bands/slice-slots').exists(),'missing durable retirement');need(ref or 'position_file_bytes=0' in log('tap') and 'EPOCH_INDEX epochs=4 file_cap=50000251 peak_extent=49896400 total_extent=165974592' in log('emit'),'rolling path');need(not ref or f'relay rows={ROWS} bands={ceil(ROWS,32768)} credit=1' in log('relay'),'reference transport')
    h.phase='join';h.run('join',[APPEND,'real-v10-quota-join'],data=160<<20)
    with product.open('rb') as f:os.fsync(f.fileno())
    fsyncdir(d);tj=clock();h.timing.update(raw_to_join_s=tj-start,prepared_to_join_s=tj-prepared,base_finish_to_join_s=tj-ts);save(d/'join-clock.json',h.timing)
    # All remaining work is proof/certification, never charged as producer speed.
    h.phase='postcheck';tbl=inventory(product,10);need(tbl[20]['sha256']==sha(q/'m20'),'in-place m20 mismatch');full=sha(product);need(sha(OLD_INDEX)==OLD_SHA,'v8 comparison source');old=inventory(OLD_INDEX,8);need(all(all(tbl[i][k]==old[i][k] for k in ('codec','bytes','count','crc','sha256')) for i in SAME),'changed stable base member')
    h.run('docs',[JOIN,'docstarts','verify',product,'--docs-map',mapdir,'--samples',1024]);need('DOCSTARTS_PARITY result=PASS' in log('docs'),'document samples')
    h.run('ctx',[JOIN,'ctx-verify','--sxi',product,'--text',raw,'--context','the','--context','and','--context','hex:1e']);need('exact=3 mismatches=0 below_min_support=0' in log('ctx'),'raw context check')
    bq=load(SRC/'source.json')['boundary_query'];pat=bytes.fromhex(bq['pattern_hex']).decode('ascii')
    for cross in (False,True):
        name='query'+str(int(cross));h.run(name,[JOIN,'search',product,'--pattern',pat,'--jsonl','--corpus',raw]+(['--cross-doc'] if cross else []));hits=[json.loads(s) for s in log(name).splitlines() if s.startswith('{')];need([(x['pos'],x['cross']) for x in hits]==([(bq['pos'],1)] if cross else []),'boundary search')
    if other:need(tbl==inventory(other,10) and same(product,other),'complete real v10 product differs')
    h.guard();out=d/'index.sxi2';os.link(product,out);product.unlink();fsyncdir(d);h.timing['postcheck_to_publication_s']=clock()-tj;h.timing['raw_to_certified_publication_s']=clock()-start
    # Keep authenticated products and logs. Only fresh closed scratch can go;
    # reference remnants/controls continue to count during the candidate arm.
    vol.retire();h.wait();need(not h.daemons-set(h.results),'filesystem worker was not reaped');h.sample();save(d/'verified.json',dict(arm=h.name,all_rows_raw_verified=True,rows=ROWS,runs=RUNS,chi=tbl[5]['count'],docs=DOCS,bytes=out.stat().st_size,full_sha256=full,full_candidate_equals_reference=not ref,table=tbl,same_v8_payloads=SAME,clocks=h.timing,front_files=front_files,position_file_bytes_each_way=8*ROWS if ref else 0,verifier=ver,plan=model(),scope='One authenticated real Pile prefix, direct Rust trie stream vs older independent resident + band tap; same v10 frozen finalizer. No 100 GB admission.'));print(f'{h.name}: byte-verified {ROWS} rows -> {out.stat().st_size} B complete v10, raw→join {tj-start:.3f} s, SHA {full}',flush=True)

def main(role,root,end,vcpu):
    private(root);need(role in ('reference','candidate') and root.parent==S and re.fullmatch(r'real-v10-quota100-[A-Za-z0-9]{6}',root.name),'fixed role and root');a=load(root/'admit.json');need(a['start']<=time.time()<end<=a['end'] and a['end']-a['start']<=1500 and a['end']-a['start']>900 and a['cores'][-1]==vcpu and not (root/'stop.requested').exists(),'time/CPU lease');need(a['pins']==pins() and a['files']==model(),'admission changed');d=root/role;d.mkdir(mode=0o700);h=Group(root,role,end);need(h.before['cores']==a['cores'][:4],'build affinity');signal.signal(signal.SIGTERM,lambda *_:h.stop.touch());signal.signal(signal.SIGINT,lambda *_:h.stop.touch())
    try:arm(h,vcpu)
    except BaseException:
        h.abort()
        if h.volume is not None:h.volume.preserve()
        raise
    finally:save(d/'status.json',h.report())

if __name__=='__main__':main(sys.argv[1],Path(sys.argv[2]),int(sys.argv[3]),int(sys.argv[4]))
