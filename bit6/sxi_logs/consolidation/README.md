# THE CONSOLIDATION: `xsa build --input` — one cargo-built command for the chain

Status: IN PROGRESS (this file is the live journal of the consolidation lane).

## What shipped

The external construction chain — previously four hand-built binaries driven
by the gate scripts — is now folded into the xsa crate as a single
`xsa build --input <corpus> --scratch <dir>` subcommand with clean provenance
from pushed main (003e314):

* Sources: `tools/package_xsa_sources.py` now snapshots `bit6/chunk_frontend.cpp`,
  `bit6/cross_lcp_merge.cpp` and `bit6/third_party/libsais/*` into
  `xsa/runtime/` alongside the (refreshed) finish sources
  (`chi_rspace_dump.cpp`, `rpfbwt_endpoints.cpp`, `slim_lce.hpp`,
  `seam_repair.hpp`, `ext_columns.hpp`), all pinned by
  `xsa/SOURCES.sha256.json` and verified at build time.
* Build: `xsa/build_tools.py` compiles the chain into the verified bundle:
  `chunk_frontend` (gcc -O3 libsais token front end, the gate flags) and
  `cross_lcp_merge` (g++ -O2 -mcx16 -std=c++17 -pthread -latomic, cpuid cx16
  gate at startup, fixed concurrent pool ON by default,
  CROSS_NO_POOL/CROSS_ASYNC_FILL/CROSS_HASH_K/CROSS_PIPELINE env knobs)
  alongside the existing rpfbwt_endpoints/slim_dump stages. The bundle is
  SHA256-verified on every use (`xsa/src/bundle.rs`).
* Driver: `xsa/src/chain.rs` runs snap preflight (fail-loud on mid-document
  input — the demonstrated refusal — before any phase), the chunk front end
  with the embedded production byte-remap (frag.remap,
  sha256 b4f387767707f3095a0981148a1753f8da5885c30c19753b3b93887153e611be,
  asserted at runtime and in a unit test), the externalized merge tree
  (`--emit-pf`), the external adopt finish (endpoints + slim consume
  `.pftext`/`.pfck`; sweep = this same xsa binary), and journals chi +
  per-phase wall / peak-RSS / disc telemetry into one uniform
  `xsa-build.log` under the scratch tree.
* Discipline: every phase runs under a hard RLIMIT_AS cap (`--memory-gb`,
  default 24 GiB; a lower inherited hard limit wins and is journaled), and a
  disc preflight refuses when the scratch filesystem holds under 15% free.
* Selftest: `xsa build --selftest [CASES]` runs the merge selftest for the
  six banked sweep seeds (11, 99, 20261002, 7, 5, 13; default 2000 cases).
* Tests: `cargo test` exercises the snap refusal (mid-document input fails
  loud) and the disc preflight boundary (85% used passes, 86% refuses), the
  production remap byte-for-byte against the banked frag.remap, and the
  time -v telemetry parser.

No algorithm changes: the bundled stages are the same sources built with the
same gate flags, so outputs must be byte-for-byte those of the reference chain.

## Gates

1. cargo build + `xsa build --help` — logs: `cargo-build.log`, `help.log`
2. fragment-scale byte identity through `xsa build --input`
   (`gate_consolidation_frag.sh`): the four merged files + the four finish
   artifacts byte-identical to the banked fragment reference
   (BANK=/home/erikg/cross-lcp-run/merged), chi = 306164765 exactly.
3. six-seed selftest via `xsa build --selftest` — logs: `selftest-*.log`
4. fresh `cargo test` (snap refusal + disc preflight) — logs: `cargo-test.log`

## Findings / blockers

* DISC: /mnt/nvme2n1 sits at ~88.2% used; the mandated disc preflight
  (>= 15% free) refuses. Both this lane's fragment gate and the supervisor's
  10 GB launch require cleanup first (escalated to the supervisor with
  numbers; project-owned scratch candidates total only ~223 GB of the
  ~480 GB needed to reach 85%).

## Logs

* `frag-xsa-build.driver.log` / `frag-xsa-build.cmp.log` — gate 2 driver + verdicts
* `selftest-sweep.log` — gate 3
* `cargo-test.log` — gate 4
