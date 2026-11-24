"""Exercise the supervisor on a tiny immutable bank. Mock grants, NOT column equality."""
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import time
from unittest.mock import patch

import overlap_segmented_decode as decode
import overlap_segmented_gate as gate
from overlap_lexparse100_test import oracle


def main(front, verifier):
    with tempfile.TemporaryDirectory() as tmp, ExitStack() as mocks:
        root=Path(tmp); cg=root/'cg';cg.mkdir();(cg/'cgroup.procs').write_text(str(os.getpid()))
        (cg/'io.stat').write_text('8:0 rbytes=4096 wbytes=4096')
        raw=root/'source';text=bytes(range(256))*30;raw.write_bytes(text); n=len(text)
        bank=root/'bank/columns';bank.mkdir(parents=True);t,p,_=oracle(text)
        for name,b in zip(gate.NAMES,[t,p]):(bank/name).write_bytes(b)
        run=root/'segmented-10g-abcdef';run.mkdir(mode=0o700);(run/'binaries.log').touch()
        (run/'front').write_text(f'#!/bin/sh\nexec "{front}" "$1" "$2" --fixture\n');(run/'front').chmod(0o700)
        (run/'verify').symlink_to(verifier)
        sizes=dict(zip(gate.NAMES,[len(t),len(p)]));counter=0
        def cgroup(grant):
            nonlocal counter
            counter+=4096
            return {'memory.max':str(grant),'cpu.max':'100000 100000','memory.swap.max':'0',
                    'memory.events':'oom 0\noom_kill 0','io.stat':f'8:0 rbytes={counter} wbytes={counter}'}
        prior=root/'segmented-1g-fedcba';prior.mkdir(mode=0o700)
        prior_front=dict(rung='1g',n=gate.ONE.n,status='FRONT_COMPARED',rc=0,wall_ratio=1,census=dict(peak_counted=1500000000),
                         equal_column_sha256=dict.fromkeys(gate.NAMES,'0'*64),raw_sha256=gate.ONE_RAW_SHA,
                         cgroup_front_after=cgroup(192<<20),mincore_resident_pages=0,
                         process=dict(read_bytes=1,write_bytes=1),cgroup_front_io_delta=None)
        (prior/'segmented.json').write_text(json.dumps(prior_front));(prior/'decode.json').write_text(json.dumps(dict(status='INTERMEDIATE_VERIFIED',raw_sha256=gate.ONE_RAW_SHA,rc=0,decode_verified=True,cgroup_after=cgroup(3<<30))))
        pin=hashlib.sha256((prior/'segmented.json').read_bytes()+(prior/'decode.json').read_bytes()).hexdigest()
        swapped=[]
        for mod in [gate,decode]:
            mocks.enter_context(patch.object(mod,'CG',cg));mocks.enter_context(patch.object(mod,'cold',swapped.append))
        mocks.enter_context(patch.object(gate,'SANDBOX',root));mocks.enter_context(patch.dict(gate.RUNGS,{'10g':(n,raw,'bank')}))
        mocks.enter_context(patch.object(gate,'exclusive'));mocks.enter_context(patch.object(gate,'cg_info',lambda:cgroup(192<<20)))
        mocks.enter_context(patch.object(gate,'current_io',lambda cg:None))
        assert gate.io_delta(None,{'8:0':{'rbytes':1,'wbytes':2}}) is None
        assert gate.io_delta(gate.stat_io('8:0 rbytes=5 wbytes=4'),gate.stat_io('8:0 rbytes=11 wbytes=7'))=={'8:0':{'rbytes':6,'wbytes':3}}
        mocks.enter_context(patch.object(decode,'memory_available',lambda:32<<30))
        mocks.enter_context(patch.object(decode,'cg_info',lambda:cgroup(16<<30)));mocks.enter_context(patch.object(gate,'baseline',lambda r:(30,dict(read_bytes=n,write_bytes=n),sizes)))
        # Tiny fixture may already be cache-hot: simulate one attributed IO block.
        process_info=gate.proc_info
        mocks.enter_context(patch.object(gate,'proc_info',lambda pid,io:dict(process_info(pid,io),read_bytes=4096)))
        def publication(path,signature):
            gate.reviewed_one(path,signature);return 'fixture',hashlib.sha256(text).hexdigest()
        mocks.enter_context(patch.object(gate,'admit_ten',publication));mocks.enter_context(patch.object(gate,'checked_native'))
        deadline=int(time.time())+1500;mocks.enter_context(patch.object(gate,'CUTOFF',deadline));report={}
        for bad in [[],[str(prior),'0'*64]]:
            try:gate.worker('10g',run,run/'front',deadline,report,bad)
            except (TypeError,RuntimeError):pass
            else:raise AssertionError('accepted absent/unreviewed prior')
            assert not (run/'in').exists()
        gate.worker('10g',run,run/'front',deadline,report,[str(prior),pin]);report['status']='FRONT_COMPARED';gate.save(run/'segmented.json',report)
        assert all(report[phase]['wall_s']>0 and report[phase]['worker_io']['read_bytes']>=0 for phase in ('copy_cold','column_compare'))
        result={};decode.decode('10g',run,deadline,result);assert result['decode_verified']
        assert swapped==[run/'in/corpus',*(run/'columns'/k for k in gate.NAMES)]
        assert all((bank/k).read_bytes()==(run/'columns'/k).read_bytes() for k in gate.NAMES)
    print('PASS fixture-only supervisor, reviewed prerequisite, post-stage bank equality and separately metered full decode (mock grants/cold)')


if __name__=='__main__':main(*sys.argv[1:])
