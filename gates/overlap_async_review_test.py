"""Small portable receipts only. Even internally rehashed corruptions must refuse."""
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from overlap_async_review import BANK, load, review, sha

class ActualPair(unittest.TestCase):
    def test_closed(self):
        d=BANK/'async-audit-1g';self.assertEqual(review(d),load(d,'review.json'))

    def test_corruptions(self):
        for f,key,wrong in [('admission.json','required_s',0),('sync/source.json','complete_arm',False),('comparison.json','verified_full_products',False)]:
            with self.subTest(f=f),tempfile.TemporaryDirectory() as tmp:
                d=Path(tmp)/'bank';shutil.copytree(BANK/'async-audit-1g',d);v=load(d,f);v[key]=wrong;(d/f).write_text(json.dumps(v));s=load(d,'source.json');s['artifacts'][f]=sha(d/f);(d/'source.json').write_text(json.dumps(s))
                with self.assertRaises(RuntimeError):review(d)

if __name__=='__main__':unittest.main()
