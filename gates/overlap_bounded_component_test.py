"""Tiny native oracle, exact-entry refusals, and cooperative-meter tests; no bank IO."""
import contextlib
import hashlib
import json
from pathlib import Path
import os
import struct
import subprocess as sp
import sys
import tempfile
import time
from unittest.mock import patch

import overlap_bounded_component as g
from overlap_row43_test import wire


def rejects(fn):
    try: fn()
    except (ValueError, RuntimeError, FileNotFoundError): return
    raise AssertionError('expected refusal')


def oracle(tmp, producer):
    cpp=Path(__file__).with_name('overlap_bounded_component_oracle.cpp')
    for tiny in (True,False):
        exe=tmp/('tiny' if tiny else 'exact')
        sp.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror',*(['-DCOMPONENT_TINY_TEST'] if tiny else []),cpp,'-lcrypto','-o',exe],check=True)
    raw=tmp/'raw'; stop=tmp/'stop'; cmd=[tmp/'tiny','--tiny-test',raw,stop]
    text=b'banana\x00banana\xff'; raw.write_bytes(text); prev=b''; rows=[]
    for p in sorted(range(len(text)+1),key=lambda i:text[i:]):
        s=text[p:]; h=next((i for i,(a,b) in enumerate(zip(s,prev)) if a!=b),min(len(s),len(prev)))
        rows.append((p,h,text[p-1] if p else 256)); prev=s
    stream=b''.join(wire(*r) for r in rows)
    digest=hashlib.sha256(b''.join(struct.pack('<III',*r) for r in rows)).hexdigest()
    def run(data,cmd=cmd): return sp.run(cmd,input=data,capture_output=True)
    r=run(stream); assert r.returncode==0 and r.stdout.decode().strip()==f'VERIFIED rows={len(rows)} final_rank={len(rows)} narrow_sha256={digest}'
    for data in (stream[12:],stream[:-1],stream[:-12],stream+stream[:12],wire(len(text)+(1<<32),0,text[-1])+stream[12:],
                 wire(len(text),1<<32,text[-1])+stream[12:],wire(len(text),0,0)+stream[12:],stream[:24]+stream[12:24]+stream[36:]):
        r=run(data); assert r.returncode and not r.stdout
    for mode in ('--tiny-test','--component-100m','--component-1g'):
        assert run(stream,[tmp/'exact',mode,raw,stop]).returncode==70
    stop.touch(); assert run(stream).returncode==75; stop.unlink()
    (tmp/'parse.u32').write_bytes(b'\0'*4)
    # No timed worker: a one-ID parse must fail before rank/sort/emit.
    for args in (['--component-100m',tmp,stop],['--component-100m',tmp,stop,'1'],['--component-1g',tmp,stop]):
        r=sp.run([producer,*args],capture_output=True); assert r.returncode and not r.stdout


def policy(tmp):
    log=f'LEXPARSE n={g.N} phrases={g.M} distinct={g.K} descriptors={g.U} max_bucket=809062 peak_counted={g.PREP_BOUND} final_columns={g.T+4*g.M} reads=5 decode_verified=0'
    assert g.shape(log)==g.PREP_BOUND
    for x in (g.M,g.K,g.U,g.PREP_BOUND): rejects(lambda:g.shape(log.replace(str(x),str(x+1))))
    for role,cap in g.CAP.items():
        cg={'memory.max':str(cap*g.MIB),'cpu.max':'100000 100000','memory.swap.max':'0'}; g.grant(cg,role)
        for key in cg: rejects(lambda:g.grant({**cg,key:'max'},role))
        rejects(lambda:g.traffic(role,{'read_bytes':g.READ[role]+1,'write_bytes':0}))
    rejects(lambda:g.before(tmp,time.time()+2,10))
    with patch.object(g,'PIN',tmp/'absent'): rejects(g.pin)
    with patch.object(g,'exclusive',side_effect=RuntimeError('active grid')), patch.object(g,'pin') as pin:
        rejects(lambda:g.preflight(tmp,time.time()+1200)); pin.assert_not_called()
    assert max(g.PREP_BOUND,g.T+4*g.M)+g.QUEUE<=2*g.N


def mock_worker(tmp,interrupt):
    # Substitute a waiting toy Python child, not a producer, cgroup or timed binary.
    d=tmp/('bounded-component-100m-'+str(interrupt)); a=d/'control'; a.mkdir(parents=True); flag=d/'stop.requested'
    cg={'cpu.max':'100000 100000','memory.max':str(192*g.MIB),'memory.swap.max':'0','memory.events':'oom 0\noom_kill 0'}
    script='import time;time.sleep(.2);print("tiny prep")'
    if interrupt: script='import pathlib,time,sys\np=pathlib.Path(sys.argv[1]);\nwhile not p.exists(): time.sleep(.02)\nsys.exit(75)'
    real=sp.Popen
    def toy(*_,**kwargs): return real([sys.executable,'-c',script,flag],**kwargs)
    with contextlib.ExitStack() as stack:
        for key,val in [('S',tmp),('lease',lambda *a:None),('hierarchy',lambda *a:{}),('cg_info',lambda:cg),('exclusive',lambda *a:None)]:
            stack.enter_context(patch.object(g,key,val))
        stack.enter_context(patch.object(sp,'Popen',toy))
        f=lambda:g.worker(d,'control','prep',time.time()+(.3 if interrupt else 10))
        if interrupt: rejects(f)
        else: f()
    rec=json.loads((a/'prep.json').read_text()); assert rec['rc']==(75 if interrupt else 0)
    assert rec['process']['rchar']>0 and 'write_bytes' in rec['process'] and rec['sampled_hwm']>0
    assert flag.exists()==interrupt


def paired_supervisor(tmp,fail):
    """Fake services simulate a short pipe or a stopped consumer. No row spool."""
    d=tmp/('pair-'+str(fail)); d.mkdir()
    def fake_unit(d,arm,role,deadline):
        a=d/arm; flag=d/'stop.requested'
        stats={'process':{'read_bytes':1,'write_bytes':0,'cpu_s':0.1},'sampled_hwm':1000,'after':{'memory.peak':'2000'}}
        (a/(role+'.json')).write_text(json.dumps(stats))
        if role=='prep':
            col=a/'columns'; col.mkdir(); (col/'parse.u32').write_bytes(b'1234'); (col/'dictionary.packed').write_bytes(b'abcdef')
            code='print("LEXPARSE n=2 phrases=1 distinct=1 descriptors=2 max_bucket=1 peak_counted=14 final_columns=10 reads=0 decode_verified=0")'
        elif role=='oracle':
            code='import sys; assert sys.stdin.buffer.read()==b"row"; print("VERIFIED rows=3 final_rank=3 narrow_sha256=tiny")' if not fail else 'import sys; sys.exit(75)'
        elif fail:
            code=f'import pathlib,time; p=pathlib.Path({str(flag)!r})\nwhile not p.exists(): time.sleep(.02)\nraise SystemExit(75)'
        else:
            code='import sys;print("RESIDENT_CHECK recurrence_ranks=1 virtual_end=0",file=sys.stderr);sys.stdout.buffer.write(b"row")'
        return [sys.executable,'-c',code]
    with contextlib.ExitStack() as ctx:
        for key,val in [('N',2),('M',1),('K',1),('T',6),('U',2),('PREP_BOUND',14),('DIGEST','tiny'),('exclusive',lambda:None),
                        ('unit',fake_unit),('private_raw',lambda dst,digest:dst.write_bytes(b'hi')),('cold',lambda _:None)]:
            ctx.enter_context(patch.object(g,key,val))
        fn=lambda:g.study(d,time.time()+1000,{'raw_sha256':'fake'})
        if fail: rejects(fn)
        else: fn()
    exits=json.loads((d/'control/exits.json').read_text())
    if fail:
        assert exits['producer']==exits['oracle']==75 and not (d/'candidate').exists() and not (d/'result.json').exists()
    else:
        assert set(json.loads((d/'result.json').read_text())['arms'])=={'control','candidate'}
        assert not (d/'control/columns').exists() and not (d/'candidate/columns').exists()


if __name__=='__main__':
    with tempfile.TemporaryDirectory() as td:
        tmp=Path(td); oracle(tmp,Path(sys.argv[1]).resolve()); policy(tmp); mock_worker(tmp,False); mock_worker(tmp,True); paired_supervisor(tmp,False); paired_supervisor(tmp,True)
    print('PASS tiny raw verification and LE digest; exact rung, shape, grant, stop, grid and zombie counter checks')
