import subprocess,pathlib,json,hashlib
logs=pathlib.Path('/tmp/sxgc-laneV/bit6/sxi_logs/pfp-agc');count=3336986759;h=hashlib.sha256();seen=0
with (logs/'yeast-reader.time').open('w') as log,open('/tmp/sxi-stream/yeast/xsa-build-gpvh5hug/collection.txt','rb') as control:
 p=subprocess.Popen(['/usr/bin/time','-v','/tmp/pfp-agc-fork/build/pfp_source_probe','agc','/home/erikg/yeast/yeast235.agc','16','0',str(count),'emit'],stdout=subprocess.PIPE,stderr=log)
 try:
  while seen<count:
   b=p.stdout.read(min(1<<20,count-seen));assert b and b==control.read(len(b)),seen;seen+=len(b);h.update(b)
  assert not p.stdout.read(1) and not control.read(1) and p.wait()==0
 finally:
  if p.poll() is None:p.terminate();p.wait()
(logs/'yeast-reader.json').write_text(json.dumps(dict(bytes=seen,byte_equal=True,sha256=h.hexdigest()),indent=2)+'\n')
print('PASS yeast canonical reader: every byte compared',seen)
