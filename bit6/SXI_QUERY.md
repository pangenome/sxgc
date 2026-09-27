# SXI query product

```sh
xsa query --sxi refs.sxi --pattern ACGT --mode auto
xsa query --sxi refs.sxi --reads reads.fa.gz --ms -j 4
xsa mems --sxi refs.sxi --reads reads.fq.gz --min-len 20 -j 4
xsa serve --sxi refs.sxi -j 4 --bind 127.0.0.1:7331
```

SXI queries and the default `mems --out native` emit deterministic JSONL. Exact hits contain `read`, `doc_id`,
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

## ropebwt3 MEM tables

```sh
xsa mems --sxi refs.sxi --reads reads.fa --out ropebwt3 --min-len 20
xsa mems --sxi refs.sxi --reads reads.fa --out ropebwt3 -p 10
xsa mems --sxi refs.sxi --reads reads.fa --out ropebwt3 --mem
xsa mems --sxi refs.sxi --reads reads.fa --out ropebwt3 --gap 100 --gap-seq
xsa mems --sxi refs.sxi --reads reads.fa --out ropebwt3 --cov
```

`--out native` is the default and preserves the existing per-occurrence
all-MEM JSONL, ordering, names, and HTTP contract. `--out ropebwt3` selects
TSV and defaults to **SMEMs**: a MEM is super-maximal iff no other MEM's
query interval strictly contains it. Containment is checked across both
strands and all references; equal query intervals coalesce into one row.
Intervals are ordered by query start then end, within input read order.
`--min-len` applies before reporting (default 20).

Each table row is `qname TAB qstart TAB qend TAB hit_count`, with zero-based
half-open query coordinates. Counts include every occurrence on both
strands in DNA mode, including both strands of palindromes. No-hit reads
have no rows. Identifiers use the first whitespace-delimited word, as in
ropebwt3's FASTA parser. Generic text mode searches only the forward strand.

No reference positions are output by default (`-p 0`). `-p N` or
`--positions N` appends at most N tab-separated `name:strand:pos` tokens
**per interval**, not per strand. The hit count always remains complete.
The deterministic sample takes forward-query matches first, then reverse
matches, in each interval's BWT row order. It is a prefix, not a uniform
random sample; `query --sample` is a separate API. Every position is the
leftmost forward-reference coordinate. A reverse-reference position would
be normalized as `rlen - (match_start + matchlen)`; xsa instead searches a
reverse-complement query against the original reference, so its annotated
offset is already that forward coordinate. Reversed reference storage is
normalized independently. SMEM count-only output needs no locate walks;
position lookup work is bounded by the requested cap per interval.

`--mem` in ropebwt3 mode disables containment filtering and aggregates the
existing per-occurrence MEMs by query interval. Here the count and sampled
positions include the **maximal occurrences** of that interval; extendible
occurrences of the same substring are not MEMs. The upstream version tested
has no corresponding all-MEM switch. `--mem` with native output explicitly
selects its existing behavior. All-MEM enumeration retains the v1 runtime
limitations above and aggregates up to O(read_length²) distinct intervals;
SMEM candidates use O(read_length) entries. Position storage is additionally
bounded by the requested per-interval cap and available occurrences.

`--gap N` emits uncovered query intervals of length at least N as
`qname TAB start TAB end TAB qlen`. `--gap-seq` appends the original query
substring. Coverage is the union of the reported long MEM intervals;
`--cov` emits `qname TAB qlen TAB covered_bases`. Like upstream, zero-coverage
reads have no coverage row, `--gap` takes precedence over `--cov`, and gap/
coverage modes suppress positions. These switches require ropebwt3 output.
`--gap-seq` requires a positive `--gap`; invalid output names and negative
position caps fail. `--output FILE` also works for TSV.

Intentional upstream differences:

* The requested sampled table appends position tokens directly after
  `hit_count`. Upstream `search.c:write_per_seq` additionally inserts a
  sampled-position-count column. Our parity test removes that column before
  comparing complete position multisets. Capped subsets and their order can
  differ because index row orders and sampling algorithms differ.
* This is an output adapter over the existing literal query core. It retains
  case and literal IUPAC/text matching. Upstream folds case and treats query
  ambiguity as a hard break; therefore equality of match sets is guaranteed
  by the tested A/C/G/T fixtures (also N gaps against A/C/G/T references),
  not arbitrary ambiguous or mixed-case references. Gap sequences retain
  original bytes instead of upstream's normalized A/C/G/T/N representation.
* Native remains the default output for compatibility. SMEM is the default
  within the new ropebwt3 output mode.

Reproducible real-tool parity, independent brute-force checks, native
regressions, and the retained yeast235 read-only gate are documented in
`sxi_logs/rope-compat/ACCEPTANCE.md`. No index construction or format changes
are needed.

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
