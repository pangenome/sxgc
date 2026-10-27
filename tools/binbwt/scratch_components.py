"""Disjoint allocated-block accounting from one du traversal per sample."""
from pathlib import Path
import subprocess


def component(relative):
    p = Path(relative)
    s = str(p)
    if s.startswith('order/waves/'): return 'waves'
    if s.startswith('order/bands/'): return 'packed_bands'
    if s == 'order/column': return 'column'
    if s == 'order/isa-sample': return 'isa_sample'
    if '.pieces/' in s: return 'pieces'
    if '.wit-' in p.name: return 'witness_spills'
    for suffix in ('rlebwt', 'sparse2', 'pfck2'):
        if p.name.endswith('.' + suffix): return suffix
    if s == 'product/product.sxi2': return 'container'
    if s == 'band-stream.bin': return 'debug_dump'
    return 'other:' + s


def snapshot(root):
    root = Path(root)
    run = subprocess.run(['du', '-a', '-B1', '--null', str(root)],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    entries = {}
    for record in run.stdout.split(b'\0'):
        if not record: continue
        size, path = record.split(b'\t', 1)
        entries[Path(path.decode())] = int(size)
    if root not in entries:
        raise RuntimeError('du did not report run root')
    # du directory records include children. Subtract direct child records to
    # retain directory metadata once, without a second, inconsistent traversal.
    own = dict(entries)
    for path, size in entries.items():
        if path != root and path.parent in own:
            own[path.parent] -= size
    parts = {}
    for path, size in own.items():
        label = component(path.relative_to(root))
        parts[label] = parts.get(label, 0) + size
    assert sum(parts.values()) == entries[root]
    order = entries.get(root/'order', 0)
    return order, entries[root], parts
