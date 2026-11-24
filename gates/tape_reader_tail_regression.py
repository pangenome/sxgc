#!/usr/bin/env python3
"""Execute both production wave predicates on the recorded P1 tail rows.

This is a predicate regression, not a completed SA/container comparison.
"""
import hashlib, json, re, subprocess, tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[1]
source = 'xsa/runtime/tools/binbwt/scatter_sort.cpp'
base = Path('/mnt/nvme2n1/erikg/TAPEREADER/p1')
corpus = Path('/mnt/nvme2n1/erikg/KEYRADIX/c3g.txt')
n = corpus.stat().st_size
with corpus.open('rb') as f:
    f.seek(n - 23)
    tail = f.read()
remap = (base / 'remap-256.bin').read_bytes()
waves = []
for line in (base / 'main/stage-0.log').read_text().splitlines():
    m = re.match(r'SS_WAVE_PLAN wave=(\d+) depth=(\d+) bytes=\[(\d+),(\d+)\).* class=(.*)', line)
    if m:
        waves.append((int(m[1]), int(m[2]), int(m[3]), int(m[4]), b'' if m[5] == '<root>' else m[5].encode()))
old = subprocess.check_output(['git', 'show', 'ea90a63:'+source], cwd=root).decode()
new = (root / source).read_text()
outputs = {}
with tempfile.TemporaryDirectory() as td:
    for name, text in [('baseline', old), ('fixed', new)]:
        matches = re.findall(r'auto wave_match_q = .*?auto wave_match = .*?\n        };', text, re.S)
        assert len(matches) == 2
        for site, predicate in enumerate(matches):
            code = '''#include <cstdint>
#include <cstdio>
#include <cstdlib>
using u64=uint64_t; using u8=uint8_t;
void die(const char*) { abort(); }
int main() {
constexpr u64 FILE_N=23, SS_SURVEY_P=128; constexpr u8 SENTINEL_SYMBOL=0;
'''
            code += 'u8 T[]={' + ','.join(map(str, tail)) + '};\n'
            code += 'u8 map[]={' + ','.join(map(str, remap)) + '}; const u8* REMAP=map;\n'
            code += 'struct Wave { u64 depth; unsigned bLo,bHi; bool sliced=false; u64 posLo=0,posHi=24; };\n'
            for w, d, lo, hi, pre in waves:
                code += '{ Wave W{'+f'{d},{lo},{hi}'+'}; u8 Wpre[]={'+(','.join(map(str,pre)) or '0')+'};\n'
                code += predicate + '\n'
                code += f'for(u64 p=0;p<=FILE_N;++p) if(wave_match(p)) printf("{w} %llu\\n",(unsigned long long)p); }}\n'
            code += '}\n'
            cpp=Path(td)/'test.cpp'; exe=Path(td)/'test'; cpp.write_text(code)
            subprocess.run(['g++','-std=c++17','-O2',str(cpp),'-o',str(exe)],check=True)
            pairs=[tuple(map(int,l.split())) for l in subprocess.check_output([str(exe)]).decode().splitlines()]
            outputs[f'{name}_{site}']=pairs
assert outputs['baseline_0']==outputs['baseline_1']
assert outputs['fixed_0']==outputs['fixed_1']
a,b=outputs['baseline_0'],outputs['fixed_0']
assert len(a)==25 and len(b)==24
assert sorted(p for w,p in b)==list(range(24))
assert [w for w,p in a if p==22]==[0,1]
assert [w for w,p in b if p==22]==[1]
# Sort each wave's tail rows by finite suffix, as the producer does.
def stream(pairs):
    return sorted(pairs,key=lambda wp:(wp[0],tail[wp[1]:]))
as_,bs=stream(a),stream(b)
i=next(i for i,(x,y) in enumerate(zip(as_,bs)) if x!=y)
def row(pair):
    w,p=pair
    return dict(wave=w,off=n-23+p,raw_key=tail[p:].hex(),symbol_key=bytes(remap[c] for c in tail[p:]).hex())
print(json.dumps(dict(test='production predicate extraction; both census and tail sites', corpus_bytes=n,
    baseline_commit='ea90a63', baseline_source_sha256=hashlib.sha256(old.encode()).hexdigest(),
    fixed_source_sha256=hashlib.sha256(new.encode()).hexdigest(),tail_rows_expected=24,
    baseline_tail_rows=len(a),fixed_tail_rows=len(b),duplicate=dict(off=n-1,raw_key='20',symbol_key='21',baseline_waves=[0,1],fixed_waves=[1]),
    first_tail_stream_divergence=dict(rank=i,baseline=row(as_[i]),fixed=row(bs[i])),
    limitation='Tail-only stream; full reader stream did not complete. No P1/P2 acceptance.'),indent=2))
