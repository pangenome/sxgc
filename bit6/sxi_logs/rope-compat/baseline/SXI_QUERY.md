# SXI query product

```sh
xsa query --sxi refs.sxi --pattern ACGT --mode auto
xsa query --sxi refs.sxi --reads reads.fa.gz --ms -j 4
xsa mems --sxi refs.sxi --reads reads.fq.gz --min-len 20 -j 4
xsa serve --sxi refs.sxi -j 4 --bind 127.0.0.1:7331
```

SXI queries emit deterministic JSONL. Exact hits contain `read`, `doc_id`,
`name`, `offset`, `len`; MEMs also contain `qstart`. DNA records include
`strand` (`+` or `-`) on every hit/vector; generic text omits strand. All
coordinates are zero-based. Offset is the leftmost coordinate in the
original named reference. MEM qstart is the leftmost coordinate in the
original read, including reverse-strand MEMs. Palindromes report both
strands. No-hit exact queries emit no records.

`query --ms` emits `{read, ms, strand?}` for each oriented read, with MS[i]
the longest substring starting at i in that orientation. The minus vector
is indexed in the reverse-complement read. Exact matches and MEMs treat
IUPAC letters literally (not as wildcard sets), preserve case, and accept
DNA alphabet `ACGTRYSWKMBDHVN` in either case. DNA mode rejects other bytes.
The reserved separator 0x1E is always rejected in queries.

Auto mode uses the source mode recorded by new builds: FASTA/FASTQ/AGC are
DNA, text is generic. `--mode dna|text` overrides this. Storage orientation
is independently recorded (AGC is reversed); `--plain` and `--revlines`
override orientation for imported legacy files. Old headers lack source
provenance, so default to text/forward; use `--mode dna --revlines` for old
AGC files. Without embedded names the synthetic name is `text` with absolute
stream offsets. Embedded names are validated and converted from mirrored
starts into sorted separator boundaries; lookup uses O(log k) predecessor.
`--ri4 FILE.sxi` also detects SXI and uses this annotated path. Raw RI4 query
commands retain their historical output/orientation conventions.

`--sample N --seed S` selects up to N rows per strand in O(N) extra memory.
`--trace-samples` adds the BWT row to each annotated exact hit, including
sampled hits. `--output` / `--ms-out` write JSONL; legacy raw RI4 MS retains
its earlier binary format.

FASTA supports wrapped sequences. FASTQ uses four-line records and checks
quality lengths, printable quality, and repeated + identifiers. Gzip is
detected by magic (including concatenated gzip members). CRLF is normalized.
Each read/physical line is capped at 65,536 bytes. Batches contain at most
256 reads and about 1 MiB of input (plus one final read). Rayon workers share
one index: run symbols and packed samples are immutable mmap views; rank
structures are built once. Each read's output is spooled to an anonymous
temporary file and drained in input order, bounding RAM even with many hits.
Disk usage remains proportional to the output of one batch. Set TMPDIR for
spool placement. Published containers must remain immutable while loaded.

MEMs are maximal per query/reference occurrence, not only globally longest
matches. Matching statistics bound candidate lengths; nested suffix intervals
exclude right-extendible rows, and the BWT preceding symbol excludes
left-extendible rows. Shorter maximal matches at other occurrences are kept.
Both strand and record boundaries are respected. This v1 favors exactness:
repeated searches can take cubic time in read length, plus emitted/visited
occurrences and LF locate walks. Long reads/highly repetitive references can
be slow. Periodic texts can have unsampled LF cycles; equivalent rotation
classes are resolved from an endpoint lane and their coordinates enumerated.

## HTTP contract

The HTTP layer is `tiny_http` 0.12: blocking HTTP/1.1 keeps the dependency and
execution model small, without an async runtime. Rayon provides a fixed
worker pool; `memmap2`, `flate2`, `serde_json`, and `tempfile` cover shared
mapping, reads, JSON, and bounded output spooling. Versions are locked in
`xsa/Cargo.lock`. The default bind is loopback; there is no authentication or
TLS, and this minimal service is intended for a trusted local deployment.

* `POST /query`: `{"pattern":"ACGT","name":"query"}` (or `read` instead
  of `pattern`) returns the same JSONL as `query --pattern ACGT`.
* `POST /ms`: the same request returns MS JSONL, identical to CLI `--ms`.
* `POST /batch`: `{"reads":[{"name":"r1","read":"ACGT"}],"min_len":3}`
  returns MEM JSONL in request order. The worker owns this batch; simultaneous
  requests use the other workers.
* `GET /stats`: one JSON object with `n`, record `k`, `runs`, `chi`, `mode`,
  and `reversed`.

Bodies are limited to 2 MiB, batches to 256 reads. Invalid queries return
HTTP 400 with a JSON error and leave the server alive. Successful responses
have `application/x-ndjson`; large result sets spool to disk before response.
The server loads and validates the container once and shares it across all
workers. The CLI and HTTP paths use the same serialization and algorithms.
