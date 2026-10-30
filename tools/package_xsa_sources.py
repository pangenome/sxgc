#!/usr/bin/env python3
"""Refresh in-crate first-party snapshots before packaging; never run at install.

This mirrors the sources-of-truth into `xsa/runtime/` (the copy the crate's
build_tools.py actually compiles) and writes NO hash manifest: the sha256 list
this script used to emit was a hand-maintained duplicate of what git already
records, and every merge touching a covered file broke the build with a stale
digest. Provenance is git's: `xsa build` journals `XSA_SOURCE git_commit=...`
into its run journal, and `xsa/INSTALL.md` says so.
"""
from pathlib import Path
import shutil
ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / 'xsa/runtime'
PATTERNS = ('bit6/*.hpp', 'bit6/binbwt_slice.h', 'bit6/phrase_radix.h',
            'bit6/pfp_ds_vendor/**/*.hpp',
            'bit6/chi_rspace_dump.cpp', 'bit6/rpfbwt_endpoints.cpp',
            'bit6/chunk_frontend.cpp', 'bit6/cross_lcp_merge.cpp',
            'bit6/xsa_finish.cpp', 'bit6/check_pieces.c',
            'bit6/third_party/libsais/*',
            'bit6/sxi_write.cpp', 'bit6/sxi_text_audit.cpp', 'bit6/phi_inverse_heads.cpp',
            'tools/binbwt/scatter_sort.cpp', 'tools/binbwt/stream_vs_bwt.cpp',
            'tools/binbwt/sa_check.cpp',
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
