#!/usr/bin/env python3
"""Gate A1 for `xsa consult`: exact next-byte-histogram parity against a
brute-force scan of the corpus text.

The oracle (gates/consult_oracle.c) answers with no index at all: it scans every
byte of the corpus and counts what follows each occurrence of the context. This
driver picks the contexts, writes both inputs, runs the oracle and `xsa consult`
over the same contexts, and compares EVERY field of every row.

Two arms per context (row layout v2, CONSULT.md §3):
  raw: the spine as the window names it (cut at the document base only - the
       0x1E scan is gone), compared against the oracle on the same raw context.
  excluded: the same window with `--exclude-source`, whose width and branch
       counts must be the raw answer minus the position's own occurrence
       (branch byte = the corpus byte AT the position; the oracle supplies the
       raw histogram and this driver subtracts deterministically).

  usage: consult_a1.py CORPUS CONTAINER XSA WINDIR [--contexts N] [--oracle PATH]

Exit 0 only if all contexts agree exactly (counts per byte included).
"""
import argparse
import json
import os
import struct
import subprocess
import sys

ROW_FIXED = 40
ROW_TAIL = 24


def pick_contexts(corpus_len, fh, count):
    """(offset_end, length) pairs, wide coverage of lengths + the text tail."""
    lengths = [1, 2, 3, 4, 5, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256,
               384, 512, 768, 1000, 1024, 7, 40, 300, 900]
    lengths = lengths[:count]
    out = []
    # spread the offsets over the first 40 GB, deterministic, and skip any window
    # that contains the END sentinel byte 0xff (the container has no such symbol)
    step = 1_500_000_000
    off = 5_000_003
    for i, ln in enumerate(lengths):
        while True:
            end = off + i * 977
            fh.seek(end - ln)
            ctx = fh.read(ln)
            if 0xFF not in ctx and len(ctx) == ln:
                break
            end += 1
        out.append((end, ln))
        off = (off + step) % (corpus_len - 2000)
    # the very end of the text: a context that reaches the last byte. (The EOF
    # flag is NOT reachable from a window at all: a position's context ends
    # BEFORE the position's own byte, so the context always occurs at least at
    # that position and its last byte can never be the text's last byte. The flag
    # is carried for row-layout parity with the emender format and is exercised by
    # the --strict cross-check instead.)
    fh.seek(corpus_len - 1024)
    tail = fh.read(1024)
    assert 0xFF not in tail
    out.append((corpus_len - 1, 1024))
    out.append((corpus_len - 1, 2))
    # THE DOCUMENT-START CASES: positions right after a 0x1E in the byte stream.
    # The spine is cut at the DOCUMENT BASE now, never at a separator; a 0x1E in
    # the stream is framing content like any other byte. These rows decide that
    # no separator scan runs: the context includes the 0x1E and the oracle sees
    # the same raw bytes.
    fh.seek(0)
    chunk = fh.read(64 << 20)
    seps = [i for i, b in enumerate(chunk[: (1 << 20) * 8]) if b == 0x1E][:6]
    for sp in seps[:3]:
        out.append((sp + 1, 1024))   # position right after a framing 0x1E
        out.append((sp + 40, 1024))
    return out


def read_rows(path):
    data = open(path, 'rb').read()
    if data[:8] != b'XSACONS1':
        sys.exit('not an XSACONS1 row file: %r' % data[:8])
    if struct.unpack_from('<I', data, 8)[0] != 2:
        sys.exit('unexpected row-layout version (want 2, the docid/base/depth tail)')
    rows = []
    p = 16
    while p < len(data):
        nsym, ctxlen, flags = struct.unpack_from('<IHH', data, p)
        doc, off, width, eof = struct.unpack_from('<QQQQ', data, p + 8)
        pairs = []
        q = p + ROW_FIXED
        for _ in range(nsym):
            b = data[q]
            c = struct.unpack_from('<Q', data, q + 1)[0]
            pairs.append((b, c))
            q += 9
        docid, base, depth = struct.unpack_from('<QQQ', data, q)
        rows.append(dict(nsym=nsym, ctxlen=ctxlen, flags=flags, doc=doc, off=off,
                         width=width, eof=eof, pairs=pairs, docid=docid,
                         base=base, depth=depth))
        p = q + ROW_TAIL
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('corpus')
    ap.add_argument('container')
    ap.add_argument('xsa')
    ap.add_argument('workdir')
    ap.add_argument('--contexts', type=int, default=26)
    ap.add_argument('--oracle', default=None, help='path to a built consult_oracle')
    ap.add_argument('--jobs', type=int, default=32)
    args = ap.parse_args()
    os.makedirs(args.workdir, exist_ok=True)
    oracle = args.oracle or os.path.join(args.workdir, 'oracle')
    if not os.path.exists(oracle):
        subprocess.run(['gcc', '-O3', '-march=native', '-o', oracle,
                        os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     'consult_oracle.c')], check=True)
    # THE TEXT IS THE CONTAINER'S TEXT (n-1: one virtual END is appended), not the
    # corpus FILE length: a container over a prefix of a bigger file declares only
    # its own span, and positions/EOF are bounded by it.
    hdr = open(args.container, 'rb').read(64)
    if hdr[:4] != b'SXI2':
        sys.exit('not an SXI2 container: %r' % hdr[:4])
    corpus_len = struct.unpack_from('<Q', hdr, 8)[0] - 1
    file_len = os.path.getsize(args.corpus)
    if file_len < corpus_len:
        sys.exit('corpus %s is %d bytes, the container text is %d' % (args.corpus, file_len, corpus_len))
    with open(args.corpus, 'rb') as fh:
        picks = pick_contexts(corpus_len, fh, args.contexts)
        ctxs = []
        own_bytes = []
        for end, ln in picks:
            fh.seek(end - ln)
            ctxs.append(fh.read(ln))
            fh.seek(end)
            own_bytes.append(fh.read(1)[0])
        # NOTE ON THE ABSENT-CONTEXT PATH: it cannot be reached through a window.
        # A window's context is corpus bytes ending before the position, so it
        # occurs at that very position and has width >= 1 whenever every byte in
        # it is a container symbol. The only unqueryable symbols are the
        # structural END (0xFF here: the corpus has none) and 0x00 (which the
        # container does carry), so a window can never name an absent context -
        # the guard in consult_answer stays as a defence, not as a code path this
        # gate can exercise.
        fh.seek(corpus_len - 1024 - 300)
        picks.append((corpus_len - 300, 1024))
        ctxs.append(fh.read(1024))
        fh.seek(corpus_len - 300)
        own_bytes.append(fh.read(1)[0])

    ctxfile = os.path.join(args.workdir, 'ctx.bin')
    with open(ctxfile, 'wb') as f:
        for c in ctxs:
            f.write(struct.pack('<I', len(c)) + c)
    winfile = os.path.join(args.workdir, 'windows.jsonl')
    with open(winfile, 'w') as f:
        for end, ln in picks:
            # a position whose left-context budget is exactly this context: the
            # corpus is offset 0, so start = end and left = len
            f.write(json.dumps({'doc': 0, 'base': 0, 'start': end, 'len': 1,
                                'left': ln}) + '\n')

    print('A1_CONTEXTS n=%d lengths=%s' % (len(ctxs), sorted({len(c) for c in ctxs})))
    def run(tag, extra):
        rowsfile = os.path.join(args.workdir, 'a1-%s.rows' % tag)
        for p in (rowsfile, rowsfile + '.journal'):
            if os.path.exists(p):
                os.remove(p)
        subprocess.run([args.xsa, 'consult', '--container', args.container,
                        '--corpus', args.corpus, '--windows', winfile,
                        '--output', rowsfile, '--rss-gb', '8', '-j', str(args.jobs)]
                       + extra, check=True)
        return rowsfile
    rowsfile = run('raw', [])
    exclfile = run('excluded', ['--exclude-source'])
    # the container declares its TEXT length (n-1); the oracle scans exactly that
    # prefix of the corpus file, which is how a container over a bigger file works
    text_len = None
    for line in open(rowsfile + '.journal'):
        if line.startswith('CONSULT_OPEN ') and 'text_bytes=' in line:
            text_len = int(line.split('text_bytes=')[1].split()[0])
    if text_len is None:
        sys.exit('A1_FAIL: the consult journal has no CONSULT_OPEN text_bytes=')
    tsv = os.path.join(args.workdir, 'oracle.tsv')
    with open(tsv, 'w') as out:
        subprocess.run([oracle, args.corpus, ctxfile, str(text_len)], check=True, stdout=out)
    raw_lines = [l.rstrip('\n') for l in open(tsv)]
    if len(raw_lines) != len(ctxs):
        sys.exit('A1_FAIL oracle rows %d != contexts %d' % (len(raw_lines), len(ctxs)))
    raw_rows = read_rows(rowsfile)
    excl_rows = read_rows(exclfile)
    if len(raw_rows) != len(ctxs) or len(excl_rows) != len(ctxs):
        sys.exit('A1_FAIL row counts %d/%d != context count %d'
                 % (len(raw_rows), len(excl_rows), len(ctxs)))

    def cmp(tag, i, line, row, ctx, want_len):
        idx, clen, width, eof, pairs = line.split('\t')
        want_pairs = [] if not pairs else [tuple(int(x) for x in p.split(':')) for p in pairs.split(',')]
        good = (int(clen) == row['ctxlen'] == want_len
                and int(width) == row['width']
                and int(eof) == row['eof']
                and [tuple(p) for p in want_pairs] == [tuple(p) for p in row['pairs']])
        if not good:
            print('A1_MISMATCH %s idx=%d ctxlen=%d\n  oracle: len=%s width=%s eof=%s pairs=%s\n'
                  '  consult: len=%d width=%d eof=%d pairs=%s'
                  % (tag, i, want_len, clen, width, eof, want_pairs, row['ctxlen'],
                     row['width'], row['eof'], row['pairs']))
        return good

    ok = 0
    for i in range(len(ctxs)):
        row = raw_rows[i]
        good = cmp('raw-spine', i, raw_lines[i], row, ctxs[i], len(ctxs[i]))
        # --exclude-source: the raw histogram minus the position's own
        # occurrence (branch byte = the corpus byte AT the position)
        excl = excl_rows[i]
        # subtract one at the own byte, dropping a zeroed pair
        subbed = []
        for b, c in row['pairs']:
            if b == own_bytes[i]:
                c -= 1
            if c > 0:
                subbed.append((b, c))
        subbed.sort(key=lambda p: (-p[1], p[0]))
        excl_good = (excl['width'] == row['width'] - 1
                     and excl['eof'] == row['eof']
                     and excl['pairs'] == subbed
                     and excl['flags'] & 4 != 0)
        if not excl_good:
            print('A1_MISMATCH exclude-source idx=%d\n  want: width=%d pairs=%s\n'
                  '  got: width=%d pairs=%s flags=%d'
                  % (i, row['width'] - 1, subbed, excl['width'], excl['pairs'], excl['flags']))
        if good and excl_good:
            ok += 1
            print('A1_ROW idx=%d len=%d width=%d eof=%d pairs=%d excl_width=%d parity=exact'
                  % (i, len(ctxs[i]), raw_rows[i]['width'], raw_rows[i]['eof'],
                     len(raw_rows[i]['pairs']), excl['width']))
    total_counts = sum(sum(c for _, c in r['pairs']) for r in raw_rows)
    print('A1_PARITY contexts=%d rows_checked=%d exact=%d mismatched=%d '
          'counts_compared=%d exclude_rows=%d result=%s'
          % (len(ctxs), 2 * len(ctxs), ok, len(ctxs) - ok, total_counts,
             len(ctxs), 'PASS' if ok == len(ctxs) else 'FAIL'))
    sys.exit(0 if ok == len(ctxs) else 1)


if __name__ == '__main__':
    main()
