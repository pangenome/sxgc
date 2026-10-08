#!/usr/bin/env python3
"""Refresh in-crate first-party snapshots before packaging; never run at install."""
from pathlib import Path
import hashlib
import shutil
import json
ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / 'xsa/runtime'
PATTERNS = ('bit6/*.hpp', 'bit6/pfp_ds_vendor/**/*.hpp',
            'bit6/chi_rspace_dump.cpp', 'bit6/rpfbwt_endpoints.cpp',
            'bit6/chunk_frontend.cpp', 'bit6/cross_lcp_merge.cpp',
            'bit6/third_party/libsais/*',
            'bit6/sxi_write.cpp', 'bit6/sxi_text_audit.cpp', 'bit6/phi_inverse_heads.cpp',
            'bit6/sxi_*.py', 'bit6/patches/pfp_agc.patch',
            'bit6/patches/rpfbwt_emit_tails.patch', 'tools/tool_manifest.py',
            'tools/upstream.lock.json', 'tools/build_slim_dump.sh',
            'agc2flat/Cargo.toml', 'agc2flat/Cargo.lock', 'agc2flat/src/*.rs', 'LICENSE')

def sources():
    return sorted({p for pattern in PATTERNS for p in ROOT.glob(pattern) if p.is_file()})

def destination(source):
    p = DEST / source.relative_to(ROOT)
    return p.with_name('Cargo.toml.in') if p.name == 'Cargo.toml' else p

if __name__ == '__main__':
    for p in sources():
        dst = destination(p)
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(p, dst)
    files = sorted(p for d in (DEST, ROOT/'xsa/vendor', ROOT/'xsa/src') for p in d.rglob('*') if p.is_file())
    files += [ROOT/'xsa/build.rs', ROOT/'xsa/build_tools.py']
    (ROOT/'xsa/SOURCES.sha256.json').write_text(json.dumps({
        str(p.relative_to(ROOT/'xsa')): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in files}, indent=2)+'\n')
