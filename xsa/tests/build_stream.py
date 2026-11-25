#!/usr/bin/env python3
"""CLI smoke test (not part of construction). Usage: build_stream.py PATH/TO/xsa"""
import os
from pathlib import Path
import struct
import subprocess as sp
import sys
import tempfile

xsa = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory() as directory:
    d = Path(directory)
    raw, index, scratch = d/'corpus', d/'index', d/'scratch dir'
    scratch.mkdir()
    # The map covers a longer stream than the input. The 0x1e inside the
    # first document must NOT create another boundary.
    text = (b'banana\x1e bandana\x00\n'*140) + b'banana'
    raw.write_bytes(text)
    cuts = [0,777,len(text)+5]
    (d/'seps.bin').write_bytes(struct.pack('<4sIII4Q',b'XSEP',1,1,64,3,len(text)+9,1,0)
                                +struct.pack('<3IQ',0,cuts[1],cuts[2]-cuts[1],0))
    cmd = [xsa,'build-stream',raw,'--docs-map',d,'-T',scratch,'-o',index,'-j','1']
    def run(argv,ok=True):
        p = sp.run(argv,capture_output=True,timeout=60)
        assert (p.returncode==0)==ok,(p.returncode,p.stdout,p.stderr)
        return p
    build = run(cmd)
    assert not build.stdout and not build.stderr, (build.stdout,build.stderr)
    first = index.read_bytes()
    assert first[:4]==b'SXI2' and int.from_bytes(first[4:8],'little')==10
    table = {r[0]:r for r in struct.iter_unpack('<IIQQQQ',first[-400:])}
    assert list(table)==[1,4,5,7,10,11,12,13,15,20] and table[15][4]==2
    assert int.from_bytes(first[table[20][2]+168:table[20][2]+176],'little')==1
    result=run([xsa,'query',index,'--count','banana']).stdout
    assert str(text.count(b'banana')).encode() in result,result
    run(cmd,False); assert index.read_bytes()==first  # atomic no overwrite
    index.unlink()
    cmd[-1]=str(min(2,len(os.sched_getaffinity(0))))
    run(cmd); assert index.read_bytes()==first  # deterministic across sort workers
    index.unlink(); raw.write_bytes(bytes(range(256)))
    assert b'256' in run(cmd,False).stderr and not index.exists()
    # A downstream map error must cancel the other workers, not publish a short stream.
    raw.write_bytes(text); (d/'seps.bin').write_bytes(b'bad map')
    run(cmd,False); assert not index.exists()
    assert not list(scratch.iterdir()) and not list(d.glob('.xsa-publish-*'))
print('build-stream: query, document boundaries, deterministic bytes and error cleanup passed')
