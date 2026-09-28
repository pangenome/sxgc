# Cargo installation review notes

The installation route compiles only inside fresh Cargo OUT_DIR stage directories.
Native FetchContent is fully disconnected and every named dependency is supplied
from a pinned archive. AGC Git submodule initialization is disabled because all
required submodules are already shipped. The adapter builds with --offline and
--locked from a separately vendored complete Cargo closure. Python scripts,
local native/seam sources, archive hashes, and build scripts are source-bound.

Runtime installation uses an atomic directory rename under a private cache.
Every embedded member (including scripts, notices and unused tools) is checked
before Python launches. Missing, nonregular, nonexecutable, or changed native
members fail closed. An embedded manifest hash authenticates installation
provenance; rewriting the on-disk manifest cannot bless a modified tool.
XSA_TOOLS overrides are separately hashed and the existing explicit source
pipeline override keeps its sibling-tool resolution behavior.

Self-hash limitation: xsa records its executing binary SHA256, but cannot embed
its own final hash. Cargo package identity and bundled stage hashes are embedded;
no runtime operation silently re-blesses cached native stages. As with ordinary
Cargo installs, registry/crate integrity is the trust anchor for the executable.
Host Python, GNU time and dynamic system libraries remain host prerequisites.

Scope: no algorithm changes, no new O(n) reconstruction step, no changes to live
466 trees/processes, no staging and no commits. The workspace already contained
many unrelated modifications on entry; this lane does not claim them.

Distribution size: the initial complete snapshots produced a 42.6 MiB crate.
Unused native test fixtures and unbuilt Rust platform/development payloads were
removed while retaining Cargo resolution metadata, target entrypoints and
licenses. The compiled Linux x86_64 dependency closure is fully vendored.
The resulting Cargo package is below 10,000,000 bytes; a compressed-payload
regression prevents exceeding the conservative registry budget. Exact omitted
paths and the retained build dependency closure are shipped in vendor metadata.
This run does not publish a registry release.

Review gate: this is an implementation self-review. Independent reviewer approval
remains required by the task contract; no supervisor/contact tool was exposed in
this session, and delegation was not authorized.

Registry size reference verified against the current Cargo Book:
https://doc.rust-lang.org/cargo/reference/publishing.html (10 MB default limit).
