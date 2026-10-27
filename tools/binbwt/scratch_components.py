"""Disjoint allocated-block accounting from one du traversal per sample."""
from pathlib import Path
import subprocess


def component(relative):
    s = str(relative)
    name = s.rsplit('/', 1)[-1]
    if s == 'order/waves' or s.startswith('order/waves/'): return 'waves'
    if s == 'order/bands' or s.startswith('order/bands/'): return 'packed_bands'
    if s == 'order/column': return 'column'
    if s == 'order/isa-sample': return 'isa_sample'
    if s.endswith('.pieces') or '.pieces/' in s: return 'pieces'
    if '.wit-' in name: return 'witness_spills'
    for suffix in ('rlebwt', 'sparse2', 'pfck2'):
        if name.endswith('.' + suffix): return suffix
    if s in ('product/product.sxi2', 'product/product.sxi2.partial'): return 'container'
    for suffix in ('bits-head', 'bits-len', 'bits-high', 'bits-low'):
        if name.endswith('.' + suffix): return suffix
    if s == 'band-stream.bin': return 'debug_dump'
    return 'other:' + s


def snapshot(root):
    root = Path(root)
    run = subprocess.run(['du', '-a', '--max-depth=2', '-B1', '--null', str(root)],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    base = str(root)
    prefix = base + "/"
    entries = {}
    for record in run.stdout.split(b'\0'):
        if not record: continue
        size, path = record.split(b'\t', 1)
        entries[path.decode()] = int(size)
    if base not in entries:
        raise RuntimeError('du did not report run root')
    # du directory records include children. Subtract direct child records to
    # retain directory metadata once, without a second, inconsistent traversal.
    own = dict(entries)
    for path, size in entries.items():
        parent = path.rsplit('/', 1)[0]
        if path != base and parent in own:
            own[parent] -= size
    parts = {}
    for path, size in own.items():
        label = component(path[len(prefix):] if path != base else ".")
        parts[label] = parts.get(label, 0) + size
    assert sum(parts.values()) == entries[base]
    order = entries.get(base+'/order', 0)
    return order, entries[base], parts
