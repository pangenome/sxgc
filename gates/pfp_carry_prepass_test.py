#!/usr/bin/env python3
"""I1: the per-band writer carries, produced by ONE sequential state-only prepass.

`xsa_finish --carry-prepass BANDS_JSON ROW0 ROW1 ... --carries-out DIR --histogram H`
replays the band row files in order and saves, at each band boundary, the
GroupCarry (v2) that band's writer would have inherited from the writer before
it: the chi sweep's cross-run state, the run directories' global sampling state,
the boundary run code and the <=256 m13 tail-stride repairs.

This gate proves the three things that make it usable, in one run:

  (b) IDENTITY - the prepass's carry.1 is EXACTLY the carry the sequential chain
      produces (writer 0 over band 0, writer 1 started on writer 0's carry-out).
      Every field is compared; the first difference is printed with both values.
  (c) CONCURRENCY - both band writers start SIMULTANEOUSLY, each on its own band
      with its precomputed carry. The total is compared against the sequential
      chain's total (the sum of the two writer walls): near the SLOWER writer
      means the bands are independent, near the sum means they are still serial.
  (d) THE PREPASS WALL - the serial residue, measured on its own.

The band pair is derived from the given manifest: the cut row is the start of the
run index that manifest names (`first_run`), which is the grid-aligned cut a
writer's m1 blocks require. `--cut-shift` moves that many frames out of band 1's
head into band 0's tail; the script then CHECKS the manifest's first_run against
the prepass's own run count, so a wrong shift is refused, not commented on.
"""
import argparse
import json
import os
import shutil
import subprocess as sp
import sys
import time

CARRY_FIELDS = ['magic', 'version', 'n', 'row_base', 'run_base', 'prev', 'open', 'c',
                'len', 'h', 't', 'top', 'interior', 'first', 'm', 'p', 'p_sa_last',
                'previous_interior', 'idx_row', 'n_fixes', 'boundary_fresh']
CARRY_ARRAYS = ['rr_len', 'rr_pos', 'rr_act', 'occ', 'cum', 'lastSample',
                'lastSampleOcc', 'sample_group']
FRAME = 12


def unpack(path):
    raw = open(path, 'rb').read()
    w = [int.from_bytes(raw[i:i + 8], 'little') for i in range(0, len(raw), 8)]
    out = {}
    for i, name in enumerate(CARRY_FIELDS):
        out[name] = w[i]
    at = len(CARRY_FIELDS)
    for name in CARRY_ARRAYS:
        out[name] = w[at:at + 256]
        at += 256
    out['fixes'] = [w[i:i + 4] for i in range(at, len(w), 4)]
    return out


def compare(a, b, label_a, label_b):
    bad = 0
    for name in CARRY_FIELDS + CARRY_ARRAYS + ['fixes']:
        if a[name] != b[name]:
            if isinstance(a[name], list):
                for i, (x, y) in enumerate(zip(a[name], b[name])):
                    if x != y:
                        print('FIELD %s[%d]: %s=%s %s=%s' % (name, i, label_a, x, label_b, y))
                        bad += 1
                        break
                if len(a[name]) != len(b[name]):
                    print('FIELD %s length: %s=%d %s=%d'
                          % (name, label_a, len(a[name]), label_b, len(b[name])))
                    bad += 1
            else:
                print('FIELD %s: %s=%s %s=%s' % (name, label_a, a[name], label_b, b[name]))
                bad += 1
    return bad


def run(argv, stdin, log, cpu):
    with open(stdin, 'rb') as fin, open(log, 'wb') as lf:
        t0 = time.time()
        p = sp.run(['taskset', '-c', cpu] + argv, stdin=fin, stdout=lf, stderr=lf)
        return p.returncode, time.time() - t0, lf.name


def logfield(path, key):
    for line in open(path, errors='replace'):
        for tok in line.split():
            if tok.startswith(key + '='):
                return tok.split('=', 1)[1]
    return 'NA'


def writer(name, rows, carry_in, carry_out, chi):
    argv = [BIN, '--from-rows', os.path.join(COLS, 'histogram'),
            os.path.join(WORK, name + '.scratch'), os.path.join(WORK, name + '.sxi2'),
            '--rows', '%d:%d' % rows, '--carry-out', carry_out, '--chi-shared', chi]
    if carry_in:
        argv += ['--carry-in', carry_in]
    return argv


def main():
    global BIN, COLS, WORK, CPU
    ap = argparse.ArgumentParser()
    ap.add_argument('bin')
    ap.add_argument('--cols', required=True, help='column dir holding histogram')
    ap.add_argument('--work', required=True)
    ap.add_argument('--manifest', required=True, help="pfp_rows' bands.json for these rows")
    ap.add_argument('--rows', required=True, help='ROW0,ROW1 (in band order)')
    ap.add_argument('--cpus', default='40,41',
                    help='two idle cores: the timed arms pin one writer per core')
    ap.add_argument('--cut-shift', type=int, default=1,
                    help="frames to move from band 1's head into band 0's tail")
    a = ap.parse_args()
    BIN, COLS, WORK, CPUS = a.bin, a.cols, a.work, a.cpus.split(',')
    CPU = CPUS[0]
    os.makedirs(WORK, exist_ok=True)

    man = json.load(open(a.manifest))
    groups = man['groups']
    files = a.rows.split(',')
    if len(groups) != 2 or len(files) != 2:
        sys.exit('this gate measures a two-band split')
    shift = a.cut_shift * FRAME
    b0 = os.path.join(WORK, 'b0.rows')
    b1 = os.path.join(WORK, 'b1.rows')
    with open(b0, 'wb') as o, open(files[0], 'rb') as f0, open(files[1], 'rb') as f1:
        shutil.copyfileobj(f0, o)
        o.write(f1.read(shift))
    with open(b1, 'wb') as o, open(files[1], 'rb') as f1:
        f1.seek(shift)
        shutil.copyfileobj(f1, o)
    n0 = os.path.getsize(b0) // FRAME
    n1 = os.path.getsize(b1) // FRAME
    bands = {'groups': [{'first_row': 0, 'rows': n0, 'first_run': groups[0]['first_run']},
                        {'first_row': n0, 'rows': n1, 'first_run': groups[1]['first_run']}]}
    manifest = os.path.join(WORK, 'bands.json')
    json.dump(bands, open(manifest, 'w'))
    sys.stderr.write('CARRY_PREPASS_SPLIT band0_rows=%d band1_first_row=%d first_run=%d\n'
                     % (n0, n0, groups[1]['first_run']))

    for stale in os.listdir(WORK):
        if (stale.startswith('carry.') or stale.endswith('.sxi2') or stale.startswith('chi')
                or stale.endswith('.partial')):
            os.unlink(os.path.join(WORK, stale))

    # ------------------------------------------------------------ (d) the prepass
    plog = os.path.join(WORK, 'prepass.log')
    argv = [BIN, '--carry-prepass', manifest, b0, b1, '--carries-out', WORK,
            '--histogram', os.path.join(COLS, 'histogram')]
    t0 = time.time()
    rc = sp.run(['taskset', '-c', CPU] + argv, stderr=open(plog, 'wb')).returncode
    prepass_wall = time.time() - t0
    if rc:
        sys.exit('FAIL (d): the prepass exited %d: %s' % (rc, open(plog).read()[-2000:]))
    pre_carry = os.path.join(WORK, 'carry.1')
    if not os.path.exists(pre_carry):
        sys.exit('FAIL (d): the prepass wrote no carry.1')
    pre_first_run = int(logfield(plog, 'run_base'))
    if pre_first_run != groups[1]['first_run']:
        sys.exit('FAIL (d): the prepass closes %d runs before band 1, the manifest says %d: '
                 '--cut-shift %d is not this split\'s grid-aligned cut'
                 % (pre_first_run, groups[1]['first_run'], a.cut_shift))
    prepass_rows = int(logfield(plog, 'rows'))
    prepass_runs = int(logfield(plog, 'runs'))
    print('(d) PREPASS wall=%.3f s rows=%d runs=%d carries=%s'
          % (prepass_wall, prepass_rows, prepass_runs, logfield(plog, 'carries')))

    # ------------------------------------------- (b) the sequential chain, unchanged
    seq_in = os.path.join(WORK, 'seq.carry.0')
    seq_out = os.path.join(WORK, 'seq.carry.1')
    walls = []
    for k, (rows, cin, cout, src) in enumerate((
            ((0, n0), None, seq_in, b0),
            ((n0, n0 + n1), seq_in, seq_out, b1))):
        log = os.path.join(WORK, 'seq%d.log' % k)
        rc, wall, _ = run(writer('s%d' % k, rows, cin, cout, os.path.join(WORK, 'chi.bin')),
                          src, log, CPUS[k])
        if rc:
            sys.exit('FAIL (b): sequential writer %d exited %d: %s'
                     % (k, rc, open(log).read()[-2000:]))
        walls.append((wall, logfield(log, 'total')))
    print('(b) SEQUENTIAL w0_wall=%.3f w1_wall=%.3f total=%.3f xsa_w0=%s xsa_w1=%s'
          % (walls[0][0], walls[1][0], walls[0][0] + walls[1][0], walls[0][1], walls[1][1]))
    # carry.1 is the state at band 1's first row, i.e. what the writer of band 0
    # leaves behind: the chain produces it as writer 0's carry-OUT.
    bad = compare(unpack(pre_carry), unpack(seq_in), 'prepass', 'sequential')
    if bad:
        sys.exit('FAIL (b): %d field(s) differ between the prepass carry.1 and writer '
                 "0's carry-out (above; group arrays are 256 entries)" % bad)
    print('(b) IDENTITY carry.1 field-for-field equal: %d header words, %d arrays x256, '
          '%d repairs' % (len(CARRY_FIELDS), len(CARRY_ARRAYS), len(unpack(pre_carry)['fixes'])))

    # --------------------------------------------------------- (c) both bands at once
    # Each band gets its OWN witness bitmap in this arm: the product's shared
    # file is published by the group that ends the stream, over the union, which
    # is a serial tail (for_each_sorted asserts the popcount it counted), not a
    # property of the row work this arm times. A K-way assembler ORs the K
    # bitmaps; the sequential arm above keeps the shared file.
    for stale in ('g0.sxi2', 'g1.sxi2', 'g0.sxi2.partial', 'g1.sxi2.partial', 'chi0.bin',
                  'chi0.bin.chi-bitmap', 'chi1.bin', 'g0.carry.0', 'g1.carry.1', 'chi.bin'):
        p = os.path.join(WORK, stale)
        if os.path.exists(p):
            os.unlink(p)
    logs = [os.path.join(WORK, 'con%d.log' % k) for k in range(2)]
    fins = [open(b0, 'rb'), open(b1, 'rb')]
    outs = [open(l, 'wb') for l in logs]
    t0 = time.time()
    procs = [sp.Popen(['taskset', '-c', CPUS[k]] +
                      writer('g%d' % k, rows, cin, cout, os.path.join(WORK, 'chi%d.bin' % k)),
                      stdin=fins[k], stdout=outs[k], stderr=outs[k])
             for k, (rows, cin, cout) in enumerate((
                 ((0, n0), None, os.path.join(WORK, 'g0.carry.0')),
                 ((n0, n0 + n1), pre_carry, os.path.join(WORK, 'g1.carry.1'))))]
    codes = [p.wait() for p in procs]
    conc_wall = time.time() - t0
    for f in fins + outs:
        f.close()
    if any(codes):
        sys.exit('FAIL (c): a concurrent writer exited %s: %s'
                 % (codes, [open(l).read()[-1500:] for l in logs]))
    xsa = [logfield(l, 'total') for l in logs]
    seq_total = walls[0][0] + walls[1][0]
    slower = max(walls[0][0], walls[1][0])
    verdict = 'CONCURRENT' if conc_wall < 0.75 * seq_total else 'STILL SERIAL'
    print('(c) CONCURRENT wall=%.3f s xsa_w0=%s xsa_w1=%s | sequential total=%.3f s '
          '(slower writer %.3f s) -> %.2fx of the sum, %.2fx of the slower writer: %s'
          % (conc_wall, xsa[0], xsa[1], seq_total, slower, conc_wall / seq_total,
             conc_wall / slower, verdict))
    # Not a gate: the concurrent arm must not change one byte of either member.
    import hashlib
    for k in range(2):
        x = os.path.join(WORK, 'g%d.sxi2' % k)
        y = os.path.join(WORK, 's%d.sxi2' % k)
        hx = hashlib.sha256(open(x, 'rb').read()).hexdigest()[:16]
        hy = hashlib.sha256(open(y, 'rb').read()).hexdigest()[:16]
        print('(c) member %d: concurrent %d B %s, sequential %d B %s, %s'
              % (k, os.path.getsize(x), hx, os.path.getsize(y), hy,
                 'equal' if hx == hy and os.path.getsize(x) == os.path.getsize(y) else 'DIFFER'))
    if verdict != 'CONCURRENT':
        sys.exit('FAIL (c): the two bands took %.2f x the sequential total' % (conc_wall / seq_total))
    print('CARRY_PREPASS_PASS prepass_wall=%.3f seq_total=%.3f concurrent_wall=%.3f '
          'first_run=%d rows=%d runs=%d'
          % (prepass_wall, seq_total, conc_wall, pre_first_run, prepass_rows, prepass_runs))


if __name__ == '__main__':
    main()
