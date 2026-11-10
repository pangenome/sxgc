#!/usr/bin/env python3
"""Compile a private copy of the production kernel plus study-only instrumentation."""
import os, pathlib, shutil, subprocess, sys, tempfile
root = pathlib.Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='chi-transition-study-') as d:
    p = pathlib.Path(d)
    shutil.copytree(root/'xsa/src', p/'src')
    manifest = (root/'xsa/Cargo.toml').read_text()
    manifest = manifest.replace('build = "build.rs"', '')
    (p/'Cargo.toml').write_text(manifest)
    shutil.copy(root/'xsa/Cargo.lock', p/'Cargo.lock')
    bundle = p/'src/bundle.rs'
    bundle.write_text(bundle.read_text().replace('include!(concat!(env!("OUT_DIR"), "/bundled_tools.rs"));', 'pub const ID: &str = "study-no-tools"; pub const FILES: &[(&str, &[u8], &str)] = &[];'))
    main = p/'src/main.rs'
    main.write_text(main.read_text().replace('fn main() {', 'fn product_main_unused() {', 1) + '\nfn main() { qlite::transition_study(); }\n')
    qlite = p/'src/qlite.rs'
    qlite.write_text(qlite.read_text() + '\n' + (pathlib.Path(__file__).with_name('probe.rs')).read_text())
    os.environ['RUSTFLAGS'] = os.environ.get('RUSTFLAGS', '') + ' -Awarnings'
    subprocess.run(['cargo','build','--locked','--release','--manifest-path',str(p/'Cargo.toml')], check=True, stdout=sys.stderr)
    subprocess.run([str(p/'target/release/xsa'), *sys.argv[1:]], check=True)
