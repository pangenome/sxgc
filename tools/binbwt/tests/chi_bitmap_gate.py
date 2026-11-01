#!/usr/bin/env python3
"""Check chi count, CRC, compact EF dimensions and exact baseline m5 bytes.

Usage: chi_bitmap_gate.py NEW BASELINE EXPECTED_CHI
Streaming reads keep validation independent of chi-sized heap allocations.
"""
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib


def member(path):
    with path.open('rb') as f:
        header = f.read(64)
        assert header[:4] == b'SXI2', header[:4]
        n = struct.unpack_from('<Q', header, 8)[0]
        count = struct.unpack_from('<I', header, 32)[0]
        flags = struct.unpack_from('<Q', header, 48)[0]
        assert flags & 1 and not flags & 64, flags
        directory = f.read(count * 40)
        for at in range(0, len(directory), 40):
            ident, codec, off, size, entries, crc, reserved = struct.unpack_from('<IIQQQII', directory, at)
            if ident == 5:
                assert codec == 105 and not reserved
                f.seek(off)
                l, lowbits, highbits = struct.unpack('<QQQ', f.read(24))
                assert lowbits == entries * l
                assert highbits == ((n >> l) + entries + 1 if entries else 0)
                assert size == 24 + (lowbits + 7) // 8 + (highbits + 7) // 8
                f.seek(off)
                digest = hashlib.sha256()
                actual_crc = 0
                remaining = size
                while remaining:
                    block = f.read(min(8 << 20, remaining))
                    assert block, 'truncated m5'
                    remaining -= len(block)
                    digest.update(block)
                    actual_crc = zlib.crc32(block, actual_crc)
                assert actual_crc == crc, (actual_crc, crc)
                return dict(n=n, entries=entries, bytes=size, bits_per_entry=8*size/entries if entries else 0,
                            sha256=digest.hexdigest(), crc32=crc)
    raise AssertionError('missing chi member')


if __name__ == '__main__':
    new, baseline = map(Path, sys.argv[1:3])
    expected = int(sys.argv[3])
    actual, old = member(new), member(baseline)
    assert actual['entries'] == expected, actual
    assert actual == old, (actual, old)
    print(json.dumps(dict(status='PASS', baseline_m5_byte_identical=True, **actual), sort_keys=True))
