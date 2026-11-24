"""Generated input only. Check grants on both sides of each lifetime boundary; no row spool."""
from pathlib import Path
import random
import struct
import subprocess as sp
import sys
import tempfile
import time
from overlap_finite_direct64_test import probe
from overlap_finite_emit64_test import byte_reference
from overlap_finite_epoch64_test import periods,run
from overlap_finite_epoch_arrays import plan
from overlap_raw_view64_test import fields
from overlap_resident_test import trigger

def budgets(exe):
    rng=random.Random(1438145);team=4;slots=4300;target=820;text=rng.randbytes(64000);n=len(text);ii,rows=byte_reference(text);m=len(ii)
    starts=[0]+[j for j in range(1,n-7) if trigger(text[j:j+8])];keys={(text[s:starts[i+1]+8] if i+1<m else text[s:],i+1==m) for i,s in enumerate(starts)};k=len(keys);u=sum(len(word)-(0 if last else 8) for word,last in keys)
    bands=probe(text,ii,team,target,images=True);available=2*n-8*m
    # Seek a witness that invalidates 'size the first epoch only'. Independent
    # Python images determine every wave extent before the native executable runs.
    selected=None
    for cap in range(n//7,available,16):
        cuts=periods(bands,slots,team,cap)
        if not isinstance(cuts,str) and len(cuts)>1 and max(b-a for a,b,_,_ in cuts)>cuts[0][1]:selected=(cap,cuts);break
    assert selected is not None;cap,cuts=selected;widths=[b-a for a,b,_,_ in cuts];other=2*n-cap;a=plan(exe,n,m,k,u,target,slots,team,other)
    base=a['pre_census'];route=base+max(widths)*a['per_active_band'];grant=max(route,a['emission_upper']);assert grant<a['all_encoder_route']
    with tempfile.TemporaryDirectory() as td:
        d=Path(td);src=d/'raw';inv=d/'inverse';file=d/'slot';stop=d/'stop';src.write_bytes(text);inv.write_bytes(struct.pack(f'<{m}Q',*ii));end=str(int(time.time())+90)
        def invoke(mode,ram):return sp.run([exe,mode,src,inv,file,str(team),str(ram),str(other),str(target),str(slots),stop,end],capture_output=True,timeout=30)
        def refuse(mode,ram,why):
            z=invoke(mode,ram);assert z.returncode==75 and not z.stdout and not file.exists() and why in z.stderr,(mode,ram,z.returncode,z.stderr)
        refuse('staged-epochs',base-1,b'direct splitter/EF array grant');refuse('staged-epochs',route-1,b'epoch encoder array grant before open');refuse('staged-epochs',base+widths[0]*a['per_active_band'],b'epoch encoder array grant before open');refuse('epochs',grant,b'direct splitter/EF array grant')
        z=invoke('staged-epochs',grant);assert z.returncode==0 and z.stdout==rows,z.stderr;l=z.stderr.decode();q=fields(l,'EPOCH_ARRAY_CHARGE');assert [int(q[x]) for x in ('pre_census','max_active_bands','encoder_bytes','route_arrays')]==[base,max(widths),route-base,route];assert int(fields(l,'FINITE_EMIT')['arrays'])<=grant
        # No full row file; only the final posting image survives.
        lo,hi,_,_=cuts[-1];assert file.read_bytes()==b''.join(x[2] for x in bands[lo:hi])+b''.join(x[3] for x in bands[lo:hi]);file.unlink()
        # Large arena, one unique EOF word, no trigger: emission, not encoding,
        # sets the maximum. This negative case never sorts/emits its long ties.
        n=1<<20;char=next(i for i in range(256) if not trigger(bytes([i])*8));src.write_bytes(bytes([char])*n);inv.write_bytes(struct.pack('<Q',1));team=1;slots=target=n;other=n
        a2=plan(exe,n,1,1,n,target,slots,team,other);assert a2['desired_bands']==1 and a2['emission_upper']>a2['all_encoder_route'];refuse('staged-epochs',a2['emission_upper']-1,b'ordered stream array grant')
    print(f'PASS exact tighter-grant stream, later-wider epoch widths={widths}, all-index guard refuses same grant; 5 pre-open/no-END array refusals incl emission-dominant one-word geometry')
if __name__=='__main__':
    assert __debug__;exe=Path(sys.argv[1]).resolve();budgets(exe);run(exe,'staged-epochs')
