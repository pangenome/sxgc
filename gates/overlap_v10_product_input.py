"""Generated integration input, NOT a corpus profile. The length also selects the pinned fixture sink."""
from hashlib import sha256
from pathlib import Path
import struct

N = 6_311_445
ROWS = N + 1
STEP, PAD = 62, b'x'
MARKS = (N + STEP - 1) // STEP
# Every maximal run except the pad runs contains a non-pad predecessor (including END).
RUNS = 2 * MARKS + 3
M_CAP = MARKS + 1  # exactly one eligible trigger site per marker, plus forced start
POST = 64 << 10
M20 = 256 << 10
LOG_CAP = 16 << 10  # except 48 KiB for finish; at most 12 bounded log drains per arm
CONTROLS = 512 << 10  # includes both arms' logs, metadata and prior arm remnants
TAU = 64

def ceil(a,b): return (a+b-1)//b

def model():
    pieces=sum(44+5*r+16*ceil(23*r,64)+36 for r in [min(65536,RUNS-i) for i in range(0,RUNS,65536)])
    # m1 is fsynced and ALL its PRC1s unlinked before m5 opens its low/high files.
    # For chi>0, l=floor(log2((n+1)/chi)), chi*l <= n+1; highbits <= 2*n+1.
    common=dict(sxls=ROWS+48*ceil(ROWS,262144),pfck=40+8*(1+ROWS//TAU),bitmap=ceil(ROWS,8),
                sparse=8*ceil(RUNS,1024),m20=M20,controls=CONTROLS)
    ef=ceil(3*ROWS+2,8)+2
    base=sum(common.values())+max(pieces,ef)
    # Separate services, sequential arms. Position band (reference only) vs inverse+post (candidate only).
    # Front dictionary = U+16k-8, plus 8m keys. k<=m<=M_CAP, U<=N.
    front=N+24*M_CAP-8+CONTROLS; rank=16*M_CAP+CONTROLS; stream=base+8*M_CAP+POST
    return dict(files=common,prc_and_progress=pieces,ef=ef,reference=base+8*32768,
                front=front,rank=rank,stream=stream,candidate=max(front,rank,stream),limit=2*N)

def trigger(word):
    h=1469598103934665603
    for c in word:h=((h^c)*1099511628211) & ((1<<64)-1)
    return h%31==0

def symbols():
    assert not trigger(PAD*8)
    codes=bytes(c for c in range(3,255) if c!=PAD[0] and sum(trigger(PAD*i+bytes([c])+PAD*(7-i)) for i in range(8))==1)[:48]
    assert len(codes)==48
    # de Bruijn(48,3) prefix: no ordered triple of marker bytes recurs. Hence an
    # equal prefix cannot include three markers and the following partial record;
    # in particular h<255. The independent raw witness enforces this before forwarding.
    a=[0]*4; seq=bytearray()
    def gen(t,p):
        if t==4:
            if 3%p==0:seq.extend(codes[i] for i in a[1:1+p])
        else:
            a[t]=a[t-p];gen(t+1,p)
            for j in range(a[t-p]+1,48):a[t]=j;gen(t+1,t)
    gen(1,1); assert len(seq)==48**3
    return seq

def make(d):
    d.mkdir(mode=0o700);seq=symbols();raw=bytearray(PAD*N);at=31
    raw[at::STEP]=seq[:len(raw[at::STEP])]
    # Unique three-marker printable search crossing ONLY a metadata cut.
    i=next(i for i in range(8,len(seq)-2) if all(65<=c<=122 and c!=92 for c in seq[i:i+3]))
    pos=at+i*STEP; pat=bytes(raw[pos:pos+2*STEP+1]);assert raw.count(pat)==1 and raw.find(pat)==pos
    cut=pos+22
    starts=struct.pack('<4sIIIQQQQIIIQ',b'XSEP',1,1,64,3,N,1,0,0,3,cut-3,0)
    used=sorted(set(raw));table=bytearray(256);order=used+sorted(set(range(256))-set(used))
    for j,c in enumerate(order,1):table[c]=j%256
    (d/'map').mkdir()
    for file,data in [('raw',raw),('checker',raw),('remap',table),('map/seps.bin',starts)]:
        p=d/file;p.write_bytes(data);p.chmod(0o400)
    assert not (d/'raw').samefile(d/'checker')
    return dict(raw=sha256(raw).hexdigest(),map=sha256(starts).hexdigest(),remap=sha256(table).hexdigest(),pattern=pat.decode('ascii'),pos=pos,cut=cut)
