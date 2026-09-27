#!/usr/bin/env python3
"""Retained yeast235 gate: direct-text MEM oracle, server/CLI byte parity,
record-count repack regression and immutable five-member comparison."""
import hashlib,json,pathlib,socket,struct,subprocess,time,urllib.request
ROOT=pathlib.Path(__file__).resolve().parents[1];LOG=ROOT/'bit6/sxi_logs/query-product';X=str(ROOT/'xsa/target/release/xsa')
WORK=pathlib.Path('/tmp/sxi-query-yeast');WORK.mkdir(exist_ok=True)
SCRATCH=pathlib.Path('/tmp/sxi-repair2/yeast235/xsa-build-26al38ee');INDEX='/tmp/sxi-query-yeast235.sxi'
def run(args,label):
    p=subprocess.run(['/usr/bin/time','-v','-o',str(LOG/(label+'.time')),*map(str,args)],capture_output=True)
    (LOG/(label+'.stderr')).write_bytes(p.stderr)
    assert p.returncode==0,(args,p.stderr.decode());return p.stdout
names=[line.split('\t') for line in (SCRATCH/'collection.txt.names.tsv').read_text().splitlines()]
n=(SCRATCH/'collection.txt').stat().st_size
reads=[]
with (SCRATCH/'collection.txt').open('rb') as f:
    for i,doc in enumerate([12,555]):
        name,fs,length=names[doc];start=n-1-int(fs)-int(length);f.seek(start+123);seq=f.read(45)[::-1]
        if i==1:seq=seq[:23]+(b'A' if seq[23:24]!=b'A' else b'C')+seq[24:]
        reads.append(('sample'+str(i),seq))
fa=WORK/'reads.fa';fa.write_bytes(b''.join(b'>'+name.encode()+b'\n'+seq+b'\n' for name,seq in reads))
rc=bytes.maketrans(b'ACGTN',b'TGCAN');br=WORK/'oriented.tsv';br.write_bytes(b''.join(name.encode()+b'\t'+strand+b'\t'+q+b'\n' for name,seq in reads for strand,q in [(b'+',seq[::-1]),(b'-',seq.translate(rc))]))
expected=run(['/tmp/sxi-query-brute',SCRATCH/'collection.txt',br,SCRATCH/'collection.txt.names.tsv',20],'yeast-brute')
want=[]
for row in expected.decode().splitlines():
    name,doc,off,length,start,strand=row.split('\t');doc=int(doc)
    want.append(dict(read=name,doc_id=doc,name=names[doc][0],offset=int(off),len=int(length),qstart=int(start),strand=strand))
actual=run([X,'mems','--sxi',INDEX,'--reads',fa,'--min-len',20,'-j',3],'yeast-mems')
canon=lambda rows:sorted(json.dumps(x,sort_keys=True) for x in rows)
assert canon(want)==canon([json.loads(x) for x in actual.splitlines()]),'MEM oracle mismatch'
(LOG/'yeast-mems.jsonl').write_bytes(actual)
print(json.dumps(dict(gate='yeast235-brute-mems',reads=len(reads),mems=len(want),status='PASS')),flush=True)
# stats reports the new named-record k from a fresh writer publication.
stats=run([X,'stats','--sxi',INDEX],'yeast-stats');(LOG/'yeast-stats.log').write_bytes(stats);assert b'9901' in stats

def members(path):
    result={}
    with open(path,'rb') as f:
        h=f.read(64);n,k,r=struct.unpack_from('<3Q',h,8);count=struct.unpack_from('<I',h,32)[0];directory=f.read(40*count)
        for i in range(count):
            id,codec,off,size,num,crc,res=struct.unpack_from('<IIQQQII',directory,40*i);f.seek(off);hash=hashlib.sha256();left=size
            while left:b=f.read(min(left,1<<20));assert b;left-=len(b);hash.update(b)
            result[id]=dict(codec=codec,bytes=size,count=num,crc=crc,sha256=hash.hexdigest())
    return dict(n=n,k=k,r=r,members=result)
before=members('/tmp/sxi-repair2/yeast235.sxi');after=members(INDEX)
assert before['k']==1 and after['k']==9901
assert before['members']==after['members'] and after['members'][5]['count']==85404336
(LOG/'yeast-members.json').write_text(json.dumps(dict(before=before,after=after),indent=2)+'\n')
# Reference-only yeast regression publication remains unchanged too.
reg=members('/mnt/nvme3n1/erikg/sxi-repair2/yeast.sxi')
prior=json.loads((ROOT/'bit6/sxi_logs/sep-convention/repair2/yeast-members.json').read_text())
(LOG/'yeast-regression-current.json').write_text(json.dumps(reg,indent=2)+'\n')
assert reg['members'][5]['count']==85404240
for id in range(1,6):
    assert reg['members'][id]['sha256']==prior['previous']['members'][str(id)]['sha256']
print('PASS yeast235 k=9901, chi=85404336, all six member hashes unchanged; yeast chi=85404240',flush=True)
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
log=(LOG/'yeast-server.log').open('wb');server=subprocess.Popen(['/usr/bin/time','-v','-o',str(LOG/'yeast-server.time'),X,'serve','--sxi',INDEX,'-j','3','--bind',f'127.0.0.1:{port}'],stderr=log,start_new_session=True)
try:
    base=f'http://127.0.0.1:{port}'
    for _ in range(1800):
        try:stats=urllib.request.urlopen(base+'/stats',timeout=1).read();break
        except OSError:time.sleep(.1)
    else:raise AssertionError('server startup timeout')
    assert json.loads(stats)['k']==9901
    for endpoint,kind,extra in [('/query','query',[]),('/ms','query',['--ms']),('/batch','mems',['--min-len','20'])]:
        if endpoint=='/batch':value={'reads':[{'name':name,'read':seq.decode()} for name,seq in reads],'min_len':20};cli=actual
        else:value={'name':reads[0][0],'read':reads[0][1].decode()};single=WORK/'single.fa';single.write_bytes(b'>'+reads[0][0].encode()+b'\n'+reads[0][1]+b'\n');cli=run([X,kind,'--sxi',INDEX,'--reads',single,*extra],'yeast-cli-'+endpoint[1:])
        got=run(['curl','--fail','--silent','-H','Content-Type: application/json','--data-binary',json.dumps(value),base+endpoint],'yeast-http-'+endpoint[1:])
        assert got==cli,endpoint
        (LOG/('yeast-http-'+endpoint[1:]+'.jsonl')).write_bytes(got)
        print('PASS HTTP/CLI byte parity '+endpoint,flush=True)
finally:
    # Kill only this test's newly created process group.
    import os,signal
    os.killpg(server.pid,signal.SIGTERM);server.wait();log.close()
