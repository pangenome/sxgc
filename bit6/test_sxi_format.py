#!/usr/bin/env python3
"""Adversarial format tests, independent of the real-corpus differential."""
import argparse,pathlib,struct,subprocess,tempfile,zlib
p=argparse.ArgumentParser();p.add_argument('--writer',default='/tmp/laneU/sxi_write');p.add_argument('--sxi2-writer',default='/tmp/sxi2_write_v4');p.add_argument('--xsa',default='xsa/target/release/xsa');p.add_argument('--cpp-reader');a=p.parse_args()
def run(cmd,ok=True):
    x=subprocess.run(list(map(str,cmd)),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    assert (x.returncode==0)==ok,(cmd,x.returncode,x.stderr.decode())
def header_crc(b):
    hs=struct.unpack_from('<I',b,36)[0];struct.pack_into('<I',b,56,0);struct.pack_into('<I',b,56,zlib.crc32(b[:hs]))
def member_crc(b,i):
    d=64+40*(i-1);off,size=struct.unpack_from('<QQ',b,d+8);struct.pack_into('<I',b,d+32,zlib.crc32(b[off:off+size]));header_crc(b)
def member_index(b,id):
    count=struct.unpack_from('<I',b,32)[0]
    return next(i+1 for i in range(count) if struct.unpack_from('<I',b,64+40*i)[0]==id)
def check_remap_container(path,ok=True):
    run([a.xsa,'sxi-info',path],ok)
    if a.cpp_reader:run([a.cpp_reader,path],ok)
with tempfile.TemporaryDirectory(prefix='sxi-format-unit-')as d:
    d=pathlib.Path(d);ri=d/'input.ri4';heads=d/'heads';chi=d/'chi';anc=d/'anc';names=d/'names'
    # Valid table with 3 singleton runs and both nonempty anchors and names.
    chars=b'\nAC';counts=[chars.count(c)for c in range(256)];C=[];acc=0
    for q in counts:C.append(acc);acc+=q
    core=struct.pack('<IIQQQ256Q',0x52585349,4,3,1,3,*C)+chars+struct.pack('<3I',1,1,1)
    heads.write_bytes(struct.pack('<3Q',2,0,1));chi.write_bytes(struct.pack('<3Q',3,0,1));anc.write_bytes(struct.pack('<IQ2Q',0x434e4158,1,0,0));names.write_text('tiny\t0\t2\n')
    for width in range(2,65):
        packed=sum(x<<(i*width)for i,x in enumerate([0,2,1]));bits=3*width
        ri.write_bytes(core+struct.pack('<QB',bits,width)+packed.to_bytes(((bits+63)//64)*8,'little'))
        out=d/f'w{width}.sxi';cmd=[a.writer,'--ri4',ri,'--heads',heads,'--chi',chi,'--anchors',anc,'--names',names,'--output',out]
        run(cmd);run([a.xsa,'sxi-info',out,'--chi-out',str(out)+'.chi']);assert pathlib.Path(str(out)+'.chi').read_bytes()==struct.pack('<3Q',0,1,3)
    source=out.read_bytes();run(cmd,False);assert out.read_bytes()==source
    # SXI2 is a separate publication; the canonical SXI1 source stays byte-identical.
    compact=d/'compact.sxi';run([a.sxi2_writer,'--sxi',out,'--output',compact,'--validator',a.xsa])
    compact_chi=d/'compact.chi';run([a.xsa,'sxi-info',compact,'--chi-out',compact_chi])
    assert compact_chi.read_bytes()==struct.pack('<3Q',0,1,3)
    assert out.read_bytes()==source and compact.read_bytes()[:4]==b'SXI2'
    # Recompute both checksums so malformed codec content reaches semantic validation.
    b=bytearray(compact.read_bytes());d1=64
    off=struct.unpack_from('<Q',b,d1+8)[0];b[off+2048]=255
    member_crc(b,1);bad=d/'compact-codebook.sxi';bad.write_bytes(b)
    run([a.xsa,'sxi-info',bad],False)
    b=bytearray(compact.read_bytes());i5=member_index(b,5);d5=64+(i5-1)*40
    off=struct.unpack_from('<Q',b,d5+8)[0];struct.pack_into('<Q',b,off,64)
    member_crc(b,i5);bad=d/'compact-ef.sxi';bad.write_bytes(b)
    run([a.xsa,'sxi-info',bad],False)
    b=bytearray(compact.read_bytes());iphi=member_index(b,8);dphi=64+(iphi-1)*40
    off=struct.unpack_from('<Q',b,dphi+8)[0]
    struct.pack_into('<Q',b,off+24,struct.unpack_from('<Q',b,off)[0])
    member_crc(b,iphi);bad=d/'compact-phi.sxi';bad.write_bytes(b)
    run([a.xsa,'sxi-info',bad],False)
    b=bytearray(compact.read_bytes());desc=64+(member_index(b,9)-1)*40
    struct.pack_into('<Q',b,desc+24,1);header_crc(b)
    bad=d/'compact-escape-count.sxi';bad.write_bytes(b)
    run([a.xsa,'sxi-info',bad],False)
    # Identity metadata must not change one byte of an accepted container.
    table=d/'remap';table.write_bytes(bytes(range(256)))
    identity=d/'identity.sxi';run(cmd[:-1]+[identity,'--remap',table]);assert identity.read_bytes()==source
    sigma=bytearray(range(256));sigma[1],sigma[65]=sigma[65],sigma[1];table.write_bytes(sigma)
    for with_names in (False,True):
        remapped=d/f'remap-{with_names}.sxi'
        remap_cmd=[a.writer,'--ri4',ri,'--heads',heads,'--chi',chi,'--anchors',anc,'--output',remapped,'--remap',table]
        if with_names:remap_cmd+=['--names',names]
        run(remap_cmd);check_remap_container(remapped)
        b=remapped.read_bytes();count=struct.unpack_from('<I',b,32)[0]
        assert count==6+with_names and struct.unpack_from('<Q',b,48)[0]&16
        last=64+40*(count-1);assert struct.unpack_from('<I',b,last)[0]==7
        off,size=struct.unpack_from('<QQ',b,last+8);assert b[off:off+size]==sigma
        for label,change in [('duplicate',lambda v:v.__setitem__(0,v[1])),
                             ('separator',lambda v:(v.__setitem__(30,31),v.__setitem__(31,30)))]:
            bad=bytearray(b);v=bytearray(sigma);change(v);bad[off:off+256]=v
            struct.pack_into('<I',bad,last+32,zlib.crc32(v));header_crc(bad)
            path=d/f'{label}-{with_names}.sxi';path.write_bytes(bad);check_remap_container(path,False)
        bad=bytearray(b);struct.pack_into('<Q',bad,48,struct.unpack_from('<Q',bad,48)[0]&~16);header_crc(bad)
        path=d/f'flag-{with_names}.sxi';path.write_bytes(bad);check_remap_container(path,False)
    for bad_table in (bytes(256),bytes(range(255)),bytes(range(256))+b'x'):
        table.write_bytes(bad_table);badout=d/'invalid-table.sxi'
        run([a.writer,'--ri4',ri,'--heads',heads,'--output',badout,'--remap',table],False);assert not badout.exists()
    # Semantic/structural corruption, recomputing checksums to avoid testing CRC alone.
    mutations=[('version',4,'<I',2),('flags',48,'<Q',2),('reserved',60,'<I',1),('count',32,'<I',7),('run-count',24,'<Q',4),('member-id',64,'<I',2),('overlap',64+40+8,'<Q',struct.unpack_from('<Q',source,72)[0]),('anchor-count',64+3*40+24,'<Q',2)]
    for label,off,fmt,value in mutations:
        b=bytearray(source);struct.pack_into(fmt,b,off,value);header_crc(b);bad=d/(label+'.sxi');bad.write_bytes(b);run([a.xsa,'sxi-info',bad],False)
    for label,member,relative,raw in [('width',2,8,b'\0'),('anchor-range',4,12,struct.pack('<Q',3)),('head-range',3,0,struct.pack('<Q',3)),('chi-overflow',5,0,b'\xff\xff\xff')]:
        b=bytearray(source);off=struct.unpack_from('<Q',b,64+40*(member-1)+8)[0]+relative;b[off:off+len(raw)]=raw;member_crc(b,member);bad=d/(label+'.sxi');bad.write_bytes(b);run([a.xsa,'sxi-info',bad],False)
    # The empty unfinished chi member is distinguishable from completed-empty.
    for complete in [False,True]:
        chi.write_bytes(b'');out=d/f'empty-{complete}.sxi';cmd=[a.writer,'--ri4',ri,'--heads',heads,'--output',out]
        if complete:cmd+=['--chi',chi]
        run(cmd);run([a.xsa,'sxi-info',out]);run([a.xsa,'sxi-info',out,'--chi-out',str(out)+'.chi'],complete)
    # Writer input failures must not publish an artifact.
    chi.write_bytes(struct.pack('<2Q',1,1));out=d/'duplicate.sxi';run([a.writer,'--ri4',ri,'--heads',heads,'--chi',chi,'--output',out],False);assert not out.exists()
    heads.write_bytes(struct.pack('<3Q',0,0,1));out=d/'singleton.sxi';run([a.writer,'--ri4',ri,'--heads',heads,'--output',out],False);assert not out.exists()
print('PASS SXI1 widths 2..64, metadata/remap/corruption, SXI2 entropy/EF/phi/escape format differential')
