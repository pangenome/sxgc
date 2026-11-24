"""Rung defaults selected from replayable finished products, not host core count.
`record` prints the deterministic decision to bank. `plan` and `run` additionally
require that decision in the repo. There is deliberately no 100 GB product mode.
"""
import argparse
import contextlib
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess as sp
import sys
import tempfile
import time

from overlap_published import published_main
from overlap_resident_gate import ONE,TEN,REPO
from overlap_scaling_policy import evaluate,experiment_order,one_series
from overlap_three_score import binaries
from overlap_timing_idle import exclusive

BANK=REPO/'gates/receipts'
FOLDERS={'1g':'radix-full-1g','10g':'scale-10g'}

def decision(rung):
    if rung=='100g':raise ValueError('100 GB product HOLD: distinct v10 contract, bounded working set, tested default, time and scratch admission required')
    pair=BANK/FOLDERS['1g'];d=BANK/FOLDERS[rung]
    if rung=='1g':
        lane,writer,stats=one_series(pair)
        # Keep the independent performance verdicts: batching need not buy 5%
        # more end-to-end speed to remove millions of writes for the same bytes.
        allowed=[0]
        if lane['matched_speed_io_pass']:
            allowed.append(1)
            if writer['matched_batch_pass']:allowed.append(2)
        fastest=min(stats[j][-1]['raw'] for j in allowed)
        near=[j for j in allowed if stats[j][-1]['raw']<=1.03*fastest]
        j=min(near,key=lambda j:(stats[j][0]['process']['sink']['syscw'],stats[j][-1]['raw']))
        b=stats[j][0];p=pair/('serial','parallel','batch')[j];cfg=ONE
        note='best verified raw-product profile here; within 3% prefer fewer sink writes'
    else:
        grid=BANK/'native-grid-10g';control=BANK/'packed-serial-10g'
        order=experiment_order(pair,grid);report=evaluate(control,pair,grid,[d/name for name in order]);profile=report['selected']
        if not profile:raise ValueError('no qualified complete 10 GB lane sweep')
        p=d/profile;b=json.loads((p/'build.json').read_text());cfg=TEN
        note='complete 1/2/4 sweep: within 3% of fastest pick lower shared CPU, then fewer lanes'
    source=(p/'binaries.sha').read_text().splitlines()[0]
    return dict(rung=rung,profile=b['profile'],reference_sha256=cfg.sha,worker_sha256=binaries(p),
                harness_revision=source,worker_revision=(p/'build-source.log').read_text().splitlines()[0] if rung=='10g' else source,
                raw_s=b['raw_to_product_s'],prepared_s=b['wall_s'],shared_gib=cfg.shared_gib,arrays_gib=cfg.arrays_gib,
                build_cpus=4,finish_threads=2,verifier_mib=cfg.oracle_mib,verifier_cpus=1,swap_max=0,
                counted_peak=b['raw_counted_highwater'],counted_limit=2*cfg.n,selection=note,
                normalized_speed_target_met=report['performance_target_met'] if rung=='10g' else None)

def checkout(dest,rev):
    sp.run(['git','merge-base','--is-ancestor',rev,'HEAD'],cwd=REPO,check=True)
    sp.run(['git','clone','--quiet','--local','--shared','--no-checkout',REPO,dest],check=True)
    def cmd(*args):sp.run(['git','-C',dest,*args],check=True)
    cmd('sparse-checkout','set','gates','xsa');cmd('checkout','--quiet','--detach',rev)

@contextlib.contextmanager
def timing_lease():
    with (Path(os.environ['XDG_RUNTIME_DIR'])/'overlap-ten-timing.lock').open('a') as f:
        fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB);exclusive();yield

def run(v,deadline):
    rev=published_main(REPO);print('DEFAULT_PUBLISHED',rev,flush=True)
    avail=int(next(l.split()[1] for l in Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')))*1024
    if avail<(v['shared_gib']+2)*2**30+(v['verifier_mib']<<20) or shutil.disk_usage(ONE.anchor.parent).free<6*v['counted_limit']:
        raise ValueError('insufficient host headroom for fixed default grant and private copies')
    tmp=Path(tempfile.mkdtemp(prefix='overlap-default-',dir='/tmp'));h=tmp/'harness'
    with timing_lease():
        checkout(h,v['harness_revision']);env={k:value for k,value in os.environ.items() if k not in ('PYTHONOPTIMIZE','CARGO_TARGET_DIR','RUSTFLAGS','CFLAGS','CC','LD_PRELOAD','CARGO_ENCODED_RUSTFLAGS') and not k.startswith(('XSA_','OMP_','GOMP_','SS_','CROSS_','PFCK_','TRIE_'))};env['CARGO_NET_OFFLINE']='true'
        if v['rung']=='1g':
            if deadline is not None:raise ValueError('1 GB default uses its original cooperative time/read guards, not a custom deadline')
            return sp.call(['bash','gates/overlap_resident_gate.sh',v['profile']],cwd=h,env=env)
        required=int(1.15*v['raw_s']+480);deadline=deadline if deadline is not None else int(time.time())+max(1800,required+600)
        if deadline-time.time()<required:raise ValueError('not enough time for measured 10 GB default')
        worker=tmp/'worker';tools=tmp/'tools';checkout(worker,v['worker_revision'])
        sp.run(['bash','gates/overlap_ten_product.sh','--prepare-tools',worker,tools],cwd=h,check=True,env=env,timeout=max(1,deadline-time.time()-required+300))
        from overlap_ten_resume import observed_profile, sha
        # Do not silently run an unmeasured worker after a compiler/toolchain change.
        for name in ('overlap_resident','pfp_sort_r','lexparse','oracle','monitor.so','bitmap.so'):
            if sha(tools/name)!=v['worker_sha256'][name]:raise ValueError('rebuilt tool differs from measured default: '+name)
    # Same measured profile launcher takes the nonblocking lease. Retain its exit
    # even if its instant /proc read raced exec; require an actual independent
    # stable child-env observation and *all* product/resource checks on this run.
    if published_main(REPO)!=rev:raise ValueError('published selection moved during preparation')
    out=tmp/'postcheck';out.mkdir();arm=observed_profile(h,worker,tools,v['profile'],deadline,out,required,env)
    print('DEFAULT_REPLAY_VERIFIED',arm,flush=True);return 0

if __name__=='__main__':
    a=argparse.ArgumentParser(description=__doc__);a.add_argument('action',choices=['plan','run','record']);a.add_argument('rung',choices=['1g','10g','100g']);a.add_argument('--deadline',type=int);args=a.parse_args()
    try:
        if sys.flags.optimize:raise ValueError('refuse disabled evidence assertions')
        v=decision(args.rung)
        if args.action!='record' and json.loads((BANK/FOLDERS[args.rung]/'default.json').read_text())!=v:
            raise ValueError('published decision does not replay')
        print(json.dumps(v,indent=2,sort_keys=True),flush=True)
        if args.action=='run':sys.exit(run(v,args.deadline))
    except (OSError,ValueError,AssertionError,RuntimeError,sp.SubprocessError) as e:
        print('OVERLAP_DEFAULT_HOLD',str(e) or type(e).__name__,file=sys.stderr);sys.exit(75)
