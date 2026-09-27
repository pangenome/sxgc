#!/usr/bin/env python3
"""Tiny pilot-mode regression and honest front-end-mode fail-closed contract."""
import pathlib,subprocess,struct,tempfile,sys
exe=sys.argv[1]
def u64(x):return struct.pack('<Q',x)
def packed(values,width):
    x=sum(v<<(i*width)for i,v in enumerate(values));bits=len(values)*width
    return u64(bits)+bytes([width])+x.to_bytes(((bits+63)//64)*8,'little')
def triple(rows,widths):
    a,b,c=widths;w=a+b+c
    values=[x|(y<<a)|(z<<(a+b))for x,y,z in rows]
    raw=packed(values,w)
    return bytes([a,b,c,w])+raw[:8]+raw[9:]
with tempfile.TemporaryDirectory(prefix='sxi-phi-unit-')as d:
    p=pathlib.Path(d)
    for text in [b'banana\0',b'aaaaa\0',b'\0',b'ACGTACGT\0']:
        n=len(text);sa=sorted(range(n),key=lambda i:text[i:]);bwt=bytes(text[(pos-1)%n]for pos in sa)
        starts=[i for i in range(n)if i==0 or bwt[i]!=bwt[i-1]];ends=[x-1 for x in starts[1:]]+[n-1]
        symbols=bytes(bwt[i]for i in starts);lens=[e-s+1 for s,e in zip(starts,ends)];r=len(starts)
        phi=[0]*n
        for row,pos in enumerate(sa):phi[pos]=sa[(row-1)%n]
        w=max(1,n.bit_length())
        index=u64(n)+packed([],1)+triple([(0,0,0)],(1,1,1))+packed([],1)+triple([(phi[i],0,i)for i in range(n)]+[(0,0,n)],(w,1,w))
        counts=[bwt.count(c)for c in range(256)];C=[];acc=0
        for x in counts:C.append(acc);acc+=x
        ri=struct.pack('<IIQQQ',0x52585349,4,n,1,r)+struct.pack('<256Q',*C)+symbols+struct.pack('<'+'I'*r,*lens)+packed([n-1-sa[e]for e in ends],w)
        (p/'index').write_bytes(index);(p/'input.ri4').write_bytes(ri)
        cmd=[exe,str(p/'index'),str(p/'input.ri4'),str(p/'heads'),'2']
        subprocess.run(cmd,check=True);assert(p/'heads').read_bytes()==b''.join(u64(sa[s])for s in starts);(p/'heads').unlink()
    q=subprocess.run([exe,'--from-front-end',str(p/'missing.rle'),str(p/'missing.tails'),str(p/'fresh.heads')],capture_output=True)
    assert q.returncode and b'tail-only Phi construction is not implemented' in q.stderr
    assert not(p/'fresh.heads').exists()
print('PASS: four tiny pilot Phi inversions; front-end mode fails before file access')
