"""Wire errors must not publish a completed stream, including at the last band."""
import io
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import overlap_v10_tap as tap


class TapTest(unittest.TestCase):
    def exercise(self, rows, suffix=b''):
        # Acknowledge only after sink flush, and check at that instant that
        # there really is just one packed position band (not a growing SA).
        with tempfile.TemporaryDirectory() as tmp, patch.object(tap, 'BAND', 2):
            d = Path(tmp)
            captured = []

            class Sink(io.BytesIO):
                def flush(inner):
                    band, = d.glob('*.sa')
                    self.assertTrue(band.with_suffix('.opened').exists())
                    captured.extend(v for v, in struct.iter_unpack('<Q', band.read_bytes()))
                    band.unlink()
                    band.with_suffix('.consumed').touch()

            sink = Sink()
            data = b''.join(struct.pack('<III', *r) for r in rows) + suffix
            try:
                tap.relay(io.BytesIO(data), sink, d, 4)
            except ValueError:
                self.assertFalse((d/'end.ready').exists())
                raise
            self.assertEqual(sink.getvalue(), data)
            self.assertEqual(captured, [p for p, h, b in rows])
            self.assertEqual((d/'end').read_text(), '3')
            self.assertTrue((d/'end.ready').exists())

    # SA of `abaa`: END, a, aa, abaa, baa.
    rows = [(4,0,97),(3,0,97),(2,1,98),(0,1,256),(1,0,97)]

    def test_partial_last_band(self):
        self.exercise(self.rows)

    def test_refusals(self):
        for bad in (self.rows[:-1], self.rows[1:], self.rows[:2]+[(5,0,0)]+self.rows[3:],
                    self.rows[:2]+[(2,3,255)]+self.rows[3:], self.rows[:-1]+[(0,0,256)],
                    self.rows[:3]+[(0,1,97)]+self.rows[4:]):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                self.exercise(bad)
        with self.assertRaises(ValueError):
            self.exercise(self.rows, b'!')


if __name__ == '__main__':
    unittest.main()
