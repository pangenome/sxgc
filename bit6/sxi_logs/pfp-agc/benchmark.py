import json,pathlib,subprocess,time,os
root=pathlib.Path('/tmp/sxgc-laneV');logs=root/'bit6/sxi_logs/pfp-agc';work=pathlib.Path('/tmp/pfp-agc-gates/bench');work.mkdir(exist_ok=True)
probe='/tmp/pfp-agc-fork/build/pfp_source_probe';agc2flat='/tmp/sxi-stream/tools/agc2flat';archive='/home/erikg/hprcv2/HPRC_r2_assemblies_0.6.1.agc'
windows=json.loads((logs/'hprc-windows.json').read_text());results=[]
def save(): (logs/'throughput.json').write_text(json.dumps(results,indent=2)+'\n')
def measured(cmd,label):
 r=subprocess.run(['/usr/bin/time','-v','-o',str(logs/(label+'.time')),*cmd],check=True,capture_output=True,text=True)
 return json.loads(r.stderr.strip().splitlines()[-1])
# Wait for the full product build to finish before measuring throughput.
while pathlib.Path('/proc/3651427').exists(): time.sleep(5)
for j in [16,48,96]:
 pair=[]
 for mode,path in [('file','/tmp/sxi-stream/yeast/xsa-build-gpvh5hug/collection.txt'),('agc','/home/erikg/yeast/yeast235.agc')]:
  x=measured([probe,mode,path,str(j),'0','3336986759'],f'bench-yeast-{mode}-{j}');x.update(dataset='yeast235',mode=mode,j=j);results.append(x);pair.append(x);save();print(json.dumps(x),flush=True)
 assert pair[0]['hash']==pair[1]['hash']
for j in [16,48,96]:
 p=subprocess.Popen(['/usr/bin/time','-v','-o',str(logs/f'bench-hprc-agc-{j}.time'),probe,'agc',archive,str(j),'requests','0'],stdin=subprocess.PIPE,stderr=subprocess.PIPE,stdout=subprocess.DEVNULL,text=True)
 file_rows=[];agc_rows=[]
 try:
  for window in windows:
   sample=work/'sample.txt';sample.write_text(window['sample']+'\n');control=work/'control.txt';forward=window['forward_start'];n=window['bytes']
   cmd=[agc2flat,archive,'--samples',str(sample),'--group',window['contig'].rsplit('#',1)[-1],'--band',f'{forward}:{forward+n}','--upper','--sep','1e','-o',str(control)]
   subprocess.run(cmd,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
   data=control.read_bytes();assert len(data)==n+1 and data[-1]==30;control.write_bytes(data[:-1][::-1]);del data
   f=measured([probe,'file',str(control),str(j),'0',str(n)],f'bench-hprc-file-{j}-{window["window"]}')
   p.stdin.write(f'{window["archive_offset"]} {n}\n');p.stdin.flush();line=p.stderr.readline();a=json.loads(line);assert a['hash']==f['hash'],(j,window['window'],a,f)
   f['window']=a['window']=window['window'];file_rows.append(f);agc_rows.append(a)
   control.unlink();control.with_suffix('.names.tsv').unlink(missing_ok=True)
  p.stdin.close();assert p.wait()==0
 finally:
  if p.poll() is None:p.terminate();p.wait()
 for mode,rows in [('file',file_rows),('agc',agc_rows)]:
  n=sum(r['bytes'] for r in rows);seconds=sum(r['read_seconds'] for r in rows)
  result=dict(dataset='HPRC-466-20-windows',mode=mode,j=j,bytes=n,read_seconds=seconds,init_seconds=rows[0]['init_seconds'] if mode=='agc' else sum(r['init_seconds'] for r in rows),MB_per_s=n/1e6/seconds,windows=rows)
  results.append(result);save();print(json.dumps({k:v for k,v in result.items() if k!='windows'}),flush=True)
