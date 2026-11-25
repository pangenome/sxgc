"""Local invariants only; do not run any sorting or open a corpus."""
import os
from pathlib import Path
import subprocess as sp
import tempfile
from overlap_real_v10_meter import collect,files
from overlap_real_v10_plan import model,N,PRODUCT

def main():
    p=model();assert p['candidate']['stream']==199001858<=2*N and p['candidate']['front']==168024532 and p['reference']['front']==167605419 and p['ef_bytes_upper']<p['prc_payload_slots']
    with tempfile.TemporaryDirectory() as td:
        r=Path(td);d=r/'candidate';(d/'in').mkdir(parents=True);raw=d/'in/raw';raw.write_bytes(b'raw');q=d/'work.sxi2.partial';q.write_bytes(b'output');slot=d/'slot';slot.write_bytes(bytes(11));os.link(slot,d/'published');hidden=d/'dead';hidden.write_bytes(bytes(7));assert q.stat().st_size<PRODUCT
        with hidden.open('rb') as f:
            hidden.unlink();child=sp.Popen(['/bin/sleep','60'],pass_fds=(f.fileno(),))
            try:
                z,u,a=files(r,{'c':child},lambda *_:None);assert (z['scratch'][0],z['output'][0],z['inputs'][0])==(18,6,3) and u==1 and a>=1
            finally:child.terminate();v=collect(child,True);assert v['rc']==-15 and v['maxrss_bytes']>0
    print('PASS: joint bounds, output/input exclusions, nested alias+unlinked accounting and nonduplicated wait4')
if __name__=='__main__':main()
