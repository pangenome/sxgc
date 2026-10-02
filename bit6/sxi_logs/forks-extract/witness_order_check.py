#!/usr/bin/env python3
"""External witness-sequence check: the distinct witnesses in file order must
equal the sorted chi stream exactly (each witness exactly once, text order)."""
import json
import sys

import numpy as np


def main(chi_path, jsonl_path, limit=None):
    chi = np.fromfile(chi_path, dtype="<u8")
    if limit:
        chi = chi[:limit]
    # Text order: the virtual end (x == 0, i.e. the event at position n-1 of
    # the stream) sorts last, not first; the chi stream is merely numeric.
    key = np.where(chi == 0, np.uint64(1 << 62), chi)
    chi = chi[np.argsort(key, kind="stable")]
    print(f"chi stream: {len(chi)} values (virtual end at position "
          f"{int(np.flatnonzero(chi == 0)[0]) if (chi == 0).any() else -1})")
    seen = 0
    prev = None
    mismatch = 0
    lines = 0
    with open(jsonl_path) as f:
        for line in f:
            lines += 1
            x = json.loads(line)["witness"]
            if x != prev:
                if seen >= len(chi):
                    print(f"FAIL: extra witness {x} beyond chi")
                    mismatch += 1
                    break
                if int(chi[seen]) != x:
                    mismatch += 1
                    if mismatch < 5:
                        print(f"MISMATCH at {seen}: emitted {x} != chi {chi[seen]}")
                seen += 1
                prev = x
    print(f"witness sequence: {seen} emitted vs {len(chi)} chi; "
          f"mismatches {mismatch}; records scanned {lines}")
    print("VERDICT:", "PASS" if mismatch == 0 and seen == len(chi) else "FAIL")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else None)
