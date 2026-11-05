#!/usr/bin/env python3
import hashlib, os, pathlib, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[2]
class Publication(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();cls.binary=pathlib.Path(cls.tmp.name)/'publication'
  subprocess.run(['g++','-std=c++17',str(ROOT/'xsa/tests/durable_piece_test.cpp'),'-Wl,--wrap=fsync','-Wl,--wrap=fdatasync','-o',str(cls.binary)],check=True)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def run_case(self,extra):
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);r=subprocess.run([self.binary,d],env=dict(os.environ,**extra),capture_output=True)
   return r,{f.name:f.read_bytes() for f in p.iterdir()}
 def test_durable_record_matches_bytes(self):
  r,f=self.run_case({});self.assertEqual(r.returncode,0,r.stderr)
  self.assertNotIn('p-0-0.partial',f)
  line=f['checkpoint.journal'].decode().splitlines()[1]
  self.assertIn('rows=17',line);self.assertIn('sha='+hashlib.sha256(f['p-0-0']).hexdigest(),line)
 def test_sha_padding_and_stream_boundaries(self):
  for size in [0,1,55,56,63,64,65,119,120,127,128,129,1048575,1048576,1048577,2097153]:
   with self.subTest(size=size):
    r,f=self.run_case({'PAYLOAD_BYTES':str(size)});self.assertEqual(r.returncode,0,r.stderr)
    expected=hashlib.sha256(bytes(i%251 for i in range(size))).hexdigest()
    self.assertIn(('sha='+expected).encode(),f['checkpoint.journal'])
 def test_failed_data_sync_never_renames_or_journals(self):
  r,f=self.run_case({'FAIL_FDATASYNC':'1'});self.assertEqual(r.returncode,2)
  self.assertIn('p-0-0.partial',f);self.assertNotIn('p-0-0',f);self.assertEqual(f['checkpoint.journal'],b'PLAN test=1\n')
 def test_failed_directory_sync_never_journals(self):
  r,f=self.run_case({'FAIL_FSYNC':'1'});self.assertEqual(r.returncode,2)
  self.assertIn('p-0-0',f);self.assertEqual(f['checkpoint.journal'],b'PLAN test=1\n')
 def test_failed_journal_sync_fails_build(self):
  r,f=self.run_case({'FAIL_FSYNC':'2'});self.assertEqual(r.returncode,2)
  self.assertIn(b'fsync checkpoint',r.stderr)
if __name__=='__main__':unittest.main()
