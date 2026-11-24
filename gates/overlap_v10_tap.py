"""Small u32 triple stream -> unchanged triples + one outstanding ctx-tap band."""
import pathlib
import struct
import sys
import time

BAND = 32768


def relay(src, sink, bands, n):
    if not BAND < n <= 8 << 20:
        raise ValueError('fixture corpus length outside bounds')
    count = zeros = band = 0
    while count <= n:
        size = min(BAND, n + 1 - count)
        block = src.read(size * 12)  # buffered blocking pipe, full read or EOF
        if len(block) != size * 12:
            raise ValueError('short row stream')
        positions = bytearray(8 * size)
        for i, (p, h, b) in enumerate(struct.iter_unpack('<III', block)):
            if (count + i == 0) != (p == n) or p > n or h > n - p or not (
                    b == 256 if p == 0 else b < 256):
                raise ValueError('invalid fixture triple')
            zeros += p == 0
            struct.pack_into('<Q', positions, 8 * i, p)
        prefix = bands / f'band_{band}'
        prefix.with_suffix('.sa').write_bytes(positions)
        prefix.with_suffix('.opened').touch(exist_ok=False)
        sink.write(block)
        sink.flush()
        while not prefix.with_suffix('.consumed').exists():
            time.sleep(.002)  # supervised with tap; never run detached
        for suffix in ('.opened', '.consumed'):
            prefix.with_suffix(suffix).unlink()
        count += size
        band += 1
    if src.read(1) or zeros != 1:
        raise ValueError('incorrect row count or unique BWT END')
    (bands / 'end').write_text(str(band))
    (bands / 'end.ready').touch(exist_ok=False)


if __name__ == '__main__':
    relay(sys.stdin.buffer, sys.stdout.buffer, pathlib.Path(sys.argv[1]), int(sys.argv[2]))
