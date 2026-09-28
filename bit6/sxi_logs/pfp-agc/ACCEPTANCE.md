# Native PFP++ AGC lane — checked completion; external review pending

Implementation is in `/tmp/pfp-agc-fork`; the protected `/home/erikg/pfp`
checkout and binary remain identical to the saved baseline. No commits or staged
files. This completion changes evidence/PR materials only, with no implementation
scope expansion. See `PATCHES.md`, `patch-verification.json`, `PR-DESCRIPTION.md`,
and `COMMANDS.md` for the exact upstream and separate downstream diffs.

## Acceptance gates

- **(a) PASS — native canonical reader.** Every one of 3,336,986,759 yeast235
  bytes compares equal to the existing accepted file control. Parse, dictionary,
  names, RLE BWT and both SA sample files also compare byte for byte.
  Evidence: `yeast-reader.json`, `yeast-intermediates.json`.
- **(b) PASS — HPRC windows.** Twenty 100,000,000-byte windows spread over the
  466-sample / 1,403,221,068,481-byte canonical collection compare byte for byte
  against independent agc2flat forward-band extraction, reversed into canonical
  coordinates. Prior bounded TEST oracle files were removed after each window;
  no new expanded-text files were created for this completion. Evidence:
  `hprc-windows.json`, `check_hprc.py`, and the twenty oracle logs.
- **(c) PASS — steady throughput.** All six dataset/thread rows exceed 50% of
  the same-j file control. Per supervisor clarification, the HPRC short-window
  startup-inclusive miss is an amortization artifact, not a failing gate.
  The measured reader optimum is j=16. Full HPRC-466 reader projection is
  47.08 minutes including initialization; first parse ETA is 6.63 hours
  (6.65 hours conservatively adding HPRC initialization). These are explicitly
  projections, not a full HPRC build. See the complete table below.
- **(d) PASS — full native publication.** `/tmp/pfp-agc-gates/yeast235-native.sxi`
  was built directly from `yeast235.agc`, audited against archive ranges, validated,
  and atomically published. All five core members were compared byte by byte
  in bounded 1 MiB reads against `/tmp/sxi-stream/yeast/yeast235-control.sxi`.
  Evidence: `yeast-published-members.json`, `compare_published.py`,
  `yeast-completion.log`, the complete stage journal and `completion-stages.json`.
- **(e) PASS — regressions and write hygiene.** All three committed Python tests,
  both Cargo test commands, and all 216,000 committed LCE checks pass. Native
  32/64-bit seek/parse tests pass at j=1/16/48/96. The prior small default/native
  publication and corruption battery remains green. No collection.txt or FIFO
  exists in the full native scratch. Required r-space files, including the
  3,228,961,452-byte `.agg` and published SXI, are allowed by the clarified policy.
  No corpus-sized extracted text was written. Evidence: `regressions.jsonl`,
  `completion-native-32.log`, `completion-native-64.log`, `completion-lce.log`,
  `native-pipeline-battery.log`, `publication-hygiene.json`, `completion-hygiene.json`,
  and the earlier final-reader syscall trace `reader-writes.json`.

The previously stopped PID 3651283 no longer existed, so the full command was
restarted from the archive. The completed scratch is `/tmp/pfp-agc-gates/xsa-build-5najb_ty`.
The earlier literal artifact-size blocker is resolved by the explicit supervisor
clarification: the write prohibition applies to extracted collection text; all
r-space construction and product artifacts are unrestricted.

## Published core member comparison

| Member | Bytes | Count | Byte equal | SHA-256 |
|---|---:|---:|---|---|
| 1 | 504,527,273 | 100,905,045 | yes | `663a45ab36f5b90e95e77bc1d652f9066777183c559e6620fe2a74ad1bec9b07` |
| 2 | 403,620,193 | 100,905,045 | yes | `851d352d51fb073900f1f56c6d54da0783606643e377db774c6e05d91fa8f51e` |
| 3 | 807,240,360 | 100,905,045 | yes | `d73ff6b02f2c73051264f13c8ec02f4839b52e632cb54f0df840f38b2aeb203e` |
| 4 | 12 | 0 | yes | `0da64314dec135e73e86cba36a52479f4664f4b2b93ae87c9ff1834e9456298e` |
| 5 | 88,747,090 | 85,404,336 | yes | `dd454f133287ff25bd5399c93d70d8310699e23c1c897485a151db8f28a75945` |

Published SXI size: 1,804,487,132 bytes. Audit output: `yeast235-native-xsa-build-5najb_ty.verify-text-sample.log`.

## Throughput and 466 parse-ETA

| Dataset | j | AGC MB/s | File MB/s | Ratio | AGC init s | AGC MB/s incl. init |
|---|---:|---:|---:|---:|---:|---:|
| yeast235 | 16 | 470.671 | 647.037 | 72.7% | 0.905 | 417.377 |
| yeast235 | 48 | 509.017 | 625.647 | 81.4% | 0.976 | 443.032 |
| yeast235 | 96 | 534.771 | 615.706 | 86.9% | 1.059 | 457.160 |
| HPRC-466-20-windows | 16 | 509.109 | 692.746 | 73.5% | 68.439 | 27.637 |
| HPRC-466-20-windows | 48 | 464.330 | 666.796 | 69.6% | 76.616 | 24.715 |
| HPRC-466-20-windows | 96 | 437.640 | 730.069 | 59.9% | 82.213 | 23.046 |

Decimal MB/s, identical FNV checksums in both paths. The acceptance gate is steady reader throughput: every row exceeds 50% of its same-j file control. The 68–82 s HPRC compressed-index startup dominates a 2 GB benchmark window; its startup-inclusive miss is a benchmark-window artifact, per the supervisor clarification. Startup is paid once and amortizes across the full corpus; it is not a decoded-text scan. File preparation is excluded.

HPRC-466 canonical size is 1,403,221,068,481 bytes (archive layout, including separators). The measured optimum among j=16/48/96 is **j=16 at 509.109 MB/s**. Projected reader time is **45.94 min**, or **47.08 min including 68.439 s startup**, yielding **496.774 MB/s** over the full corpus.

**466 parse-ETA: 6.63 hours at j=16** by linear scaling of the measured 56.80 s full yeast235 first parse (3,336,986,759 bytes); conservatively adding full HPRC startup gives **6.65 hours**. This is a planning estimate: the serial phrase parser, dictionary growth, and different repetitiveness can change it. The 47-minute reader projection is not a parse or full-index build claim. Only the reader thread optimum was swept; full HPRC parsing was not run. Formulae and inputs: `hprc466-eta.json`; raw measurements: `throughput.json`.

## Memory and architecture

Native yeast parse: 56.80 s, peak RSS 1,249,160 KiB. HPRC reader peak RSS:
1,971,204 KiB at j16; 3,319,760 KiB at j48; 5,260,776 KiB at j96.
BWT stage peak RSS: 8,828,572 KiB. These are measured per-process peaks,
not an inferred full-build process-tree bound. Timing files retain details.
No persistent decoded-text cache exists: j MiB delivery buffering plus
one decoder/active segment-pack working set per worker and shared layout.
Decoded segment/reference/pack lengths are capped at 256 MiB; metadata
scales with archive descriptors, not expanded text length.

The actual local/upstream PFP text parser is serial and has no w-1 chunk
partitioning algorithm. The implementation keeps its rolling state intact
and parallelizes seekable archive fetches. It does not claim parallel
phrase construction. File/gzip/FIFO parsing remains the original one-byte
gzread behavior, with a source wrapper and checked read errors.

The requested lh3/agc repository was unavailable; the cloned MIT upstream
is refresh-bio/agc at e67e3fc865a459779118d3d4e9fbdf42c70ba75e.
Its public API sorts samples and reported yeast two bytes short. Counting
compressed delta instructions resolves this exactly without a decoded-text
prescan. The adapter uses internal collection/segment C++ interfaces and
supports AGC format 3 only. This coupling requires upstream review.


Completion stage peak RSS: 8,830,328 KiB. The process-tree monitor began during BWT and observed 8,852,624 KiB; first-parse RSS is recorded separately. These figures do not claim a whole-run sampled tree bound.

## Review and residual limits

No local acceptance blockers remain. External reviewer approval is still required
and is not claimed here. The upstream patch clean-applies to its recorded base
and exactly reproduces all eleven files in the tested fork. The upstream PR draft
is prepared locally; nothing was committed or remotely published.

AGC support is coupled to pinned format-3 internal APIs. Archive auditing samples
32 witnesses rather than proving the complete chi set. Full HPRC parsing/index
construction was not run; HPRC coverage is the 20 byte proofs plus reader timings,
and the parse ETA remains a yeast-derived extrapolation. The contact_supervisor
tool was unavailable in this session; no coordination decision was needed beyond
the supplied supervisor clarification.
