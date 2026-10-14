import random,struct,heapq
from pathlib import Path
root=Path('/mnt/nvme2n1/erikg/sol6a-histogram')
r=random.Random(42);a=list(range(8192));r.shuffle(a)
def word(v):return struct.pack('<Q',v)
def header(m,ver):return m+struct.pack('<I',ver)+word(0)+word(len(a))+word(len(a))+word(0)
(root/'synthetic-dense.pos').write_bytes(header(b'SXP3',1)+b''.join(word(p) for p in a))
freq=[[0]*256 for _ in range(5)];streams=[]
for g in range(2):
 stream=[]
 for i in range(g*4096+1,(g+1)*4096):
  d=a[i]-a[i-1];z=2*d if d>=0 else -2*d-1;ctx=0
  while z>=128:stream.append((ctx,(z&255)|128));freq[ctx][(z&255)|128]+=1;z>>=7;ctx+=1
  stream.append((ctx,z));freq[ctx][z]+=1
 streams.append(stream)
models=[];lengths=[]
for f in freq:
 heap=[(n,c,c) for c,n in enumerate(f) if n];heapq.heapify(heap);uid=256
 if not heap:heap=[(1,0,0)]
 while len(heap)>1:
  x=heapq.heappop(heap);y=heapq.heappop(heap);heapq.heappush(heap,(x[0]+y[0],uid,(x[2],y[2])));uid+=1
 lens=[0]*256
 def visit(node,depth=0):
  if isinstance(node,int):lens[node]=max(1,depth);return
  visit(node[0],depth+1);visit(node[1],depth+1)
 visit(heap[0][2]);lengths.append(bytes(lens));model={};code=0;prev=0
 for ln,c in sorted((ln,c) for c,ln in enumerate(lens) if ln):
  code<<=ln-prev;model[c]=(code,ln);code+=1;prev=ln
 models.append(model)
blocks=[]
for stream in streams:
 bits=''.join(format(models[ctx][c][0],f'0{models[ctx][c][1]}b') for ctx,c in stream);bits+='0'*((-len(bits))%8);blocks.append(bytes(int(bits[i:i+8],2) for i in range(0,len(bits),8)))
off=56+24*2+1280;tab=b''
for g,block in enumerate(blocks):tab+=word(g*4096)+word(off)+word(a[g*4096]);off+=len(block)
(root/'synthetic-v2.pos').write_bytes(header(b'SXP4',2)+word(2)+word(0)+tab+b''.join(lengths)+b''.join(blocks))
