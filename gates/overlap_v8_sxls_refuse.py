"""Synthetic input only: partial handoffs/refusals, no complete real row stream or finish."""
from pathlib import Path
import os
import struct
import subprocess as sp
import sys
import time

N=100_000_503
S=262_144
BASE=dict(PATH=os.defpath,LC_ALL='C',XSA_TEST_V8_100M_PIECE_POOL='1',XSA_TEST_V8_100M_SXLS_SLOTS='1',XSA_TEST_EXTERNAL_TRIPLES='1',CROSS_V2_PIECES='1',CROSS_V2_PIECE_WINDOW='6',SS_LCP_SLICE_WINDOW='5',XSA_BATCH_SXLS_ESCAPES='1',PFCK_SPACING='128')

def run(exe,d):
    raw=d/'synthetic-size-only';remap=d/'identity.map';z=d/'zero';z.touch(exist_ok=False)
    with raw.open('xb') as f:f.truncate(N) # not a corpus test or raw proof
    with remap.open('xb') as f:f.write(bytes(range(256)))
    def arm(name):
        a=d/name;a.mkdir(mode=0o700)
        return a,[exe,'--corpus',raw,'--remap',remap,'--out-prefix',a/'merged/frag','--bands',a/'bands','--threads','2']
    for name,env,extra,why in [('no-pieces',{'XSA_TEST_V8_100M_PIECE_POOL':'0'},[],'fixed SXLS requires'),('bad-flag',{'XSA_TEST_V8_100M_SXLS_SLOTS':'0'},[],'fixed SXLS requires'),('short-scratch',{},['--scratch-budget-bytes','200001005'],'fixed SXLS requires'),('window',{'SS_LCP_SLICE_WINDOW':'6'},[],'fenced PRC1 handoff')]:
        a,cmd=arm(name);p=sp.run(cmd+extra,env=BASE|env,input=b'',capture_output=True,timeout=3);assert p.returncode!=0 and why.encode() in p.stderr and not list(a.iterdir())
    row=lambda p,h,b:struct.pack('<3I',p,h,b)
    packet=row(N,0,1)+row(1,0,1)*8191
    for name,wire,why in [('not-pipe',None,'requires a narrow pipe'),('truncated',packet[:-1],'truncated fixed SXLS'),('high-lcp',packet[:12]+row(1,65536,1)+packet[24:],'narrow position/LCP'),('row43',row(N,0,0x80000001)+packet[12:],'predecessor'),('duplicate-end',packet[:12]+row(0,0,256)*2+packet[36:],'SXLS END'),('oversize',row(N,0,1)+row(1,255,1)*(S-1),'SXLS extent'),('partial',packet+row(1,0,1)*(S-8192),'truncated fixed SXLS')]:
        a,cmd=arm(name)
        with z.open('rb') as f:p=sp.run(cmd,env=BASE,input=wire,stdin=f if wire is None else None,capture_output=True,timeout=20)
        assert p.returncode!=0 and why.encode() in p.stderr,(name,p.returncode,p.stderr[-4096:]);bands=a/'bands';q=a/'merged/frag.pieces';assert not (q/'terminal').exists() and not (bands/'producer.done').exists() and not list(q.glob('p-*-0'))
        if name=='partial':
            file=bands/'band_0.lcp';private=bands/'slice-slots/0';assert (bands/'band_0.lcp.done').is_file() and file.samefile(private) and file.stat().st_nlink==2 and file.read_bytes()==struct.pack('<4sI5Q',b'SXLS',1,S,0,0,0,1)+bytes(S) and not (bands/'band_1.lcp').exists()
        else:assert not list(bands.glob('*.lcp*')) and not list(bands.glob('slice-slots/*'))
    a,cmd=arm('stalled');p=sp.Popen(cmd,env=BASE,stdin=sp.PIPE,stdout=sp.PIPE,stderr=sp.PIPE)
    try:
        until=time.monotonic()+3
        while not (a/'bands/slice-slots').is_dir():assert p.poll() is None and time.monotonic()<until;time.sleep(.005)
        (a/'merged/modeb/stop.requested').touch(exist_ok=False);assert p.wait(timeout=2)==75 and b'STOP_OBSERVED where=fixed-sxls-input' in p.stderr.read() and not (a/'merged/frag.pieces/terminal').exists()
    finally:
        if p.poll() is None:p.kill();p.wait()
        p.stdin.close();p.stdout.close();p.stderr.close()
    print('PASS 4 pre-root profile refusals, 7 pipe/row/extent/truncation refusals, exact 262192-byte partial image (one inode), private STOP on stalled input; no terminal/product')
if __name__=='__main__':
    assert __debug__ and len(sys.argv)==3;run(Path(sys.argv[1]).resolve(),Path(sys.argv[2]))
