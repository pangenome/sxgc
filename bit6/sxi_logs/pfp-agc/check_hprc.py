import pathlib, subprocess, json, hashlib, time, os
root=pathlib.Path('/tmp/sxgc-laneV'); logs=root/'bit6/sxi_logs/pfp-agc'; work=pathlib.Path('/tmp/pfp-agc-gates/hprc');work.mkdir(exist_ok=True)
archive='/home/erikg/hprcv2/HPRC_r2_assemblies_0.6.1.agc';probe='/tmp/pfp-agc-fork/build/pfp_source_probe';oracle='/tmp/sxi-stream/tools/agc2flat'
rows=[line.rstrip('\n').split('\t') for line in (logs/'hprc-corrected-layout.tsv').open()];total=int(rows[-1][2])+int(rows[-1][3])+1
eligible=[r for r in rows if int(r[3])>=100_000_000]; selected=[]
for i in range(20):
 target=total*i/19
 r=min(eligible,key=lambda r:abs(int(r[2])-target));assert r not in selected;selected.append(r)
with (logs/'hprc-differential-reader.log').open('w') as err:
 p=subprocess.Popen(['/usr/bin/time','-v',probe,'agc',archive,'16','requests','0','emit'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err)
 try:
  results=[]
  for i,(sample,contig,base,length) in enumerate(selected):
   base,length=int(base),int(length);n=100_000_000;forward=(length-n)*((i*7)%20)//20
   (work/'sample.txt').write_text(sample+'\n');control=work/'control.txt'
   cmd=[oracle,archive,'--samples',str(work/'sample.txt'),'--group',contig.rsplit('#',1)[-1],'--band',f'{forward}:{forward+n}','--upper','--sep','1e','-o',str(control)]
   with (logs/f'hprc-band-{i:02}.log').open('w') as f: subprocess.run(cmd,check=True,stdout=f,stderr=f)
   data=control.read_bytes();assert len(data)==n+1 and data[-1]==30,(i,len(data));expected=data[:-1][::-1];del data
   offset=base+length-forward-n
   p.stdin.write(f'{offset} {n}\n'.encode());p.stdin.flush();pos=0;h=hashlib.sha256()
   while pos<n:
    b=p.stdout.read(min(1<<20,n-pos));assert b,(i,pos,p.poll());assert b==expected[pos:pos+len(b)],(i,pos);h.update(b);pos+=len(b)
   results.append(dict(window=i,sample=sample,contig=contig,archive_offset=offset,forward_start=forward,bytes=n,byte_equal=True,sha256=h.hexdigest(),oracle_command=cmd))
   (logs/'hprc-windows.json').write_text(json.dumps(results,indent=2)+'\n');print(f'PASS window {i+1}/20 offset {offset}',flush=True)
   control.unlink();control.with_suffix('.names.tsv').unlink(missing_ok=True);del expected
  p.stdin.close();assert p.wait()==0
 finally:
  if p.poll() is None:p.terminate();p.wait()
