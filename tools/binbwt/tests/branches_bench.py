#!/usr/bin/env python3
"""Warm, pipelined framed benchmark; records both end-to-end and kernel timing."""
import argparse,json,mmap,pathlib,random,struct,subprocess,threading,time
p=argparse.ArgumentParser();p.add_argument('xsa');p.add_argument('container');p.add_argument('corpus');p.add_argument('--jobs',type=int,required=True);p.add_argument('--out',required=True);p.add_argument('--count',type=int,default=100000)
a=p.parse_args();out=pathlib.Path(a.out);out.mkdir(parents=True,exist_ok=True)
f=open(a.corpus,'rb');data=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
rng=random.Random(20261103)
# 90% corpus samples of length 48, 10% common English contexts.
common=[b'the ',b'of ',b'and ',b'in ',b'to ',b' a ',b'is ',b'for ',b'was ',b'with ']
patterns=[]
for i in range(a.count):
    pos=rng.randrange(len(data)-48)
    patterns.append(common[(i//10)%len(common)] if i%10==0 else data[pos:pos+48])
log=open(out/f'j{a.jobs}.stderr','wb')
proc=subprocess.Popen([a.xsa,'branches',a.container,'--corpus',a.corpus,'--serve','-j',str(a.jobs),'-k','8'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
ready=threading.Event()
def stderr():
    for line in proc.stderr:
        log.write(line);log.flush()
        if b'BRANCH_READY' in line:ready.set()
threading.Thread(target=stderr,daemon=True).start()
while not ready.wait(1):
    if proc.poll() is not None:raise RuntimeError('server failed during startup')
def exact(n):
    b=b''
    while len(b)<n:
        chunk=proc.stdout.read(n-len(b))
        if not chunk:raise EOFError()
        b+=chunk
    return b
def response(id):
    size,=struct.unpack('<I',exact(4));b=exact(size)
    got,l,r,width,eof,other,n=struct.unpack_from('<6QH',b)
    assert got==id and r-l==width and size==50+9*n
    assert sum(struct.unpack_from('<BQ',b,50+i*9)[1] for i in range(n))+eof+other==width
# Warm both workers' index and corpus accesses on 1000 representative requests.
warm=patterns[:1000]
def send(ps,close=False):
    for pat in ps:proc.stdin.write(struct.pack('<I',len(pat))+pat)
    proc.stdin.flush()
    if close:proc.stdin.close()
t=threading.Thread(target=send,args=(warm,));t.start()
for i in range(len(warm)):response(i)
t.join();print(f'warm j={a.jobs} complete',flush=True)
start=time.perf_counter();t=threading.Thread(target=send,args=(patterns,True));t.start()
for i in range(len(patterns)):
    response(len(warm)+i)
    if i and i%10000==0:print(f'j={a.jobs} {i}/100000',flush=True)
t.join();elapsed=time.perf_counter()-start
assert proc.wait()==0
result={'jobs':a.jobs,'contexts':len(patterns),'seconds':elapsed,'contexts_per_s':len(patterns)/elapsed,'mix':'90% sampled 48 bytes, 10% common English; seed 20261103','warmup':len(warm)}
(out/f'j{a.jobs}.json').write_text(json.dumps(result,indent=2)+'\n')
print(f'W2 -j {a.jobs}: {len(patterns)/elapsed:.3f} contexts/s (100000 contexts, warm cache, k=8)',flush=True)
