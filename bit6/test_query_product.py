#!/usr/bin/env python3
"""Independent all-occurrence MEM oracle, source battery, and HTTP parity."""
import argparse, gzip, json, pathlib, random, socket, struct, subprocess, tempfile, time, urllib.request, urllib.error
from test_sxi_separator import cyclic_frame
P=argparse.ArgumentParser();P.add_argument('--work',required=True);P.add_argument('--xsa',default='xsa/target/release/xsa');P.add_argument('--writer',default='/tmp/sxi-query-writer');A=P.parse_args()
work=pathlib.Path(A.work);work.mkdir(parents=True,exist_ok=True)
xsa=str(pathlib.Path(A.xsa).absolute())
def run(args,ok=True):
    p=subprocess.run(list(map(str,args)),capture_output=True)
    assert (p.returncode==0)==ok,(args,p.returncode,p.stderr.decode())
    return p.stdout

def fixture(label,records,dna=True,reversed=False):
    text=b'\x1e'.join(seq[::-1] if reversed else seq for _,seq in records)+b'\x1e'
    sa,bwt,runs=cyclic_frame(text);n=len(text);r=len(runs)
    ri=work/(label+'.ri4');head=work/(label+'.heads');names=work/(label+'.names');out=work/(label+'.sxi')
    counts=[bwt.count(c) for c in range(256)];C=[];acc=0
    for c in counts:C.append(acc);acc+=c
    width=(n-1).bit_length() or 1;values=[n-1-v[3] for v in runs];packed=sum(v<<(i*width) for i,v in enumerate(values))
    ri.write_bytes(struct.pack('<IIQQQ256Q',0x52585349,4,n,1,r,*C)+bytes(v[0] for v in runs)+struct.pack(f'<{r}I',*(v[1] for v in runs))+struct.pack('<QB',r*width,width)+packed.to_bytes(((r*width+63)//64)*8,'little'))
    head.write_bytes(struct.pack(f'<{r}Q',*(v[2] for v in runs)))
    rows=[];off=0
    for name,seq in records:rows.append(f'{name}\t{n-1-off-len(seq)}\t{len(seq)}\n');off+=len(seq)+1
    names.write_text(''.join(rows))
    run([A.writer,'--ri4',ri,'--heads',head,'--names',names,'--mode','dna' if dna else 'text','--orientation','reversed' if reversed else 'forward','--output',out])
    return out
RC=bytes.maketrans(b'ACGTRYSWKMBDHVNacgtryswkmbdhvn',b'TGCAYRSWMKVHDBNtgcayrswmkvhdbn')
def brute(records,reads,min_len,dna):
    result=[]
    for name,q in reads:
        for strand,seq in ([('+',q),('-',q.translate(RC)[::-1])] if dna else [('+',q)]):
            for doc,(record,t) in enumerate(records):
                for i in range(len(seq)):
                    pos=t.find(seq[i:i+min_len]) if i+min_len<=len(seq) else -1
                    while pos>=0:
                        if not (i and pos and seq[i-1]==t[pos-1]):
                            length=min_len
                            while i+length<len(seq) and pos+length<len(t) and seq[i+length]==t[pos+length]:length+=1
                            v=dict(read=name,name=record,doc_id=doc,offset=pos,len=length,qstart=i if strand=='+' else len(seq)-i-length)
                            if dna:v['strand']=strand
                            result.append(v)
                        pos=t.find(seq[i:i+min_len],pos+1)
    return result

def canonical(v):return sorted(json.dumps(x,sort_keys=True) for x in v)
def lines(b):return [json.loads(x) for x in b.splitlines()]
def check(label,records,reads,min_len,dna=True,reversed=False,index=None):
    index=index or fixture(label,records,dna,reversed)
    fa=work/(label+'.fa');fa.write_bytes(b''.join(b'>'+name.encode()+b'\n'+seq+b'\n' for name,seq in reads))
    args=[xsa,'mems','--sxi',index,'--reads',fa,'--min-len',min_len,'--mode','dna' if dna else 'text']
    actual=run(args+['-j',3]);assert actual==run(args+['-j',1]),'thread order'
    expected=brute(records,reads,min_len,dna)
    assert canonical(lines(actual))==canonical(expected),(label,canonical(lines(actual))[:15],canonical(expected)[:15])
    (work/(label+'.mems.jsonl')).write_bytes(actual)
    print(json.dumps(dict(test=label,reads=len(reads),mems=len(expected),status='PASS')),flush=True)
    return index,fa,actual
rng=random.Random(466)
records=[('a',b'ACGACGTACG'),('b',b'ACGTTTACG'),('c',b'TTACGAC'),('pal',b'ATATAT')]
reads=[('r'+str(i),bytes(rng.choice(b'ACGTN') for _ in range(rng.randrange(1,18)))) for i in range(50)]+[('exact',records[0][1]),('repeat',b'ACGACG'),('left-shorter',b'TTACGACGT')]
index,fa,expected=check('dna',records,reads,2)
check('reversed',records,reads,2,reversed=True)
check('text',[('doc x',b'abracadabra'),('doc y',b'abra abrac')],[('r',b'abracadabracadabra')],2,dna=False)
check('periodic',[(str(i),b'ACG'*25) for i in range(7)],[('q',b'ACG'*5)],3)
for i in range(12):
    rs=[(str(j),bytes(rng.choice(b'ACGT') for _ in range(rng.randrange(3,32)))) for j in range(3)]
    qs=[(str(j),bytes(rng.choice(b'ACGT') for _ in range(15))) for j in range(5)]
    check('random'+str(i),rs,qs,1)
# Exact locate and MS independently checked at every position, both strands.
for idx, reverse in [(index, False), (work/'reversed.sxi', True)]:
    for seq in [b'ACGACGT', b'NNN', b'TTACGAC', b'ATATAT']:
        got=lines(run([xsa,'query','--sxi',idx,'--pattern',seq.decode()]))
        want=[]
        vectors=[]
        for strand,q in [('+',seq),('-',seq.translate(RC)[::-1])]:
            values=[]
            for i in range(len(q)):
                values.append(max([0]+[length for length in range(1,len(q)-i+1) if any(q[i:i+length] in t for _,t in records)]))
            vectors.append(dict(read='query',strand=strand,ms=values))
            for doc,(name,t) in enumerate(records):
                p=t.find(q)
                while p>=0:
                    want.append(dict(read='query',name=name,doc_id=doc,offset=p,len=len(q),strand=strand));p=t.find(q,p+1)
        assert canonical(got)==canonical(want)
        assert lines(run([xsa,'query','--sxi',idx,'--pattern',seq.decode(),'--ms']))==vectors
        sampled=lines(run([xsa,'query','--sxi',idx,'--pattern',seq.decode(),'--sample','2','--seed','7']))
        assert all(hit in want for hit in sampled) and len(sampled)<=4
assert b'k=4' in run([xsa,'sxi-info',index])
# All original construction battery fixtures; legacy single-record text mode.
for label in ['duplicates-600k','random-4-2k','random-4-20k','random-bin-20k','random-4-200k','satellite-18k','HOR-nested']:
    text=pathlib.Path('/tmp/laneY/bat',label+'.txt').read_bytes()
    qs=[('sample'+str(i),text[pos:pos+40].rstrip(b'\r')) for i,pos in enumerate([5,len(text)//2,len(text)-60])]
    check('battery-'+label,[('text',text)],qs,12,dna=False,index=pathlib.Path('/tmp/laneV/g0-final',label+'.sxi'))
# gzip FASTQ equivalence, malformed input and IUPAC checks.
fq=work/'reads.fq.gz'
with gzip.open(fq,'wb') as f:f.write(b''.join(b'@'+n.encode()+b'\n'+q+b'\n+\n'+b'I'*len(q)+b'\n' for n,q in reads))
assert run([xsa,'mems','--sxi',index,'--reads',fq,'--min-len',2,'-j',2])==expected
run([xsa,'query','--sxi',index,'--pattern','ACGT!'],False)
run([xsa,'query','--sxi',index,'--pattern','ACGT!','--mode','text'])
run([xsa,'query','--sxi',index,'--pattern','ARYSWKMBDHVNaryswkmbdhvn'])
bad=work/'bad.fq';bad.write_bytes(b'@r\nACGT\n+\nIII\n');run([xsa,'mems','--sxi',index,'--reads',bad],False)
# HTTP responses are the exact CLI JSONL wire representation.
with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
log=(work/'server.log').open('wb');server=subprocess.Popen([xsa,'serve','--sxi',str(index),'-j','3','--bind',f'127.0.0.1:{port}'],stderr=log)
try:
    base=f'http://127.0.0.1:{port}'
    for _ in range(300):
        try:stats=urllib.request.urlopen(base+'/stats').read();break
        except OSError:time.sleep(.1)
    else:raise AssertionError('server startup')
    assert json.loads(stats)['k']==len(records)
    seq='ACGACG'
    for endpoint,extra in [('/query',[]),('/ms',['--ms']),('/batch',['--min-len','2'])]:
        kind='mems' if endpoint=='/batch' else 'query'
        v={'reads':[{'name':'query','read':seq}],'min_len':2} if endpoint=='/batch' else {'pattern':seq}
        data=json.dumps(v).encode()
        got=run(['curl','--fail','--silent','-H','Content-Type: application/json','--data-binary',data.decode(),base+endpoint])
        assert got==run([xsa,kind,'--sxi',index,'--pattern',seq,*extra]),endpoint
    try:urllib.request.urlopen(urllib.request.Request(base+'/query',data=b'{"pattern":"!"}'));raise AssertionError('bad query accepted')
    except urllib.error.HTTPError as e:assert e.code==400
    assert json.loads(urllib.request.urlopen(base+'/stats').read())['k']==len(records)
finally:server.terminate();server.wait();log.close()
print('PASS all MEMs brute verified, deterministic threads, gzip/FASTQ, modes, IUPAC, HTTP byte parity and error survival')
