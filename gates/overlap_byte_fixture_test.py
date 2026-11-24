"""Small synthetic score/refusal checks, not observations or raw rows."""
import tempfile
from pathlib import Path
import unittest
import overlap_byte_fixture as f

class Cross(unittest.TestCase):
    def test_synthetic(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'log'; rows=[];u=100000;fixed=13056;n=((64<<20)-8192*20-fixed)//8;budget=fixed+8*((u*n+95434515)//95434516)
            for s in f.SHAPES:
                for i,a in enumerate(f.MODES):
                    b=a=='Bytes';rows.append(f'CHECKED shape={s} rep={1+i//2} order={a} dictionary_bytes=140000 words=8000 descriptors={u} rows=120000 groups=90000 largest=50 workspace={budget} peak_slots=2000 scans=2 distributions=14 digit_reads=30000 byte_partitions={int(b)} frames_peak={int(b)} frame_reserve={4160*int(b)} leaf_comparisons={3000*int(b)} shifted_slots={5000*int(b)} wall_s={.60 if b else 1.} cpu_s=.56')
            data='\n'.join(rows)+'\n';p.write_text(data);self.assertTrue(f.score(p)['fixture_hypothesis_met']);p.write_text(data.replace('wall_s=0.6','wall_s=0.90'));self.assertFalse(f.score(p)['fixture_hypothesis_met'])
            for x,y in [('shape=wide','shape=book'),('leaf_comparisons=0','leaf_comparisons=3'),('wall_s=1.0','wall_s=nan'),('peak_slots=2000','peak_slots=100000')]:
                p.write_text(data.replace(x,y));
                with self.assertRaises(RuntimeError):f.score(p)

if __name__=='__main__':unittest.main()
