"""Post-build checks: alphabet conservation and original-byte MEM oracle."""
from pathlib import Path
import json,struct,subprocess,time
root=Path(__file__).resolve().parents[3];log=root/'bit6/sxi_logs/byte-remap'
source='/home/erikg/sxgc-piletest/pile-frag.txt'
journal=next(log.glob('pile-frag-xsa-build-*.jsonl'))
records=[json.loads(s) for s in journal.read_text().splitlines()]
assert records[-1].get('status')=='PASS', 'pile build has not passed'
work=Path(records[-1]['work']);index=Path(records[-1]['output']);sigma=(work/'parse.remap').read_bytes()
counts=json.loads((log/'alphabet.jsonl').read_text().splitlines()[0])['counts']
expected=[0]*256
for c,n in enumerate(counts):expected[sigma[c]]+=n
with (work/'fresh.ri4').open('rb') as f:
 header=f.read(2080);n=struct.unpack_from('<Q',header,8)[0];C=list(struct.unpack_from('<256Q',header,32))
actual=[b-a for a,b in zip(C,C[1:]+[n])]
assert n==sum(counts) and actual==expected and actual[30]==177753
with index.open('rb') as f:
 h=f.read(64);directory=f.read(40*struct.unpack_from('<I',h,32)[0]);found=False
 for i in range(len(directory)//40):
  member,codec,offset,size,count,crc,reserved=struct.unpack_from('<IIQQQII',directory,40*i)
  if member==7:
   f.seek(offset);assert f.read(size)==sigma and count==256;found=True
 assert found
(log/'pile-alphabet-conservation.json').write_text(json.dumps({'pass':True,'n':n,'separator_count':actual[30],'original_counts':counts,'indexed_counts':actual,'sigma':list(sigma)},indent=2)+'\n')
commands=[]
cmd=[str(root/'xsa/target/release/xsa'),'mems','--sxi',str(index),'--reads',str(log/'pile-patterns.fa'),'--min-len','20','-j','4']
begin=time.monotonic()
with (log/'pile-mems.jsonl').open('wb') as output,(log/'pile-mems.log').open('wb') as stderr:
 r=subprocess.run(cmd,stdout=output,stderr=stderr)
commands.append({'command':cmd,'returncode':r.returncode,'seconds':time.monotonic()-begin});assert r.returncode==0
cmd=['python3',str(root/'tools/pilot_verify.py'),'--text',source,'--patterns',str(log/'pile-patterns.json'),'--occs',str(log/'pile-mems.jsonl'),'--slices',str(log/'pile-slices.json'),'--min-len','20']
r=subprocess.run(cmd,capture_output=True);(log/'pile-verify.log').write_bytes(r.stdout+r.stderr)
commands.append({'command':cmd,'returncode':r.returncode});(log/'pile-query-commands.json').write_text(json.dumps(commands,indent=2)+'\n')
assert r.returncode==0,r.stderr.decode()
print(r.stdout.decode(),end='')
