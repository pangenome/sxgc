#!/usr/bin/env python3
"""Fresh streamed/file AGC equivalence, archive ranges, and fail-closed FIFO gates."""
import argparse, hashlib, json, os, pathlib, random, resource, signal, struct, subprocess, time
p=argparse.ArgumentParser();p.add_argument('--work',required=True);p.add_argument('--xsa',required=True);p.add_argument('--tools',required=True);p.add_argument('--log-dir',required=True);p.add_argument('--agc',default='/home/erikg/agc/bin/agc');a=p.parse_args()
resource.setrlimit(resource.RLIMIT_AS,(18_000_000_000,18_000_000_000))
work=pathlib.Path(a.work).absolute();work.mkdir(parents=True,exist_ok=True)
logs=pathlib.Path(a.log_dir).absolute();logs.mkdir(parents=True,exist_ok=True)
tools=pathlib.Path(a.tools).absolute();env=dict(os.environ,XSA_TOOLS=str(tools))
def run(cmd,**kw):return subprocess.run(list(map(str,cmd)),check=True,env=env,**kw)
def members(path):
    result={}
    with open(path,'rb') as f:
        h=f.read(64);count=struct.unpack_from('<I',h,32)[0]
        desc=f.read(40*count)
        for i in range(count):
            ident,codec,offset,length,n,crc,res=struct.unpack_from('<IIQQQII',desc,40*i)
            f.seek(offset);digest=hashlib.sha256();left=length
            while left:
                b=f.read(min(left,1<<20));assert b;left-=len(b);digest.update(b)
            result[ident]=dict(codec=codec,length=length,count=n,sha256=digest.hexdigest())
    return result
def build(archive,label,extra=(),override=None,ok=True):
    out=work/(label+'.sxi');cmd=[a.xsa,'build','--agc',archive,'-o',out,'--scratch',work,'--threads','2','--verify-text-sample','32','--log-dir',logs,*extra]
    q=subprocess.run(list(map(str,cmd)),env=override or env,capture_output=True,text=True,timeout=180)
    (logs/(label+'.log')).write_text(q.stderr+q.stdout)
    assert (q.returncode==0)==ok,(cmd,q.stderr)
    if not ok:assert not out.exists();return q
    info=json.loads(q.stdout.splitlines()[-1]);scratch=pathlib.Path(info['work'])
    assert not (scratch/'collection.fifo').exists()
    assert (scratch/'collection.txt').exists()==('--materialize' in extra)
    return out,scratch
for fixture,length in [('random-2k',2000),('random-20k',20000),('random-200k',200000),('repeated-600k',600000),('satellite-18k',18000)]:
    rng=random.Random(171);seq=bytes(rng.choice(b'ACGT') for _ in range(length))
    if fixture.startswith('repeated'):seq=seq[:200000]+b'AC'+seq[2:200000]+b'GT'+seq[2:200000]
    if fixture.startswith('satellite'):seq=(seq[:101]*180)[:length]
    fa=work/(fixture+'.fa');fa.write_bytes(b''.join(b'>contig'+str(i).encode()+b'\n'+seq[i*len(seq)//3:(i+1)*len(seq)//3]+b'\n' for i in range(3)))
    archive=work/(fixture+'.agc')
    run([a.agc,'create','-t','2','-o',archive,fa],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    file,fw=build(archive,fixture+'-file',['--materialize'])
    stream,sw=build(archive,fixture+'-stream')
    assert members(file)==members(stream)
    for suffix in ['parse','dict']:
        assert (fw/('parse.'+suffix)).read_bytes()==(sw/('parse.'+suffix)).read_bytes()
    # Random range and seam coverage against the actual legacy stream.
    expected=(fw/'collection.txt').read_bytes()
    proc=subprocess.Popen([str(tools/'agc2flat'),str(archive),'--serve-ranges',str(sw/'collection.txt.names.tsv'),'--revlines','--upper','--sep','1e'],stdin=subprocess.PIPE,stdout=subprocess.PIPE)
    assert struct.unpack('<Q',proc.stdout.read(8))[0]==len(expected)
    ranges=[(0,len(expected))] if len(expected)<=65536 else [(0,65536),(len(expected)-65536,65536)]
    ranges += [(rng.randrange(len(expected)),rng.randrange(100)) for _ in range(30)]
    for start,length in ranges:
        length=min(length,len(expected)-start);proc.stdin.write(struct.pack('<QQ',start,length));proc.stdin.flush()
        assert proc.stdout.read(length)==expected[start:start+length]
    proc.stdin.close();assert proc.wait(timeout=10)==0
    print(json.dumps(dict(fixture=fixture,status='PASS',all_members_equal=True,parse_dict_equal=True,archive_ranges_equal=True)),flush=True)
# A real range response is deliberately corrupted; the actual audit must block publication.
wrapper=work/'corrupt-agc2flat';wrapper.write_text('''#!/usr/bin/env python3
import subprocess,sys,struct
real='''+repr(str(tools/'agc2flat'))+'''
if '--serve-ranges' not in sys.argv:
 import os
 os.execv(real,[real,*sys.argv[1:]])
p=subprocess.Popen([real,*sys.argv[1:]],stdin=subprocess.PIPE,stdout=subprocess.PIPE)
sys.stdout.buffer.write(p.stdout.read(8));sys.stdout.buffer.flush()
while True:
 q=sys.stdin.buffer.read(16)
 if not q:break
 p.stdin.write(q);p.stdin.flush();n=struct.unpack('<QQ',q)[1]
 data=p.stdout.read(n)
 sys.stdout.buffer.write(bytes([0])*len(data));sys.stdout.buffer.flush()
p.stdin.close();sys.exit(p.wait())
''');wrapper.chmod(0o755)
q=build(archive,'corrupt-range',override=dict(env,XSA_AGC2FLAT=str(wrapper)),ok=False)
assert 'verify-text-sample failed' in q.stderr
assert any('TEXT_SAMPLE_FAIL' in f.read_text() for f in logs.glob('corrupt-range-*.verify-text-sample.log'))
# Both endpoints failing, including failure before FIFO open, leave no FIFO.
for label,overrides in [('reader-fails',dict(XSA_PFP='/bin/false')),('writer-fails',dict(XSA_AGC2FLAT='/bin/false'))]:
    build(archive,label,override=dict(env,**overrides),ok=False)
assert not list(work.glob('xsa-build-*/collection.fifo'))
print('PASS corruption blocks publication; reader/writer failures unlink FIFO',flush=True)
# Same contig names in different samples must resolve to the right archive record.
rng=random.Random(22);fas=[]
for sample in range(2):
    fa=work/f'sample{sample}.fa'
    fa.write_bytes(b''.join(b'>shared'+str(i).encode()+b'\n'+bytes(rng.choice(b'ACGT') for _ in range(5000+i))+b'\n' for i in range(2)))
    fas.append(fa)
archive=work/'duplicate-names.agc';run([a.agc,'create','-t','2','-o',archive,*fas],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
file,fw=build(archive,'duplicate-names-file',['--materialize']);stream,sw=build(archive,'duplicate-names-stream')
assert members(file)==members(stream)
expected=(fw/'collection.txt').read_bytes()
cmd=[str(tools/'agc2flat'),str(archive),'--serve-ranges',str(sw/'collection.txt.names.tsv'),'--revlines','--upper','--sep','1e']
reply=subprocess.run(cmd,input=struct.pack('<QQ',0,len(expected)),capture_output=True,check=True)
assert reply.stdout==struct.pack('<Q',len(expected))+expected
# Invalid requests and names fail loudly (including an incomplete request).
for request in [struct.pack('<QQ',len(expected),1),struct.pack('<QQ',0,65537),b'x']:
    reply=subprocess.run(cmd,input=request,capture_output=True)
    assert reply.returncode!=0
bad=work/'bad.names.tsv';rows=(sw/'collection.txt.names.tsv').read_text().splitlines();parts=rows[0].split('\t');parts[1]=str(int(parts[1])+1);rows[0]='\t'.join(parts);bad.write_text('\n'.join(rows)+'\n')
badcmd=cmd.copy();badcmd[3]=str(bad);reply=subprocess.run(badcmd,input=b'',capture_output=True)
assert reply.returncode!=0 and b'invalid names coordinate' in reply.stderr
q=subprocess.run([a.xsa,'build','--text',str(fw/'collection.txt'),'-o',str(work/'invalid.sxi'),'--materialize'],capture_output=True,text=True,env=env)
assert q.returncode and '--materialize requires --agc' in q.stderr
print('PASS duplicate names across samples, invalid/short ranges, invalid sidecar and materialize CLI guard',flush=True)
