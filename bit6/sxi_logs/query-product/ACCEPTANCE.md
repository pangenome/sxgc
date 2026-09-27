# Query product — checked; independent review pending

Implemented in `/tmp/sxgc-laneV`, without commits or staged files. The inherited
seam-repair changes are preserved; `baseline/` and `lane.diff` isolate this lane.
No protected k10 path, published artifact, or pre-existing running process was
modified or terminated.

## Implemented behavior

- SXI exact queries, sampled hits, traces and MEMs emit record names and offsets.
  Validated names produce an O(k) boundary array with O(log k) predecessor lookup.
  Cyclic coordinates wrap correctly; periodic unsampled LF lanes are resolved.
- `mems --sxi --reads -j` uses matching statistics, preserves all per-occurrence
  maximal matches (including shorter matches), and supports FASTA/FASTQ/gzip.
  Bounded input batches and per-read temporary output files avoid hit-sized RAM.
- Source mode/orientation metadata supports auto/dna/text, reverse complements,
  strand reporting, literal IUPAC validation, and original-record coordinates.
- `serve` exposes `/query`, `/ms`, `/batch`, `/stats` through tiny_http, with a
  shared immutable mmap-backed index and fixed Rayon worker pool.
- SXI k counts named records. The writer emits k=9901 for yeast235; legacy
  readers derive record count from names without altering files. The native
  loader preserves the separate core byte-string count.
- `--verify-text-sample N` verifies sampled sweep witnesses directly against
  scratch text before publication. Five bounded streams and two 64 KiB buffers
  avoid mapping/expanding large text or aggregate files. Failure blocks publish.

## Checked evidence

- **PASS:** 23 small/battery fixtures, 189 read instances, **44,481 MEMs** compared
  against an independent all-occurrence brute oracle. Includes all seven battery
  fixtures, seven identical periodic records, random collections, both strands,
  both reference orientations, and shorter maximal matches.
- **PASS:** exact locate and MS vectors checked against direct text on both
  orientations; deterministic output for one/three workers; gzip FASTQ; malformed
  inputs and non-IUPAC rejection; sampled traces; corrupt k/names/flags rejected.
- **PASS:** yeast235 whole-text oracle on two sampled 45-base reads, one mutated:
  **1,720 MEMs >=20**, both strands, over all **3,336,986,759 bytes**.
- **PASS:** random-port HTTP `/query`, `/ms`, `/batch` responses match CLI bytes;
  `/stats` reports k=9901. Final binary replay passes under a **20 GB address-space
  limit**, with server peak **2,081,572 KiB**.
- **PASS:** writer-only yeast235 rebuild from certified retained intermediates:
  k=9901, chi=85,404,336, **all six member hashes unchanged**. No new full AGC/PFP
  build is claimed. Separate yeast regression: chi=85,404,240 and all five core
  member hashes equal the prior certified publication.
- **PASS:** yeast235 sampled-text gate: 32/32 witnesses, 115,908 compared bytes.
  Fresh three-record FASTA build: 32/32 witnesses, chi=1657. Corrupted text fails;
  an injected failing audit prevents final publication.
- **PASS:** native SXI loader with record k reproduces byte-identical aggregates
  versus the fresh RI4 core path. Fresh FASTA auto=dna and explicit text override
  persist and change query validation as specified.
- **PASS:** committed format and CLI regressions; release/native builds;
  `git diff --check`; no staged files. Largest measured gate process RSS is
  **3,260,416 KiB** (independent whole-text test oracle). See `memory.json`; this
  is process-level measurement, not a claimed simultaneous process-tree peak.

## Review and limits

See `REVIEW.md`: independent reviewer approval remains pending. No
`contact_supervisor` tool was exposed, so no supervisor decision or reviewer
approval is claimed. Self-review found no unresolved implementation blockers.

This v1 can be slow on long/repetitive reads (repeated interval searches and
LF walks). Output spooling uses disk proportional to a batch's output. Legacy
containers lack mode/orientation provenance and need explicit overrides for
DNA/reversed storage. New metadata flags require upgraded readers. The HTTP
service is intended for trusted local use. Sampled text checks ground selected
witnesses; they do not establish complete chi-set equality. Raw RI4 query output
retains its historical conventions; annotated JSONL is the SXI product path.

## Evidence index

`COMMANDS.md` contains reproduction commands. `changed-files.json`,
`source-hashes.json`, `lane.diff`, and `unchanged-components.json` provide scope
and hygiene evidence. `query-tests-final.log`, `yeast-gates.log`,
`yeast-mems.jsonl`, `yeast-members.json`, `yeast-regression-gate.log`,
`http-final.log`, `yeast235-audit-final.log`, `audit-tests.log`,
`metadata-tests.log`, `native-sxi-loader.log`, `format.log`, `cli.log`,
`mode-build-assertions.log`, per-stage JSONL journals and `.time` files retain
the validation results. `acceptance-report.json` is the structured handoff.
