#!/usr/bin/env python3
"""Check private chunk artifact framing and contiguous source partition."""
import pathlib
import struct
import sys


def main(directory, count, expected_bytes):
    root = pathlib.Path(directory)
    offset = total_runs = 0
    lengths = []
    for i in range(count):
        path = root / f'chunk-{i}.crle'
        with path.open('rb') as stream:
            header = stream.read(32)
        assert len(header) == 32 and header[:4] == b'SXCR', path
        version, source_offset, n, r = struct.unpack_from('<IQQQ', header, 4)
        assert version == 1 and n and r and source_offset == offset, path
        assert path.stat().st_size == 32 + 21*r, path
        lengths.append(n)
        offset += n
        total_runs += r
    assert offset == expected_bytes, (offset, expected_bytes)
    assert len(list(root.glob('chunk-*.crle'))) == count
    print(f'CHUNK_HEADER_PASS chunks={count} n={offset} sum_local_runs={total_runs} '
          f'min_n={min(lengths)} max_n={max(lengths)}')


if __name__ == '__main__':
    assert len(sys.argv) == 4, 'usage: verify_chunks.py DIR COUNT EXPECTED_BYTES'
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
