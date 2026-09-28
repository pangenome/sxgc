#!/usr/bin/env python3
"""Ensure distributable first-party sources cannot silently lag their originals."""
import importlib.util
import hashlib
import json
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
                  'build_tools.py', 'SOURCES.sha256.json', 'INSTALL.md')]
        with tarfile.open(fileobj=buffer, mode='w:gz') as archive:
            for p in sorted(files):
                archive.add(p, arcname='xsa-0.1.0/'+str(p.relative_to(ROOT/'xsa')))
        self.assertLess(len(buffer.getvalue()), 9_990_000,
                        'compressed source payload exceeds registry budget with metadata margin')

    def test_first_party_snapshots_match(self):
        for source in package.sources():
            with self.subTest(source=source):
                self.assertEqual(source.read_bytes(), package.destination(source).read_bytes())
    def test_vendored_source_checksums(self):
        expected = json.loads((ROOT/'xsa/SOURCES.sha256.json').read_text())
        actual = {str(p.relative_to(ROOT/'xsa')): hashlib.sha256(p.read_bytes()).hexdigest()
                  for d in ('runtime', 'vendor', 'src') for p in (ROOT/'xsa'/d).rglob('*') if p.is_file()}
        for name in ('build.rs', 'build_tools.py'):
            actual[name] = hashlib.sha256((ROOT/'xsa'/name).read_bytes()).hexdigest()
        self.assertEqual(actual, expected)
if __name__ == '__main__': unittest.main()
