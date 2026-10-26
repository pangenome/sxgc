#!/usr/bin/env python3
"""Bin-bwt v1 tiny product gate: independent brute padded finite order -> raw RLE
bytes, chi and every emitted witness must match, from the real producer+finish."""
import json,os,struct,subprocess
from pathlib import Path
base=Path('/mnt/nvme2n1/erikg');root=Path(os.environ.get('BINBWT_TINY_ROOT',str(base/'binbwt-tiny-product')));root.mkdir(parents=True,exist_ok=True)
producer=Path(os.environ.get('BINBWT_BIN',str(base/'binbwt-build/binbwt')));finish=Path(os.environ.get('XSA_FINISH_BIN',str(base/'sol7e-freshstart/xsa_finish')))
map_path=base/'sol6c-gate100m/remap-256.bin';mapping=map_path.read_bytes();inverse=bytes(mapping.index(i) for i in range(256))
fixtures=[b'\x00\x05\x1e\x00\x05\x1e',b'\x1e\x0a\x1e',b'\x1e\x0a\x1e'*12,b'a'*63+b'\x1e',b'ab'*31+b'\x1e',b'aaaaabaaaabaaaabaaaabaaba\x1e',b'\x00'*20+b'\x05\x1e',b'\x1e'*31,b'abcababcababcab\x1e']
with (base/'extcols-scratch/mergperf-1b-snap.txt').open('rb') as f:
    for offset in [0,10003,831123]:
        f.seek(offset);fixtures.append(f.read(127).translate(mapping)+b'\x1e')
def brute(t):
    n=len(t);sa=sorted(range(n),key=lambda p:t[p:]);runs=[]
    for p in sa:
        c=t[(p-1)%n]
        if runs and runs[-1][0]==c:runs[-1][1]+=1;runs[-1][3]=p
        else:runs.append([c,1,p,p])
    def lcp(a,b):
        k=0
        while k<min(n-a,n-b) and t[a+k]==t[b+k]:k+=1
        return k
    inf=10**30;rr=[-1]*256;pos=[0]*256;active=[False]*256;result=[]
    m=inf;prior_inside=inf;previous=None
    for c,length,head,tail in runs:
        inside=inf if length==1 else lcp(head,tail)
        if previous is not None:
            pc,pt=previous;top=lcp(pt,head);mm=min(m,prior_inside)
            if c!=pc:
                threshold=min(mm,top)
                for cc in range(1,256):
                    if threshold<rr[cc]:
                        if active[cc]:result.append(pos[cc])
                        rr[cc]=threshold;pos[cc]=0;active[cc]=False
                for cc,p in [(pc,pt),(c,head)]:
                    if top>rr[cc]:rr[cc]=top;pos[cc]=0 if p==0 else n-p;active[cc]=True
                m=inf
            else:m=min(mm,top)
        previous=(c,tail);prior_inside=inside
    result.extend(pos[cc] for cc in range(1,256) if active[cc])
    return runs,sorted(result)
results=[]
for index,t in enumerate(fixtures):
    assert not any(c in t for c in [1,2,3,4])
    _,expected=brute(t)
    # Independent padded finite suffix order -> raw RLE byte identity.
    order=[len(t)+j for j in range(10)]+sorted(range(len(t)),key=lambda p:t[p:])
    padded=t+b'\x02'*10;chars=[padded[(p-1)%len(padded)] for p in order]
    raw=[]
    for c in chars:
        if raw and raw[-1][0]==c:raw[-1][1]+=1
        else:raw.append([c,1])
    expected_rle=b''.join(struct.pack('<I',c+(length<<8)) for c,length in raw)
    for cap in [1,7]:
      for threads in [1,3]:
        d=root/f'case-{index}-cap-{cap}-t{threads}';d.mkdir(exist_ok=True);corpus=d/'corpus';corpus.write_bytes(t.translate(inverse));prefix=d/'frag';bridge=d/'bridge';bridge.mkdir(exist_ok=True)
        p=[str(producer),'--binbwt',str(corpus),'--remap',str(map_path),'--out-prefix',str(prefix),'--bin-live-cap',str(cap),'--bin-leaf','4','--threads',str(threads)]
        c=[str(finish),'--prefix',str(prefix),'--corpus',str(corpus),'--remap',str(map_path),'--output',str(d/'product.sxi2'),'--threads','2','--debug-artifacts','--bridge-dir',str(bridge),'--keep-rlebwt','--chi-expect',str(len(expected))]
        with (d/'producer.log').open('w') as log:subprocess.run(p,stdout=log,stderr=log,check=True)
        assert Path(str(prefix)+'.rlebwt').read_bytes()==expected_rle,(index,cap,'raw runs')
        with (d/'finish.log').open('w') as log:subprocess.run(c,stdout=log,stderr=log,check=True)
        b=(bridge/'frag.sA').read_bytes();got=sorted(struct.unpack('<'+'Q'*(len(b)//8),b))
        assert got==expected,(index,cap,got,expected)
        results.append(dict(fixture=index,cap=cap,threads=threads,n=len(t),chi=len(expected),raw_runs='exact',witnesses='exact'))
(root/'results.json').write_text(json.dumps(results,indent=2)+'\n')
print(f'BINBWT_TINY_PRODUCT_PASS cases={len(results)} fixtures={len(fixtures)} real_windows=3 NUL=1 byte05=1 wrap_prefix=1 periodic=1 nested=1 cap=1,7 threads=1,3 raw_runs=brute_exact chi=brute_exact witnesses=all_exact container=validated')
