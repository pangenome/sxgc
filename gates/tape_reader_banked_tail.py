#!/usr/bin/env python3
"""Execute both production wave predicates on the recorded P1 tail rows.

This is a predicate regression, not a completed SA/container comparison.
"""
import hashlib, json, re, subprocess, tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[1]
source = 'xsa/runtime/tools/binbwt/scatter_sort.cpp'
base = Path('/mnt/nvme2n1/erikg/PILE100-REHEARSAL')
corpus = Path('/mnt/nvme2n1/erikg/pile-head100.txt')
n = corpus.stat().st_size
with corpus.open('rb') as f:
    f.seek(n - 23)
    tail = f.read()
remap = (base / 'remap-256.bin').read_bytes()
waves = []
boundary = []
for line in (base / 'order-tape.log').read_text().splitlines():
    m = re.match(r'SS_WAVE_PLAN wave=(\d+) depth=(\d+) bytes=\[(\d+),(\d+)\)', line)
    if not m: continue
    w,d,lo,hi = map(int,m.groups())
    prefix = (boundary + [0]*d)[:d-1]
    assert (boundary + [0]*d)[:d] == prefix+[lo]
    waves.append((w,d,lo,hi,bytes(prefix)))
    boundary = prefix+[hi]
    while boundary and boundary[-1] == 256:
        boundary.pop()
        if boundary: boundary[-1] += 1
assert len(waves) == 123
old = subprocess.check_output(['git', 'show', '6b87aa1:'+source], cwd=root).decode()
new = (root / source).read_text()
outputs = {}
with tempfile.TemporaryDirectory() as td:
    for name, text in [('baseline', old), ('fixed', new)]:
        matches = re.findall(r'auto wave_match_q = .*?auto wave_match = .*?\n        };', text, re.S)
        if name == 'baseline':
            sym = re.search(r'auto sym_at = .*?\n    };', text, re.S).group().replace('[]', '[&]')
            pred = re.search(r'auto wave_match = .*?\n        };', text, re.S).group()
            matches = [sym + '\n' + pred]
        assert len(matches) == (1 if name == 'baseline' else 2)
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
            code += 'struct Wave { u64 depth; unsigned bLo,bHi; bool sliced=false; u64 posLo=0,posHi=24; const u8* pre=nullptr; };\n'
            for w, d, lo, hi, pre in waves:
                code += '{ Wave W{'+f'{d},{lo},{hi}'+'}; u8 Wpre[]={'+(','.join(map(str,pre)) or '0')+'};\n'
                code += 'W.pre=Wpre;\n' + predicate + '\n'
                code += f'for(u64 p=0;p<=FILE_N;++p) if(wave_match(p)) printf("{w} %llu\\n",(unsigned long long)p); }}\n'
            code += '}\n'
            cpp=Path(td)/'test.cpp'; exe=Path(td)/'test'; cpp.write_text(code)
            subprocess.run(['g++','-std=c++17','-O2',str(cpp),'-o',str(exe)],check=True)
            pairs=[tuple(map(int,l.split())) for l in subprocess.check_output([str(exe)]).decode().splitlines()]
            outputs[f'{name}_{site}']=pairs
assert outputs['fixed_0']==outputs['fixed_1']
a,b=outputs['baseline_0'],outputs['fixed_0']
assert sorted(p for w,p in b)==list(range(24))
from collections import Counter
counts = Counter(p for w,p in a)
changes = [dict(off=n-23+p, baseline=[w for w,q in a if q==p], fixed=[w for w,q in b if q==p]) for p in range(24) if [w for w,q in a if q==p] != [w for w,q in b if q==p]]
print(json.dumps(dict(test='bounded production tail predicates against recorded banked wave plan',
    corpus_bytes=n, tail_rows=24, waves=len(waves), baseline_admissions=len(a), fixed_admissions=len(b),
    duplicate_offsets=[n-23+p for p,c in counts.items() if c>1], changed_assignments=changes,
    verdict='ABSENT' if a==b else 'MANIFESTS',
    banked_source_commit='6b87aa1620505cff02bc93e6c008483d0d895a83', baseline_source_sha256=hashlib.sha256(old.encode()).hexdigest(), fixed_source_sha256=hashlib.sha256(new.encode()).hexdigest(), source_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
    tail_hex=tail.hex(), remap_sha256=hashlib.sha256(remap).hexdigest(),
    plan_sha256=hashlib.sha256((base/'order-tape.log').read_bytes()).hexdigest(),
    limitation='Read-only predicate check; no claim about unrelated banked chi defects; frozen container untouched.'),indent=2))
