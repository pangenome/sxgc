#!/usr/bin/env python3
"""Compare two m20 member files and state exactly WHERE they differ.

Two routes to the same tree can only differ in the fields they are required to
record about themselves:

  head  0..8    member format version (a container banked before format v2 says 1)
  head 152..168 the provenance field: which ordered stream produced the member
                (`source`) and what that stream cost in RAM (`source_mem`)

Usage:
  trie_member_cmp.py same-code A.bin B.bin    -> only 152..168 may differ
  trie_member_cmp.py legacy    A.bin B.bin    -> 0..8 and 152..168 may differ

Exit 0 when every differing byte lies inside the permitted fields AND the members
are equal once those fields are zeroed. It prints one line, prefixed for the gate.
"""
import hashlib
import sys

FIELDS = {
    "same-code": [(152, 168)],
    "legacy": [(0, 8), (152, 168)],
}


def masked(d, ranges):
    m = bytearray(d)
    for lo, hi in ranges:
        m[lo:hi] = b"\0" * (hi - lo)
    return bytes(m)


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in FIELDS:
        print(__doc__, file=sys.stderr)
        return 2
    mode, pa, pb = sys.argv[1], sys.argv[2], sys.argv[3]
    ranges = FIELDS[mode]
    a = open(pa, "rb").read()
    b = open(pb, "rb").read()
    if len(a) != len(b):
        print("EQUIVALENCE_BYTES mode=%s FAIL sizes %d vs %d" % (mode, len(a), len(b)))
        return 1
    diff = [i for i in range(len(a)) if a[i] != b[i]]
    inside = all(any(lo <= i < hi for lo, hi in ranges) for i in diff)
    same = masked(a, ranges) == masked(b, ranges)
    print(
        "EQUIVALENCE_BYTES mode=%s bytes=%d differing=%d permitted_fields=%s "
        "all_inside=%s masked_equal=%s sha_masked=%s"
        % (
            mode,
            len(a),
            len(diff),
            ",".join("%d..%d" % r for r in ranges),
            int(inside),
            int(same),
            hashlib.sha256(masked(a, ranges)).hexdigest(),
        )
    )
    if diff:
        print("EQUIVALENCE_OFFSETS mode=%s offsets=%s" % (mode, diff[:32]))
    print("EQUIVALENCE PASS" if (inside and same) else "EQUIVALENCE FAIL")
    return 0 if (inside and same) else 1


if __name__ == "__main__":
    sys.exit(main())
