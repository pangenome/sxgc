#!/usr/bin/env python3
"""Independent bytes.find oracle and framed streaming/CLI regression gates."""
import argparse, collections, json, mmap, pathlib, random, struct, subprocess, threading, time
p=argparse.ArgumentParser()
p.add_argument('--reuse-oracle',action='store_true');p.add_argument('--only-w4',action='store_true');p.add_argument('xsa');p.add_argument('container');p.add_argument('corpus');p.add_argument('--out',required=True)
a=p.parse_args();out=pathlib.Path(a.out);out.mkdir(parents=True,exist_ok=True)
f=open(a.corpus,'rb');data=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
rng=random.Random(20261103)
patterns=[]
for i in range(100):
    pos=rng.randrange(len(data)-128);patterns.append(data[pos:pos+rng.choice([16,24,40,64,128])])
# Edges include a separator inside the context, 4 KiB, unique suffix at EOF,
# empty, absent byte, a one-byte context, and leading '-'.
sep=data.find(b'\x1e'); rare=min(collections.Counter(data[:1000000]).items(),key=lambda x:x[1])[0]
patterns += [b'',b'\xff'*64,bytes([rare]),data[max(0,sep-8):sep+9] if sep>=0 else b'a\x1eb',data[10000:14096],data[-4096:],b'-'+data[20:40]]
if a.only_w4: patterns=patterns[:20]
cached=json.loads((out/'oracle.json').read_text()) if a.reuse_oracle else None
expected=[];positions=[]
for i,pat in enumerate(patterns):
    if cached is not None:
        row=cached[i];expected.append((collections.Counter({int(k):v for k,v in row['counts'].items()}),row['eof'],row['width']))
        positions.append([])
        continue
    counts=collections.Counter();eof=0;poslist=[]
    if not pat:
        # Independent corpus histogram; one pass rather than 256 full scans.
        try:
            import numpy as np
        except ImportError:
            np=None
        for offset in range(0,len(data),16<<20):
            block=data[offset:offset+(16<<20)]
            if np is None: counts.update(block)
            else:
                hist=np.bincount(np.frombuffer(block,dtype=np.uint8),minlength=256)
                counts.update({i:int(v) for i,v in enumerate(hist) if v})
        counts=+counts;eof=1;width=len(data)+1
    else:
        at=data.find(pat)
        while at>=0:
            if i<100:poslist.append(at)
            end=at+len(pat)
            if end==len(data):eof+=1
            else:counts[data[end]]+=1
            at=data.find(pat,at+1)
        width=sum(counts.values())+eof
    expected.append((counts,eof,width));positions.append(poslist)
    if i%10==0:print(f'oracle {i}/{len(patterns)}',flush=True)
(out/('w4-oracle.json' if a.only_w4 else 'oracle.json')).write_text(json.dumps([{'width':w,'eof':e,'counts':dict(c)} for c,e,w in expected]))
def run(k,j,unordered=False):
    frames=b''.join(struct.pack('<I',len(p))+p for p in patterns)
    r=subprocess.run([a.xsa,'branches',a.container,'--corpus',a.corpus,'--serve','-j',str(j),'-k',str(k),*(['--unordered'] if unordered else [])],input=frames,capture_output=True)
    (out/f'serve-k{k}-j{j}.stderr').write_bytes(r.stderr)
    assert r.returncode==0,r.stderr.decode(errors='replace')
    at=0;rows=[];ids=set()
    while at<len(r.stdout):
        size,=struct.unpack_from('<I',r.stdout,at);at+=4
        payload=r.stdout[at:at+size];at+=size
        id,l,hi,width,eof,other,n=struct.unpack_from('<6QH',payload)
        assert size==50+9*n
        branches=[struct.unpack_from('<BQ',payload,50+9*i) for i in range(n)]
        assert (unordered or id==len(rows)) and id not in ids and width==hi-l
        ids.add(id)
        c,e,w=expected[id];top=sorted(c.items(),key=lambda x:(-x[1],x[0]))[:k]
        assert (branches,eof,width)==(top,e,w),(id,branches,top,eof,e,width,w)
        assert sum(v for _,v in branches)+other+eof==width
        assert other==w-e-sum(v for _,v in top)
        rows.append((width,eof,other))
    assert len(rows)==len(patterns)
    return rows
if not a.only_w4:
    run(256,1);run(8,4);run(0,4,True)
    assert expected[105][1]==expected[105][2]==1, "EOF-only context must be unique"
    print('W1 100/100 exact next-byte distributions, eof, width; top-k closure 100/100',flush=True)
    print('W3 7/7 edge cases passed',flush=True)
    selected=[i for i,p in enumerate(patterns) if b'\n' not in p][:12]
    path=out/'contexts.txt';path.write_bytes(b'\n'.join(patterns[i] for i in selected)+b'\n')
    r=subprocess.run([a.xsa,'branches',a.container,'--corpus',a.corpus,'--contexts',str(path),'--jsonl','-k','8'],capture_output=True)
    assert r.returncode==0,r.stderr.decode(errors='replace')
    rows=[json.loads(row) for row in r.stdout.splitlines()]
    assert len(rows)==len(selected)
    for id,(row,i) in enumerate(zip(rows,selected)):
        c,e,w=expected[i];top=sorted(c.items(),key=lambda x:(-x[1],x[0]))[:8]
        assert row['ctx_id']==id and row['width']==w and row['eof']==e
        assert row['branches']==[{'byte':b,'count':n} for b,n in top]
        assert sum(b['count'] for b in row['branches'])+row['other']+row['eof']==w
    print(f'FILE {len(rows)}/{len(rows)} JSONL results exact',flush=True)
# W4 unchanged banked oracle patterns: 100 at 150 MB and 20 at 10 GB.
if len(data)<1_000_000_000:
    root=pathlib.Path(__file__).resolve().parents[3]
    pats=root/'tools/binbwt/pats.fa'; oracle={}
    for line in (root/'tools/binbwt/oracle.tsv').read_text().splitlines():
        if line.startswith('#'):continue
        i,c,ps=line.split('\t');oracle[int(i)]=sorted(int(v) for v in ps.split(',') if v)
    flags=['--patterns-fasta',str(pats)];wanted=100
else:
    fixture=pathlib.Path(__file__).parent/'fixtures'
    bank={}
    for line in (fixture/'query10-oracle.tsv').read_text().splitlines():
        if line.startswith('#'):continue
        i,c,ps=line.split('\t');bank[int(i)]=sorted(int(v) for v in ps.split(',') if v)
    ids=sorted((i for i in bank if len(bank[i])<=3),key=lambda i:(len(bank[i]),i))[:20]
    allp=[]
    for line in (fixture/'query10-pats.fa').read_bytes().split(b'\n'):
        if line.startswith(b'>'):allp.append(b'')
        elif allp:allp[-1]+=line
    pats=out/'w4.fa';pats.write_bytes(b''.join(b'>'+str(j).encode()+b'\n'+allp[i]+b'\n' for j,i in enumerate(ids)))
    flags=['--patterns-fasta',str(pats)];oracle={j:bank[i] for j,i in enumerate(ids)};wanted=20
for mode in ([],['--count']):
    r=subprocess.run([a.xsa,'query',a.container,'-j','16',*flags,*mode],capture_output=True)
    (out/('count.stderr' if mode else 'positions.stderr')).write_bytes(r.stderr)
    assert r.returncode==0,r.stderr.decode(errors='replace')
    actual=collections.defaultdict(list)
    for line in r.stdout.splitlines():
        fields=line.split(b'\t');i,v=int(fields[0]),int(fields[-1]);actual[i].append(v)
    for i,ps in oracle.items():assert (actual[i]==[len(ps)] if mode else sorted(actual[i])==ps),(i,actual[i],ps)
print(f'W4 {wanted}/{wanted} counts and positions unchanged',flush=True)
