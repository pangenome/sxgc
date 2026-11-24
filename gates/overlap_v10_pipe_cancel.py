"""One blocked downstream, no finish or existing row pass. Use a private temp dir and bounded service."""
import fcntl
import os
from pathlib import Path
import select
import struct
import subprocess as sp
import sys
import time

def run(xsa,d):
    d.mkdir(mode=0o700);n=65536;stop=d/'stop';out=d/'m20';raw=d/'raw';raw.write_bytes(b'x'*n)
    (d/'seps.bin').write_bytes(struct.pack('<4sIIIQQQQIQ',b'XSEP',1,1,64,1,n,1,0,0,0));assert not out.exists()
    # Only the first packet for x^n. Input stays OPEN, output pipe capped at one page, reader does not drain it.
    p=sp.Popen([xsa,'ctx-tap','--row43-pipe',stop,'--deadline',str(int(time.time())+8),'--n',str(n+1),'--corpus',raw,'--doc-starts',d,'--threads','1','--out',out],stdin=sp.PIPE,stdout=sp.PIPE,stderr=sp.PIPE,bufsize=0)
    try:
        assert fcntl.fcntl(p.stdout,fcntl.F_SETPIPE_SZ,4096)==4096
        packet=bytearray().join(struct.pack('<III',n-i,max(0,i-1),0x80000078) for i in range(32768))
        with memoryview(packet) as src:
            sent=0
            while sent<len(src):sent+=os.write(p.stdin.fileno(),src[sent:])
        assert select.select([p.stdout],[],[],3)[0];stop.touch();assert p.wait(timeout=2)==75 and 'STOP_OBSERVED' in p.stderr.read().decode()
        assert 0<len(p.stdout.read())<=4096 and not out.exists() and not (d/'m20.partial').exists()
    finally:
        if p.poll() is None:p.kill();p.wait()
        p.stdin.close();p.stdout.close();p.stderr.close()
    print('PASS blocked downstream: one undrained pipe page, STOP exit 75, no m20 or completed rows')

if __name__=='__main__':run(*(Path(x).resolve() for x in sys.argv[1:]))
