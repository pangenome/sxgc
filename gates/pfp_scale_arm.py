#!/usr/bin/env python3
"""One metered `xsa build --pfp` arm, run INSIDE a cgroup scope.

Qualification harness only: it drives the ordinary installed builder, samples
the private scratch tree, and records wall/CPU/IO/peak for one arm. It never
reads the product except to hash it after publication.

  systemd-run --user --quiet --scope --collect \
    -p AllowedCPUs=64-95 -p MemoryMax=200G -p MemorySwapMax=0 -p CPUQuota=3200% \
    python3 -B gates/pfp_scale_arm.py XSA RAW MAP OUTDIR --label 100m-j32 ...
"""
import argparse, json, os, re, signal, subprocess as sp, sys, time
from datetime import datetime, timezone
from hashlib import file_digest
from pathlib import Path
from zoneinfo import ZoneInfo


def parse():
    a = argparse.ArgumentParser()
    a.add_argument('xsa'); a.add_argument('raw'); a.add_argument('map'); a.add_argument('outdir')
    a.add_argument('--label', required=True)
    a.add_argument('--threads', type=int, required=True)
    a.add_argument('--memory', required=True, help='-m grant (sort array budget)')
    a.add_argument('--expected-sha', default='')
    a.add_argument('--scratch-limit', type=int, default=0, help='abort when sampled logical scratch exceeds this')
    a.add_argument('--timeout', type=float, default=0)
    a.add_argument('--keep', action='store_true')
    return a.parse_args()


def cgroup_read(name):
    path = Path('/sys/fs/cgroup') / Path('/proc/self/cgroup').read_text().strip().split('::')[1].lstrip('/') / name
    return path.read_text().strip() if path.exists() else None


def tree_bytes(root):
    size = block = 0
    for path, _dirs, names in os.walk(root):
        for name in ['.', *names]:
            try:
                s = (Path(path) / name).stat()
            except FileNotFoundError:
                continue
            size += s.st_size
            block += s.st_blocks * 512
    return size, block


def main():
    a = parse()
    xsa, raw, docs, d = (Path(v).resolve() for v in (a.xsa, a.raw, a.map, a.outdir))
    d.mkdir(parents=True, mode=0o700)
    scratch = d / 'scratch'; scratch.mkdir()
    out = d / 'index.sxi2'
    cmd = [str(xsa), 'build', '--pfp', str(raw), '--docs-map', str(docs), '-T', str(scratch),
           '-j', str(a.threads), '-m', a.memory, '-v', '-o', str(out)]
    ram, quota, swap = cgroup_read('memory.max'), cgroup_read('cpu.max'), cgroup_read('memory.swap.max')
    aff = sorted(os.sched_getaffinity(0))
    assert ram not in (None, 'max') and swap == '0' and aff, (ram, swap, aff)
    io0 = cgroup_read('io.stat')
    stamp = lambda: datetime.now(timezone.utc)
    start = stamp(); peak = [0, 0]; high = {}; state = {}
    t = time.monotonic()
    with (d / 'build.log').open('xb') as log:
        p = sp.Popen(cmd, stdout=log, stderr=log, start_new_session=True)
        try:
            while True:
                pid, status, usage = os.wait4(p.pid, os.WNOHANG)
                if pid:
                    p.returncode = os.waitstatus_to_exitcode(status)
                    break
                peak = list(map(max, peak, tree_bytes(scratch)))
                cur = cgroup_read('memory.current')
                if cur and int(cur) > state.get('high', 0):
                    state['high'] = int(cur)
                    high = {k: int(v) for k, v in (l.split() for l in (cgroup_read('memory.stat') or '').splitlines())
                            if k in ('anon', 'file', 'slab', 'shmem')}
                if a.scratch_limit and peak[0] > a.scratch_limit:
                    raise RuntimeError(f'sampled logical scratch {peak[0]} over limit {a.scratch_limit}')
                if a.timeout and time.monotonic() - t > a.timeout:
                    raise RuntimeError(f'arm timed out after {a.timeout}s')
                time.sleep(0.05)
        finally:
            if p.returncode is None:
                os.killpg(p.pid, signal.SIGKILL); p.wait()
    wall = time.monotonic() - t; end = stamp()
    mem_peak = cgroup_read('memory.peak')
    log_text = (d / 'build.log').read_text(errors='replace')
    ok = p.returncode == 0
    digest = size = None; post = 0.0
    if ok:
        size = out.stat().st_size
        t = time.monotonic()
        with out.open('rb') as f:
            digest = file_digest(f, 'sha256').hexdigest()
        post = time.monotonic() - t
        assert not a.expected_sha or digest == a.expected_sha, (digest, a.expected_sha)
    phases = [l for l in log_text.splitlines() if l.startswith(('PFP_', 'HEAD rows=', 'build --pfp:'))]
    result = dict(
        label=a.label, ok=ok, exit=p.returncode, threads=a.threads, memory_grant=a.memory,
        argv=cmd, input=str(raw), input_bytes=raw.stat().st_size, map_bytes=sum(f.stat().st_size for f in docs.glob('*') if f.is_file()),
        output=str(out), output_bytes=size, sha256=digest, expected_sha=a.expected_sha or None,
        sha_matches=(digest == a.expected_sha) if a.expected_sha and ok else None,
        utc_start=start.isoformat(), utc_closed=end.isoformat(),
        memphis_closed=end.astimezone(ZoneInfo('America/Chicago')).isoformat(),
        wall_s=wall, postpublication_sha_s=post, cpu_s=usage.ru_utime + usage.ru_stime,
        cpu_per_wall_s=(usage.ru_utime + usage.ru_stime) / wall,
        wait4_read_bytes=usage.ru_inblock * 512, wait4_write_bytes=usage.ru_oublock * 512,
        major_faults=usage.ru_majflt, minor_faults=usage.ru_minflt,
        cgroup_memory_peak=int(mem_peak) if mem_peak else None, high_sample=high,
        affinity=aff, memory_max=ram, cpu_max=quota, swap_max=swap,
        cgroup_io_before=io0, cgroup_io_after=cgroup_read('io.stat'),
        scratch_logical_sample=peak[0], scratch_allocated_sample=peak[1],
        phase_lines=phases,
        clock='outside the build; scratch subtree sampled every 0.05s; input and product excluded; '
              'logical bytes are not a filesystem quota; cgroup counters cover this scope only',
    )
    (d / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    tail = log_text[-3000:] if not ok else ''
    print(json.dumps({k: result[k] for k in ('label', 'ok', 'exit', 'threads', 'wall_s', 'cpu_per_wall_s',
                                            'cgroup_memory_peak', 'scratch_logical_sample', 'output_bytes',
                                            'sha256', 'sha_matches')}))
    if phases:
        print('\n'.join(phases))
    if tail:
        print(tail, file=sys.stderr)
    if not a.keep and ok:
        pass  # the caller owns the directory
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
