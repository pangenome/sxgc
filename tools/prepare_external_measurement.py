#!/usr/bin/env python3
"""Prepare the explicitly permitted cleaned measurement corpus, not a byte codec.

Reads SOURCE once, optionally copies those source bytes, maps bytes outside
6..127 and existing 0x1e to 6, and appends a unique 0x1e terminal. All outputs must be
new. This is test-data preparation; construction reads the resulting fixture
once inside PFP. Use the byte-remap lane for an exact arbitrary-byte build.
"""
import argparse
import contextlib
import hashlib
import json
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--cleaned',type=Path,required=True)
p.add_argument('--raw-copy',type=Path)
p.add_argument('--limit',type=int)
p.add_argument('--manifest',type=Path,required=True)
a=p.parse_args()
if a.limit is not None and a.limit<=0:p.error('--limit must be positive')
for path in (a.cleaned,a.raw_copy,a.manifest):
    if path is not None and path.exists():p.error(f'refusing existing output: {path}')
table=bytes(c if 6<=c<128 and c!=30 else 6 for c in range(256))
h=hashlib.sha256();clean_hash=hashlib.sha256();n=low=high=reserved=0
with contextlib.ExitStack() as stack:
    source=stack.enter_context(a.source.open('rb'))
    cleaned=stack.enter_context(a.cleaned.open('xb'))
    raw=stack.enter_context(a.raw_copy.open('xb')) if a.raw_copy else None
    while a.limit is None or n<a.limit:
        block=source.read(min(1<<20,a.limit-n) if a.limit is not None else 1<<20)
        if not block:break
        n+=len(block);h.update(block)
        low+=len(block)-len(block.translate(None,bytes(range(6))))
        high+=len(block)-len(block.translate(None,bytes(range(128,256))))
        reserved+=block.count(b'\x1e')
        if raw:raw.write(block)
        translated=block.translate(table)
        cleaned.write(translated);clean_hash.update(translated)
    if a.limit is not None and n!=a.limit:raise RuntimeError('source shorter than requested slice')
    cleaned.write(b'\x1e')
    clean_hash.update(b'\x1e')
with a.manifest.open('x') as f:
    json.dump(dict(source=str(a.source.resolve()),bytes=n,sha256=h.hexdigest(),
        cleaned_sha256=clean_hash.hexdigest(),low_bytes_changed=low,high_bytes_changed=high,
        reserved_terminal_bytes_changed=reserved,replacement=6,appended_terminal=30,
        warning='Measurement corpus only; not a lossless byte-remap implementation.'),f,indent=2)
    f.write('\n')
