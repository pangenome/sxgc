"""Small direct native path only: raw -> lex64 + keys -> inverse64 -> byte alignment and occurrence lists."""
from pathlib import Path
import random
import subprocess as sp
import sys
import tempfile
import time
from overlap_resident_test import trigger

def main(front,rank,checker):
    rng=random.Random(9123456);tag=next(i.to_bytes(8,'big') for i in range(10000) if trigger(i.to_bytes(8,'big')));prefix=rng.randbytes(448);end=str(int(time.time())+120)
    cases=[b'x'*55,bytes(range(256))*31,tag*480,b'3'+(b'ABCD'+tag)*330+tag,prefix*51+prefix[:77],(b'aa'+tag+b'b'+tag)*440+b'z',rng.randbytes(66_000)]
    for i in range(24):
        alphabet=bytes(rng.sample(range(256),(2,16,244)[i%3]));base=rng.choices(alphabet,k=rng.randrange(20,290));cases.append(bytes(base*rng.randrange(2,40)+rng.choices(alphabet,k=i)))
    checked=refused=0
    for i,text in enumerate(cases):
        with tempfile.TemporaryDirectory() as t:
            d=Path(t);raw=d/'raw';raw.write_bytes(text);stop=d/'stop';out=d/'columns';inv=d/'inverse.u64'
            f=sp.run([front,raw,out,'4',str(64<<20),str(2*len(text)),stop,end],capture_output=True)
            if i==3:
                # Dense trigger counterexample. Keep it, do not increase the front cap or omit the extra 8m.
                s=[0]+[j for j in range(1,len(text)-7) if trigger(text[j:j+8])];words={(text[p:(s[j+1]+8 if j+1<len(s) else len(text))],j+1==len(s)) for j,p in enumerate(s)};total=8*len(s)+sum(8+len(w) for w,_ in words)
                assert (len(s),len(words),total,2*len(text))==(991,6,8046,7938) and f.returncode==75 and b'column scratch/geometry' in f.stderr and not out.exists();refused+=1;continue
            assert f.returncode==0,(i,f.stderr);sp.run([rank,'rank',out/'keys.u64',inv,'4',str(64<<20),stop,end],capture_output=True,check=True);p=sp.run([checker,d],capture_output=True);assert p.returncode==0,(p.stderr,p.stdout,len(text));checked+=1
    assert checked==30 and refused==1
    print(f'PASS {checked} real native handoffs on generated text + one predicted 2N refusal; every parse-start byte LCP/position/BWT, head span, ordinal and successor checked at 1/2/4 consumers; malformed rank, address, extent and grant refusals')
if __name__=='__main__':main(*(Path(p).resolve() for p in sys.argv[1:]))
