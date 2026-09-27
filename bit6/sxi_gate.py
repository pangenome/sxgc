#!/usr/bin/env python3
"""Format differential only. Fixture heads are oracles, never from-scratch evidence."""
import argparse, pathlib, subprocess, json, struct, filecmp, zlib
ap=argparse.ArgumentParser();ap.add_argument('--work',default='/tmp/laneU/g0');ap.add_argument('--writer',default='/tmp/laneU/sxi_write');ap.add_argument('--dump',default='/tmp/laneU/slim_dump');args=ap.parse_args()
root=pathlib.Path(__file__).resolve().parents[1];work=pathlib.Path(args.work);work.mkdir(parents=True,exist_ok=True)
logs=root/'bit6/sxi_logs';logs.mkdir(exist_ok=True);xsa=root/'xsa/target/release/xsa';bat=pathlib.Path('/tmp/laneY/bat')
names=['duplicates-600k','random-4-2k','random-4-20k','random-bin-20k','random-4-200k','satellite-18k','HOR-nested']
def run(cmd,label,success=True):
    with open(logs/(label+'.log'),'w')as out:
        out.write('COMMAND '+json.dumps(list(map(str,cmd)))+'\n');out.flush()
        p=subprocess.run(list(map(str,cmd)),stdout=out,stderr=subprocess.STDOUT)
    assert (p.returncode==0)==success,(label,p.returncode)
def same(a,b):assert filecmp.cmp(a,b,shallow=False),(str(a),str(b))
for name in names:
    p=work/name;ri=bat/(name+'.ri4');sx=p.with_suffix('.sxi');head=pathlib.Path('/tmp/laneQ/pass4')/(name+'.head_sa')
    run([xsa,'chi-rspace','--stream-agg','--ri4',ri,'--agg',bat/(name+'.agg'),'-o',str(p)+'.ri.sA'],name+'.ri-sweep')
    run([args.writer,'--ri4',ri,'--heads',head,'--chi',str(p)+'.ri.sA','--names',bat/(name+'.sc'),'--output',sx],name+'.write')
    run([xsa,'sxi-info',sx,'--chi-out',str(p)+'.sorted.sA'],name+'.info')
    def vals(path):
        data=pathlib.Path(path).read_bytes();return list(struct.unpack('<'+'Q'*(len(data)//8),data))
    assert sorted(vals(str(p)+'.ri.sA'))==vals(str(p)+'.sorted.sA')
    run([args.dump,'--slim','--resolve-ri4','--dict-stream','--sxi',sx,'--parse',bat/(name+'_pfp'),'-t','8','-o',str(p)+'.sxi.agg'],name+'.dump')
    same(str(p)+'.sxi.agg',bat/(name+'.agg'))
    run([xsa,'chi-rspace','--stream-agg','--sxi',sx,'--agg',str(p)+'.sxi.agg','-o',str(p)+'.sxi.sA'],name+'.sxi-sweep')
    same(str(p)+'.ri.sA',str(p)+'.sxi.sA')
    # Both resident and streamed sweep routes must preserve emission order.
    run([xsa,'chi-rspace','--sxi',sx,'--agg',str(p)+'.sxi.agg','-o',str(p)+'.resident.sA'],name+'.resident-sweep')
    same(str(p)+'.ri.sA',str(p)+'.resident.sA')
    text=(bat/(name+'.txt')).read_bytes().splitlines()[0];patterns=p.with_suffix('.fa')
    patterns.write_bytes(b'>p0\n'+text[:20]+b'\n>p1\n'+text[50:60]+b'\n>absent\nZZZZZZ\n')
    for mode in ['ms','exact']:
        for route,path in [('ri',ri),('sxi',sx)]:
            cmd=[xsa,'query','--ri4',path,'--patterns',patterns,'--plain']
            if mode=='ms':cmd+=['--ms','--ms-out',str(p)+'.'+route+'.ms']
            else:
                cmd+=['--sample','2','--output',str(p)+'.'+route+'.exact']
                if route=='ri':cmd+=['--sidecar',bat/(name+'.sc')]
            run(cmd,name+'.'+route+'.'+mode)
        same(str(p)+'.ri.'+mode,str(p)+'.sxi.'+mode)
    print('PASS',name,'aggregate + streamed/resident sweep + exact/MS query + chi roundtrip',flush=True)
# Byte corruption in every member/header, unsupported version, and truncation.
source=(work/'duplicates-600k.sxi').read_bytes();members=struct.unpack_from('<I',source,32)[0]
for label,offset in [('header',8)]+[(f'member{i+1}',struct.unpack_from('<Q',source,64+40*i+8)[0]) for i in range(members)]:
    b=bytearray(source);b[offset]^=1;p=work/('bad-'+label+'.sxi');p.write_bytes(b)
    run([xsa,'sxi-info',p],'reject-'+label,False)
    run([args.dump,'--slim','--resolve-ri4','--sxi',p,'--parse',bat/'duplicates-600k_pfp','-o',str(p)+'.agg'],'cpp-reject-'+label,False)
for label,data in [('truncated',source[:-1]),('trailing',source+b'!')]:
    p=work/(label+'.sxi');p.write_bytes(data);run([xsa,'sxi-info',p],'reject-'+label,False)
# Semantic corruption with valid CRCs must also fail.
def resign(b,member):
    base=64+40*(member-1);off,size=struct.unpack_from('<QQ',b,base+8)
    struct.pack_into('<I',b,base+32,zlib.crc32(b[off:off+size]));struct.pack_into('<I',b,56,0)
    hs=struct.unpack_from('<I',b,36)[0];struct.pack_into('<I',b,56,zlib.crc32(b[:hs]))
for label,member in [('bad-head-range',3),('bad-chi-varint',5)]:
    b=bytearray(source);off=struct.unpack_from('<Q',b,64+40*(member-1)+8)[0]
    if member==3:struct.pack_into('<Q',b,off,struct.unpack_from('<Q',b,8)[0])
    else:b[off]=255
    resign(b,member);p=work/(label+'.sxi');p.write_bytes(b);run([xsa,'sxi-info',p],'reject-'+label,False)
print('FORMAT_DIFFERENTIAL PASS 7/7; corruption rejection PASS; NOT source-build G0; provenance=fixture-conversion-only',flush=True)
