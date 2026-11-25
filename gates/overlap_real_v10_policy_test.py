"""Local invariants only; do not run any sorting or open a corpus."""
import os
from pathlib import Path
import subprocess as sp
import tempfile
import time
from types import SimpleNamespace
from unittest.mock import patch
from overlap_real_v10_meter import Group,collect,files
from overlap_real_v10_plan import model,N,PRODUCT

def main():
    p=model();assert p['candidate']['stream']==199001906<=2*N and max(p['outer_cap'].values())==199229440<=2*N and p['candidate']['front']==168024580 and p['reference']['front']==167605419 and p['ef_bytes_upper']<p['prc_payload_slots']
    with tempfile.TemporaryDirectory() as td:
        r=Path(td);d=r/'candidate';(d/'in').mkdir(parents=True);raw=d/'in/raw';raw.write_bytes(b'raw');q=d/'work.sxi2.partial';q.write_bytes(b'output');slot=d/'slot';slot.write_bytes(bytes(11));os.link(slot,d/'published');hidden=d/'dead';hidden.write_bytes(bytes(7));assert q.stat().st_size<PRODUCT
        with hidden.open('rb') as f:
            hidden.unlink();child=sp.Popen(['/bin/sleep','60'],pass_fds=(f.fileno(),))
            try:
                z,u,a=files(r,{'c':child},lambda *_:None);assert (z['scratch'][0],z['output'][0],z['inputs'][0])==(18,6,3) and u==1 and a>=1
            finally:child.terminate();v=collect(child,True);assert v['rc']==-15 and v['maxrss_bytes']>0
        # EROFS in a guest STOP write must not leave a live child behind or
        # mask the host STOP. Use a finite child, no service/privileges here.
        guest=d/'quota';guest.mkdir();h=Group.__new__(Group);h.root=r;h.d=d;h.stop=d/'stop.requested';h.daemons=set();h.drains=[];h.errors=[];h.results={};h.starts={'probe':time.monotonic()};h.work={'probe':sp.Popen(['/bin/true'])};h.volume=SimpleNamespace(d=guest,mountinfo=lambda:True,owned=lambda:True)
        touch=Path.touch
        def fail_guest(self,*args,**kwargs):
            if guest in self.parents:raise OSError(30,'read-only guest')
            return touch(self,*args,**kwargs)
        with patch.object(Path,'touch',fail_guest):h.abort()
        assert h.stop.is_file() and (r/'stop.requested').is_file() and not (guest/'stop.requested').exists() and 'read-only guest' in h.errors[0] and h.results['probe']['rc']==0
    print('PASS: read-only guest STOP still reaps, joint bounds, output/input exclusions, nested alias+unlinked accounting and nonduplicated wait4')
if __name__=='__main__':main()
