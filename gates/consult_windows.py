#!/usr/bin/env python3
"""Generate a `--windows` set for `xsa consult` in a given geometry.

The default geometry is the one their cache uses: T2048 windows at 2048-aligned
document offsets, so within a window the spine RAMPS 1..1024 bytes and then
holds 1024 (context_bytes = min(position, 1024), context never crossing the
window start, which is what flagship_cache.write_window asserts). `--len`
changes the window length (128 reproduces the c14 benchmark sets, where the
spine can never exceed 127 bytes).

  usage: consult_windows.py OUT.jsonl|OUT.bin [--positions N] [--len 2048]
                            [--offset OFF] [--left 1024] [--doc-stride N]
"""
import argparse
import json
import struct


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--positions', type=int, default=1_000_000)
    ap.add_argument('--len', type=int, default=2048)
    ap.add_argument('--left', type=int, default=1024)
    ap.add_argument('--offset', type=int, default=3_000_000_000,
                    help='first byte of the first window (2048-aligned)')
    ap.add_argument('--doc-stride', type=int, default=0,
                    help='>0: start a new doc id every N windows')
    args = ap.parse_args()
    assert args.len >= 1 and args.left >= 1
    assert args.offset % args.len == 0 or args.len == 1
    n_win = (args.positions + args.len - 1) // args.len
    wins = []
    for i in range(n_win):
        base = args.offset + i * args.len
        doc = (i // args.doc_stride) if args.doc_stride else i
        wins.append((doc, base, 0, args.len, args.left))
    if args.out.endswith('.bin'):
        with open(args.out, 'wb') as f:
            f.write(b'XSWIN001' + struct.pack('<QQQII', 0, 0, 0, 0, 0))
            for w in wins:
                f.write(struct.pack('<QQQII', *w))
    else:
        with open(args.out, 'w') as f:
            for doc, base, start, ln, left in wins:
                f.write(json.dumps({'doc': doc, 'base': base, 'start': start,
                                    'len': ln, 'left': left}) + '\n')
    print('CONSULT_WINDOWS out=%s windows=%d positions=%d len=%d left=%d offset=%d'
          % (args.out, len(wins), len(wins) * args.len, args.len, args.left, args.offset))


if __name__ == '__main__':
    main()
