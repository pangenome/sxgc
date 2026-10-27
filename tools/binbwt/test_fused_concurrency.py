#!/usr/bin/env python3
"""Failure-path integration check: a dead emitter must never park finish."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class ConcurrentFailure(unittest.TestCase):
    def test_dead_emitter_reaps_waiting_finish(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            bundle = d / 'bundle'
            bundle.mkdir()
            (bundle / 'MANIFEST.sha256').write_text(json.dumps({'source_sha256': 'fixture'}))
            sleeper = '#!/usr/bin/env python3\nimport time\ntime.sleep(300)\n'
            for name, body in [('xsa_finish', sleeper), ('cross_lcp_merge', '#!/usr/bin/env python3\nraise SystemExit(7)\n'), ('sorter', sleeper)]:
                f = bundle / name
                f.write_text(body)
                f.chmod(0o755)
            corpus = d / 'corpus'
            corpus.write_bytes(b'x')
            run = d / 'run'
            proc = subprocess.run(['python3', str(Path(__file__).with_name('fused_gate.py')),
                                   '--bundle', str(bundle), '--sorter', str(bundle / 'sorter'),
                                   '--root', str(run), '--corpus', str(corpus), '--remap', str(corpus),
                                   '--sample-checker', str(bundle / 'sorter'), '--checker', str(bundle / 'sorter')],
                                  capture_output=True, text=True, timeout=15)
            self.assertNotEqual(proc.returncode, 0)
            result = json.loads((run / 'results.json').read_text())
            self.assertIn("('emit', 7)", result['error'])
            self.assertEqual([c['label'] for c in result['commands']], ['finish', 'order', 'emit'])
            for command in result['commands']:
                with self.assertRaises(ProcessLookupError):
                    os.kill(command['pid'], 0)


if __name__ == '__main__':
    unittest.main()
