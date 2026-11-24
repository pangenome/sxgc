"""Receipt-only tests; no corpus, packed dictionary or launch."""
import json
from pathlib import Path
import shutil
import tempfile
import unittest
import overlap_vocab_review as r

class Review(unittest.TestCase):
    def test_minimum_levels_at_boundaries(self):
        for n,seg,levels in [(1,1,0),(8,1,0),(9,2,1),(64,8,1),(65,9,2),(513,65,3)]:
            with self.subTest(n=n):
                p=r.min_levels(n,2,4,8);self.assertEqual((p['min_segments_in_largest_bucket'],p['min_merge_levels']),(seg,levels));self.assertEqual(p['raw_bytes_in_buckets_needing_multilevel_at_least'],max(0,n-64))
    def test_replay_and_reject_consistent_file_hash_but_wrong_witness(self):
        orig=r.gate.RECEIPTS/'vocab-10g';v=r.review(orig);self.assertEqual(v,r.gate.load(orig/'review.json'));self.assertEqual(v['actual_10g']['largest_finite_group_descriptors'],546042)
        with tempfile.TemporaryDirectory() as tmp:
            b=Path(tmp)/'test';shutil.copytree(orig,b);p=b/'reference.out';p.write_text(p.read_text().replace('largest=212471462','largest=212471461'))
            with self.assertRaisesRegex(RuntimeError,'provenance'):r.review(b)
            q=b/'source.json';z=r.gate.load(q);z['artifacts'][p.name]=r.gate.sha(p);q.write_text(json.dumps(z))
            with self.assertRaisesRegex(RuntimeError,'descriptor cells'):r.review(b)

if __name__=='__main__':unittest.main()
