"""Linux process-level fixtures. Usage: python3 overlap_async_audit.py PATH_TO_RESIDENT"""
import array
import fcntl
import os
from pathlib import Path
import re
import select
import struct
import subprocess as sp
import sys
import tempfile
import termios
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'gates'))
from overlap_radix_test import rows
from overlap_resident_test import prepare, trigger

EXE = Path(sys.argv[1]).resolve()
ENV = {k: v for k, v in os.environ.items() if not k.startswith('XSA_')}
ENV.update(XSA_RESIDENT_PACKED='1', XSA_RESIDENT_RADIX_STEP='1', XSA_RESIDENT_RADIX_LANES='4',
           XSA_RESIDENT_ASYNC_AUDIT='1')
N = 210007  # Every eight-comma window triggers. Largest range exceeds both lane credits.


def run(d, env, delay=None, failure=None):
    r, w = os.pipe()
    fcntl.fcntl(r, fcntl.F_SETPIPE_SZ, 4096)
    if failure == 'stop-end':
        assert os.write(w, bytes(4096)) == 4096
    with os.fdopen(r, 'rb', buffering=0) as audit, os.fdopen(w, 'wb') as writer:
        with sp.Popen([EXE, d, d/'stop', '200000000'], stdout=sp.PIPE, stderr=sp.PIPE,
                      bufsize=0, pass_fds=(w,), env=dict(env, XSA_RESIDENT_AUDIT_FD=str(w))) as p:
            try:
                writer.close()
                fcntl.fcntl(p.stdout.fileno(), fcntl.F_SETPIPE_SZ, 4096)
                if failure == 'stop-end':
                    assert select.select([p.stdout], [], [], 5)[0] and len(p.stdout.read(12)) == 12
                    (d/'stop').touch()
                if failure == 'closed':
                    audit.close()
                elif failure == 'truncated':
                    assert select.select([r], [], [], 5)[0] and len(audit.read(12)) == 12
                    audit.close()
                if failure in ('stop', 'sink-closed', 'verifier-closed-with-sink-stalled'):
                    # Let the sink make progress until the verifier pipe is full. Then
                    # leave the auditor blocked while this lane runs out of credits.
                    assert select.select([p.stdout], [], [], 5)[0]
                    assert len(p.stdout.read(12)) == 12
                    # Clear the few leading singleton rows so the next large
                    # packet can occupy the complete 4096-byte verifier pipe.
                    sink_read = 12
                    prefix = 0
                    while prefix < 8192:
                        ready = select.select([r, p.stdout], [], [], 3)[0]
                        assert ready
                        if r in ready:
                            prefix += len(os.read(r, 4096))
                        if p.stdout in ready:
                            sink_read += len(os.read(p.stdout.fileno(), 8192))
                    avail = array.array('i', [0])
                    deadline = time.monotonic() + 5
                    while avail[0] < 4096 and time.monotonic() < deadline:
                        fcntl.ioctl(r, termios.FIONREAD, avail, True)
                        if select.select([p.stdout], [], [], .01)[0]:
                            sink_read += len(os.read(p.stdout.fileno(), 8192))
                    assert avail[0] == 4096
                    if failure == 'stop':
                        # Drain the product through two large packets, but do not
                        # release the verifier. The lane needs a third credit.
                        while sink_read < 12 + 2*65536*12:
                            assert select.select([p.stdout], [], [], 3)[0]
                            sink_read += len(os.read(p.stdout.fileno(), 8192))
                        (d/'stop').touch()
                    elif failure == 'sink-closed':
                        p.stdout.close()
                    else:
                        audit.close()
                    p.wait(timeout=5)
                if failure:
                    _, log = p.communicate(timeout=5)
                    assert p.returncode != 0 and b'RESIDENT_STREAM' not in log, log
                    if failure in ('stop', 'stop-end'):
                        assert p.returncode == 75, log
                        (d/'stop').unlink()
                    return
                # Force one branch not to drain until the other has received data
                # beyond END. Timeouts catch a missing tee/incorrect dependency.
                got = []
                release = threading.Event()

                def read_audit():
                    if delay == 'audit':
                        assert release.wait(5)
                    head = audit.read(12)
                    if delay == 'sink':
                        while len(head) < 65536:
                            part = audit.read(65536-len(head))
                            assert part
                            head += part
                        release.set()
                    got.append(head + audit.read())

                t = threading.Thread(target=read_audit, daemon=True)
                t.start()
                if delay == 'sink':
                    assert release.wait(5)
                head = b''
                if delay == 'audit':
                    while len(head) < 65536:
                        assert select.select([p.stdout], [], [], 4)[0]
                        part = p.stdout.read(65536-len(head))
                        assert part
                        head += part
                    release.set()
                tail, log = p.communicate(timeout=8)
                t.join(2)
                assert p.returncode == 0 and got == [head + tail], log
                return got[0], log
            finally:
                p.kill()


def main():
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        (d/'parse.u32').write_bytes(struct.pack('<I', 1)*(N-8) + bytes(4))
        (d/'dictionary.packed').write_bytes(struct.pack('<I', 0x80000008) + b','*8
                                            + struct.pack('<I', 9) + b','*9)
        for wide, row43 in [(0, 0), (1, 0), (1, 1)]:
            env = dict(ENV, XSA_RESIDENT_WIDE_DICT=str(wide), XSA_TEST_ROW43_FIXTURE=str(row43))
            expected = b''.join(struct.pack('<III', N-i, max(0, i-1),
                                (row43 << 31) | (256 if i == N else 44)) for i in range(N+1))
            serial, base = run(d, dict(env, XSA_RESIDENT_ASYNC_AUDIT='0'))
            assert serial == expected and b'RESIDENT_ASYNC_AUDIT' not in base
            bound = lambda log: int(re.search(rb' emit_bound=(\d+)', log)[1])
            for delay in ['audit', 'sink']:
                output, log = run(d, env, delay=delay)
                assert output == expected and bound(log)-bound(base) == (2 << 20) + 65536
                assert int(re.search(rb'max_range_rows=(\d+)', log)[1]) > 2*65536
                sink, audit, peak = map(int, re.search(rb'sink_bytes=(\d+) audit_bytes=(\d+) peak_inflight_blocks=(\d+)', log).groups())
                assert sink == audit == len(expected) and 1 <= peak <= 8
        # Explicit opt-in without all prerequisites takes the original path.
        one, log = run(d, dict(env, XSA_RESIDENT_RADIX_LANES='1'))
        assert one == expected and b'RESIDENT_ASYNC_AUDIT' not in log
        for fd in (None, os.open(os.devnull, os.O_WRONLY)):
            try:
                p = sp.run([EXE, d, d/'stop', '200000000'], capture_output=True, timeout=8,
                           env=env if fd is None else dict(env, XSA_RESIDENT_AUDIT_FD=str(fd)),
                           pass_fds=() if fd is None else (fd,))
                assert p.returncode == 0 and p.stdout == expected and b'RESIDENT_ASYNC_AUDIT' not in p.stderr
            finally:
                if fd is not None:
                    os.close(fd)
        for failure in ['closed', 'truncated', 'stop-end', 'stop', 'sink-closed', 'verifier-closed-with-sink-stalled']:
            run(d, ENV, failure=failure)
        tr = next(i.to_bytes(8, 'big') for i in range(10000) if trigger(i.to_bytes(8, 'big')))
        text = b''.join(tr+bytes([i])+b'common-tail!' for i in range(1, 65)) + tr
        prepare(d, text)
        ref = rows(text)
        for wide, row43 in [(0, 0), (1, 0), (1, 1)]:
            output, log = run(d/'columns', dict(ENV, XSA_RESIDENT_WIDE_DICT=str(wide),
                                                XSA_TEST_ROW43_FIXTURE=str(row43)))
            assert output == b''.join(struct.pack('<III', p, h, b | (row43 << 31))
                                      for p, h, b in struct.iter_unpack('<III', ref))
            assert int(re.search(rb' absorbed=(\d+)', log)[1]) > 0
    print('PASS 3 widths x 2 asymmetric drains, exact bytes and emit grant, multi-credit range, closed/truncated audit, full pipe STOP and sink close')


if __name__ == '__main__':
    main()
