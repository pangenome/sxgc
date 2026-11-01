#!/usr/bin/env python3
"""Compare chi-on/off members and exact query answers; test missing-chi refusals."""
import argparse, collections, hashlib, json, struct, subprocess, tempfile, zlib
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--xsa',required=True)
p.add_argument('--off',type=Path,required=True)
p.add_argument('--on',type=Path)
p.add_argument('--patterns',type=Path,required=True)
p.add_argument('--oracle',type=Path,required=True)
p.add_argument('--prefix-corpus',type=Path,help='Independently scan shorter prefix; oracle positions outside it are excluded')
a=p.parse_args()
def directory(path):
 with path.open('rb') as f:
  h=f.read(64); count=struct.unpack_from('<I',h,32)[0]
  members={}
  for _ in range(count):
   ident,codec,off,size,n,crc,_=struct.unpack('<IIQQQII',f.read(40))
   members[ident]=(codec,off,size,n,crc)
 return h,members
def digest(path,m):
 h=hashlib.sha256()
 with path.open('rb') as f:
  f.seek(m[1]);left=m[2]
  while left:
   b=f.read(min(left,8<<20));assert b;h.update(b);left-=len(b)
 return h.hexdigest()
h,off=directory(a.off);flags=struct.unpack_from('<Q',h,48)[0]
assert flags&64 and not flags&1 and 5 not in off
if a.on:
 hh,on=directory(a.on)
 assert 5 in on and set(on)-{5}==set(off)
 assert h[8:32]==hh[8:32]
 for k in off:
  assert (off[k][0],off[k][2:])==(on[k][0],on[k][2:]),k
  assert digest(a.off,off[k])==digest(a.on,on[k]),k
 assert a.off.stat().st_size<a.on.stat().st_size
print('MEMBERS identical_except_m5=1 members='+','.join(map(str,off)),flush=True)
pats=[]
for l in a.patterns.read_bytes().splitlines():
 if l.startswith(b'>'):pats.append(b'')
 else:pats[-1]+=l
oracle={}
for l in a.oracle.read_text().splitlines():
 if l.startswith('#') or not l:continue
 fields=l.split('\t');i,count=int(fields[0]),int(fields[1]);pos=list(map(int,fields[2].split(','))) if count else []
 assert count==len(pos);oracle[i]=pos
if a.prefix_corpus:
 data=a.prefix_corpus.read_bytes()
 for i,pat in enumerate(pats):
  expected=[v for v in oracle[i] if v+len(pat)<=len(data)]
  actual=[];at=data.find(pat)
  while at>=0:actual.append(at);at=data.find(pat,at+1)
  assert actual==sorted(expected),(i,actual,expected)
  oracle[i]=actual
r=subprocess.run([a.xsa,'query',str(a.off),'--patterns-fasta',str(a.patterns),'-j','16'],capture_output=True,text=True)
assert r.returncode==0,r.stderr
actual=collections.defaultdict(list)
for l in r.stdout.splitlines():
 i,pos=map(int,l.split('\t'));actual[i].append(pos)
for i in range(len(pats)):assert sorted(actual[i])==sorted(oracle[i]),(i,actual[i],oracle[i])
print(f'QUERY counts={len(pats)}/{len(pats)} positions={sum(map(len,actual.values()))} exact=1',flush=True)
r=subprocess.run([a.xsa,'sxi-info',str(a.off)],capture_output=True,text=True)
assert r.returncode!=0 and 'missing member m5 (chi)' in r.stderr,r.stderr
# CRC-correct header lies: qlite must reject before touching the data members.
with tempfile.TemporaryDirectory() as td:
 for flags in [struct.unpack_from('<Q',h,48)[0]&~64,struct.unpack_from('<Q',h,48)[0]|1]:
  bad=Path(td)/'bad.sxi2'
  subprocess.run(['cp','--reflink=auto',str(a.off),str(bad)],check=True)
  with bad.open('r+b') as f:
   hs=struct.unpack_from('<I',h,36)[0];head=bytearray(f.read(hs));struct.pack_into('<Q',head,48,flags);head[56:60]=b'\0'*4;struct.pack_into('<I',head,56,zlib.crc32(head));f.seek(0);f.write(head)
  r=subprocess.run([a.xsa,'query',str(bad),'--patterns-fasta',str(a.patterns)],capture_output=True,text=True)
  assert r.returncode!=0 and 'chi flag/member mismatch' in r.stderr,r.stderr
print('REFUSALS full_reader=missing_m5 qlite_flag_lies=2 PASS',flush=True)
