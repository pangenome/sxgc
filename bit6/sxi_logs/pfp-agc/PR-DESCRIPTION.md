Native AGC input for PFP++

A genome archive can now be parsed directly with `-t collection.agc --agc`.
The reader supplies uppercase, per-contig reversed bytes with 0x1e separators
and terminator, preserving archive sample/contig order. It writes optional
names/coordinate metadata without an extracted text file.

The new TextSource boundary preserves the existing file/gzip/FIFO path.
This PFP++ revision's text parser is serial: it does not implement the
parallel text chunk algorithm assumed in the task. AGC mode parallelizes
independent seekable archive fetches under `-j` and feeds chunks to the same
rolling-hash parser, retaining state across chunk seams. It does not claim
parallel phrase construction.

AGC is opt-in with PFP_ENABLE_AGC and AGC_ROOT. The dependency is the
MIT-licensed refresh-bio/agc repository (the requested lh3/agc URL is not
available), pinned/tested at e67e3fc865a459779118d3d4e9fbdf42c70ba75e.
The adapter supports AGC format 3 using the library's internal C++
collection/segment interface. It needs an upstream review of that coupling.

Why compressed length indexing is necessary: the public API sorts samples,
and yeast235 has a segment advertised as 8,429 bytes that actually decodes
to 8,431. A metadata-only sum misaligns subsequent seeks. The reader counts
compressed delta instructions, expanding neither matches nor N runs, and
keeps archive-descriptor metadata. No additional expanded-text pass occurs.
Independent per-thread handles and ZSTD contexts avoid shared decoder state;
delivery buffering is j MiB and decoded data is not persistently cached.

Validation (complete local checked evidence; external review pending):

- All 3,336,986,759 yeast235 canonical bytes match the accepted file control.
  The native build publishes a complete SXI; all five core members compare
  byte for byte with the accepted control, and 32 archive-backed witnesses pass.
  See `ACCEPTANCE.md`, `yeast-reader.json`, `yeast-published-members.json`, and
  `completion-stages.json` in the accompanying acceptance bundle.
- Twenty distributed 100 MB HPRC-466 windows (2 GB total) match an independent
  agc2flat oracle byte for byte (`hprc-windows.json`). No full HPRC build is claimed.
- `THROUGHPUT.md` and `throughput.json` record j=16/48/96 reader results.
  Steady AGC/file ratios are 59.9–86.9%. HPRC's measured optimum is j=16,
  509.109 MB/s; its 68–82 s initialization dominates the 2 GB test window but
  amortizes over the full 1.403 TB canonical corpus. The full-reader projection
  is 47.08 minutes including initialization. First-parse ETA is 6.63 hours,
  extrapolated from measured yeast parsing; this is not measured HPRC parsing.
- Native 32/64-bit regressions at j=1/16/48/96 cover parse/dictionary/names,
  random seeks, chunk seams, separators, EOF and invalid ranges. Existing
  Python/Cargo/LCE regressions pass; the AGC-disabled/file path also passed.
- No expanded collection text is created in the native product path. The
  `.agg`, `.ri4`, samples and `.sxi` are intended r-space artifacts and may exceed
  1 GB. Protected PFP sources/binary remain untouched; no commits or staged files.

`pfp-agc.patch` is the complete upstream diff against
`1a5f114ae026c18e7c0049ceace1a5eabc8be44a`; `series` lists it and
`patch-verification.json` verifies clean application and exact tested-file equality.
The separate SXI pipeline diff selects native AGC parsing, retains XSA_PFP and
explicit --fifo fallback, and labels --materialize forensic-only. It is not part
of this upstream PR. Reviewer attention: pinned internal AGC API coupling and
format-3 compatibility; no additional archive versions are claimed.
