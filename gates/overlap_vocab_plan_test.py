"""Cross-check whole-cell cut thresholds exhaustively, no real inputs."""
from itertools import product
from overlap_vocab_plan import counts,greedy,minimum,study

def test():
    for l in range(1,7):
        for weights in product((1,3,4),repeat=l):
            cells=[(j,w,w*2) for j,w in enumerate(weights)];opts={}
            for mask in range(1<<(l-1)):
                seg=[weights[0]]
                for j,w in enumerate(weights[1:]):
                    if mask>>j&1:seg.append(0)
                    seg[-1]+=w
                n=len(seg);opts[n]=min(opts.get(n,1<<31),max(seg))
            for n in range(1,l+1):
                v=minimum(cells,n);cap=v['descriptor_slots'];assert cap==min(opts[j] for j in opts if j<=n)
                ranges=greedy(cells,cap,True)['ranges_first_last_descriptors_rows'];assert sum(x[2] for x in ranges)==sum(weights)
                assert 0<len(ranges)<=n and ranges[0][0]==0 and ranges[-1][1]==l-1
                assert all(x[1]+1==y[0] for x,y in zip(ranges,ranges[1:]))
    d=counts([[0,1],[2,3],[5140+2,10]]);r=counts([[0,1],[2,6],[5140+2,11]])
    v=study(18,3,2,14,25,d,r);assert v['packed_bytes']==30 and v['largest_descriptor_cell']==(5142,10,11)
    for broken in ([[0,0]],[[-1,1]],[[2,1],[1,1]],[[3,1],[3,2]]):
        try:counts(broken)
        except ValueError:pass
        else:raise AssertionError('accepted invalid counts')
    print('PASS brute contiguous capacities at 1..6 cells, occurrence/descriptor separation, limits and packed-space model')
if __name__=='__main__':test()
