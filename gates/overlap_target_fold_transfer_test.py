"""No historical payload. Exercise resegmentation inequality and adjacent-LCP work floor."""
from itertools import accumulate,product
from random import Random
from overlap_target_fold_transfer import Cuts,packet,raw_work

def live(rows,esc,cuts,a,b):
    cover=[(i,j) for i,j in zip(cuts,cuts[1:]) if a<j and i<b];left,right=cover[0][0],cover[-1][1];return b-a+right-left+16*sum(esc[left:right])

def raw(data):
    keys=sorted(data[i:] for i in range(len(data)+1));vals=[0]
    for a,b in zip(keys,keys[1:]):vals.append(next((j for j,(x,y) in enumerate(zip(a,b)) if x!=y),len(a)))
    H=max(vals);e=sum(x>=255 for x in vals);r=raw_work(len(keys),e,H);assert r['prefix_bytes_lower']<=sum(vals)<=r['prefix_bytes_upper'];return r,sum(vals)

def main():
    # Concurrent durability can log 2 before 1, but cannot duplicate, omit,
    # renumber, trade rows, or replace the wire byte count of a piece.
    fresh=lambda:Cuts([0,31,45,49],9,4);records=[(0,0,4,31,packet(4)),(1,4,8,14,packet(4)),(2,8,9,4,packet(1))];c=fresh()
    for i in (0,2,1):c.add(*records[i])
    assert c.finish()['ordinal_not_line_order']==2
    for trial in ([records[0],records[2]],2*[records[1]],[(3,*records[1][1:])],[(0,1,5,31,packet(4))],[(2,8,9,14,packet(1))],[(2,8,9,4,packet(4))]):
        c=fresh()
        try:
            for rec in trial:c.add(*rec)
            c.finish()
        except ValueError:continue
        raise AssertionError('invalid journal permutation/cut')
    rng=Random(5342);count=0
    for n in range(1,180):
        for _ in range(15):
            s=1+rng.randrange(21);u=[0]+[i for i in range(1,n) if rng.randrange(7)==0]+[n];v=list(range(0,n,s))+[n];esc=[rng.randrange(2) for _ in range(n)]
            cuts=[0]+[i for i in range(1,n) if rng.randrange(8)==0]+[n];a=0
            for b in cuts[1:]:
                lo=max(0,a-1);old=live(n,esc,u,lo,b);new=live(n,esc,v,lo,b);assert new<=old+34*(s-1) and 18*sum(esc[lo:b])<=old;count+=1;a=b
    # Same bytes with DIFFERENT folds: the transfer need not hold.
    y=[1]*201;cuts=list(range(202));h0=max(live(201,y,cuts,j,j+1) for j in range(201));assert live(201,y,list(range(0,201,4))+[201],0,201)>h0+34*3
    for n in range(10):
        for a in product((97,98),repeat=n):raw(bytes(a))
    for data in (b'a'*401,b'abc'*225,(bytes(range(200))+b'xyz')*4):raw(data)
    r,actual=raw(b'x'*400);assert r['prefix_bytes_lower']==actual and 255*r['escape_count']+r['max_lcp']*(r['max_lcp']+1)//2>actual
    for args in ((100,1,10),(500,0,300),(500,1,400)):
        try:raw_work(*args)
        except ValueError:continue
        raise AssertionError('invalid escape geometry')
    print(f'PASS {count} generated same-fold re-slicings incl overlap, invalid changed-fold counterexample, raw equal-byte floor/upper vs exact suffix arrays including overlapping long repeats')
if __name__=='__main__':main()
