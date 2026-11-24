"""Fixed generated v10 reference. Build from raw through m20 and independently check the forwarded rows.
No Pile input, Pile timing, frozen finish, position band, full SA, or container assembly.
Usage: ... NEW_DIRECTORY FRONT64 PARSE64 FINITE_EMIT64 XSA ROW43_ORACLE OLD_FIXTURE_SXI2
"""
import hashlib
import json
import os
from pathlib import Path
import random
import re
import resource
import struct
import subprocess as sp
import sys
import time

N=6_311_445; M=202_740; ROWS=N+1; CONTROL=256<<10; CAP=N//2; M20_CAP=65536
RAW='0bcf20cfcdb2ddc4bd49c52c25d6b4c810c0364fb924c8b54d71968c6cb07539'
MAP='7ad88538c8c85a80eca58591a187a27539dea9d988a80c86e7c544372befc8c3'
PRODUCT='55eea26f9d735664204fb58f33f7c601477bfbecfb8f34d298e6425ce92bcb07'

def sha(f):
    with f.open('rb') as z:return hashlib.file_digest(z,'sha256').hexdigest()

def fixture(d,front,ranker,emit,xsa,oracle,ref):
    cg=Path('/sys/fs/cgroup'+Path('/proc/self/cgroup').read_text().strip().split('::')[1]);get=lambda k:(cg/k).read_text().strip()
    assert __debug__ and (get('memory.max'),get('memory.swap.max'),get('cpu.max'))==(str(256<<20),'0','200000 100000') and len(os.sched_getaffinity(0))==2
    assert ref.stat().st_size==8_847_248 and sha(ref)==PRODUCT
    d.mkdir(mode=0o700); d=d.resolve(); raw=d/'raw'; witness=d/'checker-raw'; mp=d/'map'; mp.mkdir(); stop=d/'stop.requested'
    a=b'abracadabra!\x1e'*1536+b'LEFT'; b=b'RIGHT---LEFTRIGHT'+random.Random(100).randbytes(6<<20).translate(bytes(97+i%20 for i in range(256)))
    raw.write_bytes(a+b); witness.write_bytes(a+b); (mp/'seps.bin').write_bytes(struct.pack('<4sIIIQQQQIIIQ',b'XSEP',1,1,64,3,N,1,0,0,3,len(a)-3,0)); del a,b
    for f in (raw,witness,mp/'seps.bin'): f.chmod(0o400)
    assert not raw.samefile(witness) and sha(raw)==sha(witness)==RAW and sha(mp/'seps.bin')==MAP
    until=int(time.time())+90; arrays=128<<20; cols=d/'front'; keys=cols/'keys.u64'; inv=d/'inverse.u64'; m20=d/'m20'
    def spawn(tag,args,*,file_limit=None,**kw):
        def cap():resource.setrlimit(resource.RLIMIT_FSIZE,(file_limit,file_limit))
        with (d/(tag+'.log')).open('xb') as log:
            return sp.Popen(list(map(str,args)),cwd=d,preexec_fn=cap if file_limit else None,stderr=log,
                **({'stdin':sp.DEVNULL,'stdout':log}|kw))
    def wait(p):return p.wait(timeout=max(.1,until-time.time()+1))
    assert wait(spawn('front',[front,'pos',raw,cols,2,arrays,2*N-CONTROL,stop,until]))==0
    # Only the fresh fixture's dictionary, after front exit. No banked column or input is retired.
    tape=cols/'dictionary.lex64'; assert set(cols.iterdir())=={tape,keys} and tape.is_file() and tape.stat().st_nlink==1
    front_bytes=tape.stat().st_size+keys.stat().st_size; assert keys.stat().st_size==8*M and front_bytes+CONTROL<=2*N
    tape.unlink(); assert wait(spawn('rank',[ranker,'rank',keys,inv,2,arrays,stop,until]))==0 and inv.stat().st_size==8*M
    assert 16*M+CAP+M20_CAP+CONTROL<=2*N
    cmd=[xsa,'ctx-tap','--row43-pipe',stop,'--deadline',until,'--n',ROWS,'--corpus',raw,'--doc-starts',mp,'--threads',1,'--out',m20]
    jobs=[]; statuses=[]
    try:
        e=spawn('emit',[emit,'staged-epochs',raw,inv,d/'posting',2,arrays,2*N-CAP,12000,65536,stop,until],stdout=sp.PIPE); jobs.append(e)
        t=spawn('tap',cmd,stdin=e.stdout,stdout=sp.PIPE,file_limit=M20_CAP);jobs.append(t);e.stdout.close()
        v=spawn('oracle',[oracle,witness,stop,'--fixture'],stdin=t.stdout);jobs.append(v);t.stdout.close()
        while any(p.poll() is None for p in jobs):
            if time.time()>=until or any(p.returncode not in (None,0) for p in jobs):stop.touch(exist_ok=True)
            if time.time()>until+2:raise TimeoutError('row pipeline')
            time.sleep(.05)
        statuses=[p.returncode for p in jobs];assert statuses==[0,0,0] and not stop.exists(),statuses
    finally:
        for p in jobs:
            if p.poll() is None:stop.touch(exist_ok=True);p.terminate()
        for p in jobs:p.wait(timeout=3)
        (d/'exits.json').write_text(json.dumps([p.returncode for p in jobs])+'\n')
    e=(d/'emit.log').read_text(); epochs=int(re.search(r'^EPOCH_INDEX epochs=(\d+)',e,re.M)[1]);assert 1<epochs<=8 and f'rows={ROWS} ' in e
    assert (d/'oracle.log').read_text().startswith(f'ORACLE rows={ROWS} independent_final_rank={ROWS} all_adjacent_sa_lcp_bwt_verified=1 ')
    assert f'CONTEXT_TRIE_ROW_PIPE rows={ROWS} position_file_bytes=0 m20_bytes=34608 ' in (d/'tap.log').read_text()
    with ref.open('rb') as f:
        h=f.read(64);count,=struct.unpack_from('<I',h,32);assert h[:8]==b'SXI2\x0a\0\0\0' and count==10
        f.seek(-40*count,2);table=list(struct.iter_unpack('<IIQQQQ',f.read()));entry,=[z for z in table if z[0]==20];mid,codec,off,size,*_=entry
        assert codec==20 and size==34608;f.seek(off);assert f.read(size)==m20.read_bytes()
    found={str(f.relative_to(d)) for f in d.rglob('*') if f.is_file()};assert found=={'raw','checker-raw','map/seps.bin','front/keys.u64','inverse.u64','posting','m20','front.log','rank.log','emit.log','oracle.log','tap.log','exits.json'}
    result=dict(scope='Generated raw -> forwarded row43 and v10 member 20, not a container or a performance run.',rows=ROWS,all_rows_raw_verified=True,
        front_plus_keys_bytes=front_bytes,retained_key_and_inverse_bytes=16*M,posting_cap=CAP,epochs=epochs,exits=statuses,shared_limit=256<<20,
        shared_peak=int(get('memory.peak')),swap_limit=0,cpus=2,reference_sha256=PRODUCT,m20_bytes=size,m20_sha256=sha(m20),
        old_band_file_bytes_each_direction=8*ROWS,new_position_file_bytes=0,tagged_pipe_bytes_each_direction=12*ROWS)
    (d/'result.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n');print(f'PASS {ROWS} independent raw rows, exact historical {size}-byte m20, {epochs} rolling epochs, zero position files.',flush=True)

if __name__=='__main__':fixture(*(Path(p).absolute() for p in sys.argv[1:]))
