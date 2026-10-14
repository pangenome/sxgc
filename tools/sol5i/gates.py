#!/usr/bin/env python3
import json, os, pathlib, subprocess, sys, time
root=pathlib.Path('/mnt/nvme2n1/erikg'); size=sys.argv[1]
binary=pathlib.Path(sys.argv[2]).resolve()
stalled='--stalled' in sys.argv
fixture=root/'wmfix-tiny.txt' if size=='tiny' else root/'extcols-scratch'/f'mergperf-{size}-snap.txt'
chi={'tiny':'7283197','100m':'29716349','1b':'283296933'}[size]
tag=size+('-stalled' if stalled else '')
scratch=root/f'sol5i-gate{tag}'
cmd=[str(binary),'build','--v2-pipeline','--input',str(fixture),'--scratch',str(scratch),'--snap-1e','--chunks','8','--kway','8','--threads','32','--memory-gb','8','--timeout','3600','--chi-expect',chi]
env=dict(os.environ)
if stalled:env.update(CROSS_V2_PIECE_WINDOW='2',CROSS_V2_PRODUCER_DELAY_MS='3',PILE_FINISH_BAND_DELAY_MS='15')
peak=pieces=0; start=time.monotonic(); samples=root/'sol5i-tests'/f'du-{tag}.log'
with (root/f'sol5i-gate{tag}.log').open('w') as log,samples.open('w') as sample:
 p=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT,env=env)
 while p.poll() is None:
  if scratch.exists():
   du=subprocess.run(['du','-sb',str(scratch)],capture_output=True,text=True)
   try:current=int(du.stdout.split()[0]);peak=max(peak,current)
   except (IndexError,ValueError):current=0
   backlog=len(list(scratch.rglob('p-*-0')));pieces=max(pieces,backlog)
   sample.write(f'{time.monotonic()-start:.3f} {current} {backlog}\n');sample.flush()
  time.sleep(.5)
result=dict(command=cmd,environment={k:env[k] for k in ('CROSS_V2_PIECE_WINDOW','CROSS_V2_PRODUCER_DELAY_MS','PILE_FINISH_BAND_DELAY_MS') if k in env},rc=p.returncode,wall=time.monotonic()-start,sampled_peak_bytes=peak,sampled_max_band_pieces=pieces)
if p.returncode==0:
 bank=root/'wmfix-tiny/wmfix-tiny.sxi2' if size=='tiny' else root/'extcols-scratch'/('xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2' if size=='100m' else 'pilev2-g2-1b/frag.sxi2')
 result['cmp_rc']=subprocess.run(['cmp',str(scratch/f'{fixture.stem}.sxi2'),str(bank)]).returncode
 result['remaining_protocol_scratch']=[str(x) for pat in ('*.pieces','*.runcols.idx','runs-*.spool','seam.spool','merge.active') for x in scratch.rglob(pat)]
(root/'sol5i-tests'/f'gate-{tag}.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result));sys.exit(p.returncode or result.get('cmp_rc',0))
