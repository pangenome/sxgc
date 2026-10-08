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

## Gates — ALL PASSED (2026-10-08)

1. **cargo build + `xsa build --help`** — PASS (release build green; both build routes in help).
2. **fragment-scale byte identity through `xsa build --input`** — PASS:
   `CONSOLIDATION_FRAG_GATE_DONE pass=8 fail=0`. One command (32 chunks, 48
   threads, pool ON, ulimit -v 6 GiB per phase) reproduced the banked
   fragment reference byte-for-byte: frag.{rlebwt,rlebwt.meta,ssa,ssa_t}
   and fresh.{ri4,head_sa,agg,sA} all BYTE_IDENTICAL_TO_BANKED; chi =
   306164765 (N=1082130213, R=397723010) exactly. Emitted sidecars match the
   banked lever-gate numbers: pftext = n bytes exactly, pfck tau=22
   count=49,187,737 (bytes=393,501,936). Walls: chunk 3:33 (1.3 GB), merge
   tree 6:57:38 (31 pairs, peak RSS 2.53 GB under the cap; gate-5 no-pool
   reference was 8:52:38), endpoints 5:23 (394 MB), slim 18:20 (30 MB),
   sweep 0:59 (6 MB). Driver log: `frag-gate-driver.stdout.log`, verdicts:
   `frag-xsa-build.cmp.log`.
3. **six-seed selftest** — PASS: `xsa build --selftest 2000` (48 threads,
   capped, scratch nvme2n1) — SELFTEST_PASS for seeds 11, 99, 20261002, 7, 5,
   13 at 2000 cases each, SELFTEST_SWEEP_PASS (`selftest-sweep.log`).
4. **fresh `cargo test`** — PASS: 8/8 (snap refusal mid-document, snap aligned,
   disc gate boundary 85/86, production remap == banked frag.remap,
   time -v telemetry parse) plus the pre-existing run-width tests; live CLI
   refusals demonstrated too (`refusal-snap.log`, `refusal-disc.log`).

Additional smoke evidence (pre-gate): on a 1.09 MB corpus all 10 chain
artifacts byte-identical to the pre-consolidation /tmp/extcols reference
chain; 4-chunk vs 7-chunk trees byte-identical; chi = 744578 agrees across
the chunk route and the PFP pipeline route; the runtime remap drift check
caught (and I fixed) the map being an involution (0xFC..0xFF map back to
0x04..0x01) which the prose description had omitted.

## Disc ledger

* My authorized deletions: extcols-scratch/{f4tree 103G, fragext 93G,
  poolfix-int 13G, poolfix-nopool 13G, poolfix-rig 0.9G, poolstress.bin}
  — freed 238.4 GB. real10b-ref untouched.
* Supervisor cleared the remainder (sxgc-trend scratch 288G,
  long-window-cfac1c84 100G) → 84% used at gate-2 launch; 85% after.

## Notes for the 10 GB launch (supervisor)

`xsa build --input /mnt/nvme2n1/erikg/extcols-scratch/real10b-ref/pile-10b-snap.txt
  --scratch <fresh nvme2n1 dir> --snap-1e --memory-gb 24 --threads 48
  [--chunks 32]` — the snap sha256 gate check (d440d9e1... 10 GB snap) is
  journaled via SNAP_OK; scratch must hold >= 15% free at start (currently
  85% used — free space before launching; the run needs roughly 0.4-0.6 TB);
  byte-identity targets per gate_10b_external_rebuild.sh: merged 4 files
  + pftext/pfck + ext.{ri4,head_sa,agg,sA} vs real10b-ref. Fragment scratch
  (consolidation-frag, ~40 GB) can be deleted after inspection if space is
  needed.
