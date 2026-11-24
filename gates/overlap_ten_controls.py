"""Allowlisted 10 GB environment evidence. Never invent a missing /proc observation."""
import hashlib
import json
import os
from pathlib import Path
import re
import time
from overlap_resident_gate import CONTROL_KEYS

PIN='076980318c30e576dd2a2729be07506731253121';WORKER='8513cccb7d0ade1f4917752e3e8be33c3f7e49e3'
FIRST='packed-10g-l4-b1-t4-r1';MISSING='post-cert-first-arm.json';STABLE='stable-worker-controls.json'
AUDITED={'overlap_resident_gate.py':'50f9e993aad79c346012a3efde99b49d80fb2948d713ea6a442cb0520b0e0564',
         'overlap_ten_product.sh':'3c8b975a22ecc40ceac089878011d440a29e72c74a70c0b5b795a167b4d12731',
         'overlap_ten_score.py':'878926d5c2ccf35ee6bdf45eae2e983bfd79ee63ca7ca758ad7a5c0571800525'}
PINS={'build.json':'47c324a1fbef9769cc1d491739a4d54436a138e234d4dc290ae09b48fca71da9','binaries.sha':'b8660c2eee888f8a21a6c182755de01df3d4b479370f9484c3dba88dfb0db653','identity.json':'59521cc92c3a4a6e9a4dac45bc6edaefa351c047d70f17892544689224fd991b','resident.log':'befdf54db70389f660eaa8b0f23d801ae7453df550c20993e48ecd0ac96b20ff','oracle.json':'ff75c5c348a7b4256ed5e6612d8b7a62cd14c084ac7c4762d4b4da860ed27fcd','oracle.log':'947d2393d80cee89d568f5e58fcfb4b84e4ab7405fc069def8374fccd66428f9'}

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()

def environ(blob):
    result={}
    for item in blob.split(b'\0'):
        key,_,value=item.partition(b'=');key=os.fsdecode(key)
        if key in CONTROL_KEYS:
            if key in result:raise RuntimeError('duplicate control: '+key)
            result[key]=os.fsdecode(value)
    return result

def evidence(d,b,expected):
    actual=b['worker_controls'];assert set(actual)==set(expected) and all(actual[k] in ({},v) for k,v in expected.items()),'nonempty child control mismatch'
    exception=(d/MISSING).is_file();stable=(d/STABLE).is_file()
    assert not (stable and exception),'ambiguous provenance'
    if not (stable or exception):
        assert actual==expected,'no observed stable controls for missing startup snapshot'
        return 'native /proc'
    assert (d/'binaries.sha').read_text().splitlines()[0]==PIN,'unexpected harness for out-of-process evidence'
    if stable:
        s=json.loads((d/STABLE).read_text());assert s['profile']==b['profile'] and s['binaries_sha256']==sha(d/'binaries.sha')
        source=json.loads((d/'source.json').read_text())['source'] if (d/'source.json').is_file() else str(d.resolve());assert s['original']==source
        assert set(s['observed'])==set(expected);names={'resident':'overlap_resident','sink':'pfp_sort_r','finish':'xsa_finish_cur'}
        paths=[l.split('  ',1)[1] for l in (d/'binaries.sha').read_text().splitlines()[1:]]
        for k,v in s['observed'].items():
            assert v['controls']==expected[k] and v['pid']>1 and v['start_ticks']>0 and v['controller_pid']==s['controller_pid']
            assert Path(v['exe']).name==names[k] and v['exe'] in paths and v['same_executable_inode'] is True and v['cgroup']==s['cgroup']
            assert v['argv'][0]==v['exe'] and any(a.startswith(source+'/') for a in v['argv'])
        assert s['cpu.max']=='400000 100000' and s['memory.max']==str(200<<30) and s['memory.swap.max']=='0';return 'stable independent /proc'
    # Closed exception for ONE already-identified original, not a rule to accept absent telemetry on another arm.
    # Product/effect/grant/seam/queue/rank checks still run in verified_arm(). This does NOT certify a /proc read.
    s=json.loads((d/MISSING).read_text());assert b['profile']==FIRST and actual==dict(resident={},sink={},finish={})
    assert all(sha(d/k)==v for k,v in PINS.items()) and s['launcher_exit']==1 and s['observed_environments'] is False and s['pinned_source_files']==AUDITED
    assert s['stage']=='original post-service scorer' and s['orig_identity_sha256']==PINS['identity.json'] and s['harness']==PIN and s['worker']==WORKER
    assert any(l.startswith('SS_TAPE_START corpus_bytes=10000000000 ') and ' threads=2 ' in l for l in (d/'sink.log').read_text().splitlines())
    assert any(l.startswith('XSA_FINISH_PFCK ') and ' tau=22 ' in l and ' workers=2 ' in l for l in (d/'finish.log').read_text().splitlines())
    return 'exact first-arm launch source + independent native effects, /proc unavailable'

def stable_read(pid,expected,seconds=1):
    """Use for future *new* harnesses, not retroactive receipt repair. Empty at exec can be transient."""
    end=time.monotonic()+seconds;p=Path('/proc')/str(pid)/'environ'
    while True:
        try:actual=environ(p.read_bytes())
        except (FileNotFoundError,ProcessLookupError,PermissionError):return {}  # caller requests private STOP
        if actual==expected or actual or time.monotonic()>=end:return actual
        time.sleep(.005)
