#!/usr/bin/env python3
"""G6 fixture: a corpus that ends MID-RECORD, with a brute-force oracle.

The tail group (the final <= 24 positions, whose band keys cannot be padded
below every byte) is the one place the declared END symbol can collide with
data. Band-boundary code is where every defect this week lived, so the gate for
it is a text whose last record is shorter than a key -- not a large blob that
happens to end on a separator.

Convention under test (SPEC_CHAIN_TO_PILE.md, Phase A):
    alphabet = END(0) < every byte value; END occurs exactly once, at position n.
    SA   = suffixes of T + END, ordered as finite strings over that alphabet.
    BWT  = T'[(SA[r] + n') mod n']  -- row r carries the character BEFORE
           SA[r]; for SA[r] = 0 that character is END.

Usage:
    end_fixture.py --out DIR [--case short|tiny|records]

Writes DIR/corpus.bin (the text INCLUDING the END byte), DIR/expect.tsv
(rank, position, BWT symbol as hex) and prints a summary. Exit 1 on any
disagreement, so it can be a gate rather than a printout.
"""
import argparse
import os
import sys

END = 0          # the declared END code; the corpus byte that realizes it is 0xF5
END_BYTE = 0xF5  # free-byte realization used by valid UTF-8 corpora


def finite_sa_with_end(text: bytes):
    """SA of text·END where END is minimal and unique."""
    t = list(text) + [END]
    n = len(t)
    return sorted(range(n), key=lambda i: t[i:])


def bwt_rows(text: bytes, sa):
    t = list(text) + [END]
    n = len(t)
    return [t[(p - 1) % n] for p in sa]


def cases(name):
    if name == "tiny":
        # 3 bytes: last record shorter than any realistic key, END right after 'b'
        return b"a\x1eb"
    if name == "short":
        # two records, the second only 5 bytes: text ends well inside a 24-byte key
        return b"hello world\x1eabcde"
    if name == "records":
        # 30 records, last one 7 bytes; exercises many 0x1E occurrences at 1-byte keys
        parts = [b"rec%03d" % i + b"payload" * (i % 3) for i in range(30)]
        return b"\x1e".join(parts) + b"\x1e" + b"tail7by"
    raise SystemExit(f"unknown case {name}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--case", default="short", choices=["tiny", "short", "records"])
    a = ap.parse_args()

    text = cases(a.case)
    if bytes([END_BYTE]) in text:
        raise SystemExit("fixture contains the realization byte 0xF5 -- pick another byte")
    # the corpus as written on disc includes the END realization byte, once, at the end
    corpus = text + bytes([END_BYTE])

    sa = finite_sa_with_end(text)
    bwt = bwt_rows(text, sa)
    n1 = len(text) + 1

    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "corpus.bin"), "wb") as f:
        f.write(corpus)

    rows = []
    for r, p in enumerate(sa):
        # position in the corpus-as-written: p < len(text) is a text position;
        # p == len(text) is the END position, reported as len(text).
        rows.append((r, p, bwt[r]))
    with open(os.path.join(a.out, "expect.tsv"), "w") as f:
        f.write("# rank\tposition\tbwt_symbol_hex\n")
        for r, p, c in rows:
            f.write("%d\t%d\t%02x\n" % (r, p, c))

    # sanity: END must be the first suffix, and its row must carry the last text byte
    assert sa[0] == len(text), ("END is not the minimal suffix", sa[:3], len(text))
    assert bwt[0] == text[-1] if text else True, "row 0 must carry the character before END"
    # the row for position 0 must carry END
    r0 = sa.index(0)
    assert bwt[r0] == END, ("row of position 0 must be END", r0, bwt[r0])
    assert len(set(range(n1)) ^ set(sa)) == 0 and len(sa) == n1, "SA is not a permutation"
    # strict order
    t = list(text) + [END]
    for r in range(1, n1):
        assert t[sa[r - 1]:] < t[sa[r]:], ("order violation at rank", r)

    print("G6_FIXTURE case=%s text_bytes=%d corpus_bytes=%d n=%d END_row=0 pos0_row=%d"
          % (a.case, len(text), len(corpus), n1, r0))
    print("G6_FIXTURE_TAIL tail_len_after_last_separator=%d key_would_overrun=%s"
          % (len(text) - (text.rfind(b"\x1e") + 1),
             len(text) - (text.rfind(b"\x1e") + 1) < 24))
    print("G6_FIXTURE files: %s/corpus.bin %s/expect.tsv" % (a.out, a.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
