"""Test a built native relay, without allocating a corpus proportional to high bits."""
from pathlib import Path
import struct
import subprocess as sp
import sys
import tempfile
import time
import unittest

from overlap_row43_test import wire

EXE = Path(sys.argv.pop(1)).resolve()
BAND = 32768


class RelayTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name); self.stop = self.root/'stop'; self.tap = self.root/'tap'
        self.corpus = self.root/'corpus'; self.corpus.write_bytes(b'abaa')
        self.rows = [(4,0,97),(3,0,97),(2,1,98),(0,1,256),(1,0,97)]

    def cmd(self, n=4, wide=True):
        return [EXE, 'row43' if wide else 'narrow', self.corpus, str(n), self.tap, self.stop]

    def spawn(self, n=4, wide=True, **io):
        p = sp.Popen(self.cmd(n,wide), stdin=sp.PIPE, **io)
        def close():
            if p.poll() is None: p.kill()
            p.wait(2)
            for f in (p.stdin,p.stdout):
                if f: f.close()
        self.addCleanup(close)
        return p

    def opened(self):
        end = time.monotonic()+3
        while not (self.tap/'band_0.opened').exists():
            if time.monotonic()>end: self.fail('band did not open')
            time.sleep(.002)
        return self.tap/'band_0.sa'

    def test_exact_fanout(self):
        for wide in (True,False):
            self.tap = self.root/str(wide)
            data = b''.join(wire(*r) if wide else struct.pack('<III',*r) for r in self.rows)
            p = self.spawn(wide=wide,stdout=sp.PIPE); p.stdin.write(data); p.stdin.close()
            band = self.opened(); self.assertEqual(struct.unpack('<5Q',band.read_bytes()),(4,3,2,0,1))
            self.assertEqual(p.stdout.read(len(data)),data); self.assertIsNone(p.poll())
            band.unlink(); band.with_suffix('.consumed').write_bytes(b'trie-tap\n')
            self.assertEqual(p.wait(2),0); self.assertEqual((self.tap/'end').read_text(),'1')

    def test_corrupt(self):
        data = b''.join(wire(*r) for r in self.rows)
        for i,bad in enumerate((data[:-1],data+b'!',wire(4,0,257)+data[12:],wire(4,0,97)[0:8]+b'\0'*4+data[12:],
                                wire(3,0,97)+data[12:],data[:12]+wire(4,0,97)+data[24:],
                                data[:24]+wire(2,1<<32,98)+data[36:],data[:36]+wire(1,0,98)+data[48:],
                                data[:-12]+wire(0,0,256))):
            self.tap = self.root/str(i)
            r = sp.run(self.cmd(),input=bad,capture_output=True,timeout=2)
            self.assertEqual(r.returncode,70); self.assertFalse((self.tap/'end.ready').exists())

    def test_exact_extents_and_new_dir(self):
        self.assertEqual(sp.run(self.cmd(n=5),input=b'').returncode,70)
        with self.corpus.open('rb') as f:
            self.assertEqual(sp.run(self.cmd(),stdin=f).returncode,70)
        self.assertFalse(self.tap.exists()); self.tap.mkdir(); bank = self.tap/'band_0.sa'; bank.write_bytes(b'bank')
        self.assertEqual(sp.run(self.cmd(),input=b'').returncode,70); self.assertEqual(bank.read_bytes(),b'bank')

    def test_stop_blocked_input(self):
        p=self.spawn(); end=time.monotonic()+2
        while not self.tap.is_dir():
            if time.monotonic()>end: self.fail("relay did not start")
            time.sleep(.002)
        self.stop.touch(); self.assertEqual(p.wait(2),75)
        self.assertFalse((self.tap/'end.ready').exists())

    def test_high_prefix_blocked_output(self):
        # Metadata-only sparse extent. Send a single .39 MB synthetic prefix,
        # not a complete SA or a multi-GB corpus run. Includes valid high LCP.
        n=(2<<32)+11
        with self.corpus.open('wb') as f: f.truncate(n)
        self.assertEqual(self.corpus.stat().st_blocks,0)
        rows=[(n,0,1),(1<<32,0,2),(3,(1<<32)+1,4)]+[(i,0,5) for i in range(4,BAND+1)]
        p=self.spawn(n=n,stdout=sp.PIPE); p.stdin.write(b''.join(wire(*r) for r in rows)); p.stdin.flush()
        band=self.opened(); self.assertEqual(list(struct.unpack(f'<{BAND}Q',band.read_bytes())),[r[0] for r in rows])
        self.stop.touch(); self.assertEqual(p.wait(2),75); self.assertTrue(band.exists()); self.assertFalse(band.with_suffix('.consumed').exists())

    def test_lost_sink(self):
        p=self.spawn(stdout=sp.PIPE); p.stdout.close(); p.stdin.write(b''.join(wire(*r) for r in self.rows)); p.stdin.close()
        self.assertEqual(p.wait(2),70); self.assertTrue(self.opened().exists()); self.assertFalse((self.tap/'end.ready').exists())

    def test_no_credit_for_live_band(self):
        p=self.spawn(stdout=sp.DEVNULL); p.stdin.write(b''.join(wire(*r) for r in self.rows)); p.stdin.close()
        band=self.opened(); band.with_suffix('.consumed').touch()
        self.assertEqual(p.wait(2),70); self.assertTrue(band.exists()); self.assertFalse((self.tap/'end.ready').exists())

    def test_stop_waiting_credit(self):
        p=self.spawn(stdout=sp.DEVNULL); p.stdin.write(b''.join(wire(*r) for r in self.rows)); p.stdin.close()
        band=self.opened(); self.stop.touch(); self.assertEqual(p.wait(2),75)
        self.assertTrue(band.exists()); self.assertFalse((self.tap/'end.ready').exists())


if __name__ == '__main__': unittest.main()
