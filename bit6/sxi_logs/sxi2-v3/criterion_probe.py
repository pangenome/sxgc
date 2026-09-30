#!/usr/bin/env python3
"""Read-only SXI1 header/C-table audit for the hybrid phi criterion.

For a cyclic text T = U^m with m > 1, every character frequency is divisible
by m.  Consequently gcd(frequencies) == 1 certifies that T is primitive and
that every run passes C.  This is a sufficient, not necessary, certificate.
"""
import json
import math
import struct
import sys


def audit(path):
    with open(path, "rb") as f:
        h = f.read(64)
        assert h[:4] == b"SXI1" and struct.unpack_from("<I", h, 4)[0] == 1
        n, k, r = struct.unpack_from("<QQQ", h, 8)
        directory_count = struct.unpack_from("<I", h, 32)[0]
        directory = [f.read(40) for _ in range(directory_count)]
        assert all(len(d) == 40 for d in directory)
        members = {struct.unpack_from("<I", d)[0]: d for d in directory}
        d = members[1]
        assert struct.unpack_from("<Q", d, 24)[0] == r
        f.seek(struct.unpack_from("<Q", d, 8)[0])
        c = struct.unpack("<256Q", f.read(2048))
        assert c[0] == 0 and all(c[i] <= c[i + 1] for i in range(255))
        frequencies = [c[i + 1] - c[i] for i in range(255)] + [n - c[255]]
        assert sum(frequencies) == n
        g = math.gcd(*frequencies)
        chi = struct.unpack_from("<Q", members[5], 24)[0]
        return dict(path=path, n=n, k=k, runs=r, chi=chi,
                    frequency_gcd=g, positive_symbols=sum(x > 0 for x in frequencies),
                    certified_safe_runs=r if g == 1 else None,
                    certified_escape_runs=0 if g == 1 else None,
                    certified_escape_bytes=0 if g == 1 else None)


if __name__ == "__main__":
    print(json.dumps([audit(p) for p in sys.argv[1:]], indent=2))
