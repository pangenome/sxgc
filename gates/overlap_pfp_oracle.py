#!/usr/bin/env python3
"""Small construction oracle, not a production parser. No suffix landing/merge.
Sort finite dictionary keys; after that, row ordering uses only integer ranks.
Byte suffixes in the independent brute oracle are never passed to the builder.
"""
import argparse
from collections import defaultdict, deque
from itertools import product
from pathlib import Path
import random


def sa_ids(ids):  # suffix ranks on the integer parse, prefix doubling
    sa = list(range(len(ids))); r = list(ids); k = 1
    while k < len(ids):
        keys = [(v, r[i+k] if i+k < len(r) else -1) for i,v in enumerate(r)]
        sa.sort(key=keys.__getitem__); nxt = [0]*len(r)
        for j in range(1,len(sa)): nxt[sa[j]] = nxt[sa[j-1]] + (keys[sa[j]] != keys[sa[j-1]])
        r = nxt; k *= 2
        if r[sa[-1]] == len(sa)-1: break
    return sa


def common(a,b):
    i=0
    while i<min(len(a),len(b)) and a[i]==b[i]: i+=1
    return i


def anchors(text,w,mod,chunk):
    # Hash only genuine w-grams. Trigger decision is independent of phrase start,
    # chunk boundaries and past trigger positions (including overlapping triggers).
    recent=deque(maxlen=w); starts=[]; i=0
    for base in range(0,len(text),chunk):
        for byte in text[base:base+chunk]:
            recent.append(byte)
            if len(recent)==w:
                h=1469598103934665603
                for c in recent: h=((h^c)*1099511628211)&0xffffffffffffffff
                pos=i-w+1
                if pos>0 and h%mod==0: starts.append(pos)
            i+=1
    return [0]+starts


def build(text,w,mod,chunk):
    # Each tile [a,b) owns those positions only. Its stored phrase reaches b+w
    # when b is a real anchor. The last phrase includes a unique minimal EOF.
    n=len(text); starts=anchors(text,w,mod,chunk); t=tuple(text)+(-1,)
    phrase=[t[a:starts[j+1]+w] if j+1<len(starts) else t[a:] for j,a in enumerate(starts)]
    tiles=[b-a for a,b in zip(starts,starts[1:]+[n])]
    words=sorted(set(phrase)); wid={s:i for i,s in enumerate(words)}; pa=[wid[s] for s in phrase]
    psa=sa_ids(pa); rank={p:r for r,p in enumerate(psa)}
    # Enumerate dictionary offsets once, not full text suffixes. Descriptors
    # sharing a representative need only the continuation's integer rank.
    width={}
    for p,d in zip(pa,tiles):
        if p in width: assert width[p]==d
        width[p]=d
    buckets=defaultdict(list)
    for p,word in enumerate(words):
        for off in range(width[p]): buckets[word[off:]].append((p,off))
    keys=sorted(buckets)
    for a,b in zip(keys,keys[1:]): assert b[:len(a)]!=a, ('representative prefix',a,b)
    occurrences=defaultdict(list)
    for j,p in enumerate(pa): occurrences[p].append(j)
    out=[n]; lcp=[0]; prev=None; tied=0
    # LCP oracle implementation over IDs: skip matching whole tiles, then
    # compare the first differing dictionary phrases. Deliberately not an RMQ.
    def parse_lcp(i,j):
        h=0
        while pa[i]==pa[j]: h+=width[pa[i]]; i+=1; j+=1
        return h+common(words[pa[i]],words[pa[j]])
    for key in keys:
        group=[]
        for p,off in buckets[key]:
            for j in occurrences[p]: group.append((rank.get(j+1,-1),starts[j]+off,j))
        group.sort(key=lambda row:row[0])
        for q,(_,pos,j) in enumerate(group):
            if prev is None: h=0
            elif q:
                assert key[-1]!=-1 and j!=prev_j
                h=len(key)-w+parse_lcp(prev_j+1,j+1); tied+=1
            else: h=common(prev,key)
            out.append(pos); lcp.append(h); prev=key; prev_j=j
    assert sorted(out)==list(range(n+1))
    return out,lcp,(len(starts),len(words),len(keys),tied,max(tiles))


def check(text,w,mod,chunk):
    a,l,counts=build(text,w,mod,chunk)
    expected=sorted(range(len(text)+1),key=lambda i:text[i:]); assert a==expected
    assert l==[0]+[common(text[i:],text[j:]) for i,j in zip(expected,expected[1:])]
    return counts


def main():
    p=argparse.ArgumentParser(); p.add_argument('--corpus',type=Path); args=p.parse_args(); cases=0
    for n in range(9):
        for seq in product(range(2),repeat=n):
            for w,mod in [(2,1),(2,3),(3,5),(4,17)]:
                check(bytes(seq),w,mod,3); cases+=1
    # Counterexample: cutting every two bytes isn't an ordering boundary even
    # if we append the next w=2 bytes. The tail at offset 1 in [0,2) is
    # 'aaa'; its sibling at offset 0 is 'aaaa'. Do NOT claim an arbitrary
    # hard cap is safe. Such chunks must stay physical storage cuts only.
    artificial=b'aaaaaaaa'[:2+2]; assert artificial.startswith(artificial[1:])
    rng=random.Random(24)
    fixtures={'empty':b'', 'zero_ff':bytes(range(256))*2, 'period5':b'abcde'*90,
              'long_no_cut':b'A'*511,'repeats':(b'banana_bandana_'*25+b'x')*4,
              'random':rng.randbytes(2048)}
    if args.corpus:
        with args.corpus.open('rb') as f: fixtures['corpus_head4096']=f.read(4096)
    for name,t in fixtures.items():
        c=check(t,4,17,13);assert anchors(t,4,17,13)==anchors(t,4,17,max(1,len(t)))
        print(name,'bytes',len(t),'parse/distinct/representatives/tied_rows/max_tile',c)
        if name=='repeats': assert c[3]>len(t)//2  # exercise the integer continuation tie path
        if name=='long_no_cut': assert c[-1]==len(t)  # multiple physical chunks, no fake anchors
    print(f'PASS {cases} exhaustive cases and {len(fixtures)} fixtures: permutation, SA and every adjacent LCP including EOF')
if __name__=='__main__': main()
