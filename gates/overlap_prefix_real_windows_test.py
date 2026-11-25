"""Fixed-profile score and sparse independent anchor refusals; no real inputs."""
import os
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from overlap_prefix_real_windows import N, W, footprint, rank, score

def fixture(saved):
    lines=[];starts=[];m11=[]
    def emit(tag,**v):lines.append(tag+' '+' '.join(f'{k}={x}' for k,x in v.items()))
    for i in range(16):
        first=rank(i);emit('COVER',window=i,first=first,count=W,base=first-1,extent=W+1,key_lo=2*i,key_hi=2*i+1)
        for j in (0,1100,3200,5100,W-1):
            r=first+j;k=len(starts);starts.append(r);m11.append(k);emit('ANCHOR',window=i,block=k,rank=r,tail=r,pos=k,complete=int(j!=W-1))
        for half in range(2):
            v=dict(window=i,half=half,edges=W//2,prefix=4000,escapes=1,ge1024=1,ge4096=0,eligible_bytes=2000,max_h=2000,start_pages=200,far_pairs=70)
            for sz in ('small','large'):
                v.update({sz+'_compared':4000-saved,sz+'_skipped':saved,sz+'_lookups':1,sz+'_hits':0,sz+'_evicted':0})
            emit('PHASE',**v)
    emit('WINDOWS_PASS',sampled_edges=16*W,sorted_cover_positions=16*(W+1),reported_anchor_starts=80,large_table=397312,small_table=24832,sort_memcmp_request=3000,lcp_memcmp_request=9999);emit('RESOURCE',read=0,peak=1)
    return '\n'.join(lines),starts,m11

def main():
    log,st,m=fixture(1000);z=score(log,st,m);assert z['signal'] and (z['anchor_complete'],z['anchor_truncated'])==(64,16) and not score(*fixture(900))['signal']
    for a in (log.replace('pos=0 complete=1','pos=1 complete=1',1), log.replace('block=2 ','block=3 ',1),log.replace('complete=0','complete=1',1),log.replace('small_compared=3000','small_compared=3001',1),log.replace('COVER window=1 ','COVER window=0 ',1),log.replace('RESOURCE','alien',1)):
        try:score(a,st,m)
        except ValueError:continue
        raise AssertionError('corrupt cross-check or statistics accepted')
    with TemporaryDirectory(prefix='rank-window-meter-') as tmp:
        d=Path(tmp);own={'self':SimpleNamespace(pid=os.getpid(),returncode=None,poll=lambda:None)}
        with (d/'text').open('xb') as f:f.truncate(N) # generated sparse extent, no corpus
        (d/'nested').mkdir();sub=d/'nested'/'one';sub.write_bytes(b'abc');os.link(sub,d/'alias')
        v=footprint(d,own);assert v['aux_logical']==3 and v['duplicate_names_or_fds']>=1
        with (d/'unlinked').open('xb') as f:
            f.write(b'123');f.flush();(d/'unlinked').unlink()
            try:footprint(d,own)
            except ValueError:pass
            else:raise AssertionError('credited open deleted data')
    print('PASS predeclared signal, six rank/anchor/byte-count/record refusals, nested/dedup and open-unlinked refusal')
if __name__=='__main__':main()
