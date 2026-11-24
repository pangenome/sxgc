"""Two bounded product arms. No real-corpus CLI; no stored candidate SA, LCP or position bands."""
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import stat
import struct
import subprocess as sp
import sys
import threading
import time
import zlib

from overlap_v10_product_input import CONTROLS, LOG_CAP, M20, M_CAP, N, POST, ROWS, RUNS, TAU, make, model
from private_cold import cold

S=Path('/mnt/nvme2n1/erikg/TAPEREADER/sandbox')
OLD=Path('/tmp/native-v10-p4oy1a/tools')
JOIN=Path('/tmp/v10-fixture-1001-1206931/tools/target/release/xsa')
TAP=Path('/tmp/ctx-rowpipe-cBRVXU/xsa')
FIN=Path.home()/'.cache/xsa/13524d6bbedad508fdd8e30e7ab709dbf62ae5db64eb0cd8864dd79efa5b7b4c/xsa_finish'
FRONT=S/'pos64-front-10g-XTNbSv/front'; RANK=S/'raw64-p1-10g-TyJqjC/parse64'; EMIT=Path('/tmp/epoch-array-fence-PtDPqy/emit')
PINS={JOIN:'cb9b2dc67b5b951bccc2ec7d78b85e05c7660be71e0d292dada83a17353fbb4b',TAP:'0daa34c5ddb1575fdeb0132dc4f416e5ffeb89fff5094cf3ae40ef3dd356d020',
      FIN:'bb929a7092cfeba7faa7c37ca999b71563e3af782d73fe693d1a35f5ecdbaaa9',OLD/'pfp_sort_r':'e5c0b66bc4be56165dc64997ece798473fe5e36d6134ea751a52e9143dba94e4',
      OLD/'relay':'703827daf10dc7f75f078268e47598528b491dee9dcbbc5da8caaff238e0797b',OLD/'v10_rows':'5a154f21061d55cfa48b8fc3d8ded5d0bb62c6abd6a105468e07abd6c786d59b',
      FRONT:'463199aa720822e702082d34edfd290a6d508e0b27cffad03e8063f8cb2de5f6',RANK:'668f1a3a861fbca4c578fa643db73f4204be1e5a2e1f9e864131bb5586dafefe',EMIT:'109d1562dce50ea42c4c6165db9a1e37e6d33de18209132b0415f31b73652a5b'}
INPUT={'raw':'d24608087840588c05c8f3a7df674ee713ed66c3f0ef219f6d3f6a877ac3d8ef','map':'6022aa68d036848d23efe984ba8300d0a4a4648cbecfc80fc10933c95090f473','remap':'8abe0be4061f15f505476387304c5707eaae0b7d809e0495b12b1e72e95ff1ff'}

def digest(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def store(p,obj):
    with p.open('x') as f:json.dump(obj,f,sort_keys=True,indent=2);f.write('\n')

def optional_io_stat(cgroup):
    # IOAccounting=yes does not guarantee the io controller is delegated here.
    # Never turn absence into zero. Admission/exit/memory counters remain mandatory.
    try:return (cgroup/'io.stat').read_text().strip() or None
    except FileNotFoundError:return None

def inventory(sxi):
    data=sxi.read_bytes(); h=data[:64];assert h[:8]==b'SXI2\x0a\0\0\0' and struct.unpack_from('<Q',h,8)[0]==ROWS
    count,=struct.unpack_from('<I',h,32);assert count==10
    rec=list(struct.iter_unpack('<IIQQQQ',data[-count*40:]));assert [z[0] for z in rec]==[1,4,5,7,10,11,12,13,15,20]
    assert all(64<=off<=len(data)-count*40 and size<=len(data)-count*40-off for _,_,off,size,_,_ in rec)
    assert struct.unpack_from('<Q',h,40)[0]==len(data)
    assert zlib.crc32(h[:56]+bytes(4)+h[60:]+data[-count*40:])==struct.unpack_from('<I',h,56)[0]
    assert all(zlib.crc32(data[o:o+n])==crc for _,_,o,n,_,crc in rec)
    return [dict(id=i,codec=c,offset=o,bytes=n,count=k,stored_crc=crc,sha256=hashlib.sha256(data[o:o+n]).hexdigest()) for i,c,o,n,k,crc in rec]

class Arm:
    def __init__(self,root,arm):
        self.root=root;self.d=root/arm;self.name=arm;self.stop=root/'stop.requested';self.fd_retries=0;self.clocks={};self.work={};self.threads=[];self.errors=[];self.peak=0;self.alloc=0;self.argv={}
        self.cg=Path('/sys/fs/cgroup'+Path('/proc/self/cgroup').read_text().strip().split('::')[1]);self.until=int(time.time())+135
        assert __debug__ and self.cgval('memory.max')==str(384<<20) and self.cgval('memory.swap.max')=='0' and self.cgval('cpu.max')=='200000 100000' and len(os.sched_getaffinity(0))==2
        assert not self.stop.exists() and not self.d.exists()
        self.inp=make(self.d);d=self.d
        assert all(self.inp[k]==v for k,v in INPUT.items()) and [digest(p) for p in (d/'raw',d/'checker',d/'map/seps.bin',d/'remap')]==[INPUT[k] for k in ('raw','raw','map','remap')]
        self.raw=d/'raw';self.bands=d/'bands';self.prefix=d/'merged/frag';self.product=d/'work.partial.sxi2';self.m20=d/'m20'
        self.bands.mkdir();self.prefix.parent.mkdir();store(d/'inputs.json',self.inp)
        self.excluded=set()
        for a in ('reference','candidate'):
            self.excluded.update(root/a/q for q in ('raw','checker','map/seps.bin','remap','work.partial.sxi2','index.sxi2'))
        self.excluded.update([root/'witness'])  # fixed construction executable, like the external pinned tools
        if arm=='candidate':
            assert (root/'reference/verified.json').is_file() and self.measure() <= CONTROLS//2
        assert model()[arm]<=2*N
    def cgval(self,k):return (self.cg/k).read_text().strip()
    def drain(self,pipe,path):
        try:
            with pipe,path.open('xb') as f:
                written=0
                while data:=pipe.read(4096):
                    if written+len(data)>(48<<10 if path.name=='finish.log' else LOG_CAP):raise RuntimeError(f'{path.name} log cap')
                    f.write(data);written+=len(data)
        except Exception as e:self.errors.append(str(e));self.stop.touch()
    def spawn(self,name,argv,env=None,src=None,stream=False,cap=None):
        assert name not in self.work and len(self.work)<12 and not self.stop.exists();a=[str(x) for x in argv];self.argv[name]=a
        # Use prlimit exec, not preexec_fn in this multithreaded supervisor.
        if cap:a=['/usr/bin/prlimit',f'--fsize={cap}:{cap}','--',*a]
        env0=dict(PATH='/usr/bin:/bin',HOME=str(Path.home()),LANG='C',OMP_DYNAMIC='FALSE',OMP_WAIT_POLICY='PASSIVE',GOMP_SPINCOUNT='0',OMP_STACKSIZE='1M',RAYON_NUM_THREADS='1')
        p=sp.Popen(a,env=env0|(env or {}),cwd=self.d,start_new_session=True,stdin=src if src is not None else sp.DEVNULL,
                   stdout=sp.PIPE,stderr=sp.PIPE if stream else sp.STDOUT,bufsize=0)
        self.work[name]=p
        if src is not None:src.close()
        th=threading.Thread(target=self.drain,args=(p.stderr if stream else p.stdout,self.d/(name+'.log')),daemon=True); self.threads.append(th);th.start()
        return p.stdout if stream else None
    def measure(self):
        seen={}
        def add(p,s):
            if p not in self.excluded and stat.S_ISREG(s.st_mode):seen[(s.st_dev,s.st_ino)]=(s.st_size,s.st_blocks*512)
        for path,_,files in os.walk(self.root):
            for f in files:
                p=Path(path,f)
                try:add(p,p.lstat())
                except FileNotFoundError:pass
        # Include an unlinked regular file while any worker still holds it open.
        for p in self.work.values():
            if p.returncode is not None:continue
            # An exec()/exit can temporarily deny /proc access after poll() said
            # live. Restart the walk, or reap the actual child. Never substitute
            # an empty fd list for a live, persistently inaccessible process.
            for attempt in range(12):
                if p.poll() is not None:break
                try:
                    for fd in Path(f'/proc/{p.pid}/fd').iterdir():
                        try:
                            name=os.readlink(fd)
                            if name.startswith(str(self.root)+'/'):add(Path(name.removesuffix(' (deleted)')),fd.stat())
                        except FileNotFoundError:pass  # this fd closed
                    break
                except (FileNotFoundError,ProcessLookupError,PermissionError):
                    self.fd_retries+=1
                    if p.poll() is not None:break
                    if attempt==11 or self.stop.exists():raise
                    time.sleep(.01)
        n=sum(x for x,_ in seen.values());self.peak=max(self.peak,n);self.alloc=max(self.alloc,sum(y for _,y in seen.values()));return n
    def wait(self):
        while True:
            rcs={k:p.poll() for k,p in self.work.items()}
            if self.errors or self.stop.exists() or any(x not in (0,None) for x in rcs.values()) or time.time()>=self.until:raise RuntimeError((rcs,self.errors,'STOP/deadline or failed worker'))
            assert self.measure()<=2*N,'logical scratch alert'
            if None not in rcs.values():break
            time.sleep(.04)
        for th in self.threads:th.join(2);assert not th.is_alive()
        assert not self.stop.exists() and not self.errors
    def cmd(self,name,argv,**kw):self.spawn(name,argv,**kw);self.wait()
    def interrupt(self):
        self.stop.touch()
        for flag in [self.bands/'producer.interrupted',self.prefix.parent/'modeb/stop.requested',self.d/'tap/producer.interrupted']:
            if flag.parent.is_dir():flag.touch()
        for sig in (None,signal.SIGTERM,signal.SIGKILL):
            for p in self.work.values():
                if p.poll() is None and sig:
                    try:os.killpg(p.pid,sig)
                    except ProcessLookupError:pass
            for p in self.work.values():
                if p.poll() is None:
                    try:p.wait(timeout=.5)
                    except sp.TimeoutExpired:pass
    def query_checks(self):
        mp=self.d/'map';raw=self.raw;log=lambda tag:(self.d/(tag+'.log')).read_text()
        self.cmd('docs',[JOIN,'docstarts','verify',self.product,'--docs-map',mp,'--samples',128]);assert 'DOCSTARTS_PARITY result=PASS' in log('docs')
        self.cmd('ctx',[JOIN,'ctx-verify','--sxi',self.product,'--text',raw,'--context','xxx','--context','xxxx']);assert 'exact=2 mismatches=0 below_min_support=0' in log('ctx')
        for cross in (False,True):
            tag='search'+str(int(cross));self.cmd(tag,[JOIN,'search',self.product,'--pattern',self.inp['pattern'],'--jsonl','--corpus',raw]+(['--cross-doc'] if cross else []));hits=[json.loads(x) for x in log(tag).splitlines() if x.startswith('{')]
            assert [(q['pos'],q['cross']) for q in hits]==([(self.inp['pos'],1)] if cross else [])
    def recover_reference(self):
        # One known supervisor failure AFTER the worker joins, not a resume flag
        # for incomplete streams. Never open the failed service's files writable.
        old=Path('/tmp/rolling-v10-product-ljelES');src=old/'reference';joined=src/'work.partial.sxi2'
        assert (old/'stop.requested').is_file() and not (old/'candidate').exists() and (old/'exits.log').read_text()=='compile=0\npins=0\narm-reference=1\nlauncher=1\n'
        assert digest(old/'arm-reference.log')=='c172317e9cff4b6dd64465dbd8e7395155682d390f412f9033b77b1d477099fb' and digest(src/'status.json')=='196a3e3d43ea7083d0df034f5c0bf72f103c10703e9125bbf14ac92abd0ab09d'
        status=json.loads((src/'status.json').read_text());assert status['exits']==dict.fromkeys(('finish','join','relay','rows','sink','tap','witness'),0) and status['stop'] is True
        assert digest(src/'witness.log')=='e49e8969f876e2a546acbea6be64f8bfc47671b606a743ccf4e559efc661b7ab' and f'ORACLE rows={ROWS} independent_final_rank={ROWS} all_adjacent_sa_lcp_bwt_verified=1' in (src/'witness.log').read_text()
        assert [digest(src/p) for p in ('raw','checker','map/seps.bin','remap')]==[INPUT[k] for k in ('raw','raw','map','remap')]
        sha='921f4e473e19fefcf8a7916d431c28694941323b6262ff109ae3c7c7820f19ab';assert joined.stat().st_size==378392 and digest(joined)==sha
        with self.product.open('xb') as f:f.write(joined.read_bytes());f.flush();os.fsync(f.fileno())
        assert digest(self.product)==sha;table=inventory(self.product);self.query_checks();assert not self.stop.exists()
        os.link(self.product,self.d/'index.sxi2');self.product.unlink();fd=os.open(self.d,os.O_RDONLY|os.O_DIRECTORY);os.fsync(fd);os.close(fd)
        store(self.d/'verified.json',dict(arm='reference',scope='Read-only authentication of a completed join and separate query checks; no row replay or build clock.',original=str(src),original_supervisor_exit=1,original_stop=True,original_worker_exits=status['exits'],original_shared_peak=status['shared_peak'],original_sampled_scratch=status['sampled_logical_scratch'],raw_to_join_s=None,rows=ROWS,all_rows_raw_verified=True,inputs=self.inp,members=table,bytes=378392,sha256=sha,verification_exits={k:p.returncode for k,p in self.work.items()}))
        print('Verified previously joined reference; first launcher failure preserved; no repeated rows.',flush=True)
    def product_run(self):
        d=self.d;raw=self.raw;mp=d/'map';ref=self.name=='reference';clock=time.monotonic
        before=clock()
        for q in (raw,d/'checker',d/'remap',mp/'seps.bin'):cold(q)
        t0=clock();plan=model();common=['--corpus',raw,'--remap',d/'remap','--threads',1];ctx=['--corpus',raw,'--out',self.m20,'--doc-starts',mp,'--n',ROWS,'--depth',8,'--min-support',1000,'--docs-min-support',1000,'--threads',1]
        files={};arrays=128<<20
        if not ref:
            cols=d/'cols';keys=cols/'keys.u64';inv=d/'inverse.u64';tape=cols/'dictionary.lex64'
            self.cmd('front',[FRONT,'pos',raw,cols,2,arrays,2*N-CONTROLS,self.stop,self.until]);assert set(cols.iterdir())=={keys,tape} and 0<keys.stat().st_size<=8*M_CAP and not keys.stat().st_size%8
            files.update(tape_bytes=tape.stat().st_size,keys_bytes=keys.stat().st_size);assert tape.stat().st_nlink==1;tape.unlink()
            self.cmd('rank',[RANK,'rank',keys,inv,2,arrays,self.stop,self.until]);assert inv.stat().st_size==keys.stat().st_size and keys.stat().st_nlink==1
            files.update(inverse_bytes=inv.stat().st_size,inverse_sha256=digest(inv));keys.unlink()
            cold(inv)
        prepared=clock();self.clocks.update(cold_setup_s=t0-before,front_and_rank_barriers_s=prepared-t0)
        # No polling credit is used for the global file bound: even all 25 SXLS payloads and ALL PRC1s fit.
        self.spawn('finish',[FIN,*common,'--prefix',self.prefix,'--lcp-bands',self.bands,'--output',self.product,'--poll-timeout','120','--wit-ram-budget',128<<20,'--with-chi','--no-lcp-runs'],env={'XSA_WORK_ROOT':'driver','PFCK_SPACING':str(TAU)})
        if ref:
            tap=d/'tap';self.spawn('tap',[JOIN,'ctx-tap','--bands',tap,*ctx],cap=M20)
            p=self.spawn('rows',[OLD/'v10_rows',raw],env={'XSA_TEST_OVERLAP_BINNED':'1'},stream=True)
            p=self.spawn('relay',[OLD/'relay','row43',raw,N,tap,self.stop],src=p,stream=True)
        else:
            p=self.spawn('emit',[EMIT,'staged-epochs',raw,inv,d/'posting',2,arrays,2*N-POST,2048,32768,self.stop,self.until],stream=True)
            p=self.spawn('tap',[TAP,'ctx-tap','--row43-pipe',self.stop,'--deadline',self.until,*ctx],src=p,stream=True,cap=M20)
        p=self.spawn('witness',[self.root/'witness',d/'checker',self.stop,'--generated-v10-pipe'],src=p,stream=True)
        self.spawn('sink',[OLD/'pfp_sort_r',*common,'--out-prefix',self.prefix,'--bands',self.bands,'--scratch-budget-bytes',2*N],src=p,env={'CROSS_V2_PIECES':'1','XSA_TEST_EXTERNAL_TRIPLES':'1','SS_LCP_SLICE_WINDOW':'25','CROSS_V2_PIECE_WINDOW':'5'})
        self.wait();t1=clock();self.clocks['stream_and_finish_s']=t1-prepared;log=lambda tag:(d/(tag+'.log')).read_text()
        assert f'ORACLE rows={ROWS} independent_final_rank={ROWS} all_adjacent_sa_lcp_bwt_verified=1' in log('witness') and 'escapes=0' in log('witness')
        runs=int(re.search(r'PRODUCT_FENCE runs=(\d+)',log('witness'))[1]);assert runs<=RUNS and f'rows={ROWS}' in log('sink')
        assert 'tau=64' in log('finish') and 'LCP_RUNS member=absent' in log('finish') and f'runs={runs} ' in log('finish')
        epochs=None
        if not ref:
            epochs=int(re.search(r'EPOCH_INDEX epochs=(\d+)',log('emit'))[1]);assert 1<=epochs<=8 and 'position_file_bytes=0' in log('tap') and not (d/'tap').exists()
        else:assert {p.name for p in tap.iterdir()}=={'end','end.ready'} and f'rows={ROWS} ' in log('relay')
        m20sha=digest(self.m20);m20len=self.m20.stat().st_size;assert 0<m20len<=M20
        self.cmd('join',[JOIN,'fixture-join',self.product,self.m20,mp]);
        with self.product.open('rb') as f:os.fsync(f.fileno())
        tj=clock();self.clocks.update(raw_to_join_s=tj-t0,prepared_to_join_s=tj-prepared,join_and_publish_payload_s=tj-t1);table=inventory(self.product);assert table[-1]['sha256']==m20sha and table[-1]['bytes']==m20len; sc=self.product.stat().st_size
        self.query_checks()
        if not ref:
            r=self.root/'reference/index.sxi2';assert table==inventory(r) and self.product.read_bytes()==r.read_bytes(),'complete v10 product mismatch'
        assert not self.stop.exists() and not re.search(r'^(?:oom|oom_kill) [1-9]',self.cgval('memory.events'),re.M)
        full_sha=digest(self.product); self.clocks['postcheck_s']=clock()-tj
        result=dict(arm=self.name,rows=ROWS,runs=runs,epochs=epochs,all_rows_raw_verified=True,inputs=self.inp,members=table,bytes=sc,sha256=full_sha,full_identity=not ref,
                    limits=plan,front_files=files,position_bytes_each_way=(ROWS*8 if ref else 0),end_utc=time.strftime('%FT%TZ',time.gmtime()),**self.clocks,
                    shared_peak=int(self.cgval('memory.peak')),memory_max=int(self.cgval('memory.max')),swap_max=0,cpu_max=self.cgval('cpu.max'),cpu_stat=self.cgval('cpu.stat'),io_stat=optional_io_stat(self.cg),memory_events=self.cgval('memory.events'),
                    sampled_logical_scratch=self.peak,sampled_allocated_scratch=self.alloc,exits={k:p.returncode for k,p in self.work.items()})
        assert not self.stop.exists();out=d/'index.sxi2';os.link(self.product,out);self.product.unlink();fd=os.open(d,os.O_RDONLY|os.O_DIRECTORY);os.fsync(fd);os.close(fd)
        store(d/'verified.json',result);print(f'{self.name}: {ROWS} raw rows -> {sc} byte v10, 10 members, sha256 {full_sha}, raw-to-join {tj-t0:.3f} s',flush=True)

def main():
    if len(sys.argv)==2 and sys.argv[1]=='pins':
        for p,want in PINS.items():assert digest(p)==want,p;print(want,p)
        return
    arm,root=sys.argv[1],Path(sys.argv[2]).resolve();assert arm in ('reference','candidate','check-joined-reference') and root.parent==Path('/tmp') and root.name.startswith('rolling-v10-product-') and stat.S_IMODE(root.stat().st_mode)==0o700
    recovery=arm=='check-joined-reference'
    h=Arm(root,'reference' if recovery else arm);signal.signal(signal.SIGTERM,lambda *_:h.stop.touch())
    try:
        if recovery:h.recover_reference()
        else:h.product_run()
    except BaseException:h.interrupt();raise
    finally:store(h.d/'status.json',dict(argv=h.argv,exits={k:p.poll() for k,p in h.work.items()},stop=h.stop.exists(),errors=h.errors,clocks=h.clocks,fd_sampling_retries=h.fd_retries,sampled_logical_scratch=h.peak,shared_peak=int(h.cgval('memory.peak')),memory_events=h.cgval('memory.events')))

if __name__=='__main__':main()
