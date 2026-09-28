import pathlib,time,json,subprocess
logs=pathlib.Path(__file__).resolve().parent
while True:
    manifest=json.loads((logs/'from-zero-manifest.json').read_text())
    base=manifest['base']
    processes={};roots=set()
    for line in subprocess.check_output(['ps','-eo','pid=,ppid=,rss=,args='],text=True).splitlines():
        fields=line.strip().split(None,3)
        if len(fields)<4:continue
        pid,parent,rss=map(int,fields[:3]);command=fields[3]
        processes[pid]=(parent,rss,command)
        if command.startswith('/tmp/k10-endpoints-debug/tools/xsa build ') and base in command:roots.add(pid)
    selected=set(roots)
    while True:
        more={pid for pid,(parent,_,_) in processes.items() if parent in selected}-selected
        if not more:break
        selected.update(more)
    rows=[dict(pid=pid,rss_kib=processes[pid][1],command=processes[pid][2]) for pid in sorted(selected)]
    with (logs/'from-zero-rss.jsonl').open('a') as f:f.write(json.dumps(dict(time=time.time(),total_rss_kib=sum(r['rss_kib'] for r in rows),processes=rows))+'\n')
    if manifest.get('status')=='PASS' or any(r.get('returncode',0)!=0 for r in manifest['runs']):break
    time.sleep(5)
