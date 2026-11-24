"""Only a sparse *synthetic* size witness and empty stdin. No row build, finish or original input."""
import os
from pathlib import Path
import struct
import subprocess as sp
import sys
import tempfile

def main(exe,base):
    assert __debug__
    env=dict(PATH=os.defpath,LC_ALL='C',XSA_TEST_V8_100M_PIECE_POOL='1',XSA_TEST_EXTERNAL_TRIPLES='1',CROSS_V2_PIECES='1',CROSS_V2_PIECE_WINDOW='6',SS_LCP_SLICE_WINDOW='5',XSA_BATCH_SXLS_ESCAPES='1',PFCK_SPACING='128')
    with tempfile.TemporaryDirectory(dir=base) as td:
        d=Path(td);raw=d/'mock';remap=d/'map';remap.write_bytes(bytes(range(256)))
        with raw.open('xb') as f:f.truncate(100_000_503)
        small=d/'tiny';small.write_bytes(b'x')
        cases=[{k:'0'} for k in env if k not in ('PATH','LC_ALL')]+[{k:'1'} for k in ('TRIE_TAP','XSA_FINISH_SPOOL','XSA_PRIVATE_SXLS_RECLAIM','XSA_TEST_RESIDENT_TRIPLES','XSA_GATE_PACKED_10G','XSA_GATE_VOLATILE_PIECES','XSA_TEST_OVERLAP_STREAM')]
        for i,case in enumerate([*cases,'size','relative','public','stop','stale','empty']):
            arm=d/str(i);arm.mkdir(mode=0o700);pref=arm/'merged/frag';stop=arm/'stop.requested';old=pref.parent/'modeb/stop.requested'
            if case=='public':arm.chmod(0o755)
            if case=='relative':pref=Path(str(i))/'merged/frag'
            if case=='stop':stop.write_bytes(b'keep')
            if case=='stale':old.parent.mkdir(parents=True);old.write_bytes(b'keep')
            p=sp.run([exe,'--corpus',small if case=='size' else raw,'--remap',remap,'--out-prefix',pref,'--bands',arm/'bands','--threads','2'],cwd=d,env=env|(case if isinstance(case,dict) else {}),input=b'',capture_output=True,timeout=5)
            assert p.returncode!=0 and not (arm/'merged/frag.pieces/terminal').exists() and not (arm/'bands/producer.done').exists()
            if case=='empty':
                q=arm/'merged/frag.pieces';assert b'truncated external row stream' in p.stderr and (q/'start').read_bytes()==struct.pack('<4s4Q',b'BPS1',100_000_504,27,1,6) and not list((q/'payload-slots').iterdir())
            elif case=='stale':assert old.read_bytes()==b'keep' and set(arm.iterdir())=={arm/'merged'} and not (arm/'merged/frag.pieces').exists()
            else:assert b'fenced PRC1 handoff:' in p.stderr and set(arm.iterdir())==({stop} if case=='stop' else set())
    print(f'PASS {len(cases)+5} wrong geometry/profile/path/STOP/stale refusals; fresh start then refused empty input, no PRC1/terminal or product')
if __name__=='__main__':main(Path(sys.argv[1]).resolve(),Path(sys.argv[2]))
