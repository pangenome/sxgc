#!/usr/bin/env python3
"""The bin shape through the ONE entry (`xsa build --v2-pipeline`), and the
same binaries driven by hand, so counter identity can be checked.

  single_entry_gate.py front  {100m|1b} <scratch root>        one-command gate
  single_entry_gate.py direct {100m|1b} <scratch root>        the two manual
                                                              invocations of
                                                              the SAME bundled
                                                              binaries

Both modes print a JSON summary; the front door additionally journals its work
counters (BIN_WORK + the finish's SLIM_FP lines) inside <root>/xsa-build.log,
and `direct` writes the producer/finish logs into the root, so the counters can
be diffed by `single_entry_compare.py`.

Nothing here touches a foreign scratch directory: the root must be fresh.
"""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

BASE = Path('/mnt/nvme2n1/erikg')
FIXTURE = {'100m': BASE / 'extcols-scratch/mergperf-100m-snap.txt',
           '1b': BASE / 'extcols-scratch/mergperf-1b-snap.txt'}
BANK = {'100m': BASE / 'extcols-scratch/xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2',
        '1b': BASE / 'extcols-scratch/pilev2-g2-1b/frag.sxi2'}
CHI = {'100m': 29716349, '1b': 283296933}
REMAP = BASE / 'sol6c-gate100m/remap-256.bin'
XSA = Path('/home/erikg/worktrees/sxgc/pi-worktree-dcc02c65-433c-4f20-bf1a-ac55d1713f96-s0-0/xsa/target/release/xsa')


def bundled_root():
    """The verified tool cache root the xsa binary itself executes (built from
    THIS tree; the same bytes the front door spawns)."""
    home = Path(os.environ.get('XDG_CACHE_HOME') or (Path.home() / '.cache'))
    return home / 'xsa'


def newest_bundle(expect_stage):
    """Pick the bundle directory whose cross_lcp_merge carries binbwt support.

    The ID is the digest of the MANIFEST, so it is not predictable from here;
    the caller passes the bundle it wants explicitly via XSA_BUNDLE.
    """
    env = os.environ.get('XSA_BUNDLE')
    if not env:
        raise SystemExit('set XSA_BUNDLE=<cache dir> (the root the front door uses)')
    root = Path(env)
    missing = [n for n in expect_stage if not (root / n).is_file()]
    if missing:
        raise SystemExit(f'{root}: missing stages {missing}')
    return root


def run(cmd, log_path, env=None):
    start = time.monotonic()
    with open(log_path, 'w') as log:
        proc = subprocess.Popen(cmd, stdout=log, stderr=log, env=env)
        rc = proc.wait()
    return rc, time.monotonic() - start


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    mode, size, root = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    threads = sys.argv[4] if len(sys.argv) > 4 else '96'
    memory_gb = sys.argv[5] if len(sys.argv) > 5 else ('24' if size == '100m' else '40')
    fixture, bank = FIXTURE[size], BANK[size]
    container = root / (fixture.stem + '.sxi2')
    root.mkdir(parents=True, exist_ok=True)
    result = dict(mode=mode, size=size, root=str(root), threads=threads,
                  memory_gb=memory_gb, fixture=str(fixture), bank=str(bank))

    if mode == 'front':
        assert not (root / 'xsa-build.log').exists(), f'root not fresh: {root}'
        cmd = [str(XSA), 'build', '--v2-pipeline', '--input', str(fixture),
               '--scratch', str(root), '--threads', threads,
               '--memory-gb', memory_gb, '--timeout', '7200',
               '--chi-expect', str(CHI[size])]
        rc, wall = run(cmd, root / 'front.log')
        result.update(command=cmd, rc=rc, wall_s=wall)
    elif mode == 'direct':
        tools = newest_bundle(['cross_lcp_merge', 'pile_finish'])
        prefix = root / 'merged' / 'frag'
        (root / 'merged').mkdir(parents=True, exist_ok=True)
        assert not Path(str(prefix) + '.rlebwt').exists(), f'root not fresh: {root}'
        env = dict(os.environ, CROSS_POOL_SLOTS=os.environ.get('CROSS_POOL_SLOTS', '8388608'),
                   CROSS_V2_PIECES='1', CROSS_V2_PIECE_WINDOW='1100')
        # The cap must be the one the PLAN picked, else the two runs process
        # different windows (same work counters, but compare exactly). The plan
        # picks cap = n whenever the memory cap allows one window.
        live_cap = os.environ.get('BIN_LIVE_CAP') or str(fixture.stat().st_size)
        result['bin_live_cap'] = int(live_cap)
        producer = [str(tools / 'cross_lcp_merge'), '--binbwt', str(fixture),
                    '--remap', str(REMAP), '--out-prefix', str(prefix),
                    '--threads', threads, '--bin-live-cap', live_cap]
        finish = [str(tools / 'pile_finish'), '--prefix', str(prefix),
                  '--corpus', str(fixture), '--remap', str(REMAP),
                  '--output', str(container), '--threads', threads,
                  '--poll-timeout', '7200', '--wit-ram-budget', str((int(memory_gb) // 2) * (1 << 30)),
                  '--chi-expect', str(CHI[size])]
        start = time.monotonic()
        with open(root / 'producer.log', 'w') as pl, open(root / 'finish.log', 'w') as fl:
            p = subprocess.Popen(producer, env=env, stdout=pl, stderr=pl)
            f = subprocess.Popen(finish, env=env, stdout=fl, stderr=fl)
            pr = p.wait()
            if pr:
                f.terminate()
            fr = f.wait()
        result.update(producer=producer, finish=finish, producer_rc=pr,
                      finish_rc=fr, wall_s=time.monotonic() - start)
        rc = pr or fr
    else:
        raise SystemExit(f'unknown mode {mode}')

    if rc == 0:
        cmp_rc = subprocess.run(['cmp', str(container), str(bank)]).returncode
        result['cmp_rc'] = cmp_rc
        rc = cmp_rc
    result['ok'] = rc == 0
    (root / f'{mode}-result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    sys.exit(0 if rc == 0 else 1)


if __name__ == '__main__':
    main()
