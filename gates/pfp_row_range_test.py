#!/usr/bin/env python3
"""I1 step 1: the v10 writer's ROW-RANGE contract, proved on the 100 MB anchor.

`xsa_finish --from-rows HIST SCRATCH OUT` writes one whole ordered row stream.
A K-writer split (docs/pfp-emission-speedups.md, I1) needs a writer that can be
handed rows [A,B) of that stream and carry the two states that cross a boundary.
This gate proves the contract is exact:

  (a) `--rows 0:n` with no carry at all is the untouched path: the 100 MB anchor
      pin, byte for byte.
  (b) two groups, cut at a row where the BWT byte changes AND at a global run
      index divisible by M1_BLOCK_RUNS, merge to the single-group m1, m12 and
      m13 members byte for byte -- including m13's global (row, runs) header and
      the <=256 m13 tail strides the continuation group reports in its carry,
      and the union m5 the last group writes over the shared bitmap.
  (c) a cut off the m1 block grid is REFUSED with the numbers and leaves no
      output file.

The row stream itself comes from the ordinary installed builder: the bundle's
`xsa_finish` is replaced by a wrapper that copies the histogram and tees the
pipeline's bytes, so the groups are fed exactly what the producer wrote.

usage: pfp_row_range_test.py XSA BUNDLE XSA_FINISH [--raw R --map M] [--work DIR]
                              [--threads N] [--cpus LIST]
"""
import argparse, hashlib, os, shutil, struct, subprocess as sp, sys, tempfile
from pathlib import Path

ANCHOR = Path('/mnt/nvme2n1/erikg/TAPEREADER/sandbox/real-v10-map100-S4bZm0/inputs')
PIN = 'eac582ca1e51ead51239b60b1506c44cacf9b6713b675b16003f57fc4f3f98bd'   # 100 MB anchor
M1_BLOCK_RUNS = 1024          # bit6/xsa_finish.cpp: the m1 block width, = PF_RUNSTART_STRIDE
CARRY_MAGIC = 0x3130505247574f52  # "ROWGRP01" little endian, rowfinish::GroupCarry


# ------------------------------------------------------------------ containers
def container(path):
    b = Path(path).read_bytes()
    assert b[:4] == b'SXI2', (path, b[:4])
    version, = struct.unpack_from('<I', b, 4)
    n, k, r = struct.unpack_from('<QQQ', b, 8)
    count, _hs = struct.unpack_from('<II', b, 32)
    length, flags = struct.unpack_from('<QQ', b, 40)
    assert length == len(b), (path, length, len(b))
    members = {}
    for i in range(count):                      # v7: the member table TRAILS the bytes
        mid, codec, off, nbytes, cnt, _crc = struct.unpack_from('<IIQQQI', b, length - 40*count + 40*i)
        members[mid] = dict(codec=codec, off=off, bytes=nbytes, count=cnt, data=b[off:off+nbytes])
    return dict(version=version, n=n, k=k, r=r, flags=flags, members=members, size=length)


def m12_parts(m):
    stride, cnt, runs, row, l2s, l2, _a, _b = struct.unpack_from('<8Q', m['data'], 0)
    return dict(stride=stride, cnt=cnt, runs=runs, row=row, l2s=l2s, l2=l2,
                runstart=list(struct.unpack_from('<%dQ' % cnt, m['data'], 64 + 8*l2)))


def m13_layout(m):
    occ, gap, nsym, l2s, total, row, runs, entoff, l2off = struct.unpack_from('<9Q', m['data'], 0)
    assert nsym == 256 and occ == 256 and gap == 1024, (occ, gap, nsym)
    counts = list(struct.unpack_from('<256Q', m['data'], 72))
    offs = list(struct.unpack_from('<257Q', m['data'], 72 + 8*256))
    assert offs[256] == total and entoff + 24*total == len(m['data'])
    return dict(occ=occ, gap=gap, l2s=l2s, total=total, row=row, runs=runs,
                counts=counts, offs=offs, entoff=entoff, l2off=l2off)


def carry(path):
    b = Path(path).read_bytes()
    w = struct.unpack('<%dQ' % (len(b)//8), b)
    assert w[0] == CARRY_MAGIC and w[1] == 1, (hex(w[0]), w[1])
    at = 20 + 8*256                       # header words, then the eight 256-entry arrays
    return dict(n=w[2], row_base=w[3], run_base=w[4], prev=w[5], open=w[6], nfix=w[19],
                fixes=[tuple(w[at+4*i:at+4*i+4]) for i in range(w[19])])


# ---------------------------------------------------------------------- merge
def merge_m1(g0, g1, ref):
    """Blocks concatenate; the (payload, bitPos) directory prefix-sums."""
    a0, a1, ar = (x['members'][1] for x in (g0, g1, ref))
    assert g0['members'][1]['data'][:2336] == g1['members'][1]['data'][:2336] == ar['data'][:2336], 'm1 codebook'
    out, dirs, base = bytearray(ar['data'][:2336]), [], 0
    for m, runs in ((a0, a0['count']), (a1, a1['count'])):
        nb = (runs + M1_BLOCK_RUNS - 1)//M1_BLOCK_RUNS
        payload = m['data'][2336:len(m['data']) - 16*nb]
        out += payload
        for i in range(nb):
            off, bits = struct.unpack_from('<2Q', m['data'], len(m['data']) - 16*nb + 16*i)
            dirs.append((off + base, bits))
        base += len(payload)
    out += b''.join(struct.pack('<2Q', *d) for d in dirs)
    return bytes(out), dict(payload=base, blocks=len(dirs))


def merge_m12(g0, g1, ref):
    """run-start rows are global, so the arrays concatenate and the header rebuilds."""
    a0, a1, ar = (m12_parts(x['members'][12]) for x in (g0, g1, ref))
    rs = a0['runstart'] + a1['runstart']
    runs, l2s = ref['r'], ar['l2s']
    l2 = (runs + l2s - 1)//l2s
    body = [rs[j*l2s//a0['stride']] for j in range(l2)] + rs
    out = struct.pack('<8Q', a0['stride'], len(rs), runs, ref['n'], l2s, l2, 64, 64 + 8*l2)
    out += struct.pack('<%dQ' % len(body), *body)
    return out, dict(entries=len(rs), l2=l2)


def merge_m13(g0, g1, ref, fixes, owners):
    """Per-symbol directories concatenate; the carried repairs re-stamp the tail
    strides an earlier group could only close against its own end."""
    a0, a1, ar = (m13_layout(x['members'][13]) for x in (g0, g1, ref))
    ea0, ea1 = a0['entoff'], a1['entoff']
    entries = bytearray()
    start = {}
    for c in range(256):
        start[c] = len(entries)
        entries += g0['members'][13]['data'][ea0 + 24*a0['offs'][c]:ea0 + 24*a0['offs'][c+1]]
        s1 = len(entries)
        entries += g1['members'][13]['data'][ea1 + 24*a1['offs'][c]:ea1 + 24*a1['offs'][c+1]]
        if a1['counts'][c]:
            start[('g1', c)] = s1
    counts = [a0['counts'][c] + a1['counts'][c] for c in range(256)]
    # repair by (owning group, global run index): the owner is the group whose run
    # range holds the sample, whatever its row_base is.
    need = set((o, rn) for o, rn, _c, _s in fixes)
    done = set()
    for owner, run, ch, stride in fixes:
        base = start[ch] if owner == owners[0] else start[('g1', ch)]
        last0 = a0['counts'][ch]
        # the entry is the last sample of `ch` in its owner, and it is that run
        idx = None
        if owner == owners[0]:
            if last0 and 24*(last0-1) >= 0:
                pos = start[ch] + 24*(last0-1)
                if struct.unpack_from('<Q', entries, pos + 8)[0] == run: idx = pos
        else:
            if a1['counts'][ch]:
                pos = start[('g1', ch)] + 24*(a1['counts'][ch]-1)
                if struct.unpack_from('<Q', entries, pos + 8)[0] == run: idx = pos
        assert idx is not None, ('m13 repair target not found', owner, run, ch)
        struct.pack_into('<Q', entries, idx + 16, stride & 0xffff)
        done.add((owner, run))
    l2s = ar['l2s']
    l2total = sum((counts[c] + l2s - 1)//l2s for c in range(256))
    l2off = 72 + 8*(256 + 257)
    entoff = l2off + 16*l2total
    out = bytearray(struct.pack('<9Q', ar['occ'], ar['gap'], 256, l2s, sum(counts), ref['n'],
                                ref['r'], entoff, l2off))
    out += struct.pack('<256Q', *counts)
    offs = [0]
    for c in range(256):
        offs.append(offs[-1] + counts[c])
    out += struct.pack('<257Q', *offs)
    for c in range(256):
        for j in range(0, counts[c], l2s):
            pos = start[c] + 24*j
            out += struct.pack('<2Q', struct.unpack_from('<Q', entries, pos)[0],
                                       struct.unpack_from('<Q', entries, pos + 8)[0])
    out += entries
    return bytes(out), dict(entries=sum(counts), l2total=l2total, repairs=len(done))


# ------------------------------------------------------------------------ run
def file_sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def arm(cmd, stdin=None, env=None):
    fin = open(stdin, 'rb') if stdin else open(os.devnull, 'rb')
    with fin:
        return sp.run(cmd, stdin=fin, capture_output=True, env=env, timeout=3600)


def prepare(xsa, bundle, finish, work):
    tools = work/'tools'
    shutil.copytree(bundle, tools)
    real = tools/'xsa_finish.bin'
    shutil.copy(finish, real)
    real.chmod(0o700)
    (tools/'xsa_finish').write_text("#!/bin/sh\n"
                                    "set -eu\n"
                                    'cp "$2" "$XSA_ROW_RANGE_CAPTURE/histogram"\n'
                                    'exec tee "$XSA_ROW_RANGE_CAPTURE/rows.bin" | "$XSA_ROW_RANGE_CAPTURE/xsa_finish.bin" "$@"\n')
    (tools/'xsa_finish').chmod(0o700)
    (work/'cap').mkdir()
    os.link(real, work/'cap'/'xsa_finish.bin')
    (work/'scratch').mkdir()
    (work/'cache').mkdir()
    env = dict(os.environ, XSA_PFP_TOOLS=str(tools), XDG_CACHE_HOME=str(work/'cache'),
               XSA_ROW_RANGE_CAPTURE=str(work/'cap'))
    env.pop('XSA_FINISH_SPOOL', None)
    return tools, work/'cap'/'xsa_finish.bin', env


def row_tags(path, n):
    import array
    raw = Path(path).read_bytes()
    assert len(raw) >= 12*n, (len(raw), 12*n)
    words = array.array('I')
    words.frombytes(raw[0:12*n])
    return words[2::3]                        # every frame's tag


def pick_cut(path, n):
    """A row that starts a run (so the byte changes) on the m1 block grid, and one
    that starts a run OFF it -- the contract keeps the first and refuses the
    second, because m1's blocks and m12's samples both restart on that grid."""
    tags = row_tags(path, n)
    prev = tags[0] & 511
    runs = 0                                  # run index of the run starting at row r
    ongrid, offgrid = [], []
    for r in range(1, n):
        b = tags[r] & 511
        if b != prev:
            runs += 1                         # this row starts run `runs`
            if n//8 < r < 7*n//8:
                (ongrid if runs % M1_BLOCK_RUNS == 1 else offgrid).append((runs, r))
            prev = b
    assert ongrid, 'no run boundary is on the m1 block grid inside the middle of the stream'
    near = lambda cs: min(cs, key=lambda kr: abs(kr[1] - n//2))
    return near(ongrid), (near(offgrid) if offgrid else None), tags


def main():
    a = argparse.ArgumentParser()
    a.add_argument('xsa'); a.add_argument('bundle'); a.add_argument('xsa_finish')
    a.add_argument('--raw', default=str(ANCHOR/'raw'))
    a.add_argument('--map', default=str(ANCHOR/'map'))
    a.add_argument('--work', default='')
    a.add_argument('--threads', type=int, default=16)
    a.add_argument('--cpus', default='20-35')
    a.add_argument('--pin', default=PIN)
    args = a.parse_args()
    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix='xsa-row-range-'))
    work.mkdir(parents=True, exist_ok=True)
    tools, finish, env = prepare(Path(args.xsa).resolve(), Path(args.bundle).resolve(),
                                 Path(args.xsa_finish).resolve(), work)
    # ---------------------------------------------------------------- (a) pin
    index = work/'index.sxi2'
    build = ['taskset', '-c', args.cpus, str(Path(args.xsa).resolve()), 'build', '--pfp',
             args.raw, '--docs-map', args.map, '-T', str(work/'scratch'),
             '-j', str(args.threads), '-m', '2G', '-v', '-o', str(index)]
    hist, rows = work/'cap'/'histogram', work/'cap'/'rows.bin'
    if index.exists() and hist.exists() and rows.exists() and file_sha(index) == args.pin:
        digest = file_sha(index)   # a re-run of this gate reuses a verified arm
        print('PASS (a) default --rows 0:n  bytes=%d sha256=%s (reused)' % (index.stat().st_size, digest))
    else:
        p = arm(build, env=env)
        log = p.stdout + p.stderr
        assert p.returncode == 0, log[-4000:]
        digest = file_sha(index)
        assert digest == args.pin, (digest, args.pin)
        print('PASS (a) default --rows 0:n  bytes=%d sha256=%s' % (index.stat().st_size, digest))
    ref = container(index)
    n = ref['n']
    assert rows.stat().st_size > 12*n, (rows.stat().st_size, 12*n)
    # ---------------------------------------------------------------- (b) split
    (run_index, cut), offgrid, tags = pick_cut(rows, n)
    assert offgrid, 'the stream has no off-grid run boundary to refuse'
    run_index_off, cut_off = offgrid
    print('PASS (b) cut row=%d run=%d run%%%d=%d (off-grid cut row=%d run=%d) byte change=%s' %
          (cut, run_index, M1_BLOCK_RUNS, run_index % M1_BLOCK_RUNS, cut_off, run_index_off,
           (tags[cut-1] & 511) != (tags[cut] & 511)))
    parts = {}
    for name, lo, hi in (('p0', 0, cut), ('p1', cut, n)):
        with open(rows, 'rb') as f, open(work/name, 'wb') as g:
            f.seek(12*lo)
            left = 12*(hi - lo)
            while left:
                block = f.read(min(left, 1 << 24))
                g.write(block); left -= len(block)
        parts[name] = work/name
    groups = []
    for k, (lo, hi) in enumerate(((0, cut), (cut, n))):
        out, cm, bmp = work/f'idx{k}.sxi2', work/f'c{k}', work/'bmp.chi'
        cmd = [str(finish), '--from-rows', str(hist), str(work/'scratch'), str(out),
               '--rows', '%d:%d' % (lo, hi), '--carry-out', str(cm), '--chi-shared', str(bmp)]
        for stale in (out, Path(str(out) + '.partial')):
            if stale.exists():
                stale.unlink()
        if k:
            cmd += ['--carry-in', str(work/'c0')]
        p = arm(['taskset', '-c', args.cpus] + cmd, stdin=parts[f'p{k}'])
        assert p.returncode == 0, (p.stdout + p.stderr)[-4000:]
        groups.append(container(out))
    g0, g1 = groups
    c1 = carry(work/'c1')
    assert g0['n'] == g1['n'] == n and g1['r'] == ref['r'], (g0['n'], g1['n'], g1['r'], ref['r'])
    owners = (0, cut)
    checks = []
    m1, info1 = merge_m1(g0, g1, ref)
    checks.append(('m1', m1 == ref['members'][1]['data'], len(m1)))
    m12, info12 = merge_m12(g0, g1, ref)
    checks.append(('m12', m12 == ref['members'][12]['data'], len(m12)))
    m13, info13 = merge_m13(g0, g1, ref, c1['fixes'], owners)
    checks.append(('m13', m13 == ref['members'][13]['data'], len(m13)))
    checks.append(('m5', g1['members'][5]['data'] == ref['members'][5]['data'],
                   ref['members'][5]['count']))
    if m13 != ref['members'][13]['data']:
        want, got = ref['members'][13]['data'], m13
        lay = m13_layout(ref['members'][13])
        bad = []
        for c in range(256):
            for j in range(lay['counts'][c]):
                pos = lay['entoff'] + 24*(lay['offs'][c] + j)
                e = struct.unpack_from('<3Q', want, pos)
                d = struct.unpack_from('<3Q', got, pos)
                if e != d:
                    bad.append((c, e[1], e[2], d[2]))
        print('M13 DIVERGENCE entries=%d first=%s' % (len(bad), bad[:8]))
        head = struct.unpack_from('<9Q', want, 0), struct.unpack_from('<9Q', got, 0)
        print('m13 header want=%s got=%s' % (head[0], head[1]))
    for name, ok, size in checks:
        print('%s (b) merged %s == single-group %s (%d B)' % ('PASS' if ok else 'FAIL', name, name, size))
    print('PASS (b) m13 repairs=%d of %d fixes, entries=%d, l2=%d, runs=%d row=%d'
          % (info13['repairs'], len(c1['fixes']), info13['entries'], info13['l2total'],
             ref['r'], ref['n']))
    assert all(ok for _n, ok, _s in checks), 'merged members differ from the single-group writer'
    # ------------------------------------------------------------- (d) refusal
    assert run_index_off % M1_BLOCK_RUNS != 1
    p0d, p1d = work/'pd0', work/'pd1'
    for name, lo, hi in ((p0d, 0, cut_off), (p1d, cut_off, n)):
        with open(rows, 'rb') as f, open(name, 'wb') as g:
            f.seek(12*lo)
            left = 12*(hi - lo)
            while left:
                block = f.read(min(left, 1 << 24))
                g.write(block); left -= len(block)
    out0, out1 = work/'idxd0.sxi2', work/'idxd1.sxi2'
    bmp = work/'bmpd.chi'
    cmd0 = [str(finish), '--from-rows', str(hist), str(work/'scratch'), str(out0),
            '--rows', '0:%d' % cut_off, '--carry-out', str(work/'d0'), '--chi-shared', str(bmp)]
    p = arm(['taskset', '-c', args.cpus] + cmd0, stdin=p0d)
    assert p.returncode == 0, (p.stdout + p.stderr)[-2000:]
    cmd1 = [str(finish), '--from-rows', str(hist), str(work/'scratch'), str(out1),
            '--rows', '%d:%d' % (cut_off, n), '--carry-out', str(work/'d1'),
            '--chi-shared', str(bmp), '--carry-in', str(work/'d0')]
    p = arm(['taskset', '-c', args.cpus] + cmd1, stdin=p1d)
    text = (p.stdout + p.stderr).decode(errors='replace')
    assert p.returncode != 0, text[-2000:]
    assert 'm1 block grid' in text and str(M1_BLOCK_RUNS) in text, text[-2000:]
    assert not out1.exists() and not Path(str(out1) + '.partial').exists()
    line = [l for l in text.splitlines() if 'm1 block grid' in l][0]
    print('PASS (d) refused a cut at run %d (%d mod %d = %d): %s' %
          (run_index_off, run_index_off, M1_BLOCK_RUNS, run_index_off % M1_BLOCK_RUNS, line))
    print('ROW_RANGE_PASS pin=%s cut=%d run_base=%d m1=%d m12=%d m13=%d m5=%d repairs=%d'
          % (digest, cut, g0['r'], len(m1), len(m12), len(m13), ref['members'][5]['count'],
             info13['repairs']))
    if not args.work:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
