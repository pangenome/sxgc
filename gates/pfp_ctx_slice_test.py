#!/usr/bin/env python3
"""I1 step 3: the m20 context trailer built per ROW SLICE and merged, byte-identically.

`xsa stream-context RAW MAP` reads one whole ordered row stream and appends the
m20/m15 trailer. A K-range pipeline (docs/pfp-emission-speedups.md, I1) needs that
trailer built from a contiguous rank range [A,B) of the same stream, and two
adjacent ranges merged into the payload of the whole range.

  XSA_CTX_ROWS=<rows>   the range's row count (already landed)
  XSA_CTX_ROWS=<A>:<B>  the global rank range it covers: the range is built as a
                        MERGEABLE PARTIAL and the Builder starts at global row A
  XSA_CTX_MERGE_IN=F    merge the partial of the adjacent LOWER range in first, so
                        the two ranges together emit the payload of [A0,B)

This gate proves the layer on the 100 MB anchor:

  (a) the single range is untouched: the whole-product pin, and the whole-range
      `stream-context` payload is the m20 member of that product byte for byte;
  (b) the rows are cut at a row where the 8-byte context CHANGES, so the two halves
      share a prefix chain of depth >= 1, the hardest case for the merge: the
      merged member equals the single pass's member byte for byte;
  (c) the CLI and generated-v10 gates still pass.

usage: pfp_ctx_slice_test.py XSA [--raw R] [--map M] [--work DIR] [--index I]
                                 [--rows ROWS] [--cut ROW] [--threads N] [--cpus LIST]
"""
import argparse, array, hashlib, heapq, os, shutil, struct, subprocess as sp, sys, tempfile
from pathlib import Path

ANCHOR = Path('/mnt/nvme2n1/erikg/TAPEREADER/sandbox/real-v10-map100-S4bZm0/inputs')
PIN = 'eac582ca1e51ead51239b60b1506c44cacf9b6713b675b16003f57fc4f3f98bd'   # 100 MB anchor
HEAD, TRIE_SECTIONS, PACK_BLOCK, PACK_DIR = 192, 10, 256, 40


# ------------------------------------------------------------------ containers
def container(path):
    b = Path(path).read_bytes()
    assert b[:4] == b'SXI2', (path, b[:4])
    n, = struct.unpack_from('<Q', b, 8)
    count, = struct.unpack_from('<I', b, 32)
    length, = struct.unpack_from('<Q', b, 40)
    assert length == len(b), (length, len(b))
    members = {}
    for i in range(count):
        mid, codec, off, nbytes, cnt, _crc = struct.unpack_from(
            '<IIQQQI', b, length - 40*count + 40*i)
        members[mid] = dict(off=off, bytes=nbytes, count=cnt, data=b[off:off+nbytes])
    return dict(n=n, members=members)


def file_sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


# ------------------------------------------------------------- member decoding
def packed(d, count):
    """The block-packed u64 array at d, as a list (the reader's own layout)."""
    total, = struct.unpack_from('<Q', d, 0)
    assert total == count and struct.unpack_from('<Q', d, 8)[0] == PACK_BLOCK, 'packed head'
    blocks, = struct.unpack_from('<Q', d, 16)
    base, = struct.unpack_from('<Q', d, 24)
    assert base == PACK_DIR and blocks == (count + PACK_BLOCK - 1)//PACK_BLOCK, (blocks, count)
    base += 16*blocks          # the block payloads follow the (off, span) directory
    out = []
    for blk in range(blocks):
        off, span = struct.unpack_from('<QI', d, PACK_DIR + 16*blk)
        b = d[base + off: base + off + span]
        n = min(PACK_BLOCK, count - blk*PACK_BLOCK)
        mode = b[0]
        if mode == 0:
            out += [0]*n
            continue
        if mode == 1:
            vals, esc_at, limit = list(b[1:1+n]), 1 + n, 255
        else:
            assert mode == 2, mode
            w = b[1]
            esc_at, limit = 2 + (n*w + 7)//8, (1 << w) - 1
            vals = []
            for j in range(n):
                bit = j*w
                p = 2 + bit//8
                acc = int.from_bytes(b[p:p+5], 'little')
                vals.append((acc >> (bit % 8)) & limit)
        esc = struct.unpack_from('<I', b, esc_at)[0]
        for e in range(esc):
            idx, = struct.unpack_from('<H', b, esc_at + 4 + 10*e)
            val, = struct.unpack_from('<Q', b, esc_at + 6 + 10*e)
            assert vals[idx] == limit, 'escape list disagrees with the packed value'
            vals[idx] = val
        out += vals
    return out


class Payload:
    """The m20 member payload as columns (I1 step 3 compares them field by field)."""
    def __init__(self, d):
        self.d = d
        self.ver, self.depth, self.min_support, self.n, self.corpus_len, self.nodes, \
            self.edges, self.docs_min_support = struct.unpack_from('<8Q', d, 0)
        self.offs = list(struct.unpack_from('<%dQ' % TRIE_SECTIONS, d, 64))
        self.root, = struct.unpack_from('<Q', d, 144)
        self.source, self.source_mem, self.boundary = struct.unpack_from('<3Q', d, 152)
        self.nfixed = d[self.offs[0]:]
        self.cols = ['nfixed'] + ['nedge', 'ncount', 'ecode', 'echild', 'ecount',
                                  'dndocs', 'dtop', 'dfirst', 'dids']
        self.cache = {}

    def col(self, name):
        if name not in self.cache:
            i = self.cols.index(name)
            n = self.nodes if name in ('nedge', 'ncount', 'dndocs', 'dtop', 'dfirst') else self.edges
            if name == 'dids':
                n, = struct.unpack_from('<Q', self.d, self.offs[i])
            self.cache[name] = packed(self.d[self.offs[i]:], n)
        return self.cache[name]

    def node(self, i):
        o = 4*i
        b = self.nfixed[o:o+4]
        return b[0], struct.unpack_from('<H', b, 1)[0], b[3]

    def first_edges(self):
        if 'starts' not in self.cache:
            acc, starts = 0, []
            for v in self.col('nedge'):
                acc += v          # the column is a delta, so first_edge sums THROUGH i
                starts.append(acc)
            self.cache['starts'] = starts
        return self.cache['starts']

    def children(self, i):
        depth, nchild, _f = self.node(i)
        first = self.first_edges()[i]
        return [(self.col('ecode')[first+j], self.col('ecount')[first+j],
                 self.col('echild')[first+j]) for j in range(nchild)]


def diverge(ref, got):
    """The first node and field that differ, expected vs got (the STOP report)."""
    if (ref.nodes, ref.edges, ref.root) != (got.nodes, got.edges, got.root):
        return ('shape', 'nodes/edges/root',
                (ref.nodes, ref.edges, ref.root), (got.nodes, got.edges, got.root))
    starts_r, starts_g = ref.first_edges(), got.first_edges()
    for i in range(ref.nodes):
        a, b = ref.node(i), got.node(i)
        if a != b:
            return ('node', i, 'depth/nchild/flags', a, b)
        ra, ga = starts_r[i], starts_g[i]
        if ref.col('ncount')[i] != got.col('ncount')[i]:
            return ('node', i, 'count', ref.col('ncount')[i], got.col('ncount')[i])
        for j in range(a[1]):
            for f in ('ecode', 'ecount', 'echild'):
                x, y = ref.col(f)[ra+j], got.col(f)[ga+j]
                if x != y:
                    return ('node', i, 'child %d %s' % (j, f), x, y)
        for f in ('dndocs', 'dtop', 'dfirst'):
            x, y = ref.col(f)[i], got.col(f)[i]
            if x != y:
                return ('node', i, f, x, y)
        fa, fb = ref.node(i)[2], got.node(i)[2]
        if fa & 1 and not fa & 2:      # both estimators: the ids are not stored
            pass
        elif ref.col('dfirst')[i] != got.col('dfirst')[i]:
            pass
        else:
            x = ref.col('dfirst')[i]
            if x != 0xFFFFFFFFFFFFFFFF:
                n = ref.col('dndocs')[i]
                for k in range(n):
                    if ref.col('dids')[x+k] != got.col('dids')[got.col('dfirst')[i]+k]:
                        return ('node', i, 'dids[%d]' % k,
                                ref.col('dids')[x+k], got.col('dids')[got.col('dfirst')[i]+k])
    return ('bytes', 'identical columns but different bytes', None, None)


# ------------------------------------------------------------------------ cuts
def pick_cut(rows, raw, n, want):
    """A row where the 8-byte context CHANGES but a prefix survives: the halves then
    share a chain of nodes, which is what the merge has to join."""
    text = Path(raw).read_bytes()
    with open(rows, 'rb') as f:
        lo = max(1, want - 200000)
        f.seek(12*lo)
        frame = f.read(12*400000)
    best = None
    for k in range(len(frame)//12 - 1):
        r = lo + k + 1
        (p0, t0), (p1, t1) = (struct.unpack_from('<I', frame, 12*k)[0],
                              struct.unpack_from('<I', frame, 12*k + 8)[0]), \
                             (struct.unpack_from('<I', frame, 12*k + 12)[0],
                              struct.unpack_from('<I', frame, 12*k + 20)[0])
        a = p0 | (((t0 >> 9) & 2047) << 32)
        b = p1 | (((t1 >> 9) & 2047) << 32)
        wa = text[a:a+8] if a + 8 <= n else text[a:] + b'\0'*(8 - min(8, n - a))
        wb = text[b:b+8] if b + 8 <= n else text[b:] + b'\0'*(8 - min(8, n - b))
        if wa == wb:
            continue
        lcp = 0
        while lcp < 8 and wa[lcp] == wb[lcp]:
            lcp += 1
        key = (abs(r - want), -lcp)
        if best is None or key < best[0]:
            best = (key, r, lcp, wa, wb)
    return best[1], best[2], best[3], best[4]


# ------------------------------------------------------------------------- run
def arm(cmd, rows, lo, hi, env):
    """Feed rows [lo,hi) with EOF at the boundary, as a group's pipe would."""
    with open(rows, 'rb') as src:
        src.seek(12*lo)
        p = sp.Popen(['head', '-c', str(12*(hi - lo))], stdin=src, stdout=sp.PIPE)
        try:
            r = sp.run(cmd, stdin=p.stdout, capture_output=True, env=env, timeout=7200)
        finally:
            p.stdout.close()
            p.wait()
    assert r.returncode == 0, (r.stdout + r.stderr)[-4000:]
    return r


def trailer(out, lo, hi):
    """The (payload, m15) the stage appended after echoing the range's rows."""
    at = 12*(hi - lo)
    length, = struct.unpack_from('<Q', out, at)
    payload = out[at+8: at+8+length]
    assert len(payload) == length, (len(payload), length)
    return payload, out[at+8+length:]


def main():
    a = argparse.ArgumentParser()
    a.add_argument('xsa'); a.add_argument('--raw', default=str(ANCHOR/'raw'))
    a.add_argument('--map', default=str(ANCHOR/'map'))
    a.add_argument('--work', default='')
    a.add_argument('--index', default='', help='a pin-verified index to reuse for (a)')
    a.add_argument('--rows', default='', help='a captured row stream of that index')
    a.add_argument('--cut', type=int, default=0)
    a.add_argument('--threads', type=int, default=16)
    a.add_argument('--cpus', default='20-35')
    a.add_argument('--pin', default=PIN)
    args = a.parse_args()
    xsa = str(Path(args.xsa).resolve())
    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix='xsa-ctx-slice-'))
    work.mkdir(parents=True, exist_ok=True)
    index = Path(args.index) if args.index else work/'index.sxi2'
    rows = Path(args.rows) if args.rows else work/'rows.bin'
    # ---------------------------------------------------------------- (a) pin
    if index.exists() and file_sha(index) == args.pin:
        print('PASS (a) single range: pinned product sha256=%s (reused %s)' % (args.pin, index))
    else:
        scratch = work/'scratch'
        scratch.mkdir(exist_ok=True)
        p = sp.run(['taskset', '-c', args.cpus, xsa, 'build', '--pfp', args.raw, '--docs-map',
                    args.map, '-T', str(scratch), '-j', str(args.threads), '-m', '2G',
                    '-o', str(index)], capture_output=True, timeout=7200)
        assert p.returncode == 0, (p.stdout + p.stderr)[-4000:]
        digest = file_sha(index)
        assert digest == args.pin, (digest, args.pin)
        print('PASS (a) single range: rebuilt product sha256=%s == pin' % digest)
    ref = container(index)
    n, member = ref['n'], ref['members'][20]['data']
    assert rows.exists() and rows.stat().st_size >= 12*n, (rows, rows.stat().st_size, n)
    env = dict(os.environ)
    for k in ('XSA_CTX_ROWS', 'XSA_CTX_MERGE_IN'):
        env.pop(k, None)
    cmd = ['taskset', '-c', args.cpus, xsa, 'stream-context', args.raw, args.map]
    # ------------------------------------------- (a) the whole range, un-ranged
    r = arm(cmd, rows, 0, n, env)
    whole, m15 = trailer(r.stdout, 0, n)
    (work/'whole.m20').write_bytes(whole)
    same = whole == member
    print('%s (a) whole-range stream-context payload == m20 member of the pin (%d B)'
          % ('PASS' if same else 'FAIL', len(whole)))
    assert same, 'the un-ranged payload is not the member the pin has'
    # ------------------------------------------------------------- (b) halves
    cut = args.cut or pick_cut(rows, args.raw, n, n//2)[0]
    _c, lcp, ca, cb = pick_cut(rows, args.raw, n, cut)
    assert not args.cut or _c == cut
    print('PASS (b) cut row=%d of %d: 8-byte contexts %r vs %r share a %d-deep prefix'
          % (cut, n, ca, cb, lcp))
    env1 = dict(env, XSA_CTX_ROWS='0:%d' % cut)
    r0 = arm(cmd, rows, 0, cut, env1)
    part0, _ = trailer(r0.stdout, 0, cut)
    assert struct.unpack_from('<Q', part0, 184)[0] == int.from_bytes(b'XSLICE02', 'little'), \
        'the range payload is not a mergeable partial'
    p0 = work/'part0.trie'
    p0.write_bytes(part0)
    env2 = dict(env, XSA_CTX_ROWS='%d:%d' % (cut, n), XSA_CTX_MERGE_IN=str(p0))
    r1 = arm(cmd, rows, cut, n, env2)
    merged, m15b = trailer(r1.stdout, cut, n)
    (work/'merged.m20').write_bytes(merged)
    ok = merged == whole
    print('%s (b) merged member == single-pass member: partial=%d B lower + %d B upper '
          '-> %d B member' % ('PASS' if ok else 'FAIL', len(part0), len(merged), len(merged)))
    print('%s (b) merged m15 trailer == single-pass m15 (%d B)'
          % ('PASS' if m15b == m15 else 'FAIL', len(m15)))
    assert m15b == m15, 'the m15 trailer moved'
    if not ok:
        kind, node, field, want, got = diverge(Payload(whole), Payload(merged))
        print('M20 DIVERGENCE %s %s field=%s expected=%r got=%r' % (kind, node, field, want, got))
        if kind == 'node':
            rp, gp = Payload(whole), Payload(merged)
            print('  node %d: depth/nchild/flags ref=%s got=%s count ref=%d got=%d'
                  % (node, rp.node(node), gp.node(node), rp.col('ncount')[node],
                     gp.col('ncount')[node]))
            print('  ref children=%s' % (rp.children(node),))
            print('  got children=%s' % (gp.children(node),))
        raise SystemExit('the merge is not byte-identical (see M20 DIVERGENCE)')
    # ------------------------------------------------------------ (c) CLI gates
    for script in ('xsa/tests/build_pfp.py', 'gates/pfp_generated_v10_test.py'):
        p = sp.run(['taskset', '-c', args.cpus, 'python3', '-B', script, xsa],
                   capture_output=True, timeout=7200)
        text = (p.stdout + p.stderr).decode(errors='replace')
        assert p.returncode == 0, text[-4000:]
        print('PASS (c) %s' % script)
    root = Payload(whole)
    print('CTX_SLICE_PASS pin=%s rows=%d cut=%d chain=%d partial=%d member=%d nodes=%d edges=%d '
          'sha256=%s' % (args.pin, n, cut, lcp, len(part0), len(merged), root.nodes,
                         root.edges, hashlib.sha256(merged).hexdigest()[:16]))
    if not args.work:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
