import json,pathlib,time,os
here=pathlib.Path(__file__).resolve().parent
empty=0
while True:
    owned=[]
    for p in pathlib.Path('/proc').glob('[0-9]*'):
        try:
            cmd=(p/'cmdline').read_bytes().replace(b'\0',b' ').decode(errors='replace')
            if str(here/'work/build/rpfbwt-memory') not in cmd or 'g++' in cmd or 'cc1plus' in cmd:continue
            stat={l.split(':')[0]:l.split(':',1)[1].strip() for l in (p/'status').read_text().splitlines() if ':' in l}
            io={l.split(':')[0]:int(l.split(':')[1]) for l in (p/'io').read_text().splitlines()}
            owned.append(dict(pid=int(p.name),rss_kib=int(stat['VmRSS'].split()[0]),hwm_kib=int(stat['VmHWM'].split()[0]),vmsize_kib=int(stat['VmSize'].split()[0]),io=io))
        except (OSError,KeyError):pass
    with (here/'resources.jsonl').open('a') as out:out.write(json.dumps(dict(time=time.time(),processes=owned))+'\n')
    empty = empty+1 if not owned else 0
    if empty >= 4: break
    time.sleep(15)
