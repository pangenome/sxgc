# Complete Cargo installation

`cargo install --locked xsa` builds and installs one self-contained xsa executable.
`cargo build --release --locked --manifest-path xsa/Cargo.toml` produces the same
product in a checkout. All pipeline stages, Python scripts, the patched PFP++
parser with pinned AGC library, patched rpfbwt, seam repair, and agc2flat are
compiled from the sources shipped in this crate. No upstream repository is
fetched during either build. The source archives include dependency licenses.

This selects **in-crate vendoring**, not install-time upstream fetching. Git
revision and submodule identities are in `vendor/native-provenance.json`; exact
archive and first-party source hashes are in `SOURCES.sha256.json`. The AGC
adapter's entire Cargo dependency closure is vendored, including pinned ragc.
The outer Rust dependencies use normal Cargo registry resolution; use `--offline`
with a populated Cargo cache. Source mirrors are refreshed by
`python3 tools/package_xsa_sources.py` in the repository and guarded by
`python3 bit6/test_cargo_sources.py`.

Supported build host: native Linux x86_64, Python >= 3.12, GCC/G++ >= 10, CMake, Make,
patch, and the zlib/OpenSSL development libraries used by PFP/htslib. Runtime:
Python >= 3.12, GNU `/usr/bin/time`, and the standard native runtime libraries
reported by `ldd` (libstdc++, libgomp, zlib, OpenSSL on the tested host). AGC ghost
mode additionally needs usable FUSE; native AGC parsing and `--fifo` remain
available. `BUILD_JOBS` limits native parallelism (capped at four). Cross builds, non-Linux hosts and non-x86_64 targets are rejected explicitly.

Cargo build output is embedded in xsa. On first `xsa build`, stages are extracted
atomically below `${XDG_CACHE_HOME:-$HOME/.cache}/xsa/<manifest-sha256>/`. Every
file is checked against its embedded SHA256 before launching Python. Later
changes fail loudly; remove a damaged cache directory explicitly to re-extract.
No repository, sibling binaries, configured XSA variables, or prebuilt tools are
needed. Copying the installed executable to another directory works.

The installation manifest binds Cargo package version, vendored source identity,
upstream pins, and built native/Python hashes. Pipeline journals include it and
also record the running xsa's SHA256. A binary cannot embed its own final hash;
its self-hash is observed, while all bundled stages are bound at build time.
`XSA_TOOLS` overrides the native tool directory for development, preserving
manifest checks; `XSA_PIPELINE` selects a development source pipeline. Legacy
source manifests are still supported with that checkout's source pipeline.
`--allow-drift` remains a logged development escape for external tool overrides;
it never silently repairs the embedded bundle's cache.

`bit6/test_cargo_install.py --xsa /path/to/xsa --log-dir /fresh/logs` checks a
relocated binary with a fresh HOME, no XSA environment, end-to-end publication,
source auditing, damaged-stage rejection, manifest reblessing rejection, and
the developer override. Source archives omit unused PFP/teratools test datasets.
The package omits unused upstream test fixtures, old Python binding copies, and
unbuilt Rust platform/development payloads. Resolution manifests, target
entrypoints and notices remain; `vendor/pruned-paths.json` records every omitted
path and `vendor/rust-linux-packages.json` records the compiled Rust closure.
`tools/compact_xsa_vendor.py` maintains these archives. A size regression checks
the source payload against the registry's 10 MB default limit. Local package and
installation evidence is in `bit6/sxi_logs/cargo-vendor/`; this run does not
publish a registry release.
