"""Independent byte suffix oracle for direct-range expansion tiles, including whole giant ties and a stalled sink."""
import os
from pathlib import Path
import random
import selectors
import struct
import subprocess as sp
import sys
import tempfile
import time
from overlap_finite_emit64_test import byte_reference,large_class
from overlap_finite_direct64_test import probe
from overlap_raw_view64_test import fields
from overlap_resident_test import trigger

def run(exe,mode='tiled'):
    assert mode in ('tiled','cached')
    rng=random.Random(2357642);tag=next(i.to_bytes(8,'little') for i in range(999) if trigger(i.to_bytes(8,'little')));big=large_class(rng,tag);high=(tag+b'hello!')*9200+tag
    cases=[(bytes(range(256))*30,960,4800),(rng.randbytes(56000),14000,32000),(big,len(big),len(big)),(big,1600,8192),(high,999,4096)]
    for i in range(8):
        v=bytes(rng.choices(rng.sample(range(256),3+i*21),k=1500+200*i));cases.append((v*(1+i%3)+rng.randbytes(20),350,2000))
    end=str(int(time.time())+240);seen=bad=cuts=bypass=oversize=more=0
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);raw=d/'raw';inv=d/'inverse';idx=d/'post';stop=d/'stop'
        def args(w,target,slots):return [exe,mode,raw,inv,idx,str(w),str(160<<20),str(inv.stat().st_size),str(target),str(slots),stop,end]
        for text,target,slots in cases:
            ii,expected=byte_reference(text);raw.write_bytes(text);inv.write_bytes(struct.pack(f'<{len(ii)}Q',*ii));m=len(ii)
            for w in (1,2,4):
                p=sp.run(args(w,target,slots),capture_output=True)
                if p.returncode==75 and b'scratch' in p.stderr:
                    assert probe(text,ii,w,target) is None and not idx.exists() and not p.stdout;bad+=1;continue
                assert p.returncode==0 and p.stdout==expected,(len(text),w,p.stderr);stats=fields(p.stderr.decode(),'FINITE_EMIT');q=fields(p.stderr.decode(),'EMIT_TILES');v={k:int(x) for k,x in q.items() if not k.endswith('_s')};assert v['grain']==1536 and v['tiles']>=int(stats['leaves']) and 0<v['max_tile_descriptors']<=v['max_tile_rows'] and int(stats['row_bytes'])==12*(len(text)+1) and int(stats['counted'])<=2*len(text);seen+=1;cuts+=v['extension_descriptors'];oversize+=v['tiles_above_ring'];bypass+=int(stats['exact_bypass']);more+=v['tiles']>int(stats['leaves']);idx.unlink()
                if mode=='cached':
                    o={k:int(v) for k,v in fields(p.stderr.decode(),'CACHE_ORDER').items()};assert o['cache_capacity']==512 and o['per_lane_bytes']==24576 and o['cache_desc']+o['whole_equal']+o['slow_desc']<=int(stats['descriptors']) and o['reprobes']<=2*int(stats['descriptors'])
                if text==big and target==len(big):assert v['max_tile_descriptors']>=8311 and v['extension_descriptors'] and int(stats['bands'])==1
                if text==high:assert v['tiles_above_ring']>0 and v['max_tile_rows']>=9200
        # Native writes are still interruptible even when a one-range many-tile wave fills all slots.
        ii,_=byte_reference(big);inv.write_bytes(struct.pack(f'<{len(ii)}Q',*ii));raw.write_bytes(big);p=sp.Popen(args(4,len(big),len(big)),stdout=sp.PIPE,stderr=sp.PIPE,bufsize=0)
        try:
            with selectors.DefaultSelector() as rd:
                rd.register(p.stdout,selectors.EVENT_READ);assert rd.select(15);assert len(os.read(p.stdout.fileno(),12))==12
                assert rd.select(15),'producer never reached non-END tiles'
            stop.touch();assert p.wait(timeout=3)==75 and b'stop' in p.stderr.read()
        finally:
            if p.poll() is None:p.kill();p.wait()
            p.stdout.close();p.stderr.close()
    assert bypass and more and oversize and cuts;print(f'PASS {seen} byte-identical {mode} streams, {bad} predicted refusals; {more} multi-tile cases, whole 8311-descriptor classes and >9200-row tiles, exact bypass, stalled publication STOP')
if __name__=='__main__':run(Path(sys.argv[1]).resolve(),*sys.argv[2:])
