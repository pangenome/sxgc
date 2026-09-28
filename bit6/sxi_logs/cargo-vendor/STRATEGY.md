# Complete Cargo installation strategy

Decision: true in-crate source vendoring. Install-time upstream fetch cannot meet
an offline build or zero-upstream-drift promise. The package carries immutable
source archives for all pinned C++ projects and submodules, the exact local
patches, native stage sources (including seam repair), Python orchestration, and
an offline vendored agc2flat Linux x86_64 Cargo build dependency closure. Source SHA256s are checked
before any compilation. No existing build tree is reused.

Cargo build.rs builds into OUT_DIR only, embeds the resulting tools and scripts,
and binds their hashes plus Cargo package version and source provenance in an
embedded installation manifest. At runtime a private cache directory keyed by
that manifest holds extracted tools; every member is checked before Python or
native code is launched. A changed existing member fails rather than being
silently repaired. XSA_TOOLS keeps its development override.

The xsa executable's self-hash cannot be embedded in itself; installation
provenance instead binds the package version/source identity in the binary,
records the executing xsa SHA256 at verification time, and binds every bundled
stage's hash at build time. Existing source-tree manifests keep their original
strict verification behavior.

Host prerequisites: Linux, C/C++ compilers, CMake, Make, Python 3, system zlib and
htslib's system compression development libraries; runtime Python 3 and GNU time.
Cargo registry dependencies require the normal registry cache or connectivity;
all upstream Git and native stage dependencies are shipped in the package.
No protected 466 paths or processes are modified. No commits or staging.
