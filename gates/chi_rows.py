#!/usr/bin/env python3
"""Shared row-file tooling for the chi gates: the v3/v4 readers and the
row-by-row comparison with per-layer classification.

usage (as a library):
  from chi_rows import read_rows, read_rows_v4, classify, compare_files
"""
import struct
import sys


def read_rows(path, want_version=3):
    """The v3 row reader (XSACONS1 v3), identical to gates/structdocs_fixtures.py."""
    data = open(path, 'rb').read()
    assert data[:8] == b'XSACONS1', 'not an XSACONS1 row file: %s' % path
    version = struct.unpack_from('<I', data, 8)[0]
    assert version == want_version, \
        '%s is a v%d row file, expected v%d' % (path, version, want_version)
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


def read_rows_v4(path):
    """The v4 reader: every record is preceded by a 1-byte KIND.
    KIND 1 (FORCED) carries no pair: it decodes to [(cont, width - eof)].
    KIND 0 (FULL) is the v3 record verbatim."""
    data = open(path, 'rb').read()
    assert data[:8] == b'XSACONS1', 'not an XSACONS1 row file: %s' % path
    version = struct.unpack_from('<I', data, 8)[0]
    assert version == 4, '%s is a v%d row file, expected v4' % (path, version)
    rows = []
    p = 16
    while p < len(data):
        kind = data[p]
        p += 1
        if kind == 1:
            ctxlen, flags = struct.unpack_from('<HH', data, p)
            doc, off, width, eof = struct.unpack_from('<QQQQ', data, p + 4)
            cont = data[p + 36]
            docid, base, depth = struct.unpack_from('<QQQ', data, p + 37)
            p += 61
            assert width > eof, 'v4 FORCED row with no content pair'
            rows.append(dict(ctxlen=ctxlen, flags=flags, doc=doc, off=off,
                             width=width, eof=eof, pairs=[(cont, width - eof)],
                             docid=docid, base=base, depth=depth, forced=1))
        else:
            assert kind == 0, 'unknown v4 row kind %d at %d' % (kind, p - 1)
            nsym, ctxlen, flags = struct.unpack_from('<IHH', data, p)
            doc, off, width, eof = struct.unpack_from('<QQQQ', data, p + 8)
            pairs, q = [], p + 40
            for _ in range(nsym):
                pairs.append((data[q], struct.unpack_from('<Q', data, q + 1)[0]))
                q += 9
            docid, base, depth = struct.unpack_from('<QQQ', data, q)
            rows.append(dict(ctxlen=ctxlen, flags=flags, doc=doc, off=off,
                             width=width, eof=eof, pairs=pairs, docid=docid,
                             base=base, depth=depth, forced=0))
            p = q + 24
    return rows


def classify(rows):
    """The census classifier: the layer mix of a row file by its own fields
    (the counterpart of the journal's CONSULT_LAYERS, from the row bytes)."""
    import collections
    c = collections.Counter()
    for r in rows:
        marked = bool(r['flags'] & 16)
        if r['ctxlen'] == 0:
            c['empty'] += 1
        elif marked:
            c['marked'] += 1
        elif r['width'] == 0:
            c['absent'] += 1
        elif r['width'] == 1:
            c['width1'] += 1
        elif len(r['pairs']) == 1:
            c['single_branch'] += 1
        else:
            c['fork'] += 1
        if r['ctxlen'] <= 8 and not marked:
            c['short_ctx'] += 1
    return c


def compare_files(a_path, b_path, limit=None, label_a='a', label_b='b'):
    """Row-by-row comparison of two v3 row files up to `limit` rows.
    Returns (n_compared, n_diff, first_diff)."""
    a = read_rows(a_path)
    b = read_rows(b_path)
    n = min(len(a), len(b))
    if limit:
        n = min(n, limit)
    diffs = 0
    first = None
    for i in range(n):
        ra, rb = a[i], b[i]
        if (ra['doc'], ra['off'], ra['width'], ra['eof'], ra['pairs'],
                ra['flags'], ra['ctxlen'], ra['docid'], ra['base']) != \
           (rb['doc'], rb['off'], rb['width'], rb['eof'], rb['pairs'],
                rb['flags'], rb['ctxlen'], rb['docid'], rb['base']):
            diffs += 1
            if first is None:
                first = (i, ra, rb)
    if len(a) != len(b):
        print('COMPARE_NOTE %s has %d rows, %s has %d' % (label_a, len(a), label_b, len(b)))
    return n, diffs, first


if __name__ == '__main__':
    import json
    mode = sys.argv[1]
    if mode == 'cmp':
        n, d, first = compare_files(sys.argv[2], sys.argv[3],
                                    int(sys.argv[4]) if len(sys.argv) > 4 else None,
                                    sys.argv[2], sys.argv[3])
        print('CMP rows=%d diffs=%d' % (n, d))
        if first:
            print('FIRST_DIFF row=%d\n  %s\n  %s' % first)
        sys.exit(1 if d else 0)
    elif mode == 'mix':
        rows = read_rows(sys.argv[2])
        if len(sys.argv) > 3 and sys.argv[3] == 'v4':
            rows = read_rows_v4(sys.argv[2])
        c = classify(rows)
        total = sum(c.values()) - c['short_ctx']
        print('MIX total=%d %s' % (len(rows), json.dumps(dict(c))))
