#!/usr/bin/env python3
"""Container mode, orientation, k, and names are validated, not guessed."""
import pathlib,struct,subprocess,tempfile,zlib
ROOT=pathlib.Path(__file__).resolve().parents[1];X=ROOT/'xsa/target/release/xsa'
def crc(b):
    hs=struct.unpack_from('<I',b,36)[0];struct.pack_into('<I',b,56,0);struct.pack_into('<I',b,56,zlib.crc32(b[:hs]))
def run(args,ok=True):
    p=subprocess.run(list(map(str,args)),capture_output=True);assert (p.returncode==0)==ok,(args,p.stderr);return p.stdout
source=pathlib.Path('/tmp/sxi-query-tests5/dna.sxi').read_bytes()
with tempfile.TemporaryDirectory(prefix='query-metadata-') as tmp:
    tmp=pathlib.Path(tmp)
    for label,offset,fmt,value in [('k',16,'<Q',1),('flags',48,'<Q',32)]:
        b=bytearray(source);struct.pack_into(fmt,b,offset,value);crc(b);p=tmp/(label+'.sxi');p.write_bytes(b);run([X,'sxi-info',p],False)
    b=bytearray(source);off,size=struct.unpack_from('<QQ',b,64+5*40+8);b[off+2]=ord('9');struct.pack_into('<I',b,64+5*40+32,zlib.crc32(b[off:off+size]));crc(b);p=tmp/'names.sxi';p.write_bytes(b);run([X,'sxi-info',p],False)
    b=bytearray(source);struct.pack_into('<Q',b,16,1);struct.pack_into('<Q',b,48,0);crc(b);p=tmp/'legacy.sxi';p.write_bytes(b)
    assert b'k=4' in run([X,'sxi-info',p])
    # Legacy auto has no provenance: generic; explicit DNA rejects invalid bytes.
    run([X,'query','--sxi',p,'--pattern','!'])
    run([X,'query','--sxi',p,'--pattern','!','--mode','dna'],False)
    assert b'"row":' in run([X,'query','--sxi','/tmp/sxi-query-tests5/dna.sxi','--pattern','ACG','--trace-samples','--sample','2'])
print('PASS invalid k/flags/names rejected; legacy k normalized read-only; mode overrides; annotated sampled traces')
