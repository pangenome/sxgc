"""No corpus. Model independent producers/consumers with finite credits, including EOF.
The producer alone runs forward in row order; a blocked next piece cannot block
an already-dispatched LCP reader. No synthesized release/consumed credit.
"""
from itertools import accumulate
import json
import struct
import subprocess as sp
import sys
from overlap_finish_credits import HERE, REPO, ceil, credits, flushed, releases, review


def geometry(lengths, piece):
    cuts=[0,*accumulate(lengths)]; rows=cuts[-1]
    starts=cuts[:-1:piece]+[rows]; copied=[cuts[max(0,i-2)] for i in range(0,len(lengths),piece)]
    return starts,copied


def finishes(lengths,piece,slice_rows,wp,ws):
    # Generate actual single-writer events row by row. A piece comes out on
    # a change of symbol, not when the last row of the OLD run first arrives.
    events=[]; ranks=[0]; rows=runs=0
    for length in lengths:
        if runs and runs%piece==0:events.append('p')
        runs+=1
        for _ in range(length):
            rows+=1
            if rows%slice_rows==0:events.append('s')
        ranks.append(rows)
    if rows%slice_rows:events.append('s')
    events.append('p')
    made_p=made_s=used_p=used_s=i=0
    while True:
        old=(i,used_p,used_s)
        while i<len(events):
            if events[i]=='p':
                if made_p-used_p>=wp:break
                made_p+=1
            else:
                if made_s-used_s>=ws:break
                made_s+=1
            i+=1
        # Serial finish can make progress without relying on prefetch. Copy
        # each available SXLS and grant disk credit before awaiting the next.
        advance=max(0,min(used_p*piece,runs)-2)
        if i==len(events) and used_p==made_p:advance=runs
        want=ceil(ranks[advance],slice_rows)
        used_s=min(made_s,want)
        if used_s==want and used_p<made_p:used_p+=1
        if i==len(events) and used_p==made_p and used_s==made_s:return True
        if old==(i,used_p,used_s):return False


def reject(fn):
    try:fn()
    except ValueError:return
    raise AssertionError('bad evidence accepted')


def main():
    tests=stalls=0
    for n in range(1,11):
        for mask in range(1<<(n-1)):
            starts=[0]+[i for i in range(1,n) if mask>>(i-1)&1]+[n]; lengths=[b-a for a,b in zip(starts,starts[1:])]
            for piece in (1,2,4):
                ends,copied=geometry(lengths,piece)
                for size in (1,2,3,6):
                    c=credits(n,ends,copied,size); p=c['piece_sufficient']; s=c['slice_sufficient']
                    assert finishes(lengths,piece,size,p,s)
                    if c['slice_necessary']>1:
                        assert not finishes(lengths,piece,size,100,c['slice_necessary']-1);stalls+=1
                    tests+=1
    # At eight rows the slice is ready BEFORE the lookahead cuts piece 3.
    # At nine the same one-piece credit strands it behind that cut.
    assert finishes([1]*16,2,8,1,12) and not finishes([1]*16,2,9,1,12)
    assert not finishes([16],4,2,12,1)
    reject(lambda:credits(10,[0,7,4,10],[0,0,0],3))
    cap=4<<20; blocks=[cap-8,16,8,cap-8]; directory=b''.join(struct.pack('<QII',sum(blocks[:i]),v,0) for i,v in enumerate(blocks))
    assert flushed(directory,2*cap+8,4096)==[[2,cap+8],[4,2*cap+8]]
    reject(lambda:flushed(directory[16:],2*cap+8,3072))
    reject(lambda:releases('PIECE_ABSORBED idx=0 container_off=12 bytes=44',10,7))
    e=json.loads((HERE/'reduction.json').read_text()); r=review(e)
    assert r['historical_joint_lower']==219796260 and r['historical_floor']['bytes']==123611895
    assert r['credits']['slice_necessary']==r['credits']['slice_sufficient']==4
    assert [v['slack_vs_held']>0 for v in r['models']]==[False,False,True]
    z=sp.run([sys.executable,'-BE',REPO/'gates/overlap_product.py'],capture_output=True)
    assert z.returncode==75 and b'under-counted' in z.stderr
    print(f'PASS {tests} finite-credit schedules, {stalls} necessary-slice refusals, both crossed credit stalls, malformed evidence, v8 release floors, held-space model and retired launcher')

if __name__=='__main__':main()
