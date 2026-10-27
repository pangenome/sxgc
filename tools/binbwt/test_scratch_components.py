#!/usr/bin/env python3
"""Verify that peak evidence partitions allocated blocks without double counting."""
from pathlib import Path
import tempfile
import unittest
from scratch_components import snapshot


class ScratchComponentsTest(unittest.TestCase):
    def test_partition_and_unknown_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifacts = {
                'order/waves/w0': 'waves', 'order/bands/b0.sa': 'packed_bands',
                'order/column': 'column', 'order/isa-sample': 'isa_sample',
                'product/frag.pieces/g-0': 'pieces', 'product/frag.rlebwt': 'rlebwt',
                'product/frag.sparse2': 'sparse2', 'product/frag.pfck2': 'pfck2',
                'product/frag.wit-0': 'witness_spills',
                'product/product.sxi2': 'container', 'new-artifact': 'other:new-artifact',
            }
            for name in artifacts:
                path = root/name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'x' * 8192)
            order, total, components = snapshot(root)
            self.assertEqual(sum(components.values()), total)
            for name, category in artifacts.items():
                self.assertEqual(components[category], (root/name).stat().st_blocks*512)
            self.assertGreaterEqual(order, 4*8192)
            (root/'order/waves/w0').unlink()
            _, after, parts = snapshot(root)
            self.assertEqual(sum(parts.values()), after)
            self.assertEqual(parts.get('waves', 0), 0)
            self.assertLess(after, total)


if __name__ == '__main__':
    unittest.main()
