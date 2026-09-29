#!/usr/bin/env python3
"""Byte oracle for isolated external PFP/r-pfbwt executables.

Requires four explicit executable paths; never builds or changes the baseline.
"""
import argparse
import filecmp
import os
from pathlib import Path
import random
import subprocess
import tempfile

p=argparse.ArgumentParser(description=__doc__)
for name in ('pfp','rpf','baseline-pfp','baseline-rpf'):
    p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args()
bins={k:str(v.resolve()) for k,v in vars(a).items()}
with tempfile.TemporaryDirectory(prefix='external-front-test-') as tmp:
    root=Path(tmp)
    env=dict(os.environ,SXI_SCRATCH_DIRS=tmp,SXI_SA_BLOCK_BYTES='32768',SXI_SA_RUN_BUFFER_BYTES='4096',SXI_DICT_CACHE_BYTES='65536')
    def run(cmd):
        subprocess.run(list(map(str,cmd)),env=env,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    rng=random.Random(819)
    for case,alphabet in enumerate((b'ACGT',b' abcdefghijklmnopqrstuvwxyz0123456789.,:;!?')):
        text=root/f'case-{case}.txt'
        text.write_bytes(bytes(rng.choice(alphabet) for _ in range(200000))+b'\x1e')
        prefixes=[]
        for mode in ('external','baseline'):
            dest=root/f'{case}-{mode}';dest.mkdir();prefix=dest/'parse';prefixes.append(prefix)
            parser=bins['pfp' if mode=='external' else 'baseline_pfp']
            front=bins['rpf' if mode=='external' else 'baseline_rpf']
            run([parser,'-t',text,'-o',prefix,'-w',10,'-p',100,'-j',1,'--tmp-dir',dest])
            run([parser,'-i',str(prefix)+'.parse','-w',5,'-p',11,'-j',1,'--tmp-dir',dest])
            run([front,'--l1-prefix',prefix,'--w1',10,'--w2',5,'--threads',1,'--chunks',1,'--tmp-dir',dest])
        for ext in ('.dict','.parse','.parse.dict','.parse.parse','.rlebwt','.rlebwt.meta','.ssa','.ssa_t'):
            assert filecmp.cmp(str(prefixes[0])+ext,str(prefixes[1])+ext,shallow=False),(case,ext)
    rejected=root/'must-not-exist'
    result=subprocess.run([bins['pfp'],'-t',str(text),'-o',str(rejected),'--output-last'],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    assert result.returncode and b'external parser requires' in result.stdout
    assert not list(root.glob('must-not-exist*'))
    huge_phrase=root/'no-triggers.txt';huge_phrase.write_bytes(b'A'*1024)
    result=subprocess.run([bins['pfp'],'-t',str(huge_phrase),'-o',str(root/'limited'),'-w','10','-p','100'],
        env=dict(env,SXI_MAX_PHRASE_BYTES='128'),stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    assert result.returncode and b'phrase memory budget exceeded' in result.stdout
    assert not list(root.glob('sxi-external-*')),'owned scratch files leaked'
print('EXTERNAL_FRONTEND_PASS cases=2 byte_comparisons=16 small_blocks=32768 unsupported_properties_refused=1 phrase_budget_refused=1 scratch_clean=1')
