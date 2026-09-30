#!/usr/bin/env python3
"""Reference codec for the v3 periodic-run escape member.

Rows are sorted by original BWT run ID. Each row names a cyclic SA-value
domain and bit-packs its exact successor array. A container would checksum
the complete member in its directory. This module is a small reference
codec, not a production-scale publisher.
"""
import bisect
import struct

MAGIC = b"SXESC3\0\0"
HEADER = struct.Struct("<8sQQQB7x")  # magic, n, R, count, value width
ENTRY = struct.Struct("<QQQQ")       # run id, domain start, count, bit offset


def encode(n, runs, escape):
    """escape: run_id -> [(SA value, true SA successor), ...] cyclic order."""
    width = max(1, (n - 1).bit_length())
    rows = []
    bit_offset = 0
    payload = bytearray()
    reservoir = 0
    available = 0
    for run_id, values in sorted(escape.items()):
        assert 0 <= run_id < runs and values
        assert not rows or run_id > rows[-1][0]
        start = values[0][0]
        assert all(x == (start + i) % n and 0 <= successor < n
                   for i, (x, successor) in enumerate(values))
        rows.append((run_id, start, len(values), bit_offset))
        for _, successor in values:
            reservoir |= successor << available
            available += width
            while available >= 8:
                payload.append(reservoir & 255)
                reservoir >>= 8
                available -= 8
            bit_offset += width
    if available:
        payload.append(reservoir)
    return (HEADER.pack(MAGIC, n, runs, len(rows), width)
            + b"".join(ENTRY.pack(*row) for row in rows) + payload)


class Escape:
    def __init__(self, data):
        if len(data) < HEADER.size:
            raise ValueError("short escape header")
        magic, self.n, self.runs, count, self.width = HEADER.unpack_from(data)
        if (magic != MAGIC or not self.n or not self.runs
                or self.width != max(1, (self.n - 1).bit_length())):
            raise ValueError("invalid escape header")
        start = HEADER.size + count * ENTRY.size
        if start > len(data):
            raise ValueError("short escape directory")
        self.entries = [ENTRY.unpack_from(data, HEADER.size + i * ENTRY.size)
                        for i in range(count)]
        self.run_ids = [row[0] for row in self.entries]
        if any(a >= b for a, b in zip(self.run_ids, self.run_ids[1:])):
            raise ValueError("escape runs out of order")
        bit_end = 0
        for run_id, domain_start, length, offset in self.entries:
            if (run_id >= self.runs or domain_start >= self.n or not length
                    or length > self.n or offset != bit_end):
                raise ValueError("invalid escape entry")
            bit_end += length * self.width
        if len(data) - start != (bit_end + 7) // 8:
            raise ValueError("escape payload length")
        self.payload = memoryview(data)[start:]
        if bit_end % 8 and self.payload and self.payload[-1] >> (bit_end % 8):
            raise ValueError("nonzero escape padding")

    def successor(self, run_id, value):
        j = bisect.bisect_left(self.run_ids, run_id)
        if j == len(self.run_ids) or self.run_ids[j] != run_id:
            return None
        _, start, length, offset = self.entries[j]
        delta = (value - start) % self.n
        if delta >= length:
            raise ValueError("SA value outside escaped run domain")
        bit = offset + delta * self.width
        byte = bit // 8
        shift = bit % 8
        need = (shift + self.width + 7) // 8
        raw = int.from_bytes(self.payload[byte:byte + need], "little")
        result = (raw >> shift) & ((1 << self.width) - 1)
        if result >= self.n:
            raise ValueError("escape successor out of range")
        return result
