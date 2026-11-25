"""Inheritance bounds for a trigger-word parse on a byte-for-byte extension.
This reducer takes counts, not text identities; the real caller authenticates
prefix containment before using it on the two archived censuses.
"""
from collections import Counter

def need(ok,msg):
    if not ok: raise ValueError(msg)

def end_cells(tail):
    need(len(tail)>0,'nonempty EOF word required')
    return Counter(x*257+(1+tail[i+1] if i+1<len(tail) else 0) for i,x in enumerate(tail))

def inheritance(n0,m0,k0,u0,n,m,occ0,desc0,occ,old_eof,new_eof):
    """Two-code cells exclude the global END row. EOF word occurs once.
    For c: duplicated prefix descriptors O0[c]-D0[c] are all ordinary.
    All ordinary keys, lengths and offset cells survive extension. DO NOT
    transfer cells of the obsolete EOF word as lower bounds on the new text.
    """
    need(1<=k0<=m0<=n0<n and m0<=m<=n and k0<=u0<=n0,'prefix dimensions')
    for hist in (occ0,desc0,occ,old_eof,new_eof):
        need(all(type(c) is int and 0<=c<256*257 and type(v) is int and 0<v<=n for c,v in hist.items()),'noncanonical cell')
    e0,e=sum(old_eof.values()),sum(new_eof.values())
    need(1<=e0<=n0 and 1<=e<=n and (sum(occ0.values()),sum(desc0.values()),sum(occ.values()))==(n0,u0,n),'census totals')
    for hist in (occ0,occ,old_eof,new_eof):need(sum(v for c,v in hist.items() if c%257==0)==1,'one last-byte/END cell')
    lo,hi={},{}
    for c in occ0.keys()|occ.keys()|desc0.keys()|old_eof.keys()|new_eof.keys():
        o0,d0,o,old,new=(a.get(c,0) for a in (occ0,desc0,occ,old_eof,new_eof))
        need(old<=d0<=o0 and bool(d0)==bool(o0) and o>=o0-old+new,'inconsistent inherited ordinary coverage')
        l=max(int(o>0),d0-old+new);h=o-(o0-d0)
        need(0<=l<=h<=o and bool(l)==bool(h)==bool(o),'inconsistent target descriptor interval')
        if h:lo[c]=l;hi[c]=h
    kl,kh=k0,m-(m0-k0);ul,uh=sum(lo.values()),sum(hi.values())
    need(kl<=kh<=m and u0-e0+e<=ul<=uh==n-(n0-u0) and kl<=ul and kh<=uh,'invalid vocabulary interval')
    return dict(k_lower=kl,k_upper=kh,U_lower=ul,U_upper=uh,duplicate_occurrences_lower=m0-k0,duplicate_owned_offsets_lower=n0-u0,
                ordinary_inherited_keys=k0-1,ordinary_inherited_distinct_offsets=u0-e0),lo,hi

def price(n,m,b):
    lo=b['U_lower']+16*b['k_lower']-8;hi=b['U_upper']+16*b['k_upper']-8
    return dict(tape_lower=lo,tape_upper=hi,keys_bytes=8*m,inverse_bytes=8*m,triple_logical_upper=hi+16*m,
                prior_triple_upper=n+32*m-8,margin_plus_manifest=(4<<20)+48,
                triple_and_margin_upper=hi+16*m+(4<<20)+48,remaining_vs_2N=2*n-hi-16*m-(4<<20)-48)

def cells(lo,hi,occ,slots):
    need(slots>0 and lo.keys()==hi.keys()==occ.keys(),'cell partition keys')
    groups={s:dict(cells=0,occurrence_rows=0,descriptor_lower=0,descriptor_upper=0) for s in ('whole_cell_fits','split_necessary','unresolved')};hard=[]
    for c in sorted(occ):
        s='whole_cell_fits' if hi[c]<=slots else 'split_necessary' if lo[c]>slots else 'unresolved'
        g=groups[s];g['cells']+=1;g['occurrence_rows']+=occ[c];g['descriptor_lower']+=lo[c];g['descriptor_upper']+=hi[c]
        if s!='whole_cell_fits':hard.append(dict(cell=c,bytes_then_code=divmod(c,257),lower=lo[c],upper=hi[c],rows=occ[c],classification=s))
    return dict(slots=slots,by_class=groups,large_or_unresolved=hard)
