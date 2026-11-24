"""Scorer shape and refusals using temporary synthetic evidence, never a performance observation."""
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from overlap_bounded_shortest import K, PREFIX, RUNG, compare, load, reference


def put(f,x):f.write_text(json.dumps(x))


class SeparateArm(unittest.TestCase):
    def test_closed_shape(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp)/(PREFIX+'synthetic'); a=d/'candidate'; shutil.copytree(RUNG/'candidate',a)
            r=reference(); put(d/'reference.json', r)
            inp=load(RUNG/'inputs.json'); inp['revision']='f'*40; put(d/'inputs.json', inp)
            b=load(RUNG/'binaries.json'); b['overlap_bounded_packed']='f'*64; put(d/'binaries.json', b)
            for role in ['candidate','prep','oracle']:
                f=a/(role+'.json'); m=load(f); old=Path(m['argv'][0]).parent.parent; m['argv']=[arg.replace(str(old)+'/',str(d)+'/') for arg in m['argv']]
                if role=='candidate': m['argv'][1]='--component-shortest-100m'
                put(f,m)
            put(d/'result.json',{'arms':{'candidate':load(a/'result.json')}})
            log=a/'producer.log'; lines=log.read_text().splitlines(); lines[1]=f'shortest entries={K-1} payload_bytes={9*(K-1)} exact_peak=6545 heap_cap=8192 dry_order_needed=false seconds=0.123456'; log.write_text('\n'.join(lines)+'\n')
            v=compare(d); self.assertEqual(v['avoided_full_passes'],18); self.assertFalse(v['compared_to_archived']['control']['both_under_3x']); self.assertFalse(v['product_promotion']); self.assertTrue(v['emitting_order_counter_prediction_matched'])
            lines[2]=lines[2].replace('2+16','3+16'); log.write_text('\n'.join(lines)+'\n'); self.assertFalse(compare(d)['emitting_order_counter_prediction_matched'])
            for before,after in [('exact_peak=6545','exact_peak=6546'),('dry_order_needed=false','dry_order_needed=true'),('heap_cap=8192','heap_cap=10000')]:
                log.write_text(('\n'.join(lines)+'\n').replace(before, after))
                with self.assertRaises(ValueError): compare(d)
            log.write_text('\n'.join(lines)+'\n'); (d/'control').mkdir()
            with self.assertRaises(ValueError):compare(d)
            (d/'control').rmdir(); b['oracle']='0'*64; put(d/'binaries.json', b)
            with self.assertRaises(ValueError):compare(d)

if __name__=='__main__':unittest.main()
