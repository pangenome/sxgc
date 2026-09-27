"""Exhaustive small binary certificate audit; test-only dense oracle."""
import itertools,json,pathlib,struct,subprocess,sys,tempfile
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[3]))
from test_sxi_separator import cyclic_frame
adapter=sys.argv[1]
accepted=rejected=0
with tempfile.TemporaryDirectory(prefix='seam-certificate-') as tmp:
    root=pathlib.Path(tmp);prefix=root/'parse';ri=root/'out.ri4';heads=root/'out.heads'
    for n in range(1,10):
        for symbols in itertools.product(b'AB',repeat=n):
            text=bytes(symbols)
            _,_,padded=cyclic_frame(text+b'\x02'*10)
            pathlib.Path(str(prefix)+'.rlebwt.meta').write_bytes(struct.pack('<2Q',n+10,len(padded)))
            pathlib.Path(str(prefix)+'.rlebwt').write_bytes(b''.join(struct.pack('<I',c|(z<<8)) for c,z,_,_ in padded))
            for ext,col in [('.ssa',2),('.ssa_t',3)]:
                vals=[len(padded)]+[v[col] for v in padded]
                pathlib.Path(str(prefix)+ext).write_bytes(struct.pack(f'<{len(vals)}Q',*vals))
            ri.unlink(missing_ok=True);heads.unlink(missing_ok=True)
            result=subprocess.run([adapter,str(prefix),str(ri),str(heads),f'{text[-1]:02x}'],capture_output=True)
            if result.returncode:
                assert b'cyclic seam repair required' in result.stderr,(text,result.stderr)
                assert not ri.exists() and not heads.exists()
                rejected+=1;continue
            raw=ri.read_bytes();actual_n,k,r=struct.unpack_from('<3Q',raw,8)
            chars=raw[2080:2080+r];lengths=struct.unpack_from(f'<{r}I',raw,2080+r)
            actual_bwt=b''.join(bytes([c])*z for c,z in zip(chars,lengths))
            _,bwt,runs=cyclic_frame(text)
            actual_heads=list(struct.unpack(f'<{r}Q',heads.read_bytes()))
            offset=2080+5*r;bits,w=struct.unpack_from('<QB',raw,offset);packed=int.from_bytes(raw[offset+9:],'little')
            tails=[n-1-((packed>>(i*w))&((1<<w)-1)) for i in range(r)]
            assert actual_n==n and k==1 and bits==r*w and actual_bwt==bwt and actual_heads==[v[2] for v in runs] and tails==[v[3] for v in runs],text
            accepted+=1
assert accepted and rejected
print(json.dumps(dict(passed=True,checked=accepted+rejected,accepted=accepted,rejected=rejected,unsafe_outputs=0)))
