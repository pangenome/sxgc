"""No large input, decoder, or timed worker: reproduce file counts against banked 1/10 GB character counters."""
import unittest
from overlap_segmented_budget import ledger,passes,routed,review,RECEIPTS
from overlap_segmented_gate import load

class Ledger(unittest.TestCase):
    def test_boundaries(self):
        for n,p in [(1,0),(16,0),(17,1),(256,1),(257,2),(4096,2),(4097,3)]:self.assertEqual(passes(n),p)
        leaf=1<<20;self.assertEqual(routed(leaf),0);self.assertEqual(routed(leaf+1),leaf+1);self.assertEqual(routed(64*leaf),64*leaf)
        x=64*leaf+1;r=leaf+1;self.assertEqual(routed(x),x+63*r)
        for args in [(1000,20,80),(10**10,2**32,100),(10**10,1,2<<20)]:
            with self.subTest(args=args),self.assertRaises(RuntimeError):ledger(*args)
    def test_archived_character_io(self):
        for name in ['segmented-front-1g','segmented-front-10g']:
            p=RECEIPTS/name;f=load(p,'segmented.json');v=f['census'];x=ledger(v['n'],v['m'],v['max_phrase'],v['k'],v['dictionary_bytes'],v['initial_runs']);self.assertEqual(x['intermediate_passes'],[v['intermediate_passes']]*2)
            self.assertTrue(v['peak_counted']<=x['counted_file_payload_peak_upper']<=2*v['n']);self.assertTrue(0<=f['process']['rchar']-x['two_raw_scans_plus_runs_and_routes_logical_read_bounds'][0]<1<<16);self.assertEqual(f['process']['wchar']-x['runs_routes_and_two_output_columns_logical_write_upper'],(p/'front.log').stat().st_size)
        self.assertEqual(review(p),load(p,'review.json'))
if __name__=='__main__':unittest.main()
