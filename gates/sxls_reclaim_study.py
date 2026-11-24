"""Small private tmpfs credit study. Argument: pfp_sort_r Rust test executable."""
import hashlib
import json
import os
from pathlib import Path
import resource
import select
import struct
import subprocess
import sys
import tempfile
import time
from contextlib import contextmanager

from overlap_resident_gate import footprint

MODE = 'XSA_PRIVATE_SXLS_RECLAIM'
W, N = 8, 1024


@contextmanager
def sink(on=True, resident=True, spool=False, window=W):
    with tempfile.TemporaryDirectory(dir='/dev/shm') as tmp, tempfile.TemporaryFile() as log:
        root = Path(tmp)
        bands = root/'bands'
        bands.mkdir()
        env = {k: v for k, v in os.environ.items() if not k.startswith(('SS_', 'XSA_'))}
        if on:
            env[MODE] = '1'
        env.update(MARKER_STUDY_DIR=str(bands), XSA_TEST_RESIDENT_TRIPLES=str(int(resident)),
                   XSA_FINISH_SPOOL=str(int(spool)), SS_LCP_SLICE_WINDOW=str(window))
        with subprocess.Popen([sys.argv[1], '--ignored', '--exact', 'unit::sxls_reclaim_test::private_sink',
                               '--nocapture', '--test-threads=1'], env=env, stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=log, bufsize=0) as p:
            try:
                yield p, bands, log
            finally:
                if p.poll() is None:
                    p.kill()


def send(p):
    p.stdin.write(b'publish\n')


def publish(p, u):
    send(p)
    while select.select([p.stdout], [], [], 3)[0]:
        line = p.stdout.readline()
        assert line, f'unexpected EOF at {u}'
        if line.endswith(f'published {u}\n'.encode()):
            return
    raise TimeoutError(f'slice {u}')


def consume(d, i):
    payload = d/f'band_{i}.lcp'
    assert payload.with_suffix('.lcp.done').read_bytes() == b''
    data = payload.read_bytes()
    assert data == struct.pack('<IIQQQQQBBQQ', 0x534c5853, 1, 2, 1, 300, 2*i, 1, 1, 255, 2*i+1, 300)
    payload.unlink()
    payload.with_suffix('.lcp.consumed').touch(exist_ok=False)
    return data


def child_cpu():
    r = resource.getrusage(resource.RUSAGE_CHILDREN)
    return r.ru_utime + r.ru_stime


def arm(on):
    start, cpu0 = time.monotonic(), child_cpu()
    digest = hashlib.sha256()
    scan_wall = scan_cpu = allocated_peak = 0
    with sink(on) as (p, d, log):
        for u in range(N):
            # Exactly W-1 payloads remain after copying the oldest at steady state.
            publish(p, u)
            if u >= W-1:
                digest.update(consume(d, u-W+1))
            if (u+1) % 32 == 0:
                wall, cpu = time.monotonic(), time.process_time()
                allocated, parts = footprint(d.parent, 0, ram=True)
                scan_wall += time.monotonic()-wall
                scan_cpu += time.process_time()-cpu
                assert parts['ram_slices'] == (W-1)*66
                allocated_peak = max(allocated_peak, allocated)
        for i in range(N-W+1, N):
            digest.update(consume(d, i))
        # At a quiescent barrier: task-attributed I/O, not page-cache misses.
        io = dict(line.split(': ') for line in Path(f'/proc/{p.pid}/io').read_text().splitlines())
        status = Path(f'/proc/{p.pid}/status').read_text().splitlines()
        hwm_kib = int(next(l.split()[1] for l in status if l.startswith('VmHWM:')))
        p.stdin.close()
        assert p.wait(3) == 0
        log.seek(0)
        lines = log.readlines()
        gate_s = int(next(l.split()[1] for l in lines if l.startswith(b'scratch_gate_ns '))) / 1e9
        samples = [int(l.split(b' bands=')[1].split()[0]) for l in lines if b'phase=marker-study' in l]
        assert samples == [min(i+1, W)*66 for i in range(N)]
        markers = len(list(d.iterdir()))
        assert markers == 2*(W if on else N)
        # Exclude tempdir/test cleanup from measured arm.
        return dict(reclaim=on, slices=N, payload_sha256=digest.hexdigest(), final_markers=markers,
                    scan_cpu_s=scan_cpu, scan_wall_s=scan_wall, sink_cpu_s=child_cpu()-cpu0,
                    arm_wall_s=time.monotonic()-start, sampled_allocated_peak=allocated_peak,
                    sink_sampled_logical_peak=max(samples), sink_scratch_gate_wall_s=gate_s,
                    sink_io={k: int(io[k]) for k in ('read_bytes', 'write_bytes', 'rchar', 'wchar')},
                    sink_hwm_kib=hwm_kib)


def refused(p, log, rc, msg):
    assert p.wait(3) == rc
    log.seek(0)
    assert msg.encode() in log.read()


def safety():
    for scenario in ['nonresident', 'spool', 'zero-window', 'stale', 'active', 'nonempty', 'symlink', 'shared', 'missing_done', 'stop']:
        with sink(resident=scenario != 'nonresident', spool=scenario == 'spool',
                  window=0 if scenario == 'zero-window' else W) as (p, d, log):
            done, credit = d/'band_0.lcp.done', d/'band_0.lcp.consumed'
            if scenario in ('nonresident', 'spool', 'zero-window', 'stale'):
                if scenario == 'stale':
                    done.touch()
                send(p)
                refused(p, log, 2, 'requires fresh' if scenario == 'stale' else 'requires resident')
                assert done.exists() == (scenario == 'stale')
                continue
            for i in range(W):
                publish(p, i)
            if scenario == 'stop':
                consume(d, 1)  # Later credit is not credit for slice zero.
                send(p)
                time.sleep(.05)
                assert p.poll() is None and not (d/f'band_{W}.lcp').exists()
                (d.parent/'stop').touch()
                refused(p, log, 75, 'where=lcp-slice')
                assert done.exists() and (d/'band_1.lcp.consumed').exists()
                continue
            if scenario == 'active':
                credit.touch()
            else:
                consume(d, 0)
                if scenario == 'nonempty':
                    credit.write_bytes(b'x')
                elif scenario == 'symlink':
                    done.unlink()
                    done.symlink_to(credit)
                elif scenario == 'shared':
                    os.link(credit, d/'other')
                else:
                    done.unlink()
            send(p)
            if scenario == 'missing_done':
                refused(p, log, 2, 'marker metadata')
            else:
                refused(p, log, 2, 'without retired payload' if scenario == 'active' else 'regular markers')
            assert credit.exists() and done.exists() == (scenario != 'missing_done')
            assert (d/'band_0.lcp').exists() == (scenario == 'active')
            assert not (d/f'band_{W}.lcp').exists()
    print('refusals/stop passed', flush=True)


if __name__ == '__main__':
    safety()
    for mode in (False, True, True, False):
        print(json.dumps(arm(mode), sort_keys=True), flush=True)
