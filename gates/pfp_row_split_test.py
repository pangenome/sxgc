#!/usr/bin/env python3
"""I1 step 2: the row producer routes its ordered stream to K sinks.

`pfp_rows` writes one ordered v10 row stream. This gate proves the routing that
lets K independent writer chains consume it (docs/pfp-emission-speedups.md, I1):

  (a) the product pins still hold through the normal build. Run separately with
      `--xsa XSA` (fixtures + the 100 MB anchor), because the pins are the
      writer's and the context worker's as much as the producer's.
  (b) K=1 and K=2 come from the same columns: concatenating the two group files
      in group order equals the single-stream file BYTE FOR BYTE.
  (c) the manifest is self-consistent: the groups' row counts sum to n+1, their
      run counts sum to the total the single-stream writer reports, every
      group's first global run index is a multiple of M1_BLOCK_RUNS, and the
      first row of each later group starts a run (its byte differs from the
      previous row's byte).
  (d) the K=2 producer wall and the two groups' row/run counts.

A K=1 run WITH `--group-manifest` is also compared to the untouched K=1 stream:
the router must be byte-neutral, not merely consistent with itself.

usage: pfp_row_split_test.py PFP_ROWS COLUMNS RAW [--work DIR] [--threads N]
                            [--cpus LIST] [--xsa XSA] [--finish XSA_FINISH]
                            [--histogram HIST]
"""
import argparse, hashlib, json, os, shutil, subprocess as sp, sys, tempfile, time
from pathlib import Path

M1_BLOCK_RUNS = 1024          # stream_finish.hpp M1_BLOCK_RUNS / PF_RUNSTART_STRIDE
ROOT = Path(__file__).resolve().parent.parent


def file_sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 24), b''):
            h.update(block)
    return h.hexdigest()


def columns(source, into):
    """pfp_rows unlinks its inputs, so every arm gets its own copy."""
    into.mkdir(parents=True, exist_ok=True)
    (into/'postings').unlink(missing_ok=True)      # a re-run of this gate reuses the dir
    for name in ('parse.u64', 'phrases.ref', 'inverse', 'histogram'):
        if not (into/name).exists():
            os.link(Path(source)/name, into/name)     # a hard link is enough: read only
    return into


def run(rows, argv, stdout, cpus):
    t = time.monotonic()
    with open(stdout, 'wb') as out:
        p = sp.run(['taskset', '-c', cpus, str(rows)] + argv, stdout=out,
                   stderr=sp.PIPE, timeout=3600)
    return p, time.monotonic() - t


def phase(log, key):
    for line in log.splitlines():
        if line.startswith(key + ' '):
            return dict(kv.split('=', 1) for kv in line.split()[1:] if '=' in kv)
    raise AssertionError('missing %s line in %s' % (key, log[-2000:]))


def main():
    a = argparse.ArgumentParser()
    a.add_argument('rows'); a.add_argument('columns'); a.add_argument('raw')
    a.add_argument('--work', default='')
    a.add_argument('--threads', type=int, default=16)
    a.add_argument('--cpus', default='0-15')
    a.add_argument('--sort-bytes', type=int, default=2000000000)
    a.add_argument('--posting-bytes', type=int, default=2000000000)
    a.add_argument('--xsa', default='', help='also re-run gate (a): fixture + anchor pins')
    a.add_argument('--finish', default='', help='xsa_finish, for the single run total (c)')
    args = a.parse_args()
    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix='xsa-row-split-'))
    work.mkdir(parents=True, exist_ok=True)
    rows = Path(args.rows).resolve()
    budget = [str(args.threads), str(args.sort_bytes), str(args.posting_bytes)]
    n = Path(args.raw).stat().st_size
    print('columns=%s raw=%d rows=%d threads=%d cpus=%s work=%s'
          % (args.columns, n, n + 1, args.threads, args.cpus, work))

    # ------------------------------------------------- (b) the untouched stream
    p, wall1 = run(rows, [args.raw, str(columns(args.columns, work/'c1')), *budget],
                   work/'rows.k1', args.cpus)
    log1 = p.stderr.decode(errors='replace')
    assert p.returncode == 0, log1[-4000:]
    one = phase(log1, 'PFP_ROWS')
    assert int(one['rows']) == n + 1, one
    assert (work/'rows.k1').stat().st_size == 12*(n+1), (work/'rows.k1').stat().st_size
    print('PASS (b) K=1 no flags: %d B sha256=%s wall=%.1fs'
          % ((work/'rows.k1').stat().st_size, file_sha(work/'rows.k1'), wall1))

    # a routed single group must be the same bytes, and it reports the run total
    p, _ = run(rows, [args.raw, str(columns(args.columns, work/'c1m')), *budget,
                      '--group-manifest', str(work/'groups.k1.json')],
               work/'rows.k1r', args.cpus)
    log1r = p.stderr.decode(errors='replace')
    assert p.returncode == 0, log1r[-4000:]
    assert file_sha(work/'rows.k1r') == file_sha(work/'rows.k1'), 'the router moved a byte'
    m1 = json.loads((work/'groups.k1.json').read_text())
    assert len(m1['groups']) == 1 and m1['rows'] == n + 1, m1
    runs_total = m1['runs']
    print('PASS (b) routed K=1 is byte-identical; manifest runs=%d groups=%d'
          % (runs_total, len(m1['groups'])))

    # ---------------------------------------------------------- (d) K=2 arms
    arms = []
    for name, extra in (('grid', ['--split-runs', str(M1_BLOCK_RUNS*max(1, round(runs_total/2/M1_BLOCK_RUNS)))]),
                        ('share', [])):                    # the default equal-share rule
        p, wall = run(rows, [args.raw, str(columns(args.columns, work/('c2'+name))), *budget,
                             '--sinks', '2', '--sink-glob', str(work/('rows.k2.'+name+'.%d')),
                             '--group-manifest', str(work/('groups.k2.'+name+'.json')), *extra],
                      work/('out.k2.'+name), args.cpus)
        log = p.stderr.decode(errors='replace')
        assert p.returncode == 0, log[-4000:]
        parts = [work/('rows.k2.'+name+'.%d' % g) for g in range(2)]
        assert all(x.exists() for x in parts), parts
        concat = work/('rows.k2.'+name)
        with open(concat, 'wb') as g:
            for part in parts:
                with open(part, 'rb') as f:
                    shutil.copyfileobj(f, g, 1 << 24)
        same = file_sha(concat) == file_sha(work/'rows.k1')
        print('%s (b) K=2 %-5s -> %d + %d B, concatenation %s the single stream, wall=%.1fs'
              % ('PASS' if same else 'FAIL', name, parts[0].stat().st_size,
                 parts[1].stat().st_size, '==' if same else '!=', wall))
        assert same, 'K=2 %s concatenation differs from the K=1 stream' % name
        arms.append((name, wall, json.loads((work/('groups.k2.'+name+'.json')).read_text()),
                     phase(log, 'PFP_ROW_TIME'), parts, concat))

    # ------------------------------------------------------ (c) self-consistency
    tags = None
    for name, wall, m, timing, parts, concat in arms:
        gs = m['groups']
        assert m['rows'] == n + 1 and sum(g['rows'] for g in gs) == n + 1, m
        assert m['runs'] == runs_total, (name, m['runs'], runs_total)
        assert sum(g['runs'] for g in gs) == runs_total, (name, gs, runs_total)
        assert gs[0]['first_row'] == 0 and gs[0]['first_run'] == 0, gs[0]
        for i, g in enumerate(gs):
            assert g['first_run'] % M1_BLOCK_RUNS == 0, (name, i, g)
            assert g['runs'] > 0 and g['rows'] > 0, (name, i, g)
            if i:
                prev = gs[i-1]
                assert g['first_row'] == prev['first_row'] + prev['rows'], (name, i, prev, g)
                assert g['first_run'] == prev['first_run'] + prev['runs'], (name, i, prev, g)
                # the cut is between runs: this group's first row opens a new one
                if tags is None:
                    import array
                    words = array.array('I')
                    with open(work/'rows.k1', 'rb') as f:
                        while True:
                            b = f.read(12*(1 << 16))
                            if not b:
                                break
                            words.frombytes(b)
                    tags = words[2::3]
                here = tags[g['first_row']] & 511
                there = tags[g['first_row'] - 1] & 511
                assert here != there, (name, i, g['first_row'], here, there)
                assert here == g['first_byte'], (name, i, here, g['first_byte'])
        print('PASS (c) %-5s rows=%d runs=%d groups=%d cuts=%s'
              % (name, m['rows'], m['runs'], len(gs),
                 [(g['first_row'], g['rows'], g['first_run'], g['runs'], g['first_byte']) for g in gs]))

    # ------------------------------------ the writer's own total for the same stream
    if args.finish:
        import struct
        out = work/'single.sxi2'
        trailer = struct.pack('<QQQ', 0, 1, 0)     # empty m20, one document, empty m15
        # xsa_finish wants the row stream and then the context worker's trailer.
        with open(work/'rows.k1', 'rb') as f:
            p = sp.run(['taskset', '-c', args.cpus, args.finish, '--from-rows',
                        str(Path(args.columns)/'histogram'), str(work), str(out)],
                       input=f.read() + trailer, stderr=sp.PIPE, timeout=3600)
        log = p.stderr.decode(errors='replace')
        assert p.returncode == 0, log[-4000:]
        writer_runs = int(phase(log, 'XSA_FINISH_TIME')['runs'])
        assert writer_runs == runs_total, (writer_runs, runs_total)
        print('PASS (c) single-stream writer reports runs=%d == asset total %d' % (writer_runs, runs_total))

    # ------------------------------------------------------------------ (d) wall
    for name, wall, m, timing, parts, concat in arms:
        gs = m['groups']
        print('(d) K=2 %-5s wall=%.1fs (K=1 wall was not measured on the same core set)'
              ' order_s=%s expand_pipe_s=%s rows=[%s] runs=[%s]'
              % (name, wall, timing.get('order_s'), timing.get('expand_pipe_s'),
                 ','.join(str(g['rows']) for g in gs), ','.join(str(g['runs']) for g in gs)))

    # ------------------------------------------------------------ (a) the pins
    if args.xsa:
        for gate in ('pfp_generated_v10_test.py',):
            p = sp.run([sys.executable, str(ROOT/'gates'/gate), args.xsa], capture_output=True,
                       timeout=900, env=dict(os.environ, XSA_PFP_TOOLS=str(rows.parent)))
            print(p.stdout.decode(errors='replace') + p.stderr.decode(errors='replace')[-4000:])
            assert p.returncode == 0, 'gate (a) %s failed' % gate
        print('PASS (a) fixture pins hold with the routed producer in the bundle')

    print('ROW_SPLIT_PASS rows=%d runs=%d arms=%d' % (n + 1, runs_total, len(arms)))
    if not args.work:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
