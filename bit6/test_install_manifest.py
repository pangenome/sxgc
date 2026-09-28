#!/usr/bin/env python3
"""Installed provenance rejects modified identity, omitted tools and overrides."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from tool_manifest import BINARIES, verify
class InstallManifest(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)
        files={}
        for name in (set(BINARIES)-{'xsa'})|{'phi_inverse_heads'}:
            p=self.root/name; p.write_bytes(b'#!/bin/sh\nexit 0\n'); p.chmod(0o700)
            files[name]=hashlib.sha256(p.read_bytes()).hexdigest()
        (self.root/'xsa').write_bytes(b'executing xsa')
        self.data=dict(format='xsa-install-v1', package=dict(name='xsa', version='0.1.0'),
                       source_sha256='a'*64, upstreams={}, files=files)
        self.manifest=self.root/'MANIFEST.sha256'
        self.env=patch.dict(os.environ, {'XSA_PACKAGE_VERSION':'0.1.0'});self.env.start();self.addCleanup(self.env.stop)
        self.bless()
    def bless(self):
        self.manifest.write_text(json.dumps(self.data))
        os.environ['XSA_INSTALL_MANIFEST_SHA256']=hashlib.sha256(self.manifest.read_bytes()).hexdigest()
    def check(self, **kw):return verify(self.manifest,self.root,**kw)
    def test_version_and_observed_self_hash(self):
        r=self.check();self.assertTrue(r['verified']);self.assertEqual(r['package']['version'],'0.1.0')
        own=next(x for x in r['artifacts'] if x['artifact']=='bin/xsa')
        self.assertNotIn('expected_sha256',own)
    def test_identity_and_manifest_tamper_rejected(self):
        self.manifest.write_text(self.manifest.read_text()+' ')
        with self.assertRaisesRegex(ValueError,'manifest SHA256 mismatch'):self.check()
        self.bless();os.environ['XSA_PACKAGE_VERSION']='9.9.9'
        with self.assertRaisesRegex(ValueError,'package identity'):self.check()
    def test_incomplete_toolset_rejected(self):
        del self.data['files']['agc2flat'];self.bless()
        with self.assertRaisesRegex(ValueError,'incomplete'):self.check()
    def test_unused_tool_and_overrides_fail(self):
        (self.root/'sxi_text_audit').write_bytes(b'bad')
        with self.assertRaisesRegex(ValueError,'SHA256 mismatch'):self.check()
        self.assertFalse(self.check(allow_drift=True)['verified'])
    def test_external_tool_override_hashed(self):
        p=self.root/'other';p.write_bytes(b'bad');p.chmod(0o700)
        with self.assertRaisesRegex(ValueError,'SHA256 mismatch: pfp'):
            self.check(overrides={'pfp++': p})
if __name__=='__main__':unittest.main()
