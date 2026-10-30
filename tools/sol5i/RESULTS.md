sol5i: single-pass v2 protocol (base a48ae73)

Production source is frozen at b772d83; subsequent commits attest results.
The packaged build is /mnt/nvme2n1/erikg/sol5i-cargo/release/xsa.

As landed:

- bit6/cross_lcp_merge.cpp:2653 retires ParallelSparseRuns entirely. The
  replay sink, second heap launch, count-owner bases/counts/condition
  variables, replay directory, and v2 RCI1 publication are removed.
- bit6/cross_lcp_merge.cpp:2759 / :2960 runs one heap traversal. SequentialV2
  (:3296) receives rows directly, reduces complete runs and emits their
  canonical c/len/head/tail records in rank order. Only one ordered producer
  walks; --threads still serves hash preparation, side repair and finish.
- bit6/cross_lcp_merge.cpp:3340 publishes a piece followed by its BPG1
  progress marker. :3418 validates every capture and final flank, checks
  final compressed-work limits, writes metadata and atomically publishes
  BPT1. Band bases are 65536*b; no counts are needed for numbering.
- bit6/cross_lcp_merge.cpp:3229 enforces k<=C+W with C=-1 initially,
  overflow-safe credit, W=1100 by default, and once-per-band admission.
  Unclosed oversized classes fail before admission can deadlock.
- bit6/pile_finish.cpp:410 decodes and deletes progress markers; :462 uses
  the largest full run-band class extent and EOF clamp. :400 refuses
  backward terminal progress; final R also validates class run indexes.
  :506 checks/reads/unlinks the sole piece for each band. :2190 integrates
  captured classes before repair; C advances after LCE/sweep processing.
- bit6/pile_finish.cpp:1083 extends the existing SCS1 reader entry point
  with band-only markers, deferred captures, EOF clamp, delayed full-band
  terminal publication and malformed refusals.
- xsa/src/chain.rs:1009 fixes 65536-run bands and defaults W to 1100;
  :1117 verifies exact single-pass/no-shard log fields. Native mirrors and
  xsa/SOURCES.sha256.json are refreshed. (SUPERSEDED 2026-10-30: that digest file is
  deleted; provenance is the `XSA_SOURCE git_commit=...` journal line. Native mirrors are
  still refreshed by tools/package_xsa_sources.py.)

Retained compatibility: SparseRunSpool and the legacy WM/shard planner and
RCI1 reader remain for intermediate/legacy paths and the standing oracle.
Deleted parallel-count/replay tests are superseded by canonical logical
record comparisons and band/terminal checks. The PRC1 reserved shard word
and filename suffix remain zero in the v2 path, with one piece per band.

Evidence locations:

- tools/sol5i/model-result.json and validation.json contain the attestation.
- /mnt/nvme2n1/erikg/sol5i-tests/ holds build logs, k=1/2/8 oracle logs,
  SCS1/band reader and PFCK/flow oracle logs, gate JSON and du samples.
- /mnt/nvme2n1/erikg/sol5i-gate{tiny-stalled,100m,1b}/ are final-package
  front-door runs. The -preflow siblings preserve earlier successful runs.

Model: 1,450 randomized interleavings, 1,200 terminal/stall cases, overlapping
run-band spans, 3,510 producer inequalities, ten invalid-extent refusals,
zero deadlocks, maximum backlog 1,100. It is an abstract model, not a claim
that every production interleaving was exhaustively explored.

Standing oracles: 200 cases each at k=1/2/8, cyclic and dollar modes, exact
runs and legacy SCS1 bytes, canonical production records, trivial bases,
terminal fields and stalled legacy admission. Reader: 12 band/terminal
malformed refusals plus three legacy SCS1 refusals, padded flanks, NUL and
byte 0x05. PFCK: 77 scalar cases at grids 1/7/22/31/10000, workers 1/8 and
forced 4096-byte windows. Actual flow oracle: initial C=-1, signed-overflow
boundary windows and blocked-until-consumed behavior.

Review fixes were followed by model re-validation and complete final-package
tiny/A/B reruns. No interface deviation was required. The requested
contact_supervisor tool was absent from the callable catalog. The guard
review exceeded the requested 30-minute pivot checkpoint; this is a
workflow deviation, not an unreported protocol change.

An initial development package produced an identical tiny container but
failed the obsolete FOUR_EMIT front-door log check. The front door now
checks exact SPARSE_V2_EMIT fields; final runs pass that check.

Residual risks: the ordered heap is serial, so elapsed ceilings do not
necessarily improve when work halves. Global scratch still includes live
input columns, hash preparation and growing RLE; W bounds the piece
frontier, not that entire tree. The largest admitted class extent is
model-tested; the full 2^26-row production case and a 10 GB rung were not
run. PRC1 still has 32-bit run lengths. Early finisher failure can leave the
producer waiting until the front-door timeout; crash/resume was not added.

Final-package gates, verbatim:

```text
V2_CHI_EXPECT_OK chi=29716349 == 29716349
V2_CONTAINER_PUBLISHED path=/mnt/nvme2n1/erikg/sol5i-gate100m/mergperf-100m-snap.sxi2 bytes=48768685
XSA_BUILD_DONE chi=29716349 pipeline=v2 container=/mnt/nvme2n1/erikg/sol5i-gate100m/mergperf-100m-snap.sxi2
cmp_rc=0 remaining_protocol_scratch=[]
V2_CHI_EXPECT_OK chi=283296933 == 283296933
V2_CONTAINER_PUBLISHED path=/mnt/nvme2n1/erikg/sol5i-gate1b/mergperf-1b-snap.sxi2 bytes=467814822
XSA_BUILD_DONE chi=283296933 pipeline=v2 container=/mnt/nvme2n1/erikg/sol5i-gate1b/mergperf-1b-snap.sxi2
cmp_rc=0 remaining_protocol_scratch=[]
```

Work table (before = sol5d; counts are the claim):

| Fixture / metric | Before | After | Change |
| --- | ---: | ---: | ---: |
| 100m comparisons | 715,421,580 | 355,643,748 | -50.2889% |
| 100m symbols_compared | 8,055,757,000 | 4,017,075,186 | -50.1341% |
| 100m probes | 180,431,482 | 90,131,924 | -50.0465% |
| 100m finish verified_symbols | 1,176,027,277 | 1,176,027,277 | 0% |
| 1b comparisons | 7,354,449,974 | 3,674,629,435 | -50.0353% |
| 1b symbols_compared | 120,997,390,177 | 60,485,597,061 | -50.0108% |
| 1b probes | 3,975,078,066 | 1,987,510,452 | -50.0007% |
| 1b finish verified_symbols | 12,820,424,563 | 12,820,424,563 | 0% |

1 GB sampled scratch peak: 6,360,056,483 bytes (6.360056 GB), versus
6,447,891,237 bytes (6.447891 GB): -1.362%. Sampling
uses du -sb every 0.5 s on the gate tree, excluding corpus and build cache,
matching the baseline method. Sampled peak piece count: 4; structural
limit W=1100. Final protocol scratch count is zero.

Elapsed ceilings: tiny with W=2, producer 3 ms/band and finisher 200 ms/band
40.761 s (chi=7283197, cmp=0, sampled piece peak=1); Gate A
98.706 s; Gate B 1423.678 s. These are ceilings, not speedup claims.

Branch: pi-subagents/sxgc.codex-sol-writer-sol-5i-implement-the-single-pass-protocol-INTERFACE-DECIDED-no-sh-f0cc809d-0e8089f-c47e-s0-t0

Milestone commits:

```text
a4175c4 sol5i: model no-shard fixed run bands and EOF terminal readiness
b88ce3f sol5i: stream fixed run bands from one heap traversal with captured progress and EOF assertion
8b3a595 sol5i: retire parallel sparse replay; refuse active oversized classes and handle full-band EOF
91330d3 sol5i: test delayed full-band terminal and refuse omitted class references
36a5c28 sol5i: record gate work and enforce a slower consumer in bounded tiny test
746e106 sol5i: admit each run band once and bound signed flow-window arithmetic
ff08c77 sol5i: best-effort remove consumed band scratch on the producer side
7c8cc26 sol5i: attest model, bounded stalled tiny identity, sparse and reader oracles
167054d sol5i: attest Gate A chi 29716349 and byte identity after single-pass emission
c303bed sol5i: avoid signed credit overflow for large positive windows; validate real flow waits
0a8a167 sol5i: attest first Gate B identity and ~60.486B compared symbols; record flow-boundary oracle
b772d83 sol5i: validate terminal monotonicity and class R bounds; model overlapping band spans
42d3c0a sol5i: attest final guard model and 15 malformed-reader refusals before full gate rerun
d60cdc9 sol5i: attest final-package stalled tiny and Gate A identity after guard fixes
```
