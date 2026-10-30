#!/usr/bin/env python3
"""Ensure distributable first-party sources cannot silently lag their originals."""
import importlib.util
from pathlib import Path
import unittest
import io
import tarfile
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('package_sources', ROOT/'tools/package_xsa_sources.py')
package = importlib.util.module_from_spec(spec); spec.loader.exec_module(package)
class CargoSources(unittest.TestCase):
    def test_registry_payload_budget(self):
        buffer = io.BytesIO()
        files = [p for d in ('runtime', 'vendor', 'src') for p in (ROOT/'xsa'/d).rglob('*') if p.is_file()]
        files += [ROOT/'xsa'/n for n in ('Cargo.toml', 'Cargo.lock', 'build.rs',
                  'build_tools.py', 'INSTALL.md')]
        with tarfile.open(fileobj=buffer, mode='w:gz') as archive:
            for p in sorted(files):
                archive.add(p, arcname='xsa-0.1.0/'+str(p.relative_to(ROOT/'xsa')))
        self.assertLess(len(buffer.getvalue()), 9_990_000,
                        'compressed source payload exceeds registry budget with metadata margin')

    def test_first_party_snapshots_match(self):
        for source in package.sources():
            with self.subTest(source=source):
                self.assertEqual(source.read_bytes(), package.destination(source).read_bytes())
    def test_no_hand_maintained_source_digest(self):
        # Git is the source record. A checked-in digest of the sources is a
        # duplicate of it that goes stale on every merge and breaks the build
        # (four times on 2026-10-30 alone), and no build may require a manual
        # regeneration step after editing a covered source.
        self.assertFalse((ROOT/'xsa/SOURCES.sha256.json').exists(),
                         'xsa/SOURCES.sha256.json must not exist: provenance is git')
        for pattern in ('xsa/build.rs', 'xsa/build_tools.py', 'tools/package_xsa_sources.py'):
            with self.subTest(file=pattern):
                text = (ROOT/pattern).read_text()
                self.assertNotIn('SOURCES.sha256', text)
                self.assertNotIn('vendored source SHA256 mismatch', text)
    def test_no_tracked_source_manifest(self):
        # Nothing may reintroduce the file into the crate without failing here.
        self.assertNotIn('SOURCES.sha256.json', (ROOT/'xsa/Cargo.toml').read_text())
if __name__ == '__main__': unittest.main()
