import os,pathlib,shlex,subprocess
here=pathlib.Path(__file__).resolve().parent
base=next(shlex.split(l) for l in (here/'build.log').read_text().splitlines() if l.startswith('g++ -O3'))
procs=[]
for mode in ['baseline','memory']:
    cmd=[str(here/'dictionary_probe.cpp') if x.endswith('/rpfbwt/rpfbwt.cpp') else x for x in base if not x.endswith('/generated/version.cpp')]
    cmd=[x for x in cmd if mode=='memory' or x!='-DSXI_MEMORY_DICTIONARY']
    cmd[cmd.index('-o')+1]=str(here/'work'/('allocation-probe-'+mode))
    subprocess.run(cmd,check=True)
    f=(here/('allocations-'+mode+'.log')).open('w')
    p=subprocess.Popen(['/usr/bin/time','-v','-o',str(here/('allocations-'+mode+'.time')),cmd[-1],str(here/'work/parse-100000000-w10-p100')],env=dict(os.environ,LD_PRELOAD=str(here/'trace_alloc.so')),stdout=f,stderr=f)
    procs.append((p,f))
for p,f in procs:
    if p.wait():raise RuntimeError('allocation probe failed')
    f.close()
