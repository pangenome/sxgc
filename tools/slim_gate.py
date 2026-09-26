#!/usr/bin/env python3
"""Reproducible differential gates. Baselines are read-only external artifacts."""
import argparse, pathlib, subprocess, filecmp
ap=argparse.ArgumentParser();ap.add_argument('--stage',choices=['G0','G1','G3'],required=True);ap.add_argument('--yeast-ri4',default='/tmp/laneY/yp2new.ri4');args=ap.parse_args()
root=pathlib.Path(__file__).resolve().parents[1];work=pathlib.Path('/tmp/laneQ');work.mkdir(exist_ok=True)
bat=pathlib.Path('/tmp/laneY/bat');dump=work/'slim_dump';xsa=root/'xsa/target/release/xsa'
names=['duplicates-600k','random-4-2k','random-4-20k','random-bin-20k','random-4-200k','satellite-18k','HOR-nested','dup+unique-120k']
def run(cmd,log,fail=False):
    print('COMMAND',*map(str,cmd),flush=True)
    with open(work/log,'w') as f:r=subprocess.run(list(map(str,cmd)),stdout=f,stderr=subprocess.STDOUT)
    assert (r.returncode!=0 if fail else r.returncode==0),(cmd,r.returncode,log)
    return (work/log).read_text()
def same(a,b):
    assert filecmp.cmp(a,b,shallow=False),(a,b)
    print('BYTE-IDENTICAL',a,b,flush=True)
if args.stage=='G0':
    for name in names:
        run(['/usr/bin/time','-v',dump,'--slim','--resolve-ri4','--ri4',bat/(name+'.ri4'),'--parse',bat/(name+'_pfp'),'--flat',bat/(name+'.txt'),'-o',work/(name+'.agg'),'-t','8'],name+'.log')
        same(work/(name+'.agg'),bat/(name+'.agg'))
        # Test cache mode separately on every battery text.
        run([dump,'--slim','--resolve-ri4','--dict-stream','--ri4',bat/(name+'.ri4'),'--parse',bat/(name+'_pfp'),'--flat',bat/(name+'.txt'),'-o',work/(name+'.stream.agg'),'-t','8'],name+'.stream.log')
        same(work/(name+'.stream.agg'),bat/(name+'.agg'))
    name='duplicates-600k'
    for mode in ['old','stream']:
        cmd=[xsa,'chi-rspace','--ri4',bat/(name+'.ri4'),'--agg',work/(name+'.agg'),'-o',work/(name+'.'+mode+'.sA')]
        if mode=='stream':cmd+=['--stream-agg']
        run(cmd,'G0.sweep.'+mode+'.log')
    same(work/(name+'.old.sA'),work/(name+'.stream.sA'))
    print('G0 PASS 8/8 (seven named baseline cases plus dup+unique), resident+streamed dict; witness byte identity',flush=True)
elif args.stage=='G1':
    grl=pathlib.Path('/mnt/nvme3n1/erikg/sxgc-yeast/grl')
    run(['/usr/bin/time','-v',dump,'--slim','--resolve-ri4','--ri4',args.yeast_ri4,'--parse',grl/'yeast2_pfp','--flat',grl/'yeast_pfp2.txt','-o',work/'yeast.slim.agg','-t','32'],'yeast.dump.log')
    same(work/'yeast.slim.agg','/tmp/laneY/yeast_pfp2.agg')
    log=run(['/usr/bin/time','-v',xsa,'chi-rspace','--stream-agg','--ri4',args.yeast_ri4,'--agg',work/'yeast.slim.agg','-o',work/'yeast.slim.sA'],'yeast.sweep.log')
    assert 'chi = 85404240 ' in log
    import numpy as np
    a=np.fromfile(work/'yeast.slim.sA',dtype='<u8');b=np.fromfile(grl/'chi_yeast_pfp2.sA',dtype='<u8')
    a.sort();b.sort();assert len(a)==len(b)==85404240 and np.array_equal(a,b)
    print('G1 PASS chi=85404240 numpy sorted-set equality=True',flush=True)
else:
    for name in names:
        common=[dump,'--slim','--resolve-ri4','--tau1','2','--ri4',bat/(name+'.ri4'),'--parse',bat/(name+'_pfp'),'--flat',bat/(name+'.txt'),'-o',work/(name+'.tau2.agg'),'-t','8']
        run(common,name+'.tau2.log');same(work/(name+'.tau2.agg'),bat/(name+'.agg'))
        log=run(common+['--inject-fingerprint-error'],name+'.fault.log',fail=True)
        assert 'FATAL SLIM: fingerprint verification mismatch' in log
        print('G3 FAIL-LOUD verified',name,flush=True)
