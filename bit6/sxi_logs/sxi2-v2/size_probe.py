#!/usr/bin/env python3
"""Read-only SXI1 member and idealized SXI2 source-codec sizing probe.

This is an estimate, not a container writer. It never touches the input file.
"""
import argparse
import heapq
import json
import math
import struct

import numpy as np


def huffman_bits(counts):
    heap = [(int(c), i) for i, c in enumerate(counts) if c]
    heapq.heapify(heap)
    if len(heap) == 1:
        return heap[0][0]
    bits = 0
    serial = 256
    while len(heap) > 1:
        a, _ = heapq.heappop(heap)
        b, _ = heapq.heappop(heap)
        bits += a + b
        heapq.heappush(heap, (a + b, serial))
        serial += 1
    return bits


def probe(path):
    with open(path, "rb") as f:
        h = f.read(64)
        if h[:4] != b"SXI1":
            raise ValueError("not SXI1")
        n, k, r = struct.unpack_from("<QQQ", h, 8)
        count = struct.unpack_from("<I", h, 32)[0]
        directory = f.read(40 * count)
        members = {}
        for i in range(count):
            member_id, codec, offset, size, items, crc, reserved = struct.unpack_from(
                "<IIQQQII", directory, 40 * i
            )
            members[member_id] = dict(bytes=size, count=items, offset=offset)
    m = members[1]
    assert m["count"] == r and m["bytes"] == 2048 + 5 * r
    heads = np.memmap(path, mode="r", dtype="u1", offset=m["offset"] + 2048, shape=(r,))
    lengths = np.memmap(
        path, mode="r", dtype="<u4", offset=m["offset"] + 2048 + r, shape=(r,)
    )
    counts = np.zeros(256, dtype=np.int64)
    for start in range(0, r, 1 << 22):
        counts += np.bincount(heads[start : start + (1 << 22)], minlength=256)
    head_bits = huffman_bits(counts)
    gamma_bits = 0
    for start in range(0, r, 1 << 22):
        chunk = lengths[start : start + (1 << 22)]
        if np.any(chunk == 0):
            raise ValueError("zero length")
        # Every u32 is exact as f64; frexp gives exact integer bit length.
        exponent = np.frexp(chunk.astype(np.float64))[1]
        gamma_bits += int(np.sum(2 * exponent - 1, dtype=np.int64))
    chi = members[5]["count"]
    # Elias-Fano universe is [0,n], including the virtual end.
    low = max(0, int(math.floor(math.log2((n + 1) / chi)))) if chi else 0
    ef_bits = chi * low + chi + ((n + 1) >> low) + 1 if chi else 0
    return dict(
        path=path, n=n, k=k, r=r, chi=chi,
        sxi1_members={str(i): dict(bytes=v["bytes"], count=v["count"]) for i, v in members.items()},
        huffman_head_bits=head_bits, gamma_length_bits=gamma_bits,
        idealized_rlbwt_bytes=(head_bits + gamma_bits + 7) // 8,
        elias_fano_low_bits=low, idealized_ef_bytes=(ef_bits + 7) // 8,
        caveat="Excludes codebook/checkpoints, LF map, phi map, anchors, directory and padding",
    )


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("paths", nargs="+")
    a = p.parse_args()
    print(json.dumps([probe(path) for path in a.paths], indent=2))
