#!/usr/bin/env python3
"""Independent tiny suffix-array oracle + unpatched .ssa preservation.

The tiny oracle is test-only. Production performs no SA or text walks.
"""
import argparse, json, pathlib, struct, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('--patched',default='/tmp/rpfbwt-sxgc/build-sxgc/rpfbwt');p.add_argument('--original',default='/home/erikg/r-pfbwt/build/rpfbwt');a=p.parse_args()
def run(cmd):
    subprocess.run(list(map(str,cmd)),stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
def values(path):
    return [x[0] for x in struct.iter_unpack('<Q',path.read_bytes())]
with tempfile.TemporaryDirectory(prefix='tap-test-') as d:
    d=pathlib.Path(d)
    # Repetition, singletons, chunk boundaries and tail-only merge ranges.
    for name in ['random-4-2k','satellite-18k']:
        text=(pathlib.Path('/tmp/laneY/bat')/(name+'.txt')).read_bytes()
        src=d/(name+'.txt');src.write_bytes(text);prefix=d/name
        run(['/home/erikg/pfp/build/pfp++','-t',src,'-o',prefix,'-w',10,'-p',100,'-j',2,'--tmp-dir',d])
        run(['/home/erikg/pfp/build/pfp++','-i',str(prefix)+'.parse','-w',5,'-p',11,'-j',2,'--tmp-dir',d])
        # The producer text is circular text + ten internal 0x02 bytes.
        padded=text+b'\x02'*10
        sa=sorted(range(len(padded)),key=lambda i:padded[i:]+padded[:i])
        bwt=[padded[(i-1)%len(padded)] for i in sa]
        heads=[sa[i] for i in range(len(sa)) if i==0 or bwt[i]!=bwt[i-1]]
        tails=[sa[i] for i in range(len(sa)) if i+1==len(sa) or bwt[i]!=bwt[i+1]]
        for chunks in [1,7,50]:
            flags=['--l1-prefix',prefix,'--w1',10,'--w2',5,'--threads',2,'--chunks',chunks,'--tmp-dir',d]
            run([a.original,*flags]);old=pathlib.Path(str(prefix)+'.ssa').read_bytes()
            run([a.patched,*flags]);new=pathlib.Path(str(prefix)+'.ssa').read_bytes()
            assert old==new,(name,chunks,'head output changed')
            got=values(pathlib.Path(str(prefix)+'.ssa_t'))
            assert got==[len(tails),*tails],(name,chunks,'tail oracle mismatch',got[:10],tails[:10])
            if chunks==1:assert values(pathlib.Path(str(prefix)+'.ssa'))==[len(heads),*heads]
            print(json.dumps(dict(text=name,chunks=chunks,ssa_identical=True,tail_oracle=True,r=len(tails))),flush=True)
print('PASS tap: original head bytes unchanged, all tails match independent cyclic-SA oracle')
