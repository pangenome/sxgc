#!/usr/bin/env python3
import json,pathlib,subprocess,time,os,stat
logs=pathlib.Path(__file__).resolve().parent
root_pid=3696204
work=pathlib.Path('/tmp/pfp-agc-gates/xsa-build-5najb_ty')
maximum=0;samples=0
while pathlib.Path(f'/proc/{root_pid}').exists():
    rows=subprocess.check_output(['ps','-eo','pid=,ppid=,rss='],text=True).splitlines()
    processes=[tuple(map(int,r.split())) for r in rows]
    tree={root_pid}
    while True:
        before=len(tree);tree.update(pid for pid,ppid,rss in processes if ppid in tree)
        if len(tree)==before:break
    rss=sum(rss for pid,ppid,rss in processes if pid in tree)
    maximum=max(maximum,rss);samples+=1
    time.sleep(2)
files=[]
for f in sorted(work.rglob('*')):
    st=f.lstat();files.append(dict(path=str(f),bytes=st.st_size,regular=stat.S_ISREG(st.st_mode),fifo=stat.S_ISFIFO(st.st_mode),symlink=f.is_symlink()))
assert not (work/'collection.txt').exists()
assert not any(f['fifo'] for f in files)
report=dict(work=str(work),files=files,no_collection_text=True,no_fifo=True,observed_tree_peak_rss_kib=maximum,rss_samples=samples,memory_note='Monitor started during BWT; first parse RSS is separately recorded in its stage timing file',write_policy='Only expanded collection text prohibited; r-space outputs unrestricted')
(logs/'publication-hygiene.json').write_text(json.dumps(report,indent=2)+'\n')
subprocess.run(['python3',str(logs/'compare_published.py')],check=True,stdout=(logs/'yeast-published-members.log').open('w'))
print('PASS: published core byte comparison and scratch hygiene')
