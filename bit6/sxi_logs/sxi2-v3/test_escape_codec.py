#!/usr/bin/env python3
"""Reference escape-member roundtrip and malformed-input checks."""
import struct
import unittest

from escape_codec import ENTRY, HEADER, Escape, encode


class EscapeCodecTests(unittest.TestCase):
    def test_cyclic_domain_and_missing_run(self):
        member = Escape(encode(16, 4, {0: [(15, 4), (0, 8), (1, 12)]}))
        self.assertEqual([member.successor(0, x) for x in (15, 0, 1)],
                         [4, 8, 12])
        self.assertIsNone(member.successor(1, 0))
        with self.assertRaises(ValueError):
            member.successor(0, 2)

    def test_reject_truncated_and_noncanonical(self):
        valid = encode(16, 4, {0: [(15, 4), (0, 8), (1, 12)]})
        for damaged in (valid[:HEADER.size - 1], valid[:-1], valid + b"\x00"):
            with self.assertRaises(ValueError):
                Escape(damaged)
        # Three 4-bit values occupy two bytes; the high nibble is padding.
        damaged = bytearray(valid)
        damaged[-1] |= 0xf0
        with self.assertRaisesRegex(ValueError, "padding"):
            Escape(damaged)

    def test_reject_unsorted_run_ids(self):
        valid = bytearray(encode(16, 4, {0: [(0, 1)], 2: [(2, 3)]}))
        struct.pack_into("<Q", valid, HEADER.size + ENTRY.size, 0)
        with self.assertRaisesRegex(ValueError, "order"):
            Escape(valid)


if __name__ == "__main__":
    unittest.main()
