import pathlib,json,time,re,sys,os,signal
root=pathlib.Path(sys.argv[1]);stop_marker=len(sys.argv)>2
commands=json.loads((root/'commands.json').read_text());wrapper=next(x['pid'] for x in commands if x['label']=='finish-v2')
children=pathlib.Path(f'/proc/{wrapper}/task/{wrapper}/children').read_text().split();pid=int(children[0])
peak=json.loads((root/'external-peak.json').read_text()) if (root/'external-peak.json').exists() else {};sample=0
while pathlib.Path(f'/proc/{pid}').exists():
 status=pathlib.Path(f'/proc/{pid}/status').read_text();vals={k:int(v) for k,v in re.findall(r'^(VmRSS|VmHWM|RssAnon|RssFile|RssShmem):\s+(\d+)',status,re.M)}
 vals['pid']=pid;vals['time']=time.time()
 if vals['VmRSS']>peak.get('VmRSS',0):peak=vals;(root/'external-peak.json').write_text(json.dumps(peak,indent=2))
 if sample%30==0:
  s=pathlib.Path(f'/proc/{pid}/smaps').read_text();d={};name=''
  for l in s.splitlines():
   if re.match(r'^[0-9a-f]+-',l):name=l.split(maxsplit=5)[-1] if len(l.split(maxsplit=5))==6 else '[anonymous]'
   if l.startswith('Rss:'):d[name]=d.get(name,0)+int(l.split()[1])
  (root/'smaps-latest.json').write_text(json.dumps(sorted(d.items(),key=lambda x:-x[1]),indent=2))
 chis=re.findall(r'chi_so_far=(\d+)',(root/'finish-v2.log').read_text()) if stop_marker else []
 if chis and int(chis[-1])>3000000000:
  # Sample again AFTER observing the qualifying progress receipt.
  status=pathlib.Path(f'/proc/{pid}/status').read_text()
  vals={k:int(v) for k,v in re.findall(r'^(VmRSS|VmHWM|RssAnon|RssFile|RssShmem):\s+(\d+)',status,re.M)}
  vals['pid']=pid;vals['time']=time.time()
  if vals['VmRSS']>peak.get('VmRSS',0):
   peak=vals;(root/'external-peak.json').write_text(json.dumps(peak,indent=2))
  (root/'marker.json').write_text(json.dumps(dict(chi_so_far=int(chis[-1]),status=vals,peak=peak,termination='measurement stopped after chi > 3e9; no completed container'),indent=2))
  for c in commands:
   try:os.kill(c['pid'],signal.SIGTERM)
   except ProcessLookupError:pass
  print('Marker reached; stopped measurement pipeline',flush=True);break
 sample+=1;time.sleep(1)
