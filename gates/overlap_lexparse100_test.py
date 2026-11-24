"""Fixture-only. Arguments: new executable, frozen-source lexparse10 executable."""
import random
import re
import resource
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from overlap_resident_test import trigger


def oracle(text):
    starts = [0] + [i for i in range(1, len(text)-7) if trigger(text[i:i+8])]
    words = [(text[a:b+8], False) for a, b in zip(starts, starts[1:])]
    words.append((text[starts[-1]:], True))
    ids = {v: i for i, v in enumerate(sorted(set(words)))}
    tape = b''.join(struct.pack('<I', len(s) | (end << 31)) + s for s, end in ids)
    desc = sum(len(s) - (0 if end else 8) for s, end in ids)
    return (tape, b''.join(struct.pack('<I', ids[v]) for v in words), desc)


def fd_limit():
    resource.setrlimit(resource.RLIMIT_NOFILE, (96, 96))


def main(new, old):
    tr = next(i.to_bytes(8, 'big') for i in range(10000) if trigger(i.to_bytes(8, 'big')))
    rng = random.Random(1701)
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        src = d/'raw'
        cases = [b'A'*400, b'A'*76+tr, rng.randbytes(12345), (b'A'*64+tr)*5000,
                 b'A'*(1 << 20), (b'Q'*7000+tr)*80]
        for i, text in enumerate(cases):
            src.write_bytes(text)
            tape, parse, desc = oracle(text)
            out = d/f'c{i}'
            small = i in (2, 3, 5)
            p = subprocess.run([new, src, out, '--fixture-small' if small else '--fixture'], capture_output=True, preexec_fn=fd_limit)
            assert p.returncode == 0, p.stderr
            v = {k:int(n) for k,n in re.findall(r'(\w+)=(\d+)', p.stdout.decode())}
            assert v['peak_counted'] <= 2*len(text) and (v['m'], v['descriptors']) == (len(parse)//4, desc)
            k = v['k']; assert v['parse_extent'] == v['m']+6*k
            assert v['D'] == desc+9*k-7 and v['D']+3*k-1 == len(tape)
            if small: assert v['intermediate_passes'] >= 1, v
            ref = d/f'old{i}'
            subprocess.run([old, src, ref, '--fixture'], check=True, capture_output=True)
            assert sorted(x.name for x in out.iterdir()) == ['dictionary.packed', 'parse.u32']
            for name, data in [('parse.u32', parse), ('dictionary.packed', tape)]:
                assert (out/name).read_bytes() == (ref/name).read_bytes() == data
            assert src.read_bytes() == text
        for i, (text, flag, why) in enumerate([(b'x', False, b'2N-scratch'),
                (tr*1200, False, b'2N-scratch'),
                (b'A'*((1<<20)+1), False, b'phrase-cap'), (b'A'*400, True, b'requested')]):
            src.write_bytes(text)
            if flag: (d/'stop.requested').touch()
            out = d/f'fail{i}'
            p = subprocess.run([new, src, out, '--fixture-small'], capture_output=True)
            assert p.returncode == 75 and why in p.stderr and not p.stdout, p
            live, peak, cap, attempted = map(int, re.search(rb'SCRATCH live=(\d+) peak=(\d+) cap=(\d+) attempted=(\d+)', p.stderr).groups())
            assert live <= peak <= cap == 2*len(text)
            if why == b'2N-scratch': assert live+attempted > cap
            assert out.is_dir() and sum(x.stat().st_size for x in out.iterdir()) <= 2*len(text)
            if text == tr*1200: assert any(x.stat().st_size for x in out.iterdir())
            assert src.read_bytes() == text
        (d/'stop.requested').unlink()
        src.write_bytes((b'A'*64+tr)*40000)
        out = d/'mid-stop'
        with subprocess.Popen([new, src, out, '--fixture-small'], stdout=subprocess.PIPE, stderr=subprocess.PIPE) as proc:
            try:
                deadline = time.monotonic()+20
                while not (out/'r0.0').exists():
                    assert proc.poll() is None and time.monotonic()<deadline
                    time.sleep(.001)
                (d/'stop.requested').touch()
                stdout, stderr = proc.communicate(timeout=10)
                assert proc.returncode == 75 and b'requested' in stderr and not stdout
            finally:
                if proc.poll() is None: proc.kill()
        assert (out/'r0.0').exists()
        before = sorted(out.iterdir())
        p = subprocess.run([new, src, out, '--fixture-small'], capture_output=True)
        assert p.returncode == 70 and sorted(out.iterdir()) == before
        p = subprocess.run([new, src, d/'unset'], capture_output=True)
        assert p.returncode == 70 and not (d/'unset').exists()
    print('PASS packed oracle + lexparse10; forced segmented merges/skew, jumbo boundary, stop, hard 2N')


if __name__ == '__main__':
    main(*sys.argv[1:])
