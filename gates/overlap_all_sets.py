"""Stream an entire plan from banked input columns in ONE capped producer cgroup.
Triples go only to stdout. Arm directory must be fresh and caller-owned.
"""
import json, pathlib, re, shutil, struct, subprocess, sys, time
D,T=map(pathlib.Path,sys.argv[1:3]); I=D/'in'; n=(I/'corpus').stat().st_size;m=(I/'aligned.u32').stat().st_size//4
cg=pathlib.Path('/sys/fs/cgroup')/pathlib.Path('/proc/self/cgroup').read_text().strip().split('::/')[1]; assert (cg/'memory.max').read_text().strip()==str(192*2**20) and (cg/'memory.swap.max').read_text().strip()=='0'
base=sum((I/f).stat().st_size for f in ['physical.refs','cells.bin','plan','aligned.u32','pos-next.u32x2','pred.u8','loc.u32']); start=time.monotonic(); total=rows=ix=nextwin=peak=0
assert not (D/'boundary').exists(); log=(D/'sets.jsonl').open('x',buffering=1)
def fields(line):return {k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)}
while True:
 spill=D/'spill';side=D/'side';assert not spill.exists() and not side.exists();
 def tool(cmd):return subprocess.check_output(list(map(str,cmd)),text=True).strip()
 a=fields(tool([T/'overlap_macro',I/'corpus',I/'physical.refs',I/'plan',I/'loc.u32',spill,m,ix,'joined']));assert a['first']==nextwin and a['limit']>nextwin and a['peak_counted']==base+a['spill'];
 live=base+a['spill']+2**20 # reserve carried boundary
 b=fields(tool([T/'overlap_list_join',I,spill,side,a['limit']-nextwin,n,live,nextwin]));peak=max(peak,b['peak_counted']);assert peak<=2*n
 with (D/f'emit-{ix}.log').open('x') as el:subprocess.run(list(map(str,[T/'overlap_spill_emit',I,spill,I/'plan',a['limit'],n,side,nextwin,D/'boundary'])),check=True,stderr=el)
 c=fields((D/f'emit-{ix}.log').read_text().splitlines()[-1]);assert c['index']+1==a['limit'];rows+=c['cumulative_rows'];total+=a['reads']+b['reads']+c['reads'];assert total<=500*n,'aggregate read guard';nextwin=a['limit'];ix+=1
 print(json.dumps(dict(set=ix,sets=a['sets'],through=nextwin,rows=rows,wall=round(time.monotonic()-start,3),reads=total,route=a['reads'],join=b['reads'],emit=c['reads'],peak_counted=peak)),file=log)
 shutil.rmtree(spill);shutil.rmtree(side)
 if ix==a['sets']:break
assert rows==n+1 and nextwin==a['total_windows'];print(f'FULL rows={rows} windows={nextwin} sets={ix} counted_peak={peak} shared_peak={(cg/"memory.peak").read_text().strip()} reads={total} wall={time.monotonic()-start:.3f}',file=sys.stderr)
