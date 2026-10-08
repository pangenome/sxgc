#!/usr/bin/env python3
"""Build every packaged stage offline, entirely below Cargo OUT_DIR."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent
OUT = Path(sys.argv[1]).resolve()
VERSION = sys.argv[2]

def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def run(*args, cwd=None, env=None):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, env=env, check=True)

sources = json.loads((ROOT/'SOURCES.sha256.json').read_text())
for name, expected in sources.items():
    if digest(ROOT/name) != expected:
        raise RuntimeError('vendored source SHA256 mismatch: '+name)
# No rebuild uses an old partial source/build tree.
work = Path(tempfile.mkdtemp(prefix='stages-', dir=OUT))
deps = work/'deps'; deps.mkdir()
with tarfile.open(ROOT/'vendor/native.tar.xz') as archive:
    archive.extractall(deps, filter='data')
repo = work/'repo'; shutil.copytree(ROOT/'runtime', repo)
(repo/'agc2flat/Cargo.toml.in').rename(repo/'agc2flat/Cargo.toml')
for name, patch in [('pfp', 'pfp_agc.patch'), ('rpfbwt', 'rpfbwt_emit_tails.patch')]:
    run('patch', '-p1', '--batch', '-i', repo/'bit6/patches'/patch, cwd=deps/name)
# Fully supplied submodules: upstream initialization must never run Git/network.
p = deps/'agc/makefile'
p.write_text(p.read_text().replace('$(call INIT_SUBMODULES)', '# shipped submodules'))
# Production library build does not configure omitted upstream test fixtures.
p = deps/'agc/refresh.mk'
p.write_text(p.read_text().replace('-DZLIB_COMPAT=ON',
    '-DZLIB_COMPAT=ON -DZLIB_ENABLE_TESTS=OFF -DZLIBNG_ENABLE_TESTS=OFF -DWITH_GTEST=OFF'))
# Prevent ambient Git checkout identity from entering version metadata.
for name in ('pfp', 'rpfbwt'):
    p = deps/name/'CMakeLists.txt'
    import re
    text = re.sub(r'execute_process\(.*?OUTPUT_STRIP_TRAILING_WHITESPACE\)', '', p.read_text(), flags=re.S)
    pin = json.loads((repo/'tools/upstream.lock.json').read_text())[name]['commit']
    p.write_text('set(GIT_BRANCH "vendored")\nset(GIT_COMMIT_HASH "'+pin+'")\n'+text)
jobs = str(max(1, min(4, int(os.environ.get('BUILD_JOBS', os.environ.get('NUM_JOBS', '2'))))))
env = dict(os.environ, BUILD_JOBS=jobs, CARGO_BUILD_JOBS=jobs,
           SXI_DEPS=str(deps), SXI_RPFBWT_BUILD=str(work/'rpfbwt-build'))
# Nested Cargo must not inherit the outer jobserver or target settings.
for name in ('CARGO_MAKEFLAGS', 'MAKEFLAGS', 'MFLAGS', 'CARGO_ENCODED_RUSTFLAGS'):
    env.pop(name, None)
flags = ['-DCMAKE_BUILD_TYPE=Release', '-DFETCHCONTENT_FULLY_DISCONNECTED=ON']
flags += [f'-DFETCHCONTENT_SOURCE_DIR_{p.name.upper()}={p}' for p in sorted(deps.iterdir())]
(deps/'htslib/htscodecs/htscodecs/version.h').write_text('#define HTSCODECS_VERSION_TEXT "1.6.7"\n')
run('make', '-C', deps/'htslib', '-j', jobs, 'libhts.a', env=env)
run('make', '-C', deps/'agc', '-j', jobs, 'libagc', env=env)
flags += [f'-DHTSlib_INCLUDE_DIR={deps}/htslib/htslib', f'-DHTSlib_LIBRARY={deps}/htslib/libhts.a']
run('cmake', '-S', deps/'pfp', '-B', work/'pfp-build', '-DPFP_ENABLE_AGC=ON', f'-DAGC_ROOT={deps}/agc', *flags, env=env)
run('cmake', '--build', work/'pfp-build', '-j', jobs, '--target', 'pfp++', env=env)
run('cmake', '-S', deps/'rpfbwt', '-B', work/'rpfbwt-build', *flags, env=env)
run('cmake', '--build', work/'rpfbwt-build', '-j', jobs, '--target', 'rpfbwt', env=env)
bundle = work/'bundle'; bundle.mkdir()
for name, path in [('pfp++', work/'pfp-build/pfp++'), ('rpfbwt', work/'rpfbwt-build/rpfbwt')]:
    shutil.copyfile(path, bundle/name)
for name, src in [('rpfbwt_endpoints', 'rpfbwt_endpoints.cpp'), ('slim_dump', 'chi_rspace_dump.cpp')]:
    run('bash', repo/'tools/build_slim_dump.sh', bundle/name, 'bit6/'+src, env=env)
# External construction chain: libsais chunk front end + externalized cross-LCP
# merge (gate flags: -O3 front end; -O2 -mcx16 -pthread -latomic merge with the
# cpuid cx16 startup gate; pool knobs CROSS_* stay env-pass-through).
run('gcc', '-O3', '-I', repo/'bit6', '-c', repo/'bit6/third_party/libsais/token-libsais.c',
    '-o', work/'token-libsais.o', env=env)
run('g++', '-O3', '-std=c++17', repo/'bit6/chunk_frontend.cpp', work/'token-libsais.o',
    '-o', bundle/'chunk_frontend', env=env)
run('g++', '-O2', '-mcx16', '-std=c++17', '-pthread', repo/'bit6/cross_lcp_merge.cpp',
    '-o', bundle/'cross_lcp_merge', '-latomic', env=env)
for name in ('sxi_write', 'sxi_text_audit', 'phi_inverse_heads'):
    run('g++', '-O3', '-std=c++17', '-fopenmp', repo/f'bit6/{name}.cpp', '-o', bundle/name, env=env)
with tarfile.open(ROOT/'vendor/agc-cargo.tar.xz') as archive:
    archive.extractall(work/'agc-vendor', filter='data')
config = (ROOT/'vendor/agc-cargo-config.toml').read_text().replace('@VENDOR@', str(work/'agc-vendor'))
config_path = work/'agc-config.toml'; config_path.write_text(config)
run(os.environ.get('CARGO', 'cargo'), 'build', '--offline', '--locked', '--release',
    '--manifest-path', repo/'agc2flat/Cargo.toml', '--target-dir', work/'agc-target',
    '--config', config_path, env=env, cwd=work)
shutil.copyfile(work/'agc-target/release/agc2flat', bundle/'agc2flat')
for subdir, pattern in [('bit6', 'sxi_*.py'), ('tools', '*.py'), ('tools', '*.json')]:
    for p in (repo/subdir).glob(pattern):
        dest=bundle/subdir/p.name; dest.parent.mkdir(exist_ok=True)
        shutil.copyfile(p, dest)
# Retain dependency notices with the installed binary as well as source archives.
for tree, label in [(deps, 'native'), (work/'agc-vendor', 'rust')]:
    for p in sorted(tree.rglob('*')):
        if p.is_file() and p.name.lower().startswith(('license', 'copying', 'notice', 'copyright')):
            dest = bundle/'licenses'/label/p.relative_to(tree)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(p, dest)
files = {str(p.relative_to(bundle)): digest(p) for p in sorted(bundle.rglob('*')) if p.is_file()}
manifest = {'format': 'xsa-install-v1', 'package': {'name': 'xsa', 'version': VERSION},
            'source_sha256': digest(ROOT/'SOURCES.sha256.json'),
            'upstreams': json.loads((ROOT/'vendor/native-provenance.json').read_text()),
            'files': files}
(bundle/'MANIFEST.sha256').write_text(json.dumps(manifest, sort_keys=True, indent=2)+'\n')
# Generated Rust embeds bytes directly; no runtime tar dependency or build paths.
lines = ['pub const FILES: &[(&str, &[u8], &str)] = &[']
for p in sorted(bundle.rglob('*')):
    if p.is_file():
        lines.append(f'({json.dumps(str(p.relative_to(bundle)))}, include_bytes!({json.dumps(str(p), ensure_ascii=False)}), "{digest(p)}"),')
lines += ['];', f'pub const ID: &str = "{digest(bundle/"MANIFEST.sha256")}";']
(OUT/'bundled_tools.rs').write_text('\n'.join(lines)+'\n')
