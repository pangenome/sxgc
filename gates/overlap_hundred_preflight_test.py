"""Tiny negative fixtures; no scale inputs or external tools."""
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from overlap_hundred_preflight import layout, projected, small


class HundredPreflightTest(unittest.TestCase):
    def test_reject_wrong_directory_even_if_file_length_fits(self):
        with TemporaryDirectory() as tmp:
            p = Path(tmp)/'reference'
            p.write_bytes(bytes(464))
            with self.assertRaisesRegex(ValueError, 'changed header/directory'):
                layout(p, 10, 10, '0'*64)

    def test_oversize_receipt_refused(self):
        with TemporaryDirectory() as tmp:
            p = Path(tmp)/'receipt'
            p.write_bytes(bytes(257))
            with self.assertRaisesRegex(ValueError, 'oversize'):
                small(p, 256)

    def test_ten_model_matches_banked_front(self):
        row = projected(318164488, 234472723, 11239544612)
        self.assertEqual(row['signed_extent'], 1725000826)
        self.assertEqual(row['emit_arrays'], 123166376628)


if __name__ == '__main__':
    unittest.main()
