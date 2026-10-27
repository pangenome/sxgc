#!/usr/bin/env python3
"""Verify that peak evidence partitions allocated blocks without double counting."""
from pathlib import Path
import tempfile
import json
import subprocess
import sys
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
                **{'product/frag.'+k: k for k in ('bits-head','bits-len','bits-high','bits-low')},
            }
            for name in artifacts:
                path = root/name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'x' * 8192)
            order, total, components = snapshot(root)
            self.assertEqual(sum(components.values()), total)
            for name, category in artifacts.items():
                expected = (root/name).stat().st_blocks*512
                if category in ('waves', 'packed_bands', 'pieces'):
                    expected += (root/name).parent.stat().st_blocks*512
                self.assertEqual(components[category], expected)
            self.assertGreaterEqual(order, 4*8192)
            partial = root/'product/product.sxi2.partial'
            partial.write_bytes(b'y'*8192)
            _, _, with_partial = snapshot(root)
            self.assertEqual(with_partial['container'],
                             partial.stat().st_blocks*512 +
                             (root/'product/product.sxi2').stat().st_blocks*512)
            partial.unlink()
            (root/'order/waves/w0').unlink()
            _, after, parts = snapshot(root)
            self.assertEqual(sum(parts.values()), after)
            self.assertEqual(parts.get('waves', 0), (root/'order/waves').stat().st_blocks*512)
            self.assertLess(after, total)

    def test_report_uses_coincident_peak_and_rejects_over_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'results.json').write_text(json.dumps(dict(passed=True, n=100, sha256='test')))
            rows = [dict(elapsed=0, phase='emit', total_bytes=200, components={'waves': 200}),
                    dict(elapsed=1, phase='finish', total_bytes=200, components={'pieces': 200})]
            series = root/'series.jsonl'
            series.write_text(''.join(json.dumps(r)+'\n' for r in rows))
            command = [sys.executable, str(Path(__file__).with_name('scratch_report.py')),
                       '--results', str(root/'results.json'), '--series', str(series),
                       '--output-dir', str(root/'out')]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
            report = json.loads((root/'out/space-gate.json').read_text())
            self.assertEqual(report['observed_whole_peak_over_n'], 2)
            self.assertEqual(sum(report['component_peaks'].values()), 400)
            rows.append(dict(elapsed=2, phase='finish', total_bytes=301,
                             components={'pieces': 200, 'other:product/product.sxi2.partial': 101}))
            series.write_text(''.join(json.dumps(r)+'\n' for r in rows))
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)
            report = json.loads((root/'out/space-gate.json').read_text())
            self.assertFalse(report['space_gate_passed'])
            self.assertEqual(report['component_peaks']['container'], 101)


if __name__ == '__main__':
    unittest.main()
