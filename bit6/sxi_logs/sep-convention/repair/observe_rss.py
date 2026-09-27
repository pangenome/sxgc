import json,pathlib,subprocess,time
roots={2429971,2449801}
log=pathlib.Path(__file__).with_name('observed-rss.jsonl')
peak=0
while True:
    table={}
    for line in subprocess.check_output(['ps','-eo','pid=,ppid=,rss='],text=True).splitlines():
        pid,parent,rss=map(int,line.split());table[pid]=(parent,rss)
    selected=roots & table.keys()
    while True:
        expanded=selected|{pid for pid,(parent,_) in table.items() if parent in selected}
        if expanded==selected:break
        selected=expanded
    rss=sum(table[pid][1] for pid in selected);peak=max(peak,rss)
    with log.open('a') as f:f.write(json.dumps(dict(time=time.time(),rss_kib=rss,peak_observed_kib=peak,pids=sorted(selected)))+'\n')
    if not selected:break
    time.sleep(5)
