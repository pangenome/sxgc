"""Tiny materialised oracle ONLY. Literal SA, finite descriptor mass and a
separate document-aware branch counter check the frozen writer/reader dump.
"""
from bisect import bisect_right
from collections import Counter,defaultdict
import json
from pathlib import Path
import random
import sys
from overlap_m20_prefix_cuts import SEP,parse_dump,proof,trigger8
from overlap_prefix_vocab_bounds import need,end_cells,inheritance

SLOTS=80

def input_bytes():
    rnd=random.Random(9445);parts=[b' '*820+b' end', b''.join(b'Qt'+bytes([65+i%24,97+(i*5)%19]) for i in range(270)),
        b'literal\x1econtent '+b' the end '*22,b'',bytes(range(256)),b'the end '*39,rnd.randbytes(159),b' '*80+b'\x00\xff final']
    starts=[];t=bytearray()
    for j,p in enumerate(parts):starts.append(len(t));t.extend(p);t.extend(b'\x1e' if j+1<len(parts) else b'')
    return bytes(t),starts

def finite(t):
    # Complete ordinary records end with an eight-byte overlap. A terminal
    # descriptor shorter than nine codes gets END, never a made-up next byte.
    mask=(1<<64)-1;starts=[0]
    for i in range(1,len(t)-7):
        x=1469598103934665603
        for b in t[i:i+8]:x=(1099511628211*(x^b))&mask
        if x%31==0:starts.append(i)
    words=Counter((False,t[i:j+8]) for i,j in zip(starts,starts[1:]));words[True,t[starts[-1]:]]+=1
    distinct=Counter();weighted=Counter();u=0
    for (e,w),f in words.items():
        for i in range(len(w) if e else len(w)-8):
            if len(w)-i>=9:assert trigger8(w[i+1:i+9])==(not e and len(w)-i==9)
            k=tuple(1+b for b in w[i:i+9]);k=k if len(k)==9 else k+(0,);u+=1
            for j in range(2,len(k)+1):distinct[k[:j]]+=1;weighted[k[:j]]+=f
    two=lambda v:{(k[0]-1)*257+k[1]:x for k,x in v.items() if len(k)==2}
    return (len(t),len(starts),len(words),u),two(weighted),two(distinct),end_cells(t[starts[-1]:]),distinct,weighted

def material(t,starts):
    big=finite(t);sub=finite(t[:933]);_,lo,hi=inheritance(*sub[0],len(t),big[0][1],sub[1],sub[2],big[1],sub[3],big[3]);hard=sorted(k for k,x in hi.items() if x>SLOTS)
    return big,lo,hi,hard

def literal(t,starts):
    first={};mass=Counter();branches=defaultdict(Counter);branches[b''][0]=1
    for r,p in enumerate(sorted(range(len(t)+1),key=lambda i:t[i:])):
        if not r:assert p==len(t);continue
        k=tuple(b+1 for b in t[p:p+9]);k=k if len(k)==9 else k+(0,)
        for j in range(2,len(k)+1):mass[k[:j]]+=1;first.setdefault(k[:j],r)
        idx=bisect_right(starts,p);end=starts[idx] if idx<len(starts) else len(t)
        for j in range(min(8,end-p)+1):branches[t[p:p+j]][1+t[p+j] if p+j<end else 0]+=1
    return first,mass,branches

def check_law():
    # At every possible internal cut either place a boundary after 0x1e,
    # keep a literal 0x1e there, or put a normal byte there. No delimiter scan.
    from itertools import product
    cases=0
    for n in range(1,8):
        for code in product(range(3),repeat=n):
            raw=bytes(65 if x==0 else SEP for x in code);starts=[0]+[i+1 for i,x in enumerate(code[:-1]) if x==2];_,_,cut=literal(raw,starts);_,_,flat=literal(raw,[0]);cases+=1
            assert cut[bytes([SEP])][0]-flat[bytes([SEP])][0]==len(starts)-1
            for p,hist in flat.items():
                if SEP not in p:assert cut[p]==hist
                else:assert sum(hist.values())<=raw.count(SEP)
    return cases

def run(d,setup):
    t,starts=input_bytes();big,lo,hi,hard=material(t,starts)
    if setup:
        d.mkdir(mode=0o700);(d/'toy.raw').write_bytes(t);(d/'toy.starts').write_text(''.join(f'{s}\n' for s in starts));(d/'requests').write_text(''.join(f'{k}\n' for k in hard));return
    raw=(d/'dump').read_bytes();head,nodes=parse_dump(raw);first,mass,ch=literal(t,starts);assert big[5]==mass
    for k,(_,_,edges) in nodes.items():assert {c:v for c,(v,_) in edges.items()}==ch[k]
    def test_leaf(k,r,o,l,h):assert first[k]==r and o==mass[k] and l<=big[4][k]<=h
    result=proof(big[1],lo,hi,len(starts),SLOTS,head,nodes,test_leaf);assert not result['all_prefix_leaves_fit'] and result['parents_now_certified']>=1 and result['residual_raw_rows']>SLOTS and result['raw_owned_rows_accounted']==len(t)
    assert sum(ch[bytes([SEP])].values())>len(starts)-1 and ch[bytes([SEP])][0]==len(starts)-1 # also internal content
    # Actual non-SEP boundary (not merely a wrong doc count) breaks the premise.
    wrong=sorted(starts+[2]);_,_,ch2=literal(t,wrong);assert ch2[bytes([SEP])][0]<len(wrong)-1 and ch2[b' '][0]>ch[b' '].get(0,0)
    for docs,view,cap in [(len(wrong),nodes,SLOTS),(len(starts),{k:v for k,v in nodes.items() if k!=b' '*8},SLOTS),(len(starts),nodes,1)]:
        try:proof(big[1],lo,hi,docs,cap,head,view)
        except ValueError:pass
        else:raise AssertionError('invalid cuts/dump admitted')
    try:parse_dump(raw[:raw.rfind(b'DONE')])
    except ValueError:pass
    else:raise AssertionError('incomplete observation accepted')
    report=dict(n=len(t),docs=len(starts),boundary_law_schedules=check_law(),checked_materialised_nodes=len(nodes),every_leaf_raw_ranks_and_descriptor_bounds=True,refusals=4,result=result)
    with (d/'generated.json').open('x') as f:json.dump(report,f,sort_keys=True,indent=2);f.write('\n')
    print(f'PASS {len(t)} generated bytes, {len(starts)} docs, {len(nodes)} observed node histograms vs literal clipped text; {result["leaf_count"]} true-rank leaves vs raw SA and finite keys; {len(result["residual"])} residual depth-nine cells, four refusals')
if __name__=='__main__':
    d=Path(sys.argv[2]);need(__debug__ and sys.argv[1] in ('setup','check') and d.parent.parent==Path('/tmp') and d.parent.name.startswith('m20-cut-tools-') and d.name=='fixture' and d.parent.resolve()==d.parent,'generated private fixture');run(d,sys.argv[1]=='setup')
