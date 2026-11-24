"""Generated-only diagnostic for the frozen v8 entry frame (no Pile input or sweep).
Use two dedicated services: ready 192 MiB/2 CPUs, hard DATA/STACK=60/3 MiB;
proof 128 MiB/1 CPU. The failed 64/1 MiB corpus study stays retired.
"""
import hashlib
import os
from pathlib import Path
import random
import re
import resource
import shutil
import stat
import subprocess as sp
import sys
import time

import overlap_parse64_study as s

M=1<<20; SIZE=2*M-9; T=128; COUNT=(SIZE+1)//T+1; EXT=40+8*COUNT; BANK=s.RECEIPTS/'pfck128-ready-entry-failure';FAIL='ca5134432c76f66504a433a7b3de53690ca85e4cb1a76a37e525c398c28fad8c'

def tools():
    assert s.sha(BANK/'ready.json')==FAIL and s.load(BANK,'ready.json')['rc']==-11
    p=s.load(BANK,'source.json');assert p['main_fixed_frame_bytes']==2_222_464 and p['stack_cap_bytes']==M
    # Open *only these executables*, never the original raw/remap/STOP or any old output.
    orig=Path(p['original_directory']);hashes=s.load(BANK,'tools.json');paths={k:orig/k for k in ('finish','monitor.so','proof')}
    assert hashes['finish']=='9a0bde6739ba1819f8d298beca2c8a9bf53480fab19f637b49cf6e3c8e200fa7'
    assert all(f.resolve()==f and f.is_file() and s.sha(f)==hashes[k] for k,f in paths.items());return paths

def ledger(d,pid):
    # Regular file extents in this flat fixture, including open/deleted inodes in the native child.
    raw=(d/'synthetic').stat();tab={}
    for f in d.iterdir():
        st=f.lstat();assert stat.S_ISREG(st.st_mode);tab[st.st_dev,st.st_ino]=st
    for f in Path(f'/proc/{pid}/fd').glob('*'):
        try:
            if not os.readlink(f).startswith(str(d)+'/'):continue
            st=f.stat();assert stat.S_ISREG(st.st_mode);tab[st.st_dev,st.st_ino]=st
        except (FileNotFoundError,ProcessLookupError):pass
    return sum(st.st_size for key,st in tab.items() if key!=(raw.st_dev,raw.st_ino)),sum(st.st_blocks*512 for st in tab.values())

def child(d,name,cmd,env,grant,cpu,affinity):
    stop=d/'stop.requested';until=int(time.time())+18;assert not stop.exists() and not (d/(name+'.json')).exists()
    before=s.grant(grant,cpu);r=dict(status='INCOMPLETE',before=before,affinity=affinity,argv=list(map(str,cmd)),env=env,hard_native_fsize=EXT,start_unix=int(time.time()));p=None
    def caps():os.sched_setaffinity(0,affinity);resource.setrlimit(resource.RLIMIT_FSIZE,(EXT,EXT))
    try:
        reason=None;start=time.monotonic();counted=allocated=rss=0
        with (d/(name+'.log')).open('xb') as log:
            p=sp.Popen(cmd,stdin=sp.DEVNULL,stdout=log,stderr=log,cwd=d,env={'PATH':os.defpath,'LC_ALL':'C'}|env,preexec_fn=caps)
            with open(f'/proc/{p.pid}/io') as io:
                while True:
                    done=s.finished(p);x=s.proc_info(p.pid,io);rss=max(rss,x['VmHWM']);live,blocks=ledger(d,p.pid);counted=max(live,counted);allocated=max(blocks,allocated)
                    traffic=[(x['read_bytes'],x['write_bytes'])]+[(v['rbytes'],v['wbytes']) for v in (s.io_delta(s.stat_io(before['io.stat']),s.current_io(s.CG)) or {}).values()]
                    if time.time()>=until or stop.exists() or live>2*EXT+(128<<10) or shutil.disk_usage(d).free<64*M or any(a>24*M or b>4*M for a,b in traffic):reason='deadline/space/IO/stop';stop.touch(exist_ok=True)
                    if done:break
                    time.sleep(.01)
            r.update(rc=p.wait(),wall_s=time.monotonic()-start,process=x,stop=reason,sampled_rss=rss,counted_sampled=counted,whole_allocation_sampled=allocated)
        g=s.grant(grant,cpu);r.update(after=g,per_device_io=s.io_delta(s.stat_io(before['io.stat']),s.stat_io(g['io.stat'])))
        assert r['rc']==0 and reason is None and int(g['memory.peak'])<=grant;r['status']='COMPLETED'
    finally:
        if p is not None and p.poll() is None:stop.touch(exist_ok=True);p.wait(timeout=3)
        s.save(d/(name+'.json'),r)

def fixture(mode,d):
    assert __debug__ and d.resolve()==d and d.parent==Path('/tmp') and re.fullmatch(r'pfck-stack-fixture-[A-Za-z0-9]{6}',d.name) and d.stat().st_uid==os.getuid() and d.stat().st_mode&0o777==0o700
    s.exclusive();t=tools();a=s.cores()[:2];stop=d/'stop.requested';assert not stop.exists() and shutil.disk_usage(d).free>=64*M
    raw,tab,ck=d/'synthetic',d/'map',d/'frag.pfck2'
    if mode=='ready':
        s.grant(192*M,2);assert not any(d.iterdir()) and resource.getrlimit(resource.RLIMIT_STACK)==(3*M,3*M) and resource.getrlimit(resource.RLIMIT_DATA)==(60*M,60*M) and Path('/sys/module/kernel/parameters/ignore_rlimit_data').read_text().strip()=='N'
        # New fixture partition: even the *sum of the two ceilings* drops (65 -> 63 MiB).
        # Neither this sum nor LimitDATA purports to bound all cache/mappings. No stack setter/interposer.
        assert 2_222_464<3*M and (60+3)*M<(64+1)*M and COUNT==16384 and EXT==131112 and 2*EXT+(128<<10)<2*SIZE
        gen=random.Random(61428).randbytes(SIZE).translate(bytes(31+b%63 for b in range(256)));used=sorted(set(gen));assert used==list(range(31,94));absent=sorted(set(range(256))-set(used));remap=bytearray(256)
        for i,b in enumerate(used+absent):remap[b]=(i+1)%256
        with raw.open('xb') as f:f.write(gen)
        with tab.open('xb') as f:f.write(remap)
        r=dict(source='generated Python Random(61428), bytes 31..93; NOT a Pile slice',raw_sha256=hashlib.sha256(gen).hexdigest(),remap_sha256=hashlib.sha256(remap).hexdigest(),raw_bytes=SIZE,actual_stack_soft_hard=list(resource.getrlimit(resource.RLIMIT_STACK)),actual_data_soft_hard=list(resource.getrlimit(resource.RLIMIT_DATA)),frozen_tools={k:s.sha(f) for k,f in t.items()});del gen
        for f in (raw,tab):f.chmod(0o400);s.cold(f)
        s.save(d/'fixture.json',r|dict(cold_pages=0));cmd=[t['finish'],'--corpus',raw,'--remap',tab,'--prefix',d/'frag','--output',d/'absent/product','--lcp-bands',d/'absent/bands','--with-chi','--container-stream','--keep-pfck','--threads','2','--wit-ram-budget','134217728','--poll-timeout','1'];env=dict(LD_PRELOAD=str(t['monitor.so']),XSA_GATE_PRIVATE_STOP=str(stop),MALLOC_ARENA_MAX='2',PFCK_SPACING='128',XSA_FINISH_RSS_READINESS_ONLY='1')
        child(d,'ready',cmd,env,192*M,2,a);log=(d/'ready.log').read_text();assert ck.stat().st_size==EXT and f'n={SIZE+1} tau=128 count={COUNT} bytes={EXT} ' in log and 'workers=2 segments=8 worker_bytes=1572864' in log and 'stage=readiness-pfck-histogram-probe' in log and not (d/'absent').exists();ck.chmod(0o400);print(f'FIXTURE_READY n={SIZE+1} extent={EXT}',flush=True)
    elif mode=='proof':
        s.grant(128*M,1);assert s.load(d,'ready.json')['status']=='COMPLETED';ref=s.load(d,'fixture.json');assert s.sha(raw)==ref['raw_sha256'] and s.sha(tab)==ref['remap_sha256'] and ck.stat().st_size==EXT
        for f in (ck,raw,tab):s.cold(f)
        child(d,'proof',[t['proof'],'--fixture',raw,tab,ck,stop,str(int(time.time())+18)],{},128*M,1,a[:1]);assert (d/'proof.log').read_text().strip()==f'PFCK_PROVED raw_bytes={SIZE} n={SIZE+1} tau=128 checkpoints={COUNT} file_bytes={EXT} used_symbols=63 virtual_end=1 exact_mod_2pow64=1'
        s.save(d/'identity.json',dict(pfck_sha256=s.sha(ck),count=COUNT,size=EXT,cold_pages=0));print('PASS generated remainder-120 PFCK, 16384 independent equations; no native slice/row/product',flush=True)
    else:raise ValueError('ready|proof')
if __name__=='__main__':fixture(sys.argv[1],Path(sys.argv[2]))
