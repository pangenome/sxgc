"""Check the predeclared lookup score and size proof with generated counterexamples; no actual timings."""
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from overlap_lex_lookup import MIN_QUERIES, MODES, SCHEDULES, score
from overlap_lex_direct_plan import wire

def mock_log():
    out=[]
    for name,k,u in [('book',5928,192164),('wide',32768,32768*25),('deep',16384,16384*112+8)]:
        q=(k+15)//16;raw=u+8*(k-1);lit=q*480 if name=='deep' else raw;restart=lit+8*(q+1)+2*k-q;direct=wire(k,lit,1<<20,lit)['base_min_bytes']
        out.append(f'DECK shape={name} k={k} raw={raw} owned={u} literals={lit} flat={raw+16+8*(k+1)} restart={restart} direct={direct}')
        for how in SCHEDULES:
            n={'head':k,'scatter':u,'scan':raw+2*k}[how];c=(MIN_QUERIES+n-1)//n;found=raw if how=='scan' else n;end=int(how=='scan');v=n if how=='scatter' else k;heads=v if how=='scatter' else 136*(k//16)+(k%16)*(k%16+1)//2
            first,hops,maxh=(q,120*q,15) if (name,how)==('deep','head') else (found,0,0)
            out.append(f'TRACE shape={name} schedule={how} queries={n} cycles={c} views={v} restart_headers={heads} raw={found} end={end} absent={n-found-end} hops={hops} max_hops={maxh} literal_hits={first}')
            for i,mode in enumerate(MODES):
                clock={'direct':1.5,'restart':3,'flat':1}[mode];out.append(f'LOOKUP shape={name} schedule={how} rep={i//3} table={mode} verified={c*n} wall_s={clock} cpu_s={clock}')
    return '\n'.join(out)+'\n'

class Ledger(unittest.TestCase):
    def test_bound(self):
        for sizes in ([9],[257]*16,[65536]+[9]*18,[254,66000,255,65536]*16):
            # Distinct lexical records; compute independent exact direct wire from byte comparisons.
            words=sorted(bytes([i//16])*(n-1)+bytes([i]) for i,n in enumerate(sizes));total=0;blocks=[]
            for start in range(0,len(words),16):
                g=words[start:start+16];p=[0]+[next((j for j,(x,y) in enumerate(zip(a,b)) if x!=y),min(len(a),len(b))) for a,b in zip(g,g[1:])];e=sum(map(len,g))-sum(p);blocks.append(e);width=lambda v:1+sum(v>=t for t in (256,65536,16777216));total+=1+width(max(p))*(len(g)-1)+width(e)*len(g)+e
            w=wire(len(words),sum(blocks),max(sizes),max(blocks));size=16+8*(1+len(blocks))+total;self.assertLessEqual(w['base_min_bytes'],size);self.assertLessEqual(size,w['wire_upper'])
    def test_fixed_10g(self):
        v=wire(234472723,7674858399,88468,89135);self.assertEqual((v['base_min_bytes'],v['wire_upper']),(8261040237,8718961516))
    def test_refusals(self):
        for args in ((0,1,8,8),(4,-1,5,5),(1,1024,(1<<20)+1,1024)):
            with self.assertRaises(ValueError):wire(*args)

class Contract(unittest.TestCase):
    def test_score(self):
        with TemporaryDirectory() as tmp:
            p=Path(tmp)/'test.log';src=mock_log();p.write_text(src);v=score(p);self.assertTrue(v['scatter_hypothesis_met'] and v['all_lookup_workloads_under_2x_flat']);self.assertFalse(v['product_promotion'])
            p.write_text(src.replace('wall_s=1.5','wall_s=3',1));self.assertFalse(score(p)['scatter_hypothesis_met'])
            for old,new in [('wall_s=1.5','wall_s=nan'),('max_hops=15','max_hops=14'),('table=flat','table=direct'),('literals=','lit='),('schedule=scan','schedule=head')]:
                p.write_text(src.replace(old,new,1));
                with self.assertRaises(RuntimeError):score(p)
if __name__=='__main__':unittest.main()
