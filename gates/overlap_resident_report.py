"""Summarize a completed resident run. Do not turn partial runs into passes."""
import json
from pathlib import Path
import sys


def main(path):
    def load(name): return json.loads((path/name).read_text())
    identity=load('identity.json'); build=load('build.json'); oracle=load('oracle.json'); reserve=load('scratch-plan.json')
    assert identity['verified'] and oracle['rc']==0 and build['rc']=={'resident':0,'sink':0,'finish':0} and not build['stop']
    assert build['counted_sampled_peak']<=reserve['counted_bound']<=reserve['limit']
    phases={}; stream={}
    for line in (path/'resident.log').read_text().splitlines():
        tag,*fields=line.split(); vals=dict(f.split('=',1) for f in fields if '=' in f)
        if tag=='RESIDENT_PHASE': phases[vals['phase']]=vals
        if tag=='RESIDENT_STREAM': stream=vals
    assert len(phases)==8 and int(stream['rows'])==(3_000_000_001 if build.get('profile') in ('packed-3g','packed-3g-bitmap') else 1_000_000_666)
    print('Phase | seconds | peak RSS (GiB) | read GB | write GB')
    print('--- | ---: | ---: | ---: | ---:')
    def row(name,secs,rss,r,w):print(f'{name} | {float(secs):.2f} | {int(rss)/2**30:.3f} | {int(r)/1e9:.3f} | {int(w)/1e9:.3f}')
    packed=build.get('profile') in ('packed-raw','packed-3g','packed-3g-bitmap')
    if packed:
        front=load('front-end.json'); row('lexparse (raw input)',front['wall_s'],front['sampled_peak_rss'],front['process']['read_bytes'],front['process']['write_bytes'])
    for name,v in phases.items(): row(name,v['seconds'],v['peak_rss'],v['read_bytes'],v['write_bytes'])
    for name in ('sink','finish'):
        p=build['process'][name]; row(name+' (concurrent)',build['sampled_wall_s'][name],build['sampled_peak_rss'][name],p['read_bytes'],p['write_bytes'])
    if packed:
        print(f"\nRaw to product: {build['raw_to_product_s']:.3f} s including {front['cold_barrier_s']:.3f} s private cold transition; raw worker IO bytes {build['raw_worker_io']}; raw scratch high-water {build['raw_counted_highwater']} B.")
    label='packed words/IDs' if packed else 'IDs/refs'
    print(f"\nFrom {label} to finished product: {build['wall_s']:.3f} s; includes streaming to oracle; excludes cold copies, post-run certification, and making the lexparse.")
    print('Emit write calls (s): sink',stream['sink_write_s'],'audit',stream['audit_write_s'],'(inside emit elapsed, not additive to it).')
    p=build['process']; print('Worker-attributed physical read/write GB:',*[round(sum(w[k] for w in p.values())/1e9,4) for k in ('read_bytes','write_bytes')]); print('Oracle separately: CPU s',oracle['process']['cpu_s'],'read GB',round(oracle['process']['read_bytes']/1e9,4))
    print('Peak shared GiB:',round(int(build['shared_after']['memory.peak'])/2**30,3)); print('Counted scratch: sampled',build['counted_sampled_peak'],'reserved',reserve['counted_bound'],'limit',reserve['limit'])
    print('Whole private directory (allocated, sampled):',build['whole_dir_sampled_allocated'],'bytes. Includes oracle and output.')
    if 'bitmap_placement' in build:
        print('Bitmap in private tmpfs:',build['bitmap_placement']['sampled_bytes'],'B. Includes bitmap once in counted scratch; all-phase sampled disk allocation',build['raw_dir_sampled_allocated'],'B.')
    print('SHA-256:',identity['sha256'])

if __name__=='__main__': main(Path(sys.argv[1]))
