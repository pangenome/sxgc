"""Tiny producer fixtures. Expected SHA comes from Python suffix slice order."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess as sp
import sys
import tempfile

from overlap_resident_test import prepare
from overlap_ten_dict_grid import GRID, before, score


def encoded(columns, text):
    tape = (columns/'dictionary.packed').read_bytes()
    codes = {c: j+2 for j,c in enumerate(sorted(set(text)))}
    i = 0; result = bytearray()
    while i < len(tape):
        (tag,) = struct.unpack_from('<I', tape, i); i += 4
        end = i + (tag & 0x7fffffff)
        result.extend(codes[c] for c in tape[i:end]); i = end
        if tag >> 31: result.append(1)
        result.append(0)
    return bytes(result)


def test(exe):
    base = dict(PATH=os.defpath, **GRID, XSA_TEST_ROW43_FIXTURE='1')
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp); col = d/'columns'; stop = d/'stop'
        def run(grant=200_000_000, **opts):
            return sp.run([exe,col,stop,str(grant)],env={**base,**opts}, capture_output=True)
        for text in [b'x', b'banana\0\xff'*29, bytes(range(211))*2]:
            prepare(d,text); data = encoded(col,text)
            sa = sorted(range(len(data)), key=lambda i:data[i:])
            expected = hashlib.sha256(b''.join(struct.pack('<q',i) for i in sa)).hexdigest()
            r = run(); assert r.returncode == 0 and not r.stdout,r.stderr
            log = r.stderr.decode(); arms = score(log,len(sa))['arms']
            assert all(a['sha256']==expected for a in arms) and 'RESIDENT_STREAM' not in log
            assert log.count('kernel=wide_via_narrow') == 3
        for opts in [dict(XSA_RESIDENT_AUDIT_FD='1'),dict(XSA_RESIDENT_DICT_GRID='0'),dict(XSA_TEST_ROW43_FIXTURE='0'),
                     dict(XSA_RESIDENT_WIDE_DICT='0'),dict(XSA_RESIDENT_DICT_THREADS='4'),dict(OMP_THREAD_LIMIT='2'),
                     dict(OMP_WAIT_POLICY='ACTIVE'),dict(grant=200_000_001)]:
            r = run(**opts); assert r.returncode and not r.stdout and b'DICT_GRID threads=' not in r.stderr,opts
        import re
        dict_bound = int(re.search(r'dict_bound=(\d+)',log)[1]); r = run(grant=dict_bound-1)
        assert r.returncode and not r.stdout and b'resident plan exceeds array grant' in r.stderr
        stop.touch(); r = run(); assert r.returncode == 75 and not r.stdout
        stop.unlink(); r = run(XSA_GATE_PACKED_10G='1',XSA_TEST_ROW43_FIXTURE='0')
        assert r.returncode and not r.stdout
    def line(t,secs,reads):
        return f'DICT_GRID threads={t} entries=3 wall_s={secs} cpu_s=1 peak_rss=100 read_bytes={reads} write_bytes=0 sha256={"a"*64} hash_s=1\n'
    trial=line(1,10,0)+line(4,8,0)+line(2,8.3,0)
    assert score(trial,3)['selected_threads']==2
    assert score(trial.replace(line(2,8.3,0),line(2,8.3,1)),3)['selected_threads']==4
    for bad in [trial.replace('a'*64,'f'*64,1), trial[:trial.rfind('DICT_GRID ')]]:
        try: score(bad,3)
        except AssertionError: pass
        else: raise AssertionError('accepted malformed grid')
    try: before(1)
    except AssertionError: pass
    else: raise AssertionError('accepted expired deadline')
    print('PASS naive SHA-256 of all LE i64 entries x3 arms; no rows; flags, caps, stop, digest, deadline and selection checks')

if __name__=='__main__': test(Path(sys.argv[1]).resolve())
