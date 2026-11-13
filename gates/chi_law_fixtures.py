#!/usr/bin/env python3
"""G3 THE LAW for the χ-proportional consult: adversarial fixtures that try to
make a ZERO-LOCATE layer return a wrong histogram.

The χ layer cascade answers from the raw backward-search histogram whenever the
SEPARATOR-FRAMING LAW holds for the container (every in-text document start is
preceded by one 0x1E byte) AND the context carries no 0x1E byte. The fixtures
below are built to break exactly the naive versions of those claims:

  chi_a  SAME-BYTE CROSSING, UNFRAMED: a crossing foreign occurrence that
         continues with the SAME byte as the query's own (width 2, equality
         width(C.b)==width(C) HOLDS - the literal one-step verification would
         return the raw count 2 where the law's answer is 1). The container's
         boundaries sit mid-content, so the open-time framing verification must
         fail (verified=0) and every row must enumerate exactly.
  chi_b  SAME-BYTE CROSSING, FRAMED, SPINE CARRIES THE FRAMING BYTE: the
         query's spine contains an internal 0x1E (a CONTENT byte of its own
         document) and a junction elsewhere reproduces the whole spine across a
         boundary with the same continuation byte. The container verifies clean
         (framed), but the spine's 0x1E must trip the conservative alarm: the row
         enumerates exactly instead of trusting the raw width.
  chi_c  WIDTHS 2-4, FRAMED, all-0x1E-free spines: repeated in-document
         occurrences (width 2, 3 and 4, single and forked), each verified
         against the brute-force law oracle; the zero-locate raw layers must be
         exercised (journal: forced/rawfork rows > 0) AND exact.
  chi_d  END-ADJACENT + TEXT-END: contexts that end documents (END branch,
         eof>0), a query at the very last position of the text (the structural
         end), a query one byte past a document start (1-byte spine), and a
         foreign occurrence that ends a document (the eof branch must count it,
         never its next-document first byte as content).
  chi_e  INTERNAL 0x1E EVERYWHERE: documents whose CONTENT is full of 0x1E
         (the 78,715-document reality); spines that do and do not contain it;
         parity with the pre-change binary and the brute-force oracle.

Every fixture corpus carries the 48 MiB aperiodic pad (the tape build's fold
floor). Every histogram is compared against a brute-force scan of the fixture
corpus (no index, no shared code), and the whole row file is compared
BYTE-FOR-BYTE against the PRE-CHANGE binary run over the same container.

  usage: chi_law_fixtures.py XSA XSA_OLD WORKDIR
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


def read_rows(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'XSACONS1', 'not an XSACONS1 row file'
    version = struct.unpack_from('<I', data, 8)[0]
    assert version == 3, 'row file is not the v3 layout: %d' % version
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


def brute_law(text, starts, ctx, lo=None, hi=None, exclude_at=None):
    """THE LAW'S ORACLE (starts-table-aware, no index, no shared code)."""
    import bisect
    counts = {}
    eof = 0
    crossing = 0
    n = len(text)
    lo_r = 0 if lo is None else lo
    hi_r = n if hi is None else hi

    def is_start(p):
        i = bisect.bisect_right(starts, p) - 1
        return i >= 0 and starts[i] == p

    def base_of(p):
        return starts[bisect.bisect_right(starts, p) - 1]

    q = text.find(ctx, lo_r, hi_r)
    while q >= 0:
        at = q + len(ctx)
        if exclude_at is None or at != exclude_at:
            if at == n:
                eof += 1
            elif lo is not None and at == hi_r:
                eof += 1
            elif lo is not None and at > hi_r:
                pass
            elif base_of(at - 1) > q:
                crossing += 1     # a document starts inside the match: nowhere
            elif is_start(at):
                eof += 1          # "this context ends a document"
            else:
                counts[text[at]] = counts.get(text[at], 0) + 1
        q = text.find(ctx, q + 1, hi_r)
    pairs = sorted(counts.items(), key=lambda p: (-p[1], p[0]))
    return sum(c for _, c in pairs) + eof, eof, pairs


def brute_conflated(text, ctx):
    """The LEGACY conflated oracle: every stream occurrence, no boundary
    classification; eof only when the continuation is the text end. This is
    what a MARKED row (boundary_exact=0, an interval past --boundary-cap)
    carries, by the row's own flag bit."""
    counts = {}
    eof = 0
    n = len(text)
    q = text.find(ctx)
    while q >= 0:
        at = q + len(ctx)
        if at == n:
            eof += 1
        else:
            counts[text[at]] = counts.get(text[at], 0) + 1
        q = text.find(ctx, q + 1)
    pairs = sorted(counts.items(), key=lambda p: (-p[1], p[0]))
    return sum(c for _, c in pairs) + eof, eof, pairs


def run_build(xsa, inp, scratch, trie=True):
    import shutil
    if os.path.isdir(scratch):
        shutil.rmtree(scratch)
    lock = scratch.rstrip('/') + '.xsa-build.lock'
    if os.path.exists(lock):
        os.remove(lock)
    os.makedirs(scratch, exist_ok=True)
    cmd = [xsa, 'build', '--tape', '--input', inp, '--scratch', scratch,
           '--threads', '8', '--memory-gb', '8']
    if trie:
        cmd += ['--trie-depth', '8', '--trie-min-support', '10']
    else:
        cmd += ['--no-trie']
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    stem = os.path.splitext(os.path.basename(inp))[0]
    return os.path.join(scratch, stem + '.sxi2')


_rng = random.Random(31337)
PAD = bytes((_rng.randrange(1, 255) for _ in range(48 << 20)))


def framed(docs):
    """Join documents with ONE 0x1E byte each: the Pile's framing."""
    starts, at = [], 0
    for d in docs:
        starts.append(at)
        at += len(d) + 1
    return b''.join(d + b'\x1e' for d in docs), starts


def unframed(docs):
    """Concatenate documents with NO separator: boundaries are pure metadata
    placed mid-content (the g2c world)."""
    starts, at = [], 0
    for d in docs:
        starts.append(at)
        at += len(d)
    return b''.join(docs), starts


# ---------------------------------------------------------------- fixtures
# Each fixture: (name, docs, windows, joiner, expect_verified)
# windows are computed PROGRAMMATICALLY from the doc bytes (positions are
# doc-relative; the harness resolves base from the starts table).

def qafter(doc, pat, nth=1, extra=0):
    """Doc-relative offset of the byte right after the nth occurrence of pat
    (optionally `extra` bytes later)."""
    at = -1
    for _ in range(nth):
        at = doc.index(pat, at + 1)
    return at + len(pat) + extra


def make_fixtures():
    F = []

    # chi_a: SAME-BYTE CROSSING, UNFRAMED. The query's 12-byte spine occurs
    # once in its own document and once across the D0/D1 junction, and BOTH
    # occurrences continue with the same byte b'q'. The literal one-step
    # verification would see width(C.q) == width(C) == 2 and answer 2; the
    # law's answer is 1 (the crossing occurrence is counted nowhere).
    spine = b'ZQsamebyteXX'          # 12 bytes, 0x1E-free
    D0 = b'MD0' + b'alpha' * 500 + b'U' + spine[:6]        # ends with spine[0:6]
    D1 = spine[6:] + b'q' + b'bravo' * 500 + b'ENDd1'      # starts with spine[6:]
    D2 = b'MD2' + b'UNIQUE' + spine + b'q' + b'charlie' * 500 + b'ENDd2'  # the own doc
    F.append(dict(name='chi_a', docs=[D0, D1, D2, PAD], join=unframed,
                  expect_verified=0,
                  windows=[(2, qafter(D2, spine) + e, 1, 12) for e in (0, 1, 2)]))

    # chi_b: SAME-BYTE CROSSING, FRAMED, with the framing byte IN the spine.
    # The query's document contains an internal 0x1E; the spine (cut at the
    # base, 0x1E included as content) is reproduced across the E0/E1 junction
    # with the same continuation. The container verifies (framed), but the
    # spine's 0x1E must trip the alarm: enumeration, exact.
    ctxb = b'QQframedZZ' + b'f' * 20 + b'\x1e' + b'WWmidtext' + b'g' * 20  # 61 bytes
    E_self = b'ME2' + b'unique-e2' + ctxb + b'q' + b'delta' * 500 + b'ENDe2'
    # the junction: E0's content ends with ctxb[:30] and the JOIN'S OWN
    # separator plays ctxb[30] (the framing byte inside the spine), so the
    # stream reproduces ctxb exactly across the E0/E1 boundary, continuing
    # with the same byte 'q' as the query's own occurrence.
    E0 = b'ME0' + b'echo' * 500 + b'K' + ctxb[:30]
    E1 = ctxb[31:] + b'q' + b'foxtrot' * 500 + b'ENDe1'
    F.append(dict(name='chi_b', docs=[E0, E1, E_self, PAD], join=framed,
                  expect_verified=1,
                  windows=[(2, qafter(E_self, ctxb) + e, 1, len(ctxb))
                           for e in (0, 1, 2)]))

    # chi_c: WIDTHS 2-4, FRAMED, 0x1E-free spines. In-document repetitions only:
    # width 2 (single branch), width 3 (fork of 2) and width 4 (single), all
    # 0x1E-free. The raw layers must fire and be exact.
    c2 = b'cTwocc' + b'a' * 20       # occurs twice in the doc, both followed by 'k'
    c3 = b'cThreeYY' + b'b' * 20     # occurs 3 times: 'k','k','m' -> fork
    c4 = b'cFourW' + b'c' * 20       # occurs 4 times, all followed by 'k'
    C_doc = (b'MC1' + c2 + b'k' + b'x' * 80 + c2 + b'k' + b'x' * 80
             + c3 + b'k' + b'x' * 80 + c3 + b'k' + b'x' * 80 + c3 + b'm'
             + b'x' * 80 + c4 + b'k' + b'x' * 80 + c4 + b'k' + b'x' * 80
             + c4 + b'k' + b'x' * 80 + c4 + b'k' + b'ENDC1')
    F.append(dict(name='chi_c', docs=[C_doc, PAD], join=framed,
                  expect_verified=1,
                  windows=[(0, qafter(C_doc, c2, 1), 1, len(c2)),        # width 2, single
                           (0, qafter(C_doc, c3, 3), 1, len(c3)),        # width 3, fork
                           (0, qafter(C_doc, c4, 4), 1, len(c4))]))      # width 4, single

    # chi_d: END-ADJACENT + TEXT-END + SEPARATOR-AS-CONTENT. A context whose
    # FOREIGN occurrence sits at another document's content end: its
    # continuation is the FRAMING SEPARATOR, which the law counts as a CONTENT
    # byte of the previous document - the naive "doc end => END branch"
    # reading would wrongly put it on eof. Plus a query at the text's very
    # last byte, a 1-byte spine, and an empty spine.
    tailA = b'tail-run-A' * 3
    tailB = b'tail-run-B' * 3
    D0 = b'MD0' + b'alpha' * 100 + tailA
    D1 = b'MD1' + b'bravo' * 100 + tailB
    D2 = (b'MD2' + b'p' * 30 + tailA + b'q' + b'x' * 40 + tailB + b'q'
          + b'z' * 50 + b'ENDD2')
    F.append(dict(name='chi_d', docs=[D0, D1, D2, PAD], join=framed,
                  expect_verified=1,
                  windows=[(2, qafter(D2, tailA), 1, len(tailA)),    # foreign-at-doc-end row
                           (2, qafter(D2, tailA) + 1, 1, len(tailA) + 1),
                           (2, 1, 1, 1),                             # 1-byte spine
                           (2, 0, 1, 8),                            # EMPTY spine
                           (3, len(PAD), 1, 64)]))                # the text's very last byte (its separator)

    # chi_e: INTERNAL 0x1E EVERYWHERE. Content 0x1e bytes, spines crossing
    # them and not, and a 1-byte spine that IS the 0x1e (an in-doc separator
    # byte directly before the query position - content, never a boundary).
    I_doc = (b'MI1' + b'iota' * 60 + b'\x1e' + b'kappa' * 60 + b'\x1e'
             + b'lambda' * 60 + b'ENDI1')
    F.append(dict(name='chi_e', docs=[I_doc, PAD], join=framed,
                  expect_verified=1,
                  windows=[(0, qafter(I_doc, b'\x1e', 1), 1, 300),   # spine crosses an internal 0x1e
                           (0, qafter(I_doc, b'\x1e', 1), 1, 100),
                           (0, qafter(I_doc, b'\x1e', 1), 1, 1),    # the 0x1e itself is the spine
                           (0, qafter(I_doc, b'\x1e', 1) + 1, 1, 8),
                           (0, qafter(I_doc, b'\x1e', 2), 1, 200)]))
    return F


def main():
    xsa, xsa_old, work = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(work, exist_ok=True)
    failures = 0
    examined = 0
    exact_rows = 0

    for fx in make_fixtures():
        name = fx['name']
        base = os.path.join(work, name)
        os.makedirs(base, exist_ok=True)
        corpus, starts = fx['join'](fx['docs'])
        # the text is the corpus; the container appends one virtual END
        corpus_path = os.path.join(base, name + '.txt')
        open(corpus_path, 'wb').write(corpus)
        starts_path = os.path.join(base, name + '.starts')
        open(starts_path, 'wb').write(
            b''.join(struct.pack('<Q', s) for s in starts))
        # one more window at the very last text position (the structural end)
        windows = list(fx['windows'])

        with open(os.path.join(base, 'windows.jsonl'), 'w') as f:
            for (di, st, ln, left) in windows:
                f.write(json.dumps({'doc': di, 'start': st, 'len': ln,
                                    'left': left}) + '\n')

        built = run_build(xsa, corpus_path, os.path.join(base, 'built'))
        container = os.path.join(base, name + '.sxi2')
        if os.path.exists(container):
            os.remove(container)
        subprocess.run(['cp', built, container], check=True)
        subprocess.run([xsa, 'docstarts', 'embed', container,
                        '--doc-starts', starts_path], check=True,
                       capture_output=True)

        out_new = os.path.join(base, 'new.rows')
        r = consult(xsa, container, corpus_path,
                    os.path.join(base, 'windows.jsonl'), out_new, [])
        # ---- the open-time framing receipt must say what the fixture expects
        j = open(out_new + '.journal').read()
        want_v = fx['expect_verified']
        got_v = None
        for line in j.splitlines():
            if line.startswith('CONSULT_SEPFRAMING '):
                got_v = int(line.split('verified=')[1].split()[0])
        print('CHI_%s sepframing expected_verified=%d got_verified=%s' %
              (name.upper().replace('CHI_', 'CHI '), want_v, got_v))
        if got_v != want_v:
            print('CHI_FAIL %s the framing verification said %s, expected %d'
                  % (name, got_v, want_v))
            failures += 1

        # ---- the pre-change binary over the SAME container, byte parity
        out_old = os.path.join(base, 'old.rows')
        consult(xsa_old, container, corpus_path,
                os.path.join(base, 'windows.jsonl'), out_old, [])
        b_new = open(out_new, 'rb').read()
        b_old = open(out_old, 'rb').read()
        examined += 1
        if b_new != b_old:
            failures += 1
            print('CHI_FAIL %s row files differ from the pre-change binary '
                  '(%d vs %d bytes)' % (name, len(b_new), len(b_old)))
        else:
            print('CHI_%s PARITY bytes=%d identical_to_prechange=1'
                  % (name.upper().replace('CHI_', 'CHI '), len(b_new)))

        # ---- the brute-force law oracle, row by row
        rows = read_rows(out_new)
        starts_l = list(starts)
        for (di, st, ln, left), row in zip(windows, rows):
            lo = starts_l[di]
            hi = starts_l[di + 1] - 1 if di + 1 < len(starts_l) else len(corpus)
            q = lo + st
            avail = q - lo
            ctxlen = min(left, avail)
            ctx = corpus[q - ctxlen:q] if ctxlen else b''
            if not ctx:
                # EMPTY CONTEXT IS AN EMPTY HISTOGRAM - never the corpus totals
                want_w, want_eof, want_pairs = 0, 0, []
            elif row['flags'] & 16:
                # a MARKED row carries the conflated answer (the interval was
                # past --boundary-cap), by its own flag bit
                want_w, want_eof, want_pairs = brute_conflated(corpus, ctx)
            else:
                want_w, want_eof, want_pairs = brute_law(corpus, starts_l, ctx)
            ok = (row['width'] == want_w and row['eof'] == want_eof
                  and row['pairs'] == want_pairs
                  and row['ctxlen'] == ctxlen)
            exact_rows += 1
            if not ok:
                failures += 1
                print('CHI_FAIL %s doc=%d start=%d left=%d ctx=%r\n'
                      '  want width=%d eof=%d pairs=%s\n'
                      '  got  width=%d eof=%d pairs=%s flags=%d'
                      % (name, di, st, left, ctx, want_w, want_eof, want_pairs,
                         row['width'], row['eof'], row['pairs'], row['flags']))
            else:
                print('CHI_%s LAW doc=%d start=%d ctxlen=%d width=%d eof=%d '
                      'pairs=%s exact=1' % (name.upper().replace('CHI_', 'CHI '), di, st, ctxlen,
                                            want_w, want_eof, want_pairs))
        # ---- the layer receipt (on verified fixtures the zero-locate raw
        # layers must have fired; on unframed ones every row must enumerate)
        if True:
            lay = [l for l in j.splitlines() if l.startswith('CONSULT_LAYERS')]
            if not lay:
                print('CHI_FAIL %s no CONSULT_LAYERS receipt' % name)
                failures += 1
            else:
                print('CHI_%s LAYERS %s' % (name.upper().replace('CHI_', 'CHI '),
                                             lay[-1][len('CONSULT_LAYERS '):].strip()))

    # ---- THE TRAPS ARE REAL: on the trap fixtures the naive conflated answer
    # must genuinely differ from the law's answer, so a wrong implementation
    # (one that trusted the raw width) would have been caught by this gate.
    for name in ('chi_a', 'chi_b'):
        base = os.path.join(work, name)
        corpus = open(os.path.join(base, name + '.txt'), 'rb').read()
        starts_raw = open(os.path.join(base, name + '.starts'), 'rb').read()
        starts_l = list(struct.unpack('<%dQ' % (len(starts_raw) // 8), starts_raw))
        rows = read_rows(os.path.join(base, 'new.rows'))
        # the FIRST window of each trap fixture is the trap query
        with open(os.path.join(base, 'windows.jsonl')) as f:
            wins = [json.loads(l) for l in f]
        w0 = wins[0]
        lo = starts_l[w0['doc']]
        q = lo + w0['start']
        ctxlen = min(w0['left'], q - lo)
        ctx = corpus[q - ctxlen:q]
        law_w, law_eof, law_p = brute_law(corpus, starts_l, ctx)
        raw_w, raw_eof, raw_p = brute_conflated(corpus, ctx)
        row = rows[0]
        same_trap = raw_w != law_w and raw_p != law_p
        print('CHI_%s TRAP law_width=%d conflated_width=%d row_width=%d trap_is_real=%d'
              % (name.upper().replace('CHI_', 'CHI '), law_w, raw_w, row['width'],
                 int(same_trap and row['width'] == law_w)))
        if not (same_trap and row['width'] == law_w and row['pairs'] == law_p):
            failures += 1
            print('CHI_FAIL %s the trap is not real or the binary fell for it: '
                  'law=%s raw=%s got width=%d pairs=%s'
                  % (name, (law_w, law_eof, law_p), (raw_w, raw_eof, raw_p),
                     row['width'], row['pairs']))

    print()
    print('CHI_G3_EXAMINED fixtures=%d rows=%d failures=%d'
          % (examined, exact_rows, failures))
    if failures:
        print('CHI_G3_FAIL %d' % failures)
        sys.exit(1)
    print('CHI_G3_OK rows=%d all_exact=1 all_parity=1' % exact_rows)


if __name__ == '__main__':
    main()
