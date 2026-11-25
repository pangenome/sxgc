"""Generated run geometry only; independent row-ordered credit machine including EOF."""
from array import array
from itertools import accumulate
from overlap_finish_credits_test import finishes
from overlap_v10_target_credits import bounds,ceil,geometry

def run(lengths,p,b,s):
    y=[0,*accumulate(lengths)];n,r=y[-1],len(lengths);starts=array('Q',y[:-1:p]+[n]);blocks=array('Q',y[:-1:b]);chunks=[blocks[i:i+3] for i in range(0,len(blocks),3)];lo,hi=bounds(n,r,p,b,starts,chunks,lambda:None);exact=[y[max(0,min(r,i*p)-2)] for i in range(len(starts))];assert all(a<=t<=z for a,t,z in zip(lo,exact,hi));g=geometry(n,starts,lo,hi,s,lambda:None);assert finishes(lengths,p,s,g['piece_sufficient'],g['slice_sufficient']);w=g['slice_necessary'];assert w==1 or not finishes(lengths,p,s,100,w-1)
    peak=0
    for a,z in zip(exact,exact[1:]+[n]):
        start=max(0,a-1)
        if a<z:peak=max(peak,z-start+17*(min(n,ceil(z,s)*s)-start//s*s))
    assert g['fold_worst_case_bytes_upper']>=peak;return g

def main():
    tests=0
    for n in range(1,11):
        for mask in range(1<<(n-1)):
            y=[0]+[i for i in range(1,n) if mask>>(i-1)&1]+[n];l=[b-a for a,b in zip(y,y[1:])]
            for p,b in ((1,1),(2,1),(4,2),(6,3)):
                for s in (1,3,6):run(l,p,b,s);tests+=1
    assert run([80],8,2,10)['six_piece_five_slice']=='insufficient' and run([1]*80,8,2,10)['six_piece_five_slice']=='sufficient_for_credit_schedule'
    for samples in ([0,2,2],[0,5,6],[0,2],[0,2,4,6]):
        try:bounds(5,5,4,2,array('Q',[0,4,5]),[array('Q',samples)],lambda:None)
        except ValueError:continue
        raise AssertionError('accepted bad m12')
    print(f'PASS {tests} sampled schedules vs actual credit machine; delayed rows/fold upper, short-window deadlock, terminal cuts, corrupted block/super alignment')
if __name__=='__main__':main()
