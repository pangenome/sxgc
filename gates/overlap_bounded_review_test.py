"""Replay small receipts, including semantic refusals with internally rehashed test manifests."""
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from overlap_bounded_review import RUNG, load, review, sha


class Bank(unittest.TestCase):
    def test_actual(self):
        self.assertEqual(review(RUNG), load(RUNG/'review.json'))

    def test_refusals(self):
        for file, change in (
            ('candidate/oracle.log', lambda p: p.write_text(p.read_text().replace('final_rank=100000504','final_rank=100000503'))),
            ('candidate/cold-columns.json', lambda p:p.write_text(p.read_text().replace('"mincore_resident_pages": 0','"mincore_resident_pages": 1'))),
            ('candidate/candidate.json', lambda p:p.write_text(p.read_text().replace('536870912','2147483648'))),
            ('result.json', lambda p:p.write_text(p.read_text().replace('4.927520730629808','0.50'))),
        ):
            with self.subTest(file=file), tempfile.TemporaryDirectory() as tmp:
                d=Path(tmp)/'bank'; shutil.copytree(RUNG, d); before=sha(d/file); change(d/file)
                s=load(d/'source.json'); s['artifacts'][file]=sha(d/file); self.assertNotEqual(s['artifacts'][file],before)
                (d/'source.json').write_text(json.dumps(s))
                with self.assertRaises(ValueError): review(d)

if __name__=='__main__': unittest.main()
