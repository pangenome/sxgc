"""Generated exact-word extension and EOF-changing counterexamples; no real input."""
from collections import Counter
from itertools import product
from random import Random
from overlap_prefix_vocab_bounds import cells, end_cells, inheritance, price

def actual(t,w,trigger):
    start=[0]+[i for i in range(1,len(t)-w+1) if trigger(t[i:i+w])];freq=Counter()
    for j,i in enumerate(start):
        end=j+1==len(start);key=t[i:] if end else t[i:start[j+1]+w];freq[end,key]+=1
    desc=Counter();obs=Counter();finite=Counter()
    for (end,key),f in freq.items():
        for o in range(len(key)-(0 if end else w)):
            c=key[o]*257+(key[o+1]+1 if o+1<len(key) else 0);desc[c]+=1;obs[c]+=f;finite[(end,key[o:])]+=1
    # Independent literal offset coverage, no grouping by word.
    direct=Counter(t[j]*257+(t[j+1]+1 if j+1<len(t) else 0) for j in range(len(t)))
    assert direct==obs
    return (len(t),len(start),len(freq),sum(desc.values())),direct,desc,end_cells(t[start[-1]:]),finite

def compare(a,b,slots):
    (n,m,k,u),occ,d,e,g=b;v,lo,hi=inheritance(*a[0],n,m,a[1],a[2],occ,a[3],e);z=price(n,m,v)
    assert v['k_lower']<=k<=v['k_upper'] and v['U_lower']<=u<=v['U_upper'] and all(lo[c]<=d[c]<=hi[c] for c in occ)
    assert z['tape_lower']<=u+16*k-8<=z['tape_upper'] and len(g)>=len(a[4])-sum(a[3].values()) and max(g.values())>=max(a[4].values())-1
    for h in cells(lo,hi,occ,slots)['large_or_unresolved']:
        if h['classification']=='split_necessary':assert d[h['cell']]>slots
    assert all(d[c]<=slots for c in occ if hi[c]<=slots)
    return v

def main():
    ncase=0;correction=False;repeats=0
    for f in (lambda x:True,lambda x:x==bytes(2),lambda x:x[0]!=x[1],lambda x:False):
        for n in range(2,10):
            for word in product((0,255),repeat=n):
                t=bytes(word);b=actual(t,2,f)
                for i in range(1,n):
                    a=actual(t[:i],2,f);v=compare(a,b,2);ncase+=1;repeats+=v['duplicate_owned_offsets_lower']>0
                    correction|=any(a[2][c]>b[2][c] for c in a[2])
    assert correction and repeats
    def trigger(q):
        h=1469598103934665603
        for x in q:h=((h^x)*1099511628211)&((1<<64)-1)
        return h%31==0
    r=Random(40911)
    for _ in range(120):
        s=r.randbytes(r.randrange(16,80));t=s*r.randrange(2,12)+b'\x00';i=r.randrange(1,len(t));compare(actual(t[:i],8,trigger),actual(t,8,trigger),5);ncase+=1
    a=actual(b'\xff'*12,2,lambda q:True);b=actual(b'\x00'*19,2,lambda q:True)
    try:compare(a,b,10)
    except ValueError:pass
    else:raise AssertionError('incompatible cells not refused')
    from io import BytesIO
    from overlap_target_prefix_certificate import certify, read_meta, sha
    raw=b'abcdefhijklmn\x00new data';p=13;noop=lambda:None
    for chunk in range(1,16):
        f=BytesIO(raw);assert certify(f,p,5,sha(raw[:p]),noop,chunk)==raw[p-5:p] and f.tell()==p
    for text,h in ((raw,sha(raw)),(raw[:p-1],sha(raw[:p]))):
        try:certify(BytesIO(text),p,5,h,noop,2)
        except ValueError:pass
        else:raise AssertionError('short/modified prefix accepted')
    calls=0
    def stop():
        nonlocal calls
        calls+=1
        if calls>2:raise ValueError('test STOP')
    f=BytesIO(raw)
    try:certify(f,p,5,sha(raw[:p]),stop,2)
    except ValueError as e:assert str(e)=='test STOP' and f.tell()==4
    else:raise AssertionError('ignored cancellation')
    meta=read_meta();assert meta[0]==(10000000000,318164488,234472723,9129290112) # archived receipts only
    print(f'PASS {ncase} prefix-extension schedules incl rolling w8, EOF replacement, duplicate ordinary offsets and cell/finite-group bounds; naive EOF transfer falsified, inconsistent coverage refused')
if __name__=='__main__':main()
