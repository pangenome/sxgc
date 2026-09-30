#!/usr/bin/env python3
"""Regression cases for assumptions required by a chunk BWT merge."""
import unittest
from itertools import combinations, product

from monotonicity_probe import SEP, chi, cyclic_bwt, maximal, requirements, scopes


def brute_min_cover(t):
    """Independent Definition 9 minimum-cover check for tiny strings."""
    need = requirements(t)
    for k in range(len(t) + 1):
        for xs in combinations(range(1, len(t) + 1), k):
            covered = {(w, c) for w, c in need if any(
                t[:x][-len(w) - 1:] == w + (c,) for x in xs)}
            if covered == need:
                return k
    raise AssertionError("all positions failed to cover requirements")


class ChunkMergePremiseTests(unittest.TestCase):
    def test_old_nonwitness_can_become_required(self):
        # Position 2 is dominated by position 3 in A. Appending B makes
        # context (1,2) right-maximal and position 2 a distinct maximal class.
        a = (1, 2, 2, SEP)
        b = (1, SEP)
        before, after = scopes(a), scopes(a + b)
        self.assertNotIn(1, maximal(before))
        self.assertIn(1, maximal(after))
        self.assertEqual((chi(before), chi(after)), (3, 5))
        self.assertEqual((brute_min_cover(a), brute_min_cover(a + b)), (3, 5))

    def test_within_chunk_cyclic_sa_order_can_reverse(self):
        # Both chunks have nonempty, separator-aligned documents. A is
        # aperiodic. The local suffix order of rows 1 and 3 reverses.
        a = (1, 1, SEP, 1, SEP)
        b = (2, 1, 1, SEP)
        local = sorted(range(len(a)), key=lambda i: (a[i:] + a[:i], i))
        joined = a + b
        global_order = sorted(range(len(a)),
                              key=lambda i: (joined[i:] + joined[:i], i))
        self.assertEqual(local, [0, 3, 1, 4, 2])
        self.assertEqual(global_order, [0, 1, 3, 2, 4])
        self.assertNotEqual(local, global_order)
        self.assertEqual(len(cyclic_bwt(a)), len(a))
        self.assertEqual(len(cyclic_bwt(joined)), len(joined))

    def test_180_aligned_pairs_show_revival_is_not_isolated(self):
        checked = violated = 0
        for na in (1, 2, 3, 4):
            for nb in (1, 2):
                for aa in product((1, 2), repeat=na):
                    a = aa + (SEP,)
                    old = maximal(scopes(a))
                    for bb in product((1, 2), repeat=nb):
                        new = maximal(scopes(a + bb + (SEP,)))
                        checked += 1
                        violated += any(i not in old and i in new
                                        for i in range(len(a)))
        self.assertEqual((checked, violated), (180, 26))


if __name__ == "__main__":
    unittest.main()
