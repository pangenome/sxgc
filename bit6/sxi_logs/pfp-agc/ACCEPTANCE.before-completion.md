# Native PFP++ AGC lane — checked evidence, NOT COMPLETE

Implementation is in `/tmp/pfp-agc-fork`; the protected `/home/erikg/pfp`
checkout and binary were not modified. No commits or staged files.
Apply `pfp-agc.patch` to PFP commit `1a5f114`; `pipeline.patch` is the isolated
SXI change relative to this lane's pre-existing dirty pipeline.
`pipeline-from-main.patch` alternatively applies to the current home SXI
checkout, preserving its existing multi-string validation policy; these
two pipeline patches are alternatives, not cumulative.
`tools/build_pfp_agc.sh` builds the isolated fork. Upstream description:
`PR-DESCRIPTION.md`; commands: `COMMANDS.md`.

## Gates

- **(a) PASS.** Every one of 3,336,986,759 canonical yeast235 reader bytes
  compared equal to the accepted file control. Native parse, dictionary,
  names, RLE BWT and both SA sample files also compare byte for byte.
  Evidence: `yeast-reader.json`, `yeast-intermediates.json`, raw logs.
- **(b) PASS.** 20 windows of 100,000,000 bytes each, spread across the
  466-sample / 1,403,221,068,481-byte HPRC archive. Every byte compared with
  independent agc2flat `--band` extraction, reversed to canonical coordinates.
  This oracle's `--band` applies to forward contig coordinates, so the harness
  selects one sample/contig and reverses precisely that band; it does not
  silently rely on `--revlines --band` (which the oracle ignores).
  Evidence: `hprc-windows.json`, `check_hprc.py`, 20 oracle logs.
  Only a 100 MB window is written at a time in the TEST oracle scratch and
  removed; the product source never writes expanded text. No HPRC full build.
- **(c) Steady reader PASS; startup-inclusive HPRC FAIL.** All six measured
  steady reader rates exceed 50% of same-j file control. Full table below.
  Both paths run identical FNV checksums; archive initialization is reported
  separately. HPRC's 68–82 second compressed index startup means the sampled
  2 GB workloads fail 50% if startup is included. This limitation is not waived.
- **(d) NOT COMPLETE.** Native full yeast build reached BWT construction and
  all six intermediate comparisons passed. Pipeline Python PID **3651283**
  is stopped before downstream index generation/publication. The literal
  user requirement “zero writes > 1 GB in the product path” conflicts with
  the unchanged accepted pipeline's 3.1 GB `fresh.agg` and multi-GB SXI.
  An explicit decision about allowing these required index artifacts is
  pending. Do not resume the process without that decision. No full native
  SXI has been published or claimed equivalent.
- **(e) PARTIAL.** All three committed Python regressions and both Cargo test
  suites pass (`regressions.jsonl`). Native 32/64-bit seek/parse regressions
  pass at 1/16/48/96 threads, including duplicate names, chunk seams,
  separators and invalid ranges. Default/native small pipeline battery
  publishes all admitted fixtures and compares all six members, with
  corruption blocking publication. AGC-disabled build/file equivalence pass.
  Product files through BWT are at most 807,240,360 bytes; no collection.txt,
  FIFO or FUSE mount exists in native scratch. Final-reader syscall trace
  confirms every output path stays below 1 GB and no shared file mapping
  or truncation can hide a write (`reader-writes.json`, compressed trace). Full-publication hygiene remains untested.

## Throughput

| Dataset | j | AGC MB/s | File MB/s | Ratio | AGC init s | AGC MB/s incl. init |
|---|---:|---:|---:|---:|---:|---:|
| yeast235 | 16 | 470.671 | 647.037 | 72.7% | 0.905 | 417.377 |
| yeast235 | 48 | 509.017 | 625.647 | 81.4% | 0.976 | 443.032 |
| yeast235 | 96 | 534.771 | 615.706 | 86.9% | 1.059 | 457.160 |
| HPRC-466-20-windows | 16 | 509.109 | 692.746 | 73.5% | 68.439 | 27.637 |
| HPRC-466-20-windows | 48 | 464.330 | 666.796 | 69.6% | 76.616 | 24.715 |
| HPRC-466-20-windows | 96 | 437.640 | 730.069 | 59.9% | 82.213 | 23.046 |

Decimal MB/s, same FNV checksum in both source paths. Read time excludes archive indexing. All six steady read rates pass 50%; HPRC sampled-window end-to-end rates including indexing do not. File preparation time is not counted. Archive index startup is compressed-instruction work, not a text scan.

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

The supplied contact_supervisor tool was not available in this session.
The write-limit decision was requested through the available user-input tool.
Independent reviewer approval is still required; none is claimed.
