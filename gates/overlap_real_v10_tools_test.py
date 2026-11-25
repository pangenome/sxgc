"""Empty/invalid pipes and profile separation only. No Pile data; no product."""
import os
from pathlib import Path
import struct
import subprocess as sp
import sys

from overlap_v8_sxls_refuse import BASE


def test(exe,d):
    raw=d/'synthetic-size';remap=d/'identity';remap.write_bytes(bytes(range(256)))
    with raw.open('xb') as f:f.truncate(100_000_503)
    legacy='XSA_TEST_V8_100M_';key='XSA_TEST_REAL_V10_100M_POOLS'
    env={k:v for k,v in BASE.items() if not k.startswith(legacy)}|{key:'1','PFCK_SPACING':'64'}
    for i,(changes,scratch,starts) in enumerate([({},'200001006',True),({},'200001005',False),({},None,False),({key:'0'},'200001006',False),({legacy+'PIECE_POOL':'1'},'200001006',False),({legacy+'SXLS_SLOTS':'1'},'200001006',False),({'PFCK_SPACING':'128'},'200001006',False),({'XSA_TEST_EXTERNAL_TRIPLES':'0'},'200001006',False)]):
        a=d/f'wrong-{i}';a.mkdir(mode=0o700);cmd=[exe,'--corpus',raw,'--remap',remap,'--threads','2','--out-prefix',a/'merged/frag','--bands',a/'bands']
        if scratch:cmd+=['--scratch-budget-bytes',scratch]
        p=sp.run(cmd,env=env|changes,input=b'',capture_output=True,timeout=5)
        assert p.returncode!=0 and not (a/'merged/frag.pieces/terminal').exists() and not (a/'bands/producer.done').exists()
        if starts:assert b'SS_V10_HANDOFF' in p.stderr and b'truncated fixed SXLS narrow input' in p.stderr and not list((a/'bands/slice-slots').iterdir())
        else:assert b'v10 pools:' in p.stderr and not list(a.iterdir())
    # The old v8 profile cannot be switched to PFCK=64 via the new code.
    a=d/'old';a.mkdir(mode=0o700);cmd=[exe,'--corpus',raw,'--remap',remap,'--threads','2','--out-prefix',a/'merged/frag','--bands',a/'bands'];p=sp.run(cmd,env=BASE|{'PFCK_SPACING':'64'},input=b'',capture_output=True,timeout=5);assert b'private exact 100 MB v8 narrow profile required' in p.stderr and not list(a.iterdir())
    print('PASS: 7 real profile refusals, legacy isolation, fresh start and refused EOF/no terminal or product')
    # Both real verifier modes insist on the ABI even on packet zero. Reject
    # the wrong high fields before using the following placeholder positions.
    for tag,opt in [(0,'--real-v10-rolling-100m'),(1<<31,'--real-v10-reference-100m')]:
        p=sp.run([d.parent/'witness',opt,raw,d/'STOP','4102444800'],input=struct.pack('<III',100000503,0,tag+1)*8192,env={'PATH':os.defpath},capture_output=True,timeout=4);assert p.returncode==70 and b'wrong input ABI' in p.stderr and not p.stdout
    print('PASS: reference/rolling wire mismatch refused before first packet')
if __name__=='__main__':test(Path(sys.argv[1]),Path(sys.argv[2]))
