#!/usr/bin/env python3
"""I1 K-pipeline QUALIFICATION arm: does routing the ordered row stream to K
(context, writer) chains buy wall time?

This is a measurement harness, not a shipped path. Nothing here is wired into
`xsa build`; the three moving parts it drives already exist and are committed:

  * the producer routes its ordered stream to K sinks on the m1 block grid
    (`pfp_rows --sinks K --sink-glob DIR/rows.%d --group-manifest PATH`);
  * the writer accepts a contiguous rank range with an explicit group carry
    (`xsa_finish --from-rows HIST SCRATCH OUT --rows A:B --carry-in F
    --carry-out F --chi-shared F`);
  * the context worker builds a range-local m20 (`XSA_CTX_ROWS=A:B`).

Per K the arm is:

  1. `pfp_rows RAW COLUMNS T M CAP` (K=1, the control: no split flags) or the
     same plus the routing flags, writing group g's rows into FIFO g so the
     producer and the chains overlap exactly as they do in the product build;
  2. K chains `xsa stream-context RAW MAP | [head -c rows*12] | xsa_finish ...`
     reading those FIFOs. A group that does not end the stream must hand the
     writer rows only (the writer refuses a trailer there), so its range-local
     context trailer is cut by `head`; the last group keeps it, and that is what
     carries m20/m15/m5 (m5 is written once over the union of the shared bitmap);
  3. the K group containers are assembled into one SXI2 by the rules the writer's
     row-range contract states and /tmp/i1-rowrange/proto.py proved byte for
     byte: m1 payload concat + prefix-summed (payload,bitPos) directory, m12
     runStart concat with rebuilt l2 samples, m13 per-char concat with the
     carried tail repairs, m11 concat;
  4. the assembled container is compared member by member with a K=1 run of the
     same columns.

A group's row count is not known until the producer has run (the cut is on the
run grid at the group's share of the observed run/row rate), so every K>1 arm
runs the producer twice: once with the sinks drained to /dev/null to obtain the
manifest, once for real. The two manifests are compared, so a non-deterministic
split is refused rather than silently mis-measured.

Gates (the arm stops on a failure):
  (i)   K=1 reproduces the pin (--pin/--pin-bytes), the current 100 MB anchor;
  (ii)  K=2 assembled m1/m12/m13/m5 are byte-identical to the control;
  (iii) the K=2 assembled container opens with `xsa sxi-info` and
        `xsa query --count` answers the control for three patterns.

m20 is NOT assembled. A group's context trailer is range-local (XSLICE02) and
only two adjacent ranges can be merged today, so the assembled container carries
the last group's range-local trailer in slot 20 and the report says so; comparing
m20 needs the group row streams on disk (they are streamed here) and is left out.

usage: pfp_split_arm.py XSA RAW MAP COLUMNS WORKDIR [--bin DIR] [--threads N]
           [--memory M] [--split-runs R] [--ks 1,2,4] [--cpus LIST]
           [--pin SHA [--pin-bytes N]] [--patterns P ...] [--timeout S]
"""
import argparse
import fcntl
import hashlib
import json
import os
import shutil
import struct
import subprocess as sp
import sys
import threading
import time
import zlib
from pathlib import Path

ROW = 12                      # finiteemit::Row on the wire: pos, lcp, tag
M1_BLOCK_RUNS = 1024          # the m1 block grid the row-range contract cuts on
M1_HEADER = 2336
CODEC = {1: 116, 5: 105, 10: 110, 11: 111, 12: 112, 13: 115}
FLAGS_M20_M15 = 256 | 512


# ------------------------------------------------------------------ containers
def directory(path):
    """The member table of one SXI2, without decoding any member body."""
    with open(path, 'rb') as f:
        head = f.read(64)
        assert head[:4] == b'SXI2', (path, head[:4])
        version, = struct.unpack_from('<I', head, 4)
        n, k, r = struct.unpack_from('<QQQ', head, 8)
        count, hs = struct.unpack_from('<II', head, 32)
        length, flags = struct.unpack_from('<QQ', head, 40)
        assert length == os.path.getsize(path), (length, os.path.getsize(path))
        assert hs == 64 + 40 * count, (hs, count)
        f.seek(length - 40 * count)
        members = {}
        for _ in range(count):
            mid, codec, off, nbytes, cnt, crc = struct.unpack_from('<IIQQQI', f.read(40))
            members[mid] = dict(codec=codec, off=off, bytes=nbytes, count=cnt, crc=crc)
    return dict(path=str(path), version=version, n=n, k=k, r=r, count=count, hs=hs,
                flags=flags, members=members, size=length, head=head)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


class Member:
    """A container member as an offset-addressable view of the assembly file."""

    def __init__(self, mm, c, mid):
        m = c['members'][mid]
        self.mm, self.off, self.bytes, self.count, self.codec = mm, m['off'], m['bytes'], m['count'], m['codec']

    def __len__(self):
        return self.bytes

    def __getitem__(self, key):
        if isinstance(key, slice):
            lo, hi, _ = key.indices(self.bytes)
            return bytes(self.mm[self.off + lo:self.off + hi])
        return self.mm[self.off + key]


def write_container(path, n, k, r, flags, members):
    """members: list of (id, codec, count, chunks). Layout is the one
    ContainerOut::finish writes: 64-byte header, 8-aligned members in ID order,
    trailing 40-byte directory, header CRC over header + directory."""
    members = sorted(members, key=lambda m: m[0])
    with open(path, 'wb') as f:
        f.write(b'\0' * 64)
        at, table = 64, []
        for mid, codec, cnt, chunks in members:
            pad = (-at) % 8
            if pad:
                f.write(b'\0' * pad)
                at += pad
            begin, state = at, 0
            for chunk in chunks:
                if not chunk:
                    continue
                f.write(chunk)
                state = zlib.crc32(chunk, state)
                at += len(chunk)
            table.append((mid, codec, begin, at - begin, cnt, state))
        pad = (-at) % 8
        f.write(b'\0' * pad)
        at += pad
        # a directory entry is 40 bytes: the C writer's (id, codec, off, bytes,
        # count, crc) plus four zero bytes of padding
        for e in table:
            f.write(struct.pack('<IIQQQI', *e) + b'\0' * 4)
        length = at + 40 * len(table)
        head = bytearray(64)
        head[0:4] = b'SXI2'
        struct.pack_into('<I', head, 4, 10)
        struct.pack_into('<QQQ', head, 8, n, k, r)
        struct.pack_into('<II', head, 32, len(table), 64 + 40 * len(table))
        struct.pack_into('<QQ', head, 40, length, flags)
        hc = zlib.crc32(bytes(head), 0xffffffff)
        for e in table:
            hc = zlib.crc32(struct.pack('<IIQQQI', *e) + b'\0' * 4, hc)
        struct.pack_into('<I', head, 56, (~hc) & 0xffffffff)
        f.seek(0)
        f.write(bytes(head))
    return length


# ---------------------------------------------------------------- merge rules
def byte_size(text):
    """`-m` takes BYTES on the producer's command line (tools/build_pfp.py turns
    `2G` into its number before it is passed): accept the same spelling here."""
    t = str(text).strip()
    mult = 1
    if t and t[-1] in 'KMGTP':
        mult = 1 << (10 * ('KMGTP'.index(t[-1]) + 1))
        t = t[:-1]
    return int(float(t) * mult)


def acc(counts):
    out, s = [], 0
    for x in counts:
        out.append(s)
        s += x
    out.append(s)
    return out


def merge_m1(groups, runs_of_group):
    """m1: the 2336-byte head is group-invariant, payload blocks concatenate and
    their (payload, bitPos) directory prefix-sums by the preceding payload."""
    heads, payloads, dirs = [], [], []
    for g, runs in zip(groups, runs_of_group):
        d = g[1]
        nb = (runs + M1_BLOCK_RUNS - 1) // M1_BLOCK_RUNS
        pay = len(d) - M1_HEADER - 16 * nb
        assert pay >= 0, (len(d), nb)
        assert struct.unpack_from('<Q', d, 2312)[0] == M1_BLOCK_RUNS
        heads.append(d[:M1_HEADER])
        payloads.append(d[M1_HEADER:M1_HEADER + pay])
        dirs.append([struct.unpack_from('<2Q', d, M1_HEADER + pay + 16 * i) for i in range(nb)])
    assert all(h == heads[0] for h in heads), 'm1 head is not group-invariant'
    merged, base = [], 0
    for payload, entries in zip(payloads, dirs):
        for bitpos, bits in entries:
            merged.append(struct.pack('<2Q', bitpos + base, bits))
        base += len(payload)

    out = [heads[0]] + payloads + merged
    return out, M1_HEADER + base + 16 * len(merged)


def merge_m12(groups, n, runs):
    """m12: runStart is indexed by the GLOBAL run, so the arrays concatenate and
    the head is rebuilt for the whole run count (stride and l2 stride are
    group-invariant)."""
    parsed = []
    for g in groups:
        d = g[12]
        stride, cnt, gruns, row, l2s, l2, a, b = struct.unpack_from('<8Q', d, 0)
        body = struct.unpack_from('<%dQ' % (l2 + cnt), d, 64)
        assert a == 64 and b == 64 + 8 * l2 and len(body) == l2 + cnt
        parsed.append((stride, l2s, list(body[l2:])))
    assert all(p[0] == parsed[0][0] and p[1] == parsed[0][1] for p in parsed), 'm12 head moved'
    stride, l2s = parsed[0][0], parsed[0][1]
    runstart = [v for p in parsed for v in p[2]]
    assert len(runstart) == runs, (len(runstart), runs)
    l2 = (runs + l2s - 1) // l2s
    l2vals = [runstart[j * l2s // stride] for j in range(l2)]
    head = struct.pack('<8Q', stride, runs, runs, n, l2s, l2, 64, 64 + 8 * l2)
    return [head,
            struct.pack('<%dQ' % len(l2vals), *l2vals),
            struct.pack('<%dQ' % runs, *runstart)]


def carry_fixes(path):
    """The m13 tail-stride repairs a continued group reports about earlier ones:
    (owner group's row_base, global run index, symbol, the stride write() would
    have written for that entry)."""
    b = Path(path).read_bytes()
    assert len(b) % 8 == 0 and len(b) >= 8 * 20
    w = struct.unpack('<%dQ' % (len(b) // 8), b)
    assert w[0] == 0x3130505247574f52, hex(w[0])
    nfix = w[19]
    at = 20 + 8 * 256
    assert len(w) == at + 4 * nfix, (len(w), at, nfix)
    return [(w[at + 4 * i], w[at + 4 * i + 1], w[at + 4 * i + 2], w[at + 4 * i + 3]) for i in range(nfix)]


def merge_m13(groups, carries, row_base_of_group, runs, n):
    """m13: per-char sample lists concatenate in group order, once each continued
    group's reported tail stride is re-stamped into the entry that owns it."""
    parsed = []
    for g in groups:
        d = bytearray(g[13])
        occ, gap, nsym, l2s, total, row, gruns, entoff, l2off = struct.unpack_from('<9Q', d, 0)
        counts = list(struct.unpack_from('<256Q', d, 72))
        offs = list(struct.unpack_from('<257Q', d, 72 + 8 * 256))
        assert nsym == 256 and offs == acc(counts) and sum(counts) == total
        assert 72 + 8 * (256 + 257) + 16 * sum((c + l2s - 1) // l2s for c in counts) == entoff
        assert entoff + 24 * total == len(d), (entoff, total, len(d))
        parsed.append(dict(d=d, occ=occ, gap=gap, l2s=l2s, counts=counts, offs=offs,
                           entoff=entoff, l2off=l2off))
    patched = 0
    for path in carries:
        if not path:
            continue
        for owner_row, run, c, stride in carry_fixes(path):
            p = parsed[row_base_of_group[owner_row]]
            assert p['counts'][c], ('m13 repair has no entry', owner_row, c)
            at = p['entoff'] + 24 * (p['counts'][c] - 1)
            got, = struct.unpack_from('<Q', p['d'], at)
            assert got == run, ('m13 repair names run %d, entry holds %d' % (run, got))
            struct.pack_into('<Q', p['d'], at + 16, stride & 0xffff)
            patched += 1
    occ, gap, l2s = parsed[0]['occ'], parsed[0]['gap'], parsed[0]['l2s']
    assert all(p['occ'] == occ and p['gap'] == gap and p['l2s'] == l2s for p in parsed), 'm13 head moved'
    ents = [b''.join(p['d'][p['entoff'] + 24 * p['offs'][c]:p['entoff'] + 24 * p['offs'][c + 1]]
                     for p in parsed) for c in range(256)]
    counts = [len(e) // 24 for e in ents]
    l2tot = sum((x + l2s - 1) // l2s for x in counts)
    l2off = 72 + 8 * (256 + 257)
    entoff = l2off + 16 * l2tot
    out = [struct.pack('<9Q', occ, gap, 256, l2s, sum(counts), n, runs, entoff, l2off),
           struct.pack('<256Q', *counts),
           struct.pack('<257Q', *acc(counts))]
    for c in range(256):
        e = ents[c]
        for j in range(0, counts[c], l2s):
            out.append(e[24 * j:24 * j + 16])
    out.extend(ents)
    return out, patched, sum(counts)


def assemble(out_path, group_paths, carries, row_bases, runs_of_group, control):
    """Assemble the K group containers into one whole-range container."""
    dirs = [directory(p) for p in group_paths]
    for c, runs in zip(dirs, runs_of_group):
        assert c['members'][1]['count'] == runs, (c['members'][1]['count'], runs)
    runs = sum(runs_of_group)
    last = dirs[-1]
    assert last['r'] == runs and last['n'] == control['n'], (last['r'], runs, last['n'], control['n'])
    assert last['flags'] & 1 and last['flags'] & FLAGS_M20_M15 == FLAGS_M20_M15, hex(last['flags'])
    assert last['flags'] == control['flags'], (hex(last['flags']), hex(control['flags']))
    opens = [open(p, 'rb') for p in group_paths]
    mm = [m for m in (__import__('mmap').mmap(f.fileno(), 0, access=__import__('mmap').ACCESS_READ)
                      for f in opens)]
    try:
        groups = [{mid: Member(m, c, mid) for mid in c['members']} for m, c in zip(mm, dirs)]
        m1, m1_bytes = merge_m1([{1: g[1]} for g in groups], runs_of_group)
        m12 = merge_m12(groups, last['n'], runs)
        m13, patched, m13_entries = merge_m13(groups, carries, row_bases, runs, last['n'])
        parts, sparse_total = [], 0
        for g in groups:
            d = g[11]
            stride, cnt = struct.unpack_from('<2Q', d, 0)
            assert stride == 10 and len(d) == 16 + 8 * cnt
            parts.append(d[16:])
            sparse_total += cnt
        members = []
        for mid in (1, 4, 5, 7, 10, 11, 12, 13, 15, 20):
            if mid == 1:
                members.append((1, CODEC[1], runs, m1))
            elif mid == 12:
                members.append((12, CODEC[12], runs, m12))
            elif mid == 13:
                members.append((13, CODEC[13], runs, m13))
            elif mid == 11:
                members.append((11, CODEC[11], sparse_total,
                                [struct.pack('<2Q', 10, sparse_total)] + parts))
            elif mid == 10:
                # the member is a count with no body: the writer stamps the
                # GLOBAL run count
                members.append((10, CODEC[10], runs, []))
            else:
                members.append((mid, groups[-1][mid].codec, groups[-1][mid].count,
                                [bytes(groups[-1][mid])]))
        size = write_container(out_path, last['n'], last['k'], runs, last['flags'], members)
        return dict(bytes=size, runs=runs, m13_tail_repairs=patched, m13_entries=m13_entries,
                    m1_bytes=m1_bytes, m12_bytes=sum(len(x) for x in m12),
                    m13_bytes=sum(len(x) for x in m13), sparse_samples=sparse_total)
    finally:
        for m in mm:
            m.close()
        for f in opens:
            f.close()


# ------------------------------------------------------------------- the arms
class Sampler(threading.Thread):
    """Peak RSS per process, from /proc/<pid>/status VmHWM."""

    def __init__(self):
        super().__init__(daemon=True)
        self.stop = threading.Event()
        self.pids = {}
        self.peak = {}

    def run(self):
        while not self.stop.is_set():
            for tag, pid in list(self.pids.items()):
                try:
                    with open('/proc/%d/status' % pid) as f:
                        for line in f:
                            if line.startswith('VmHWM:'):
                                self.peak[tag] = max(self.peak.get(tag, 0), int(line.split()[1]) * 1024)
                                break
                except OSError:
                    pass
            time.sleep(0.25)


def fifo_both_ends(path):
    """Both ends of a FIFO, opened without blocking and left blocking, so the two
    children can be given one each before either runs."""
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


def drain_fifo(fifo, log):
    """A dry-run sink: read the rows and throw them away."""
    return sp.Popen(['cat'], stdin=open(fifo, 'rb'), stdout=sp.DEVNULL, stderr=log)


def producer_run(bin_dir, cfg, K, split_runs, sink_glob=None, manifest=None, stdout=None, log=None):
    n = os.path.getsize(cfg.raw)
    budget = max(65536, 2 * n)
    reserve = max(16384, n // 8)
    bitmap = (n + 8) // 8
    cap = budget - reserve - 2048 - bitmap
    argv = ['taskset', '-c', cfg.cpus, str(bin_dir / 'pfp_rows'), cfg.raw, str(cfg.cols),
            str(cfg.threads), str(cfg.memory), str(cap)]
    if K > 1:
        argv += ['--sinks', str(K), '--sink-glob', sink_glob, '--group-manifest', manifest]
        if split_runs:
            argv += ['--split-runs', str(split_runs)]
    return sp.Popen(argv, stdout=stdout if stdout is not None else sp.DEVNULL, stderr=log)


def fresh_columns(cfg, dest):
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    for name in ('parse.u64', 'phrases.ref', 'inverse', 'histogram'):
        shutil.copyfile(Path(cfg.columns) / name, dest / name)
    return dest


def split_manifest(bin_dir, cfg, work, K, split_runs):
    """The producer decides its group boundaries from the observed run/row rate,
    so the counts a chain needs exist only after one pass. Drain the sinks, read
    the manifest, and refuse anything that is not a clean partition."""
    if K == 1:
        return [dict(first_row=0, rows=os.path.getsize(cfg.raw) + 1, first_run=0, runs=0, first_byte=0)], 1
    dry = work / ('dry%d' % K)
    cols = fresh_columns(cfg, dry / 'columns')
    groups = dry / 'groups'
    groups.mkdir(parents=True, exist_ok=True)
    log = open(dry / 'producer.log', 'wb')
    fifos = []
    for g in range(K):
        f = groups / ('rows.%d' % g)
        if f.exists():
            f.unlink()
        os.mkfifo(f)
        fifos.append(f)
    cfg_dry = type('C', (), dict(cfg.__dict__, cols=str(cols)))
    p = producer_run(bin_dir, cfg_dry, K, split_runs, str(groups / 'rows.%d'),
                     str(dry / 'groups.json'), log=log)
    drains = [drain_fifo(str(f), log) for f in fifos]
    rc = p.wait()
    for d in drains:
        d.wait()
    log.close()
    assert rc == 0, 'dry producer failed, see %s' % (dry / 'producer.log')
    m = json.load(open(dry / 'groups.json'))
    gs = m['groups']
    assert gs[0]['first_row'] == 0 and sum(g['rows'] for g in gs) == m['rows'], gs
    assert [g['first_row'] for g in gs] == [sum(x['rows'] for x in gs[:i]) for i in range(len(gs))]
    assert sum(g['runs'] for g in gs) == m['runs']
    shutil.rmtree(dry)
    return gs, m['runs']


def arm(bin_dir, cfg, work, K, split_runs, timeout):
    groups, total_runs = split_manifest(bin_dir, cfg, work, K, split_runs)
    if len(groups) != K:
        print('NOTE K=%d: the producer cut only %d groups (the stream ran out of grid '
              'boundaries); reporting %d.' % (K, len(groups), len(groups)))
    K = len(groups)
    runs_of_group = [g['runs'] for g in groups]
    row_base_of_group = {g['first_row']: i for i, g in enumerate(groups)}
    real = work / ('k%d' % K)
    cols = fresh_columns(cfg, real / 'columns')
    gdir = real / 'groups'
    gdir.mkdir(parents=True, exist_ok=True)
    scratch = real / 'scratch'
    scratch.mkdir(exist_ok=True)
    out = real / 'index.sxi2'
    if out.exists():
        out.unlink()
    chi = gdir / 'chi.bmp'
    if chi.exists():
        chi.unlink()
    # a container left by an interrupted arm must not be appended to (the writer
    # creates its .partial with O_EXCL)
    for stale in list(gdir.glob('g*.sxi2')) + list(gdir.glob('g*.partial')):
        stale.unlink()
    hist = str(cols / 'histogram')
    log = open(real / 'run.log', 'wb')
    fifos = []
    for g in range(K):
        f = gdir / ('rows.%d' % g)
        if f.exists():
            f.unlink()
        os.mkfifo(f)
        fifos.append(f)

    sampler = Sampler()
    sampler.start()
    t0 = time.time()
    stages = {}
    carry_ready = [threading.Event() for _ in range(K)]
    live = []

    def start_ctx(k, rfd=None):
        g = groups[k]
        last = k == K - 1
        env = dict(os.environ)
        for key in ('XSA_CTX_ROWS', 'XSA_CTX_MERGE_IN'):
            env.pop(key, None)
        if not last:
            env['XSA_CTX_ROWS'] = '%d:%d' % (g['first_row'], g['first_row'] + g['rows'])
        ctx = sp.Popen(['taskset', '-c', cfg.cpus, cfg.xsa, 'stream-context', cfg.raw, cfg.map],
                       stdin=(rfd if rfd is not None else open(fifos[k], 'rb')),
                       stdout=sp.PIPE, stderr=log, env=env)
        sampler.pids['k%d.g%d.ctx' % (K, k)] = ctx.pid
        live.append(ctx)
        src, head = ctx.stdout, None
        if not last:
            # the writer accepts a group's rows only: cut the range-local trailer
            head = sp.Popen(['head', '-c', str(ROW * g['rows'])], stdin=src, stdout=sp.PIPE, stderr=log)
            src.close()
            sampler.pids['k%d.g%d.cut' % (K, k)] = head.pid
            live.append(head)
            src = head.stdout
        stages['g%d' % k] = dict(ctx_start=time.time())
        return ctx, head, src

    def start_finish(k, ctx, head, src):
        g = groups[k]
        last = k == K - 1
        # the control writes the product's container name; a group writes its own
        argv = [str(bin_dir / 'xsa_finish'), '--from-rows', hist, str(scratch),
                str(out if K == 1 else gdir / ('g%d.sxi2' % k))]
        if not (K == 1 and g['first_row'] == 0):
            argv += ['--rows', '%d:%d' % (g['first_row'], g['first_row'] + g['rows'])]
            if k:
                argv += ['--carry-in', str(gdir / ('carry.%d' % (k - 1)))]
            if not last:
                argv += ['--carry-out', str(gdir / ('carry.%d' % k))]
            argv += ['--chi-shared', str(chi)]
        fin = sp.Popen(['taskset', '-c', cfg.cpus] + argv, stdin=src, stdout=log, stderr=log)
        src.close()
        sampler.pids['k%d.g%d.finish' % (K, k)] = fin.pid
        live.append(fin)
        stages['g%d' % k]['finish_start'] = time.time()
        return fin

    def drain(k, ctx, head, src, fin):
        stages['g%d' % k]['finish_rc'] = fin.wait()
        stages['g%d' % k]['finish_end'] = time.time()
        try:
            stages['g%d' % k]['ctx_rc'] = ctx.wait(timeout=120)
        except sp.TimeoutExpired:
            ctx.kill()
            stages['g%d' % k]['ctx_rc'] = 'killed'
        if head is not None:
            head.wait()
        if k < K - 1:
            carry_ready[k].set()

    def chain(k):
        if k:
            assert carry_ready[k - 1].wait(timeout), 'group %d carry never appeared' % (k - 1)
        fins[k] = start_finish(k, *started[k])
        installed[k].set()

    def watchdog():
        time.sleep(timeout)
        print('TIMEOUT after %ds: killing the arm' % timeout, file=sys.stderr)
        for p in list(live):
            if p.poll() is None:
                p.kill()
        if 'prod' in sampler.pids:
            os.kill(sampler.pids['prod'], 9)

    started = {}
    cfg_real = type('C', (), dict(cfg.__dict__, cols=str(cols)))
    t_prod_start = time.time()
    if K == 1:
        # the control: the parent owns both ends of the single sink, so the
        # context worker and the producer can be started in either order
        rfd, wfd = fifo_both_ends(str(fifos[0]))
        started[0] = start_ctx(0, rfd)
        prod = producer_run(bin_dir, cfg_real, 1, 0, stdout=wfd, log=log)
        os.close(rfd)
        os.close(wfd)
    else:
        # the router opens its K sinks itself, so it goes first and blocks in
        # open() until each chain's context worker takes the read end
        prod = producer_run(bin_dir, cfg_real, K, split_runs, str(gdir / 'rows.%d'),
                            str(real / 'groups.json'), log=log)
        for k in range(K):
            started[k] = start_ctx(k)
    sampler.pids['prod'] = prod.pid
    live.append(prod)
    dog = threading.Thread(target=watchdog, daemon=True)
    dog.start()
    fins = [None] * K
    installed = [threading.Event() for _ in range(K)]
    waiters = []
    for k in range(K):
        if k == 0:
            fins[0] = start_finish(0, *started[0])
            installed[0].set()
        else:
            t = threading.Thread(target=chain, args=(k,))
            t.start()
            waiters.append(t)
    rc_prod = prod.wait()
    t_prod_end = time.time()
    for k in range(K):
        assert installed[k].wait(timeout), 'group %d writer never started' % k
        drain(k, *started[k], fins[k])
    for t in waiters:
        t.join()
    total = time.time() - t0
    sampler.stop.set()
    log.close()
    if os.path.exists(gdir / 'groups.json'):
        got = json.load(open(gdir / 'groups.json'))
        assert got['groups'] == groups, 'the split moved between the dry and the real run'
    finish_rcs = [stages['g%d' % k]['finish_rc'] for k in range(K)]
    assert rc_prod == 0 and all(r == 0 for r in finish_rcs), (rc_prod, finish_rcs, str(real / 'run.log'))
    report = dict(K=K, split_runs=split_runs, runs=total_runs,
                  groups=[{k2: g[k2] for k2 in ('first_row', 'rows', 'first_run', 'runs', 'first_byte')}
                          for g in groups],
                  producer_s=round(t_prod_end - t_prod_start, 3),
                  total_s=round(total, 3),
                  stage_s={k: {'ctx': round(stages[k]['finish_end'] - stages[k]['ctx_start'], 3),
                               'finish': round(stages[k]['finish_end'] - stages[k]['finish_start'], 3),
                               'start': round(stages[k]['ctx_start'] - t0, 3),
                               'end': round(stages[k]['finish_end'] - t0, 3)}
                           for k in stages},
                  peak_rss_bytes=sampler.peak,
                  container=str(out), group_containers=[str(gdir / ('g%d.sxi2' % k)) for k in range(K)],
                  carries=[str(gdir / ('carry.%d' % k)) for k in range(K - 1)],
                  work=str(real))
    return report, row_base_of_group, runs_of_group


def compare(control, assembled, mids):
    ok, rows = True, {}
    for mid in mids:
        a = control['members'][mid]
        b = assembled['members'][mid]
        same = a['bytes'] == b['bytes'] and a['count'] == b['count']
        if same:
            with open(control['path'], 'rb') as fa, open(assembled['path'], 'rb') as fb:
                fa.seek(a['off'])
                fb.seek(b['off'])
                while True:
                    x = fa.read(1 << 22)
                    y = fb.read(1 << 22)
                    if x != y:
                        same = False
                        break
                    if not x:
                        break
        rows[mid] = dict(control_bytes=a['bytes'], assembled_bytes=b['bytes'],
                         control_count=a['count'], assembled_count=b['count'], identical=same)
        ok = ok and same
    return ok, rows


def main():
    a = argparse.ArgumentParser()
    a.add_argument('xsa')
    a.add_argument('raw')
    a.add_argument('map')
    a.add_argument('columns')
    a.add_argument('workdir')
    a.add_argument('--bin', default='', help='stage dir with xsa_finish and a pfp_rows that has the routing flags')
    a.add_argument('--threads', type=int, default=16)
    a.add_argument('--memory', default='2G')
    a.add_argument('--split-runs', type=int, default=0)
    a.add_argument('--ks', default='1,2,4')
    a.add_argument('--cpus', default='0-63')
    a.add_argument('--pin', default='')
    a.add_argument('--pin-bytes', type=int, default=0)
    a.add_argument('--patterns', nargs='*', default=[])
    a.add_argument('--control', default='', help='a pin-verified K=1 container to compare against')
    a.add_argument('--timeout', type=float, default=1800)
    args = a.parse_args()
    cfg = type('C', (), dict(xsa=str(Path(args.xsa).resolve()), raw=str(Path(args.raw).resolve()),
                             map=str(Path(args.map).resolve()), columns=str(Path(args.columns).resolve()),
                             threads=args.threads, memory=str(byte_size(args.memory)),
                             cpus=args.cpus))
    bin_dir = Path(args.bin).resolve() if args.bin else Path(cfg.xsa).parent
    assert (bin_dir / 'xsa_finish').is_file(), 'no xsa_finish in %s' % bin_dir
    work = Path(args.workdir).resolve()
    work.mkdir(parents=True, exist_ok=True)
    ks = [int(x) for x in args.ks.split(',')]
    result = dict(label='pfp-split-arm', raw=cfg.raw, map=cfg.map, columns=cfg.columns,
                  threads=cfg.threads, memory=cfg.memory, cpus=cfg.cpus,
                  bin=str(bin_dir), split_runs=args.split_runs, arms={})
    control = directory(args.control) if args.control else None
    for K in ks:
        print('== K=%d' % K, flush=True)
        rep, row_base_of_group, runs_of_group = arm(bin_dir, cfg, work, K, args.split_runs, args.timeout)
        rep['sha256'] = sha256(rep['container'])
        rep['bytes'] = os.path.getsize(rep['container'])
        arm_dir = Path(rep['work'])
        if K == 1:
            control = directory(rep['container'])
            rep['control'] = dict(bytes=rep['bytes'], sha256=rep['sha256'],
                                  members=len(control['members']), runs=control['r'])
            if args.pin:
                rep['pin'] = dict(expected=args.pin, bytes=args.pin_bytes,
                                  sha_match=rep['sha256'] == args.pin,
                                  bytes_match=(not args.pin_bytes) or rep['bytes'] == args.pin_bytes)
                print('   gate (i) %s: %d B sha256=%s' % (
                    'PASS' if rep['pin']['sha_match'] and rep['pin']['bytes_match'] else 'FAIL',
                    rep['bytes'], rep['sha256']), flush=True)
                assert rep['pin']['sha_match'] and rep['pin']['bytes_match'], rep['pin']
            result['arms']['k1'] = rep
            continue
        asm = arm_dir / ('assembled.sxi2')
        print('   assembling %d group containers' % K, flush=True)
        info = assemble(str(asm), rep['group_containers'], rep['carries'], row_base_of_group,
                        runs_of_group, control)
        rep['assembled'] = dict(info, sha256=sha256(asm), bytes=os.path.getsize(asm))
        asmd = directory(asm)
        ok, members = compare(control, asmd, (1, 12, 13, 5))
        rep['assembled']['member_equality'] = members
        with open(asm, 'rb') as f:
            f.seek(asmd['members'][20]['off'] + 184)
            magic = f.read(8)
        rep['assembled']['m20'] = dict(
            control_bytes=control['members'][20]['bytes'],
            assembled_bytes=asmd['members'][20]['bytes'],
            assembled_magic=magic.decode('latin1'),
            note='m20 is the LAST group\'s range-local trailer (XSLICE02), not the '
                 'whole-range trie: the two-range merge only covers K=2 and the group row '
                 'streams are not kept on disk here, so m20 is not compared')
        info_sxi = sp.run(['taskset', '-c', cfg.cpus, cfg.xsa, 'sxi-info', str(asm)],
                          capture_output=True, text=True)
        rep['assembled']['sxi_info_rc'] = info_sxi.returncode
        rep['assembled']['sxi_info'] = info_sxi.stdout.strip().splitlines()[:1]
        ok_info = info_sxi.returncode == 0
        counts = {}
        ok_q = True
        for p in args.patterns[:3]:
            cmd = ['taskset', '-c', cfg.cpus, cfg.xsa, 'query', '%s', p, '--count']
            c0 = sp.run([c if c != '%s' else control['path'] for c in cmd], capture_output=True, text=True)
            c1 = sp.run([c if c != '%s' else str(asm) for c in cmd], capture_output=True, text=True)
            same = c0.returncode == 0 and c1.returncode == 0 and c0.stdout == c1.stdout
            counts[p] = dict(control=c0.stdout.strip(), assembled=c1.stdout.strip(),
                             rc=(c0.returncode, c1.returncode), same=same)
            ok_q = ok_q and same
        rep['assembled']['query_count'] = counts
        print('   gate (ii) %s: m1/m12/m13/m5 bit-identical'
              % ('PASS' if ok else 'FAIL'), flush=True)
        print('   gate (iii) %s: sxi-info rc=%d, %d/%d patterns answer the same'
              % ('PASS' if ok_info and ok_q else 'FAIL', info_sxi.returncode,
                 sum(1 for v in counts.values() if v['same']), len(counts)), flush=True)
        rep['gates'] = dict(members_identical=ok, opens=ok_info, query_matches=ok_q)
        result['arms']['k%d' % K] = rep
        if not (ok and ok_info and ok_q):
            result['STOPPED_AT'] = K
            break
    out = work / 'report.json'
    out.write_text(json.dumps(result, indent=1) + '\n')
    print(json.dumps({k: dict(producer_s=v['producer_s'], total_s=v['total_s'],
                              bytes=v['bytes'])
                      for k, v in result['arms'].items()}, indent=1))
    print('report %s' % out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
