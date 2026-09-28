import hashlib,json,os,pathlib,resource,struct,subprocess,time
root=pathlib.Path('/tmp/sxgc-laneV');work=pathlib.Path('/tmp/sxi-stream/yeast');work.mkdir(parents=True,exist_ok=True)
logs=root/'bit6/sxi_logs/stream-mode';resource.setrlimit(resource.RLIMIT_AS,(18_000_000_000,18_000_000_000))
env=dict(os.environ,XSA_TOOLS='/tmp/sxi-stream/tools',RAYON_NUM_THREADS='8')
for name,flags in [('yeast235-stream',[]),('yeast235-control',['--materialize'])]:
 cmd=['/tmp/sxi-stream/xsa-target/release/xsa','build','--agc','/home/erikg/yeast/yeast235.agc','-o',str(work/(name+'.sxi')),'--scratch',str(work),'--log-dir',str(logs),'--threads','8','--verify-text-sample','32','--verbose',*flags]
 (logs/(name+'.command.json')).write_text(json.dumps(cmd))
 with (logs/(name+'.log')).open('w') as out:
  start=time.time();p=subprocess.Popen(cmd,stdout=out,stderr=subprocess.STDOUT,env=env)
  peak=0
  while p.poll() is None:
   queue=[p.pid];rss=0
   while queue:
    pid=queue.pop()
    try:
     queue.extend(map(int,pathlib.Path(f'/proc/{pid}/task/{pid}/children').read_text().split()))
     status=pathlib.Path(f'/proc/{pid}/status').read_text()
     rss+=next((int(x.split()[1]) for x in status.splitlines() if x.startswith('VmRSS:')),0)
    except (FileNotFoundError,ProcessLookupError):pass
   peak=max(peak,rss);time.sleep(1)
  result=dict(build=name,returncode=p.returncode,wall_seconds=time.time()-start,tree_peak_rss_kib=peak)
  print(json.dumps(result),flush=True)
  assert p.returncode==0 and peak*1024<20_000_000_000,result
 record=json.loads((logs/(name+'.log')).read_text().splitlines()[-1]);scratch=pathlib.Path(record['work'])
 assert not (scratch/'collection.fifo').exists()
 assert (scratch/'collection.txt').exists()==bool(flags)
# Compare every byte of all five core members with bounded buffers.
results=[]
with (work/'yeast235-stream.sxi').open('rb') as a,(work/'yeast235-control.sxi').open('rb') as b:
 a.seek(64);ad=a.read(240);b.seek(64);bd=b.read(240)
 for i in range(5):
  x=struct.unpack_from('<IIQQQII',ad,i*40);y=struct.unpack_from('<IIQQQII',bd,i*40)
  assert x[:2]+x[3:]==y[:2]+y[3:]
  a.seek(x[2]);b.seek(y[2]);left=x[3];h=hashlib.sha256()
  while left:
   u=a.read(min(left,1<<20));v=b.read(len(u));assert u==v and u;h.update(u);left-=len(u)
  results.append(dict(member=x[0],bytes=x[3],count=x[4],sha256=h.hexdigest(),byte_equal=True))
(logs/'yeast-members.json').write_text(json.dumps(results,indent=2)+'\n')
print('PASS full yeast235 stream/control: all five core members byte-identical',flush=True)
