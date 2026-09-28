#!/usr/bin/env python3
"""Fail-closed distribution regressions; no upstream builds required."""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
from tool_manifest import BINARIES, create, verify
REPO = pathlib.Path(__file__).resolve().parents[1]


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        (self.root / 'tools').mkdir()
        (self.root / 'tools/upstream.lock.json').write_text(
            '{"pfp":{"commit":"' + 'a' * 40 + '","url":"https://example.org/pfp"}}\n')
        self.prefix = self.root / 'prefix'
        self.prefix.mkdir()
        for name in BINARIES:
            p = self.prefix / name
            p.write_bytes(b'#!/bin/sh\nexit 0\n')
            p.chmod(0o755)
        self.manifest = self.prefix / 'MANIFEST.sha256'
        create(self.prefix, self.manifest, self.root)

    def verify(self, **kwargs):
        return verify(self.manifest, self.prefix, root=self.root, **kwargs)

    def test_provenance_captures_exact_manifest_and_every_tool(self):
        result = self.verify()
        self.assertTrue(result['verified'])
        self.assertEqual(result['manifest'], self.manifest.read_text())
        self.assertEqual(result['manifest_sha256'], hashlib.sha256(self.manifest.read_bytes()).hexdigest())
        self.assertEqual(len([a for a in result['artifacts'] if a['artifact'].startswith('bin/')]), 8)

    def test_each_stage_byte_drift_reports_exact_line(self):
        for name in BINARIES:
            with self.subTest(name=name):
                path = self.prefix / name
                original = path.read_bytes()
                line = next(l for l in self.manifest.read_text().splitlines() if f'bin/{name} #' in l)
                path.write_bytes(b'!' + original[1:])
                with self.assertRaises(ValueError) as caught:
                    self.verify()
                self.assertIn(line, str(caught.exception))
                self.assertIn('SHA256 mismatch', str(caught.exception))
                path.write_bytes(original)

    def test_override_is_hashed_instead_of_default(self):
        other = self.root / 'other'
        other.write_bytes(b'wrong tool'); other.chmod(0o755)
        with self.assertRaisesRegex(ValueError, 'bin/pfp\\+\\+'):
            self.verify(overrides={'pfp++': other})

    def test_debug_escape_retains_drift_and_actual_hash(self):
        path = self.prefix / 'slim_dump'; path.write_bytes(b'drift')
        result = self.verify(allow_drift=True)
        self.assertFalse(result['verified'])
        self.assertTrue(result['allow_drift'])
        self.assertIn('bin/slim_dump', result['drift'][0])
        actual = next(a for a in result['artifacts'] if a['artifact'] == 'bin/slim_dump')
        self.assertEqual(actual['sha256'], hashlib.sha256(b'drift').hexdigest())

    def test_missing_duplicate_unknown_and_malformed_lines_fail(self):
        text = self.manifest.read_text()
        line = next(l for l in text.splitlines() if 'bin/xsa #' in l)
        for broken in (text.replace(line + '\n', ''), text + line + '\n',
                       text.replace('bin/xsa #', 'bin/unknown #'), text + 'oops\n'):
            self.manifest.write_text(broken)
            with self.assertRaises(ValueError):
                self.verify()

    def test_missing_unused_tool_and_nonexecutable_fail(self):
        path = self.prefix / 'agc2flat'
        path.chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'missing executable'):
            self.verify()
        path.unlink()
        with self.assertRaisesRegex(ValueError, 'missing executable'):
            self.verify()

    def test_changed_pin_and_source_fail(self):
        path = self.root / 'tools/upstream.lock.json'
        path.write_text(path.read_text().replace('a' * 40, 'b' * 40))
        with self.assertRaises(ValueError) as caught:
            self.verify()
        self.assertIn('pin/pfp', str(caught.exception))
        self.assertIn('repo/tools/upstream.lock.json', str(caught.exception))

    def test_pipeline_refuses_before_scratch_then_journals_debug_bypass(self):
        # Use the real entrypoint, with a complete isolated fake toolset.
        create(self.prefix, self.manifest, REPO)
        parser = self.prefix / 'pfp++'
        parser.write_bytes(b'#!/bin/sh\nexit 37\n')
        source = self.root / 'text'; source.write_bytes(b'ACGT\x1e')
        scratch = self.root / 'scratch'
        logs = self.root / 'logs'
        output = self.root / 'output.sxi'
        env = {k: v for k, v in os.environ.items() if not k.startswith('XSA_')}
        env['XSA_TOOLS'] = str(self.prefix)
        command = [sys.executable, str(REPO / 'bit6/sxi_pipeline.py'),
                   '--text', str(source), '--output', str(output),
                   '--scratch', str(scratch), '--log-dir', str(logs)]
        result = subprocess.run(command, env=env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        expected = next(l for l in self.manifest.read_text().splitlines() if 'bin/pfp++ #' in l)
        self.assertIn(expected, result.stderr)
        self.assertFalse(scratch.exists())
        self.assertFalse(logs.exists())
        self.assertFalse(output.exists())
        result = subprocess.run(command + ['--allow-drift'], env=env,
                                capture_output=True, text=True)
        self.assertIn('DEBUG ONLY', result.stderr)
        self.assertIn('parse failed (37)', result.stderr)
        self.assertFalse(output.exists())
        entries = [json.loads(l) for l in next(logs.glob('*.jsonl')).read_text().splitlines()]
        self.assertEqual(entries[0]['stage'], 'tool-provenance')
        self.assertTrue(entries[0]['allow_drift'])
        self.assertFalse(entries[0]['verified'])
        self.assertIn(expected, entries[0]['drift'][0])


if __name__ == '__main__':
    unittest.main()
