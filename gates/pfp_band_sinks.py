#!/usr/bin/env python3
"""I1 step 2b: BAND sinks. Run formation is band-local -- a run cannot span a
band boundary, because the bands are cut at a byte change -- so band r's writer
can encode its own m1 blocks, m12/m13 samples and m20 slice with no shared state
and the members are cat'd afterwards.

This gate drives the producer's new flags and stops at the first failure:

  (a) no new flags: the 100 MB anchor still produces 87,069,576 B / e99fb40e...
      (K=1: pfp_rows -> stream-context -> xsa_finish, no routing flags);
  (b) --band-sinks 2 to regular files: `cat rows.0 rows.1` is byte-identical to
      the single stream (1,200,006,048 B / a22b816a...), the groups' rows sum to
      n+1, and every group after the first starts on a global run index that is
      a multiple of M1_BLOCK_RUNS=1024;
  (c) the same split with each band's chain started CONCURRENTLY (both chains
      live before the producer writes): per-chain wall, per-chain
      XSA_FINISH_TIME, the producer's expand_pipe_s and the total.

Not a shipped path and not wired into `xsa build`: this measures the shape.
usage: pfp_band_sinks.py BIN_DIR XSA RAW MAP COLUMNS WORK [--cpus LIST]
           [--threads N] [--memory BYTES] [--skip-a] [--skip-c] [--timeout S]
"""
import argparse
import fcntl
import hashlib
import json
import os
import shutil
import subprocess as sp
import sys
import threading
import time
from pathlib import Path

ROW = 12
M1_BLOCK_RUNS = 1024
PIN_BYTES, PIN_SHA = 87069576, 'e99fb40e1e11d57a2a4687b4c75491ed83dd3d6308fc27fb2336dd35efdd7f28'
ROWS_BYTES, ROWS_SHA = 1200006048, 'a22b816a6fec53658ba9020e8bf36ad2eec1368c6f1aa9b8e8a4413e3dd073a3'


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 22), b''):
            h.update(block)
    return h.hexdigest()


def fifo_both_ends(path):
    r = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    try:
        w = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
    except OSError:
        os.close(r)
        raise
    for fd in (r, w):
        fl = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, fl & ~os.O_NONBLOCK)
    return r, w


def fields(text, tag):
    for line in text.splitlines():
        if line.startswith(tag):
            return dict(p.split('=', 1) for p in line.split()[1:])
    return {}


class Cfg:
    def __init__(self, args):
        self.bin = Path(args.bin).resolve()
        self.xsa = str(Path(args.xsa).resolve())
        self.raw = str(Path(args.raw).resolve())
        self.map = str(Path(args.map).resolve())
        self.columns = Path(args.columns).resolve()
        self.cpus = args.cpus
        self.threads = args.threads
        self.memory = args.memory
        n = os.path.getsize(self.raw)
        self.n = n
        self.cap = max(65536, 2 * n) - max(16384, n // 8) - 2048 - (n + 8) // 8


def columns(cfg, dest):
    """The producer RETIRES the columns it consumes, so every arm gets a copy."""
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    for name in ('parse.u64', 'phrases.ref', 'inverse', 'histogram'):
        shutil.copyfile(cfg.columns / name, dest / name)
    return dest


def producer(cfg, work, cols, log, stdout=sp.DEVNULL, band=None):
    argv = ['taskset', '-c', cfg.cpus, str(cfg.bin / 'pfp_rows'), cfg.raw, str(cols),
            str(cfg.threads), str(cfg.memory), str(cfg.cap)]
    if band:
        argv += ['--band-sinks', str(band[0]), '--band-sink-glob', band[1], '--band-manifest', band[2]]
    return sp.Popen(argv, stdout=stdout, stderr=log)


def finish(cfg, cols, out, rows, carry_in, carry_out, log, stdin):
    argv = [str(cfg.bin / 'xsa_finish'), '--from-rows', str(cols / 'histogram'), str(out.with_suffix('.scratch')), str(out)]
    if rows is not None:
        argv += ['--rows', '%d:%d' % rows, '--carry-out', carry_out]
        if carry_in:
            argv += ['--carry-in', carry_in]
    return sp.Popen(['taskset', '-c', cfg.cpus] + argv, stdin=stdin, stdout=log, stderr=log)


def chain(cfg, work, cols, groups, k, log, gate):
    """One band's chain: stream-context | head -c rows*12 | xsa_finish."""
    g = groups[k]
    last = k == len(groups) - 1
    env = dict(os.environ)
    env.pop('XSA_CTX_ROWS', None)
    env['XSA_CTX_ROWS'] = '%d:%d' % (g['first_row'], g['first_row'] + g['rows'])
    fifo = work / ('rows.%d' % k)
    ctx = sp.Popen(['taskset', '-c', cfg.cpus, cfg.xsa, 'stream-context', cfg.raw, cfg.map],
                   stdin=open(fifo, 'rb'), stdout=sp.PIPE, stderr=log, env=env)
    cut = sp.Popen(['head', '-c', str(ROW * g['rows'])], stdin=ctx.stdout, stdout=sp.PIPE, stderr=log)
    ctx.stdout.close()
    out = work / ('g%d.sxi2' % k)
    for stale in (out, out.with_suffix('.partial')):
        if stale.exists():
            stale.unlink()
    fin = finish(cfg, cols, out,
                 (g['first_row'], g['first_row'] + g['rows']),
                 str(work / ('carry.%d' % (k - 1))) if k else None,
                 str(work / ('carry.%d' % k)), log, cut.stdout)
    cut.stdout.close()
    gate['k%d' % k] = dict(ctx_start=time.time())
    return ctx, cut, fin, out


def main():
    a = argparse.ArgumentParser()
    a.add_argument('bin')
    a.add_argument('xsa')
    a.add_argument('raw')
    a.add_argument('map')
    a.add_argument('columns')
    a.add_argument('work')
    a.add_argument('--cpus', default='20-35')
    a.add_argument('--threads', type=int, default=16)
    a.add_argument('--memory', type=int, default=2147483648)
    a.add_argument('--skip-a', action='store_true')
    a.add_argument('--skip-c', action='store_true')
    a.add_argument('--timeout', type=float, default=900)
    args = a.parse_args()
    cfg = Cfg(args)
    work = Path(args.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    report = dict(bin=str(cfg.bin), raw=cfg.raw, cpus=cfg.cpus, threads=cfg.threads,
                  memory=cfg.memory, cap=cfg.cap, gates={}, stage={})
    g = report['gates']
    stop = False

    # ---- (a) no new flags: the anchor pin, and this arm's control timing ------
    if not args.skip_a:
        d = work / 'a'
        d.mkdir(exist_ok=True)
        cols = columns(cfg, d / 'cols')
        log = open(d / 'run.log', 'wb')
        os.mkfifo(str(work / 'a.rows'), 0o666) if not (work/'a.rows').exists() else None
        rfd, wfd = fifo_both_ends(str(work / 'a.rows'))
        t0 = time.time()
        env = dict(os.environ)
        env.pop('XSA_CTX_ROWS', None)
        ctx = sp.Popen(['taskset', '-c', cfg.cpus, cfg.xsa, 'stream-context', cfg.raw, cfg.map],
                       stdin=rfd, stdout=sp.PIPE, stderr=log, env=env)
        os.close(rfd)
        prod = producer(cfg, work, cols, log, stdout=wfd)
        os.close(wfd)
        out = d / 'index.sxi2'
        fin = finish(cfg, cols, out, None, None, None, log, ctx.stdout)
        ctx.stdout.close()
        rc = [prod.wait(), fin.wait()]
        ctx.wait()
        total = time.time() - t0
        log.close()
        logtext = (d / 'run.log').read_text(errors='replace')
        ok = rc == [0, 0] and out.exists() and os.path.getsize(out) == PIN_BYTES and sha256(out) == PIN_SHA
        g['a'] = dict(rc=rc, total_s=round(total, 3), bytes=os.path.getsize(out) if out.exists() else 0,
                      sha256=sha256(out) if out.exists() else '',
                      expand_pipe_s=fields(logtext, 'PFP_ROW_TIME').get('expand_pipe_s'),
                      finish=fields(logtext, 'XSA_FINISH_TIME'), pass_=ok)
        print('gate (a) %s: %d B sha256=%s total=%.3fs' % ('PASS' if ok else 'FAIL', g['a']['bytes'],
                                                          g['a']['sha256'][:16], total), flush=True)
        stop = not ok

    # ---- (b) --band-sinks 2 to files: the concatenation is the single stream --
    if not stop:
        d = work / 'b'
        d.mkdir(exist_ok=True)
        cols = columns(cfg, d / 'cols')
        log = open(d / 'run.log', 'wb')
        t0 = time.time()
        prod = producer(cfg, work, cols, log,
                        band=(2, str(d / 'rows.%d'), str(d / 'bands.json')))
        rc = prod.wait()
        producer_s = time.time() - t0
        log.close()
        logtext = (d / 'run.log').read_text(errors='replace')
        man = json.loads((d / 'bands.json').read_text()) if (d / 'bands.json').exists() else {}
        cat = sp.Popen(['bash', '-c', 'cat "%s" "%s" | sha256sum' % (d / 'rows.0', d / 'rows.1')],
                       stdout=sp.PIPE)
        got_sha = cat.communicate()[0].split()[0].decode()
        wc = sp.run(['bash', '-c', 'cat "%s" "%s" | wc -c' % (d / 'rows.0', d / 'rows.1')],
                    stdout=sp.PIPE).stdout.split()[0].decode()
        gs = man.get('groups', [])
        grid = all(x['first_run'] % M1_BLOCK_RUNS == 0 for x in gs[1:])
        cover = sum(x['rows'] for x in gs) == cfg.n + 1 if gs else False
        sizes = [os.path.getsize(d / ('rows.%d' % k)) for k in range(len(gs) or 2)]
        ok = (rc == 0 and int(wc) == ROWS_BYTES and got_sha == ROWS_SHA and bool(gs)
              and grid and cover and sizes == [ROW * x['rows'] for x, _ in zip(gs, sizes)])
        g['b'] = dict(rc=rc, producer_s=round(producer_s, 3), cat_bytes=int(wc), cat_sha256=got_sha,
                      group_sizes=sizes, manifest=man, grid=grid, coverage=cover,
                      expand_pipe_s=fields(logtext, 'PFP_ROW_TIME').get('expand_pipe_s'), pass_=ok)
        print('gate (b) %s: cat %s B sha256=%s groups=%d grid=%s producer=%.3fs'
              % ('PASS' if ok else 'FAIL', wc, got_sha[:16], len(gs), grid, producer_s), flush=True)
        stop = not ok

    # ---- (c) both band chains live before the producer writes ----------------
    if not stop and not args.skip_c:
        d = work / 'c'
        d.mkdir(exist_ok=True)
        man = g['b']['manifest']
        groups = man['groups']
        cols = columns(cfg, d / 'cols')
        for path in list(d.glob('rows.*')) + list(d.glob('carry.*')) + list(d.glob('g*.sxi2')):
            path.unlink()
        for k in range(len(groups)):
            os.mkfifo(d / ('rows.%d' % k))
        log = open(d / 'run.log', 'wb')
        gate_t = {}
        chains = []
        t0 = time.time()
        # chain 0 first: its context takes the read end of FIFO 0, which the
        # producer's open() waits for; chain 1's writer cannot start until chain
        # 0's carry is on disk (the writer loads it before it reads a row).
        chains.append(chain(cfg, d, cols, groups, 0, log, gate_t))
        ready = threading.Event()

        def second():
            while not (d / 'carry.0').exists():
                time.sleep(0.02)
            chains.append(chain(cfg, d, cols, groups, 1, log, gate_t))
            ready.set()
        threading.Thread(target=second, daemon=True).start()
        prod = producer(cfg, work, cols, log,
                        band=(len(groups), str(d / 'rows.%d'), str(d / 'bands.json')))
        rc_prod = prod.wait()
        t_prod = time.time() - t0
        outer = threading.Thread(target=lambda: None)
        ready.wait(timeout=args.timeout)
        rcs = [c[2].wait() for c in chains]
        for c in chains:
            c[0].wait()
            c[1].wait()
        total = time.time() - t0
        log.close()
        logtext = (d / 'run.log').read_text(errors='replace')
        per = {}
        for k, c in enumerate(chains):
            st = gate_t['k%d' % k]
            per['k%d' % k] = dict(wall_s=round(time.time() - st['ctx_start'], 3),
                                  out_bytes=os.path.getsize(c[3]) if c[3].exists() else 0)
        ok = rc_prod == 0 and all(r == 0 for r in rcs)
        g['c'] = dict(rc_prod=rc_prod, rc_chains=rcs, producer_s=round(t_prod, 3),
                      total_s=round(total, 3), per_chain=per,
                      expand_pipe_s=fields(logtext, 'PFP_ROW_TIME').get('expand_pipe_s'),
                      finish=fields(logtext, 'XSA_FINISH_TIME'), groups=groups, pass_=ok)
        print('gate (c) %s: total=%.3fs producer=%.3fs chains=%s' %
              ('PASS' if ok else 'FAIL', total, t_prod, per), flush=True)
        print('gate (c) XSA_FINISH_TIME ' + json.dumps(g['c']['finish']), flush=True)

    (work / 'report.json').write_text(json.dumps(report, indent=1))
    print('BAND_SINKS_REPORT ' + str(work / 'report.json'))
    return 0 if all(v.get('pass_') for v in g.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
