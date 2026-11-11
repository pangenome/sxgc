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
    assert version in (2, 3), 'row file is not the v2/v3 layout: %d' % version
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
                         base=base, depth=depth, version=version))
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
    """The LEGACY conflated oracle (the pre-law rule): every stream occurrence,
    no boundary classification at all. Used for the BEFORE arm (the old binary
    equals its own rule exactly) and for --cross-doc arms - never for the law
    arms, which use brute_law below."""
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


def brute_law(text, starts, ctx, lo=None, hi=None, exclude_at=None):
    """THE LAW'S ORACLE (starts-table-aware, no index, no shared code):
    - the occurrence is a CROSSING match (counted nowhere) iff a document
      start lies inside (q, q+len-1];
    - the continuation at at = q+len is the END branch iff at == len(text)
      or at is a document start; otherwise it is the content byte text[at].
    With lo/hi the occurrences are restricted to [lo, hi) (the --doc-restrict
    arm: at == hi is the document end and lands on END).
    Returns (width, eof, pairs)."""
    counts = {}
    eof = 0
    crossing = 0
    n = len(text)
    lo_r = 0 if lo is None else lo
    hi_r = n if hi is None else hi

    def is_start(p):
        import bisect
        i = bisect.bisect_right(starts, p) - 1
        return i >= 0 and starts[i] == p

    def base_of(p):
        import bisect
        return starts[bisect.bisect_right(starts, p) - 1]

    q = text.find(ctx, lo_r, hi_r)
    while q >= 0:
        at = q + len(ctx)
        if exclude_at is None or at != exclude_at:
            if at == n:
                eof += 1
            elif lo is not None and at == hi_r:
                eof += 1          # the restricted arm: the document ends here
            elif lo is not None and at > hi_r:
                pass              # unreachable: the match is fully inside
            elif base_of(at - 1) > q:
                crossing += 1     # a document starts inside the match: not a match
            elif is_start(at):
                eof += 1          # "this context ends a document"
            else:
                counts[text[at]] = counts.get(text[at], 0) + 1
        q = text.find(ctx, q + 1, hi_r)
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

    # ---- X1: the no-cross law, adversarial ---------------------------------
    # x1a: FOUR documents END with TAIL, TWO contain it mid-document followed
    # by b'Q'. Under the law the histogram of TAIL is exactly END=4 (the four
    # doc-ending occurrences), content Q=2, and NOTHING ELSE - in particular
    # the FIRST BYTES of the documents that follow the four doc-ending
    # occurrences must NOT appear as content.
    TAIL = b'ENDRUN' + b'z' * 7
    A1 = b'UNIQa1' + b'alpha' * 100 + TAIL
    A2 = b'UNIQa2' + b'bravo' * 100 + TAIL
    A3 = b'UNIQa3' + b'echox' * 100 + TAIL
    B1 = b'UNIQb1' + TAIL + b'Q' + b'beta' * 300 + b'ENDb1'
    B2 = b'UNIQb2' + b'delta' * 100 + TAIL + b'Q' + b'ENDb2'
    # doc order chosen so the byte after each doc-ending TAIL is a DIFFERENT
    # next-document first byte (U/G/p...): none of them may pollute the histogram
    q = len(A1) + len(B1) + len(TAIL)      # first byte after TAIL inside B1
    fixtures['x1a'] = ([A1, B1, A2, B2, A3, PAD], q, [1])

    # x1b: INTERNAL 0x1E. CTX occurs twice mid-document, each followed by an
    # INTERNAL 0x1E (content by the law: the separator a document OWNS is its
    # last byte, and these are not doc ends), and once at a document END (eof).
    # The distinction between content-0x1E and END is the whole point.
    CTX = b'INSEP' + b'w' * 5
    C1 = b'UNIQc1' + b'gamma' * 80 + CTX + b'\x1e' + b'internal-continues' + b'ENDc1'
    C2 = b'UNIQc2' + b'kilo' * 80 + CTX                      # this doc ENDS with CTX
    C3 = b'UNIQc3' + b'lima' * 80 + CTX + b'\x1e' + b'internal-tail' + b'ENDc3'
    fixtures['x1b'] = ([C1, C2, C3, PAD], None, [0, 1, 2])

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
        if name.startswith('x1'):
            continue          # the X1 law fixtures have their own gate below
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

        # ---- the brute-force answers for the NEW rule (the law, starts-aware)
        raw_w, raw_eof, raw_p = brute_law(text, starts, ctx)
        res_w, res_eof, res_p = brute_law(text, starts, ctx, lo, hi)
        exc_w, exc_eof, exc_p = brute_law(text, starts, ctx, lo, hi, exclude_at=q)

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
        # document's window runs to the text end (the map carries starts only).
        # This arm checks base resolution and row GEOMETRY, not histogram
        # exactness, and the fixture text is adversarially repetitive
        # ('alpharun' x 2000), where a width-9 context has ~2000 occurrences:
        # the exact enumeration that the no-cross default runs would enumerate
        # millions of rows here. --boundary-cap 64 keeps the arm on the
        # marked-fallback path for those; the exactness claims live in the
        # X1 cases, whose contexts have width 3..5.
        consult(xsa, container, corpus_path, os.path.join(base, 'windows.jsonl'),
                os.path.join(base, 'whole.rows'), ['--boundary-cap', '64'])
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

    # ============================== X1: the no-cross law, adversarial =====
    #
    # x1a: contexts whose occurrences END documents. The histogram must show
    # an END branch with the EXACT count of doc-ending occurrences and NO
    # content byte for them (the next documents' first bytes must not appear).
    # x1b: internal 0x1E stays a CONTENT branch, never END - the distinction
    # is the whole point. Both are checked against brute_law (memmem + the
    # starts table, no index, no shared code), then the boundary-aware TRIE
    # (m20, built post-hoc from the container's own m15) is checked against
    # the same oracle, and the UNAWARE trie's query surface must REFUSE by
    # default and answer conflated only under --cross-doc.
    x1_failures = 0

    def x1_case(name, docs, query_maker, expect_tail):
        """Build the fixture, consult the law, and check the row against the
        oracle. query_maker(text, starts) -> (doc_idx, off, ctx_len)."""
        nonlocal x1_failures
        base = os.path.join(work, name)
        os.makedirs(base, exist_ok=True)
        corpus = b''.join(docs) + b'\x1e'
        starts, at = [], 0
        for d in docs:
            starts.append(at)
            at += len(d)
        corpus_path = os.path.join(base, name + '.txt')
        open(corpus_path, 'wb').write(corpus)
        open(os.path.join(base, name + '.starts'), 'wb').write(
            b''.join(struct.pack('<Q', s) for s in starts))
        doc_idx, off, ctx_len = query_maker(corpus, starts)
        ctx = corpus[off - ctx_len:off]
        lo = starts[doc_idx]
        hi = starts[doc_idx + 1] if doc_idx + 1 < len(starts) else len(corpus)

        built = run_build(xsa, corpus_path, os.path.join(base, 'built'))
        container = os.path.join(base, name + '.sxi2')
        if os.path.exists(container):
            os.remove(container)
        subprocess.run(['cp', built, container], check=True)
        subprocess.run([xsa, 'docstarts', 'embed', container,
                        '--doc-starts', os.path.join(base, name + '.starts')],
                       check=True, capture_output=True)

        wpath = os.path.join(base, 'x1-q.jsonl')
        with open(wpath, 'w') as f:
            f.write(json.dumps({'doc': doc_idx, 'start': off - lo,
                                'len': 1, 'left': ctx_len}) + '\n')
        consult(xsa, container, corpus_path, wpath,
                os.path.join(base, 'x1-plain.rows'), ['--left', str(ctx_len)])
        consult(xsa, container, corpus_path, wpath,
                os.path.join(base, 'x1-excl.rows'),
                ['--left', str(ctx_len), '--exclude-source'])
        consult(xsa, container, corpus_path, wpath,
                os.path.join(base, 'x1-cross.rows'),
                ['--left', str(ctx_len), '--cross-doc'])
        plain = read_rows_v2(os.path.join(base, 'x1-plain.rows'))[0]
        excl = read_rows_v2(os.path.join(base, 'x1-excl.rows'))[0]
        cross = read_rows_v2(os.path.join(base, 'x1-cross.rows'))[0]

        want_w, want_eof, want_p = brute_law(corpus, starts, ctx)
        exc_w, exc_eof, exc_p = brute_law(corpus, starts, ctx, exclude_at=off)
        old_w, old_eof, old_p = brute(corpus, ctx)

        print('X1_HISTOGRAM %s arm=LAW width=%d eof=%d pairs=%s' %
              (name, want_w, want_eof, want_p))
        print('X1_HISTOGRAM %s arm=CONFLATED_legacy width=%d eof=%d pairs=%s' %
              (name, old_w, old_eof, old_p))

        def x1_check(tag, row, w, eof, pairs, want_flag_exact):
            ok = (row['width'] == w and row['eof'] == eof and row['pairs'] == pairs)
            exact_bit = bool(row['flags'] & 16)
            if exact_bit != (not want_flag_exact):
                ok = False
            if not ok:
                x1_failures += 1
                print('X1_MISMATCH %s %s want width=%d eof=%d pairs=%s flag_exact=%s '
                      'got width=%d eof=%d pairs=%s flag_exact=%s'
                      % (name, tag, w, eof, pairs, want_flag_exact,
                         row['width'], row['eof'], row['pairs'], not exact_bit))
            return ok

        x1_check('default', plain, want_w, want_eof, want_p, True)
        x1_check('exclude-source', excl, exc_w, exc_eof, exc_p, True)
        # the --cross-doc row is the legacy conflated answer, MARKED inexact
        x1_check('cross-doc', cross, old_w, old_eof, old_p, False)
        # the doc-ending occurrences' continuations must not appear as content
        bad_bytes = set()
        for i, s in enumerate(starts):
            nxt = starts[i + 1] if i + 1 < len(starts) else len(corpus)
            tail_here = (nxt - len(expect_tail) >= s
                         and corpus[nxt - len(expect_tail):nxt] == expect_tail)
            if tail_here and nxt < len(corpus):
                bad_bytes.add(corpus[nxt])
        polluted = [b for b in bad_bytes if dict(plain['pairs']).get(b, 0) > 0]
        if polluted:
            x1_failures += 1
            print('X1_MISMATCH %s next-document first bytes %r appear as content' %
                  (name, polluted))
        else:
            print('X1_NO_LEAK %s doc_ending_occurrences=%d next_doc_bytes=%r '
                  'absent_from_content=1' %
                  (name, want_eof, sorted(bad_bytes)))

        # ---- the boundary-aware TRIE (m20 built from the container's m15)
        trie_container = os.path.join(base, name + '-trie.sxi2')
        if os.path.exists(trie_container):
            os.remove(trie_container)
        r = subprocess.run([xsa, 'ctx-build', '--sxi', container,
                            '--text', corpus_path, '--out', trie_container,
                            '--depth', '16', '--min-support', '2',
                            '--threads', '8'], capture_output=True, text=True)
        if r.returncode != 0:
            x1_failures += 1
            print('X1_MISMATCH %s ctx-build failed: %s' % (name, r.stderr[-500:]))
            return
        aware_line = [ln for ln in r.stderr.splitlines() if 'CONTEXT_TRIE_BOUNDARY' in ln]
        print('X1_TRIE_BUILD %s %s' %
              (name, aware_line[0].split('CONTEXT_TRIE_BOUNDARY ')[1]
               if aware_line else 'NO_BOUNDARY_LINE'))
        if not aware_line or 'aware=1' not in aware_line[0]:
            x1_failures += 1
            print('X1_MISMATCH %s the trie is not boundary-aware' % name)
        # branch-of against the oracle: children (content+END) and count
        r = subprocess.run([xsa, 'branch-of', trie_container, 'hex:' + ctx.hex()],
                           capture_output=True, text=True)
        if r.returncode != 0:
            x1_failures += 1
            print('X1_MISMATCH %s branch-of failed: %s' % (name, r.stderr[-400:]))
            return
        got_pairs = []
        got_end = 0
        got_count = 0
        for ln in r.stdout.splitlines():
            if ln.startswith('present '):
                got_count = int(ln.split('count=')[1].split()[0])
            if ln.startswith('child '):
                if ln.startswith('child END '):
                    got_end = int(ln.split('count=')[1].split()[0])
                else:
                    b = int(ln.split('child 0x')[1].split()[0], 16)
                    got_pairs.append((b, int(ln.split('count=')[1].split()[0])))
        got_p = sorted(got_pairs, key=lambda p: (-p[1], p[0]))
        ok = (got_count == want_w and got_end == want_eof and got_p == want_p)
        print('X1_TRIE_BRANCH_OF %s count=%d end=%d pairs=%s oracle=width=%d eof=%d pairs=%s %s' %
              (name, got_count, got_end, got_p, want_w, want_eof, want_p,
               'exact' if ok else 'MISMATCH'))
        if not ok:
            x1_failures += 1
        # trie-stat: the count is the NON-CROSSING width
        r = subprocess.run([xsa, 'trie-stat', trie_container, 'hex:' + ctx.hex()],
                           capture_output=True, text=True)
        if r.returncode == 0:
            for ln in r.stdout.splitlines():
                if ln.startswith('present '):
                    if int(ln.split('count=')[1].split()[0]) != want_w:
                        x1_failures += 1
                        print('X1_MISMATCH %s trie-stat count' % name)
        else:
            x1_failures += 1
            print('X1_MISMATCH %s trie-stat failed: %s' % (name, r.stderr[-300:]))
        # ctx-verify: the trie against a CORPUS SCAN under the law
        r = subprocess.run([xsa, 'ctx-verify', '--sxi', trie_container,
                            '--text', corpus_path, '--samples', '200',
                            '--seed', '7', '--sample-nodes', '200'],
                           capture_output=True, text=True)
        ver = [ln for ln in r.stdout.splitlines() if ln.startswith('TRIE_VERIFY ')]
        print('X1_TRIE_VERIFY %s %s' %
              (name, ver[0] if ver else ('FAILED: ' + r.stderr[-300:])))
        if r.returncode != 0 or not ver or 'mismatches=0' not in ver[0]:
            x1_failures += 1

        # ---- the UNAWARE trie must REFUSE by default, answer under --cross-doc
        plain_container = built       # the v7 build, no m15, no trie
        unaware = os.path.join(base, name + '-unaware.sxi2')
        if os.path.exists(unaware):
            os.remove(unaware)
        r = subprocess.run([xsa, 'ctx-build', '--sxi', plain_container,
                            '--text', corpus_path, '--out', unaware,
                            '--depth', '16', '--min-support', '2',
                            '--threads', '8'], capture_output=True, text=True)
        if r.returncode != 0:
            x1_failures += 1
            print('X1_MISMATCH %s unaware ctx-build failed: %s' % (name, r.stderr[-400:]))
            return
        r = subprocess.run([xsa, 'branch-of', unaware, 'hex:' + ctx.hex()],
                           capture_output=True, text=True)
        refused = r.returncode != 0 and 'boundary-UNAWARE' in r.stderr
        print('X1_UNAWARE_REFUSAL %s rc=%d refused=%s' %
              (name, r.returncode, 'yes' if refused else 'NO'))
        if not refused:
            x1_failures += 1
            print('X1_MISMATCH %s branch-of on the unaware trie did not refuse: %s' %
                  (name, (r.stderr or r.stdout)[-300:]))
        r = subprocess.run([xsa, 'branch-of', unaware, 'hex:' + ctx.hex(), '--cross-doc'],
                           capture_output=True, text=True)
        got_pairs = []
        got_end = 0
        got_count = 0
        for ln in r.stdout.splitlines():
            if ln.startswith('present '):
                got_count = int(ln.split('count=')[1].split()[0])
            if ln.startswith('child '):
                if ln.startswith('child END '):
                    got_end = int(ln.split('count=')[1].split()[0])
                else:
                    b = int(ln.split('child 0x')[1].split()[0], 16)
                    got_pairs.append((b, int(ln.split('count=')[1].split()[0])))
        got_p = sorted(got_pairs, key=lambda p: (-p[1], p[0]))
        ok = (got_count == old_w and got_end == old_eof and got_p == old_p)
        print('X1_UNAWARE_CROSS_DOC %s count=%d end=%d pairs=%s %s' %
              (name, got_count, got_end, got_p,
               'parity=conflated_exact' if ok else 'parity=MISMATCH'))
        if not ok:
            x1_failures += 1

    # x1a: the query sits right after TAIL inside B1, so its context IS TAIL
    # (13 bytes: ENDRUN + seven z's - the length must match the fixture or the
    # context silently becomes a PREFIX of TAIL whose continuation is TAIL's
    # own last byte, which is exactly the trap this gate is here to catch)
    x1a_docs = fixtures['x1a'][0]
    TAIL = b'ENDRUN' + b'z' * 7
    TAILLEN = len(TAIL)
    assert TAILLEN == 13
    def q_x1a(text, starts):
        # doc 1 (B1) contains TAIL followed by Q; query at the Q
        needle = TAIL + b'Q'
        at = text.find(needle, starts[1], starts[2])
        assert at >= 0
        return 1, at + TAILLEN, TAILLEN
    x1_case('x1a', x1a_docs, q_x1a, TAIL)

    # x1b: the query sits ON the internal 0x1E inside C1, so its context is CTX
    x1b_docs = fixtures['x1b'][0]
    def q_x1b(text, starts):
        needle = b'INSEP' + b'w' * 5 + b'\x1e'
        at = text.find(needle, starts[0], starts[1])
        assert at >= 0
        return 0, at + 10, 10
    x1_case('x1b', x1b_docs, q_x1b, b'INSEP' + b'w' * 5)

    print('X1_RESULT %s' % ('PASS' if x1_failures == 0 else 'FAIL'))

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
    # G3 checks the exclusion relation (width-1, one pair decremented) which the
    # fallback rows satisfy exactly like the enumerated rows; the repetitive
    # fixture text would make full-width enumeration cost minutes per position,
    # so the arm runs with a small cap and both arms take the same path per row.
    consult(xsa, container, corpus_path, wpath, os.path.join(base, 'g3-off.rows'),
            ['--left', '128', '--boundary-cap', '256'])
    consult(xsa, container, corpus_path, wpath, os.path.join(base, 'g3-on.rows'),
            ['--left', '128', '--exclude-source', '--boundary-cap', '256'])
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

    total = failures + x1_failures
    print('G2_G3_RESULT %s' % ('PASS' if failures == 0 and both >= 100 else 'FAIL'))
    print('X1_FINAL x1_failures=%d all_failures=%d result=%s'
          % (x1_failures, total, 'PASS' if total == 0 and both >= 100 else 'FAIL'))
    sys.exit(0 if total == 0 and both >= 100 else 1)


if __name__ == '__main__':
    main()
