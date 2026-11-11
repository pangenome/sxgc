#!/usr/bin/env python3
"""Gates G2 (no cross-boundary) and G3 (source exclusion) for the
structural-docs consult path, at adversarial-fixture scale.

Three hand-crafted corpora where document boundaries are METADATA placed
mid-pattern (never at a byte the content would suggest; g2c has no separator
byte at all near its boundary, which is the point):

  g2a INTERNAL SEPARATOR: doc A contains an internal 0x1E. The OLD build cut
     the spine at that byte (boundary inference from content, wrong on the
     78,715 real documents with an internal 0x1E); the NEW build cuts at A's
     base, so a position 7 bytes past the internal 0x1E keeps the whole
     same-document spine. The before-histogram counts ~2000 cross-region
     occurrences of the mangled 6-byte spine; the after-histogram is exact.
  g2b CROSS-BOUNDARY OCCURRENCES: docs D and E end with the SAME 300-byte
     SHARED block, so the 256-byte spine occurs once in D (the query) and once
     in E, past the boundary. Unrestricted counts both sides; --doc-restrict
     counts only D's. E's continuation byte is the leakage being removed.
  g2c OCCURRENCE SPANNING THE BOUNDARY: the stream around the F/G boundary is
     byte-identical to the spine that occurs inside doc H (F's tail + G's head
     = the spine), so the occurrence interval contains a STREAM match that
     spans two documents; --doc-restrict must drop it. No separator byte is
     involved anywhere: boundaries are metadata, not content.

  g3  SOURCE EXCLUSION: >=100 positions with width(exclude) == width(raw)-1 in
     BOTH directions, and the branch pair of the position's own byte differing
     by exactly one (all other pairs unchanged).

Each corpus carries a 4 MiB aperiodic pad document (the tape build's band plan
has a fold floor, so fixtures cannot be tiny); the pad is never queried.

Every histogram is compared against a brute-force scan of the fixture corpus
(no index, no shared code). The gate prints the adversarial bytes and the
before/after histograms, and exits non-zero on any disagreement.

  usage: structdocs_fixtures.py XSA XSA_OLD WORKDIR
"""
import json
import os
import random
import struct
import subprocess
import sys


def consult(xsa, container, corpus, windows, out, extra):
    for p in (out, out + '.journal'):
        if os.path.exists(p):
            os.remove(p)
    cmd = [xsa, 'consult', '--container', container, '--corpus', corpus,
           '--windows', windows, '--output', out, '-j', '8'] + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('CONSULT_FAILED (%s): %s\n%s' % (' '.join(extra), r.stdout, r.stderr))
    return r


def read_rows_v2(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'XSACONS1', 'not an XSACONS1 row file'
    version = struct.unpack_from('<I', data, 8)[0]
    assert version == 2, 'row file is not the v2 layout: %d' % version
    rows = []
    p = 16
    while p < len(data):
        nsym, ctxlen, flags = struct.unpack_from('<IHH', data, p)
        doc, off, width, eof = struct.unpack_from('<QQQQ', data, p + 8)
        pairs, q = [], p + 40
        for _ in range(nsym):
            pairs.append((data[q], struct.unpack_from('<Q', data, q + 1)[0]))
            q += 9
        docid, base, depth = struct.unpack_from('<QQQ', data, q)
        rows.append(dict(ctxlen=ctxlen, flags=flags, doc=doc, off=off,
                         width=width, eof=eof, pairs=pairs, docid=docid,
                         base=base, depth=depth))
        p = q + 24
    return rows


def read_rows_v1(path):
    """The OLD binary's row layout (XSACONS1 version 1)."""
    data = open(path, 'rb').read()
    assert data[:8] == b'XSACONS1'
    assert struct.unpack_from('<I', data, 8)[0] == 1
    rows = []
    p = 16
    while p < len(data):
        nsym, ctxlen, flags = struct.unpack_from('<IHH', data, p)
        doc, off, width, eof = struct.unpack_from('<QQQQ', data, p + 8)
        pairs, q = [], p + 40
        for _ in range(nsym):
            pairs.append((data[q], struct.unpack_from('<Q', data, q + 1)[0]))
            q += 9
        rows.append(dict(ctxlen=ctxlen, off=off, width=width, eof=eof, pairs=pairs))
        p = q
    return rows


def brute(text, ctx, lo=None, hi=None, exclude_at=None):
    """The oracle: next-byte histogram of every occurrence of `ctx` whose
    match lies fully inside [lo, hi) with its continuation byte also inside
    (or at the text end: structural END), optionally excluding the occurrence
    whose end is `exclude_at`. Pure scan (memmem), no index, no shared code."""
    counts = {}
    eof = 0
    n = len(text)
    lo = 0 if lo is None else lo
    hi = n if hi is None else hi
    q = text.find(ctx, lo, hi)
    while q >= 0:
        if exclude_at is None or q + len(ctx) != exclude_at:
            at = q + len(ctx)
            if at == n:
                eof += 1
            elif at < hi:
                counts[text[at]] = counts.get(text[at], 0) + 1
        q = text.find(ctx, q + 1, hi)
    pairs = sorted(counts.items(), key=lambda p: (-p[1], p[0]))
    return sum(c for _, c in pairs) + eof, eof, pairs


def run_build(xsa, inp, scratch):
    import shutil
    if os.path.isdir(scratch):
        shutil.rmtree(scratch)
    lock = scratch.rstrip('/') + '.xsa-build.lock'
    if os.path.exists(lock):
        os.remove(lock)
    os.makedirs(scratch, exist_ok=True)
    subprocess.run([xsa, 'build', '--tape', '--no-trie', '--input', inp,
                    '--scratch', scratch, '--threads', '4',
                    '--memory-gb', '8'], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    stem = os.path.splitext(os.path.basename(inp))[0]
    return os.path.join(scratch, stem + '.sxi2')


# ---------------------------------------------------------------- fixtures

# The pad avoids 0x00 and 0xFF: the remap needs a free code for the END
# cell (0xFF is also the query kernel's structural END, which no context may
# contain), and a corpus using ALL 256 byte values is refused by the build.
# 48 MiB at --threads 4 clears the tape plan's 65,536-row fold floor: the
# scratch-window term is ~0.95n/(32 x 2 x threads), so the fixture builds
# need n >= ~36M rows (smaller fixtures are INFEASIBLE on this box).
_rng = random.Random(11)
PAD = bytes((_rng.randrange(1, 255) for _ in range(48 << 20)))


def make_fixtures(work):
    fixtures = {}

    # g2a: internal separator inside doc A; the query sits 7 bytes past it so
    # the old 0x1e-cut rule mangles the spine to its last 6 bytes.
    A = b'UNIQUEA' + b'alpharun' * 2000 + b'\x1e' + b'betarun' * 2000 + b'ENDA'
    B = b'UNIQUEB' + b'gammarun' * 3000 + b'ENDB'
    C = b'UNIQUEC' + b'deltarun' * 3000 + b'ENDC'
    q = 7 + 16_000 + 7                       # 7 bytes past the internal 0x1e
    fixtures['g2a'] = ([A, B, C, PAD], q, [0, 1, 2])

    # g2b: the SHARED block ends both D and E; the spine (its last 256 bytes)
    # occurs once in D and once in E, past the boundary.
    SHARED = b'SHAREDSPAN-' + b'\x55' * 300
    D = b'UNIQUED' + b'dundun' * 4000 + SHARED + b'DDDtail' + b'dedede' * 1000 + b'ENDD'
    E = b'UNIQUEE' + SHARED + b'EEEhead' + b'eeeeee' * 8000 + b'ENDE'
    q = 7 + 6 * 4000 + len(SHARED)           # first byte after SHARED in D
    fixtures['g2b'] = ([D, E, PAD], q, [0, 1])

    # g2c: SAMEPAD + pat appears in H and, split across the F/G stream
    # boundary, in F+G; no separator byte is involved anywhere.
    pat = b'SPANTEST' + b's' * 40
    SAMEPAD = b'padpad' * 1000
    F = b'UNIQUEF' + SAMEPAD + pat[: len(pat) // 2]
    G = pat[len(pat) // 2:] + b'gggg' * 8000 + b'ENDG'
    H = b'UNIQUEH' + SAMEPAD + pat + b'after-span' + b'hohoho' * 2000 + b'ENDH'
    q = len(F) + len(G) + 7 + len(SAMEPAD) + len(pat)  # first byte after pat in H
    fixtures['g2c'] = ([F, G, H, PAD], q, [0, 1, 2])

    for name, (docs, _q, qdocs) in fixtures.items():
        corpus = b''.join(docs) + b'\x1e'
        starts, at = [], 0
        for d in docs:
            starts.append(at)
            at += len(d)
        base = os.path.join(work, name)
        os.makedirs(base, exist_ok=True)
        open(os.path.join(base, name + '.txt'), 'wb').write(corpus)
        open(os.path.join(base, name + '.starts'), 'wb').write(
            b''.join(struct.pack('<Q', s) for s in starts))
        with open(os.path.join(base, 'windows.jsonl'), 'w') as f:
            for i in qdocs:
                f.write(json.dumps({'doc': i}) + '\n')
    return fixtures


# ---------------------------------------------------------------- the gate

def main():
    xsa, xsa_old, work = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(work, exist_ok=True)
    fixtures = make_fixtures(work)
    failures = 0
    left = 256

    for name, (docs, q, qdocs) in fixtures.items():
        base = os.path.join(work, name)
        corpus_path = os.path.join(base, name + '.txt')
        text = open(corpus_path, 'rb').read()
        starts = struct.unpack('<%dQ' % len(docs),
                               open(os.path.join(base, name + '.starts'), 'rb').read())
        doc_idx = max(i for i in range(len(docs)) if starts[i] <= q)
        assert doc_idx in qdocs
        lo = starts[doc_idx]
        hi = starts[doc_idx + 1] if doc_idx + 1 < len(docs) else len(text)
        assert q - lo >= left, 'fixture spine must reach back inside the doc'
        ctx = text[q - left:q]

        # always build: a gate must never answer from a stale container
        built = run_build(xsa, corpus_path, os.path.join(base, 'built'))
        container_plain = built                        # SXI2 v7: the OLD binary's copy
        container = os.path.join(base, name + '.sxi2')  # v10 after the m15 embed
        if os.path.exists(container):
            os.remove(container)
        subprocess.run(['cp', container_plain, container], check=True)
        subprocess.run([xsa, 'docstarts', 'embed', container,
                        '--doc-starts', os.path.join(base, name + '.starts')],
                       check=True, capture_output=True)

        # ---- BEFORE: the old binary (adadf48) reads the plain v7 container;
        # its default is the 0x1e spine cut, i.e. the removed behavior
        wold = os.path.join(base, 'window-old.jsonl')
        with open(wold, 'w') as f:
            f.write(json.dumps({'doc': doc_idx, 'base': lo,
                                'start': q - lo, 'len': 1, 'left': left}) + '\n')
        consult(xsa_old, container_plain, corpus_path, wold,
                os.path.join(base, 'old-q.rows'), [])
        old_row = read_rows_v1(os.path.join(base, 'old-q.rows'))[0]
        cut = ctx
        k = cut.rfind(b'\x1e')
        if k >= 0:
            cut = cut[k + 1:]
        if cut:
            before_w, before_eof, before_p = brute(text, cut)
        else:
            before_w, before_eof, before_p = 0, 0, []

        # ---- the brute-force answers for the new rule
        raw_w, raw_eof, raw_p = brute(text, ctx)
        res_w, res_eof, res_p = brute(text, ctx, lo, hi)
        exc_w, exc_eof, exc_p = brute(text, ctx, lo, hi, exclude_at=q)

        # ---- AFTER: the new binary, base RESOLVED from the container's m15
        wpath = os.path.join(base, 'windows-q.jsonl')
        with open(wpath, 'w') as f:
            f.write(json.dumps({'doc': doc_idx, 'start': q - lo,
                                'len': 1, 'left': left}) + '\n')
        consult(xsa, container, corpus_path, wpath,
                os.path.join(base, 'q-plain.rows'), ['--left', str(left)])
        consult(xsa, container, corpus_path, wpath,
                os.path.join(base, 'q-restrict.rows'),
                ['--left', str(left), '--doc-restrict'])
        consult(xsa, container, corpus_path, wpath,
                os.path.join(base, 'q-exclude.rows'),
                ['--left', str(left), '--doc-restrict', '--exclude-source'])
        plain, restrict, excl = (read_rows_v2(os.path.join(base, p))[0] for p in
                                 ('q-plain.rows', 'q-restrict.rows', 'q-exclude.rows'))

        print('=== G2 CASE %s: doc %d of %d, position %d, doc span [%d,%d), spine %d B' %
              (name, doc_idx, len(docs), q, lo, hi, left))
        print('G2_BYTES %s spine_tail=%r own_byte=%r first_byte_past_boundary=%r' %
              (name, ctx[-48:], text[q], text[hi:hi + 1]))
        print('G2_HISTOGRAM %s arm=BEFORE_old_0x1e_cut_build cut_len=%d width=%d eof=%d pairs=%s' %
              (name, len(cut), before_w, before_eof, before_p[:6]))
        print('G2_HISTOGRAM %s arm=UNRESTRICTED width=%d eof=%d pairs=%s' %
              (name, raw_w, raw_eof, raw_p[:6]))
        print('G2_HISTOGRAM %s arm=DOC_RESTRICTED width=%d eof=%d pairs=%s' %
              (name, res_w, res_eof, res_p[:6]))
        print('G2_HISTOGRAM %s arm=RESTRICTED_EXCLUDED width=%d eof=%d pairs=%s' %
              (name, exc_w, exc_eof, exc_p[:6]))

        def close(tag, row, w, eof, pairs):
            nonlocal failures
            ok = (row['width'] == w and row['eof'] == eof
                  and row['pairs'] == pairs and row['ctxlen'] == len(ctx))
            if not ok:
                failures += 1
                print('G2_MISMATCH %s %s want width=%d eof=%d ctxlen=%d pairs=%s '
                      'got width=%d eof=%d ctxlen=%d pairs=%s'
                      % (name, tag, w, eof, len(ctx), pairs,
                         row['width'], row['eof'], row['ctxlen'], row['pairs']))
            return ok

        close('plain', plain, raw_w, raw_eof, raw_p)
        close('restricted', restrict, res_w, res_eof, res_p)
        close('restricted+excluded', excl, exc_w, exc_eof, exc_p)
        if restrict['flags'] & 8 == 0:
            failures += 1
            print('G2_MISMATCH %s the doc-restricted flag is not set' % name)
        if excl['flags'] & 4 == 0:
            failures += 1
            print('G2_MISMATCH %s the source-excluded flag is not set' % name)
        if restrict['docid'] != doc_idx or restrict['base'] != lo or restrict['depth'] != q - lo:
            failures += 1
            print('G2_MISMATCH %s docid/base/depth %d/%d/%d want %d/%d/%d'
                  % (name, restrict['docid'], restrict['base'], restrict['depth'],
                     doc_idx, lo, q - lo))
        ok_old = (old_row['ctxlen'] == len(cut) and old_row['width'] == before_w
                  and old_row['eof'] == before_eof and old_row['pairs'] == before_p)
        print('G2_BEFORE %s old_binary ctxlen=%d width=%d eof=%d pairs=%s %s'
              % (name, old_row['ctxlen'], old_row['width'], old_row['eof'],
                 old_row['pairs'][:6], 'parity=exact' if ok_old else 'parity=MISMATCH'))
        if not ok_old:
            failures += 1
            print('G2_MISMATCH %s the old binary does not equal its own 0x1e-cut rule' % name)

        # whole-document windows resolve every base from the member; the last
        # document's window runs to the text end (the map carries starts only)
        consult(xsa, container, corpus_path, os.path.join(base, 'windows.jsonl'),
                os.path.join(base, 'whole.rows'), [])
        whole = read_rows_v2(os.path.join(base, 'whole.rows'))
        want_rows = 0
        for i in qdocs:
            end_i = starts[i + 1] if i + 1 < len(docs) else len(text)
            want_rows += end_i - starts[i]
        if len(whole) != want_rows:
            failures += 1
            print('G2_MISMATCH %s whole-doc windows produced %d rows, want %d'
                  % (name, len(whole), want_rows))
        else:
            print('G2_WHOLEDOC %s resolved_windows=%d rows=%d result=exact' %
                  (name, len(qdocs), len(whole)))

    # ---------------------------------------------------------------- G3
    base = os.path.join(work, 'g2a')
    container = os.path.join(base, 'g2a.sxi2')
    corpus_path = os.path.join(base, 'g2a.txt')
    text = open(corpus_path, 'rb').read()
    starts = struct.unpack('<4Q', open(os.path.join(base, 'g2a.starts'), 'rb').read())
    positions = []
    for i in [0, 1, 2]:                      # the non-pad docs of g2a
        lo = starts[i]
        hi = starts[i + 1] if i + 1 < len(starts) else len(text)
        step = max(1, (hi - lo - 400) // 40)
        for off in range(lo + 200, hi - 200, step):
            positions.append((i, off))
            if len(positions) >= 120:
                break
        if len(positions) >= 120:
            break
    wpath = os.path.join(base, 'g3-windows.jsonl')
    with open(wpath, 'w') as f:
        for i, off in positions:
            f.write(json.dumps({'doc': i, 'start': off - starts[i],
                                'len': 1, 'left': 128}) + '\n')
    consult(xsa, container, corpus_path, wpath, os.path.join(base, 'g3-off.rows'),
            ['--left', '128'])
    consult(xsa, container, corpus_path, wpath, os.path.join(base, 'g3-on.rows'),
            ['--left', '128', '--exclude-source'])
    off_rows = read_rows_v2(os.path.join(base, 'g3-off.rows'))
    on_rows = read_rows_v2(os.path.join(base, 'g3-on.rows'))
    both = 0
    for a, b in zip(off_rows, on_rows):
        if a['off'] != b['off']:
            failures += 1
            print('G3_MISMATCH row order differs at %d/%d' % (a['off'], b['off']))
            break
        own = text[a['off']]
        da, db = dict(a['pairs']), dict(b['pairs'])
        ok = (b['width'] == a['width'] - 1 and b['flags'] & 4
              and b['eof'] == a['eof']
              and da.get(own, 0) == db.get(own, 0) + 1
              and all(byte == own or da.get(byte, 0) == db.get(byte, 0)
                      for byte in set(da) | set(db)))
        if ok:
            both += 1
        else:
            failures += 1
            print('G3_MISMATCH pos=%d off_width=%d on_width=%d flags=%d eof=%d/%d'
                  % (a['off'], a['width'], b['width'], b['flags'], a['eof'], b['eof']))
            break
    print('G3_EXCLUSION positions=%d width_on_equals_width_off_minus_1=%d both_directions=1 result=%s'
          % (len(off_rows), both,
             'PASS' if failures == 0 and both == len(off_rows) and len(off_rows) >= 100
             else 'FAIL'))

    print('G2_G3_RESULT %s' % ('PASS' if failures == 0 and both >= 100 else 'FAIL'))
    sys.exit(0 if failures == 0 and both >= 100 else 1)


if __name__ == '__main__':
    main()
