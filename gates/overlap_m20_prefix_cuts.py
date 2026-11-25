"""Offline prospective cuts for *finite* word tails using raw two-code counts and
bounded observations from the frozen document-aware trie. Not an emitter plan.
"""
import hashlib
import struct
from overlap_prefix_vocab_bounds import need

SEP=0x1e

def trigger8(word):
    need(len(word)==8,'wrong trigger width');h=1469598103934665603
    for x in word:h=((h^x)*1099511628211)&((1<<64)-1)
    return h%31==0

def parse_dump(data):
    need(len(data)<=8<<20,'oversized trie observations');ls=iter(data.splitlines());fields=next(ls).split();need(fields[0]==b'M20','missing trie head')
    head={k.decode():int(v) for k,v in (x.split(b'=') for x in fields[1:])};nodes={};ids=set();end=False;edges=0
    for line in ls:
        a=line.split();need(not end and a,'trailing/truncated observations')
        if a[0]==b'DONE':need(list(map(int,a[1:]))==[len(nodes),edges],'partial observer output');end=True;continue
        need(a[0]==b'NODE' and len(a)>=6,'bad node record');p=b'' if a[1]==b'-' else bytes.fromhex(a[1].decode());i,d,n,nc=map(int,a[2:6]);need(len(p)==d<=head['depth'] and p not in nodes and 0<=i<head['nodes'] and i not in ids and len(a)==6+nc and 1<=nc<=257,'node id/depth/width');ids.add(i)
        ch={};total=0;prev=-1
        for item in a[6:]:
            code,c,child=map(int,item.split(b':'));need(prev<code<=256 and 0<c<=n and -1<=child<i,'child shape/order');prev=code;total+=c
            need((child>=0)==(code!=0 and d<head['depth'] and c>=head['support']),'pruned supported child or spurious child');ch[code]=(c,child)
        need(total==n,'incomplete branch histogram');nodes[p]=(i,n,ch);edges+=nc
    need(end and len(nodes)<=4096 and head['version']==3 and head['depth']==8 and head['aware']==1,'wrong/incomplete trie protocol');return head,nodes

def proof(occ,low,high,docs,slots,head,nodes,check_leaf=None):
    n=sum(occ.values());need(0<head['support']<=slots and head['rows']==n+1 and head['corpus']==n and occ.keys()==low.keys()==high.keys() and docs>=1,'counts/profile')
    used=set();expanded={};root_id=head['nodes']-1
    def take(p,i,n):
        need(p in nodes and nodes[p][:2]==(i,n),'missing/wrong node at '+p.hex());used.add(p);return nodes[p][2]
    root=take(b'',root_id,n+1);one=[0]*256
    for k,v in occ.items():
        need(0<=k<256*257 and 0<low[k]<=high[k]<=v,'base cell interval');one[k//257]+=v
    need(root.get(0)==(1,-1) and all(root.get(1+x,(0,-1))[0]==v for x,v in enumerate(one)) and sum(occ.get(x*257,0) for x in range(256))==1,'root vs raw histogram/END')
    def first(x):
        if x not in expanded:
            count,idx=root.get(x+1,(0,-1));need(idx>=0,'missing first-byte node');ch=take(bytes([x]),idx,count);expanded[x]=ch
            dc=[occ.get(x*257+b,0) for b in range(257)];hc=[ch.get(b,(0,-1))[0] for b in range(257)]
            if x==SEP:
                need(all(a>=b for a,b in zip(dc[1:],hc[1:])) and hc[0]-dc[0]==sum(a-b for a,b in zip(dc[1:],hc[1:]))==docs-1,'not all internal cuts follow the framing byte')
            else:need(hc==dc,'unclipped first-byte children disagree with raw scan')
        return expanded[x]
    # The direction matters. Literal 0x1e INSIDE a document remains content.
    # Prove every cut follows 0x1e, not that every 0x1e is a document end.
    first(SEP);need(one[SEP]<slots,'framing-byte mass not below slot cap')
    leaf_sha=hashlib.sha256();unresolved=[];row=1;last=();run_mass=0;packs=0;segments=0;prev_safe=False;total_lo=total_hi=0;depths={};n_leaves=0;split_at={};hard_parents=set();unresolved_parents=set()
    def leaf(key,r,l,h):
        nonlocal row,last,run_mass,packs,segments,prev_safe,total_lo,total_hi,n_leaves
        need(1<=len(key)<=9 and key>last and (not last or key[:len(last)]!=last),'unsorted or shared cut prefix')
        need(0<l<=h<=r and key.count(0)==int(key[-1]==0) and (not key.count(0) or r==l==h==1),'bad finite leaf extent');last=key
        leaf_sha.update(struct.pack('<B'+'H'*len(key)+'4Q',len(key),*key,row,r,l,h));n_leaves+=1
        stats=depths.setdefault(len(key),dict(leaves=0,rows=0));stats['leaves']+=1;stats['rows']+=r;total_lo+=l;total_hi+=h
        if h>slots:
            need(len(key)==9 and key[-1]!=0,'unresolved prematurely');parent=(key[0]-1)*257+key[1];unresolved_parents.add(parent)
            pat=bytes(x-1 for x in key);tie=trigger8(pat[1:])
            unresolved.append(dict(prefix_hex=pat.hex(),first_rank=row,past_rank=row+r,raw_rows=r,descriptor_lower=l,descriptor_upper=h,split_proved_necessary=l>slots,
                trigger_at_one_forces_whole_finite_key=tie,raw_tenth_code_split_would_preserve_ties=not tie));run_mass=0;prev_safe=False
        else:
            if not prev_safe:segments+=1
            if not run_mass or run_mass+h>slots:packs+=1;run_mass=0
            run_mass+=h;prev_safe=True
        if check_leaf:check_leaf(key,row,r,l,h)
        row+=r
    def walk(p,r,l,h,idx):
        key=tuple(1+x for x in p)
        if h<=slots or len(p)==9:leaf(key,r,l,h);return
        need(SEP not in p and idx>=0,'no safe expansion law/node');ch=take(p,idx,r);split_at[len(p)]=split_at.get(len(p),0)+1
        for code,(c,child) in ch.items():
            # The other r-c real rows carry at most r-c distinct descriptors.
            cl=max(1,l-(r-c));cu=min(c,h)
            if code:walk(p+bytes([code-1]),c,cl,cu,child)
            else:leaf(key+(0,),c,cl,cu)
    for k,r in sorted(occ.items()):
        x,y=divmod(k,257);l,h=low[k],high[k]
        if h<=slots:leaf((x+1,y),r,l,h);continue
        need(y and x!=SEP and y-1!=SEP,'heavy cell contains framing or END');hard_parents.add(k);c,child=first(x).get(y,(0,-1));need(c==r,'two-code raw total');walk(bytes([x,y-1]),r,l,h,child)
    need(row==n+1 and used==nodes.keys(),'incomplete lexical coverage or unrequested node');bad=sum(x['raw_rows'] for x in unresolved)
    return dict(all_prefix_leaves_fit=not unresolved,slots=slots,n=n,global_END_rank_separate=0,raw_owned_rows_accounted=row-1,base_uncertified_parents=len(hard_parents),parents_now_certified=len(hard_parents-unresolved_parents),parents_with_residual=sorted(unresolved_parents),root_framing_bytes=one[SEP],proved_internal_cuts_after_framing_byte=docs-1,
        examined_nodes=len(used),expanded_at_depth=split_at,leaf_depths=depths,leaf_count=n_leaves,leaf_sha256=leaf_sha.hexdigest(),sum_leaf_lower=total_lo,sum_leaf_upper=total_hi,raw_rows_in_certified_leaves=n-bad,residual_raw_rows=bad,residual=unresolved,
        certified_contiguous_segments=segments,greedy_packs_across_only_consecutive_certified_leaves=packs,complete_range_count_if_certified=None if unresolved else packs)
