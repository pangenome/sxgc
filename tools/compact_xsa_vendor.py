#!/usr/bin/env python3
"""Maintainer-only: compact pinned archives without dropping Linux build sources.

Usage: compact_xsa_vendor.py RAW_NATIVE_TAR CARGO_VENDOR_DIR LINUX_PACKAGES_JSON
The package list comes from the non-dev dependency closure of cargo metadata
--locked --filter-platform x86_64-unknown-linux-gnu for agc2flat. Unused packages
retain resolution manifests, target entrypoints and notices, but not their
unused platform/development payloads. No surviving source bytes are changed.
"""
import io
import json
from pathlib import Path
import sys
import tarfile
import tomllib

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT/'xsa/vendor'
NATIVE_OMIT = ('gsacak/experiments/', 'htslib/test/', 'htslib/htscodecs/tests/',
              'agc/3rd_party/zstd/tests/', 'agc/3rd_party/zlib-ng/test/',
              'agc/3rd_party/zlib-ng/doc/', 'agc/3rd_party/mimalloc/doc/',
              'agc/3rd_party/mimalloc/docs/', 'agc/3rd_party/pybind11/docs/',
              'agc/3rd_party/pybind11/tests/', 'agc/3rd_party/pybind11-2.11.1.old/')

def notice(path):
    return path.name.lower().startswith(('license', 'copying', 'notice', 'copyright'))

if __name__ == '__main__':
    native, cargo, packages = map(Path, sys.argv[1:])
    used = set(map(tuple, json.loads(packages.read_text())))
    omissions = {'native': [], 'rust': []}
    with tarfile.open(native) as src, tarfile.open(DEST/'native.tar.xz', 'w:xz', preset=6) as dst:
        for member in src:
            if member.name.startswith(NATIVE_OMIT):
                omissions['native'].append(member.name)
                continue
            dst.addfile(member, src.extractfile(member) if member.isfile() else None)
    with tarfile.open(DEST/'agc-cargo.tar.xz', 'w:xz', preset=6) as archive:
        for crate in sorted(cargo.iterdir()):
            manifest = tomllib.loads((crate/'Cargo.toml').read_text())
            meta = manifest['package']
            active = (meta['name'], meta['version']) in used
            entrypoints = {'Cargo.toml', '.cargo-checksum.json', 'src/lib.rs', 'src/main.rs', 'build.rs'}
            for section in ('lib', 'bin', 'example', 'test', 'bench'):
                targets = manifest.get(section, [])
                if isinstance(targets, dict): targets = [targets]
                entrypoints.update(t['path'] for t in targets if 'path' in t)
            if isinstance(meta.get('build'), str): entrypoints.add(meta['build'])
            files = [p for p in sorted(crate.rglob('*')) if p.is_file()]
            kept = {str(p.relative_to(crate)) for p in files
                    if active or str(p.relative_to(crate)) in entrypoints or notice(p)}
            for path in files:
                relative = str(path.relative_to(crate))
                if relative not in kept:
                    omissions['rust'].append(str(path.relative_to(cargo)))
                    continue
                raw = path.read_bytes()
                member = archive.gettarinfo(path, arcname=str(path.relative_to(cargo)))
                if relative == '.cargo-checksum.json':
                    checksum = json.loads(raw)
                    checksum['files'] = {n: h for n, h in checksum['files'].items() if n in kept}
                    raw = json.dumps(checksum, sort_keys=True).encode()
                    member.size = len(raw)
                archive.addfile(member, io.BytesIO(raw))
    (DEST/'pruned-paths.json').write_text(json.dumps(omissions, indent=2)+'\n')
    (DEST/'rust-linux-packages.json').write_text(json.dumps(sorted(used), indent=2)+'\n')
