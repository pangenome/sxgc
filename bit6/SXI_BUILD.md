# Fresh-source SXI build

**Current construction evidence:** the repaired separator path is checked in
[sxi_logs/sep-convention/repair2/ACCEPTANCE.md](sxi_logs/sep-convention/repair2/ACCEPTANCE.md).
Query-product gates and residual limits are recorded separately in
[sxi_logs/query-product/ACCEPTANCE.md](sxi_logs/query-product/ACCEPTANCE.md).

Build all tools from a fresh checkout (Linux, GCC/G++ with OpenMP, CMake,
Make, Git, Python 3, Rust/Cargo, zlib/liblzma/libbz2/libcurl development
headers, and GNU time):

```sh
BUILD_JOBS=4 bash tools/build_all.sh /absolute/new/sxi-tools
/absolute/new/sxi-tools/xsa build --text collection.txt -o collection.sxi \
  --threads 8 --scratch /path/on/source/filesystem --verbose
/absolute/new/sxi-tools/xsa build --agc collection.agc -o collection.sxi --verbose
/absolute/new/sxi-tools/xsa build --fasta oriented.fa -o collection.sxi --verbose \
  --expect-heads accepted.head_sa --expect-ri4 accepted.ri4
/absolute/new/sxi-tools/xsa build --fastq oriented.fq -o collection.sxi --verbose \
  --expect-heads accepted.head_sa --expect-ri4 accepted.ri4
```

The prefix must be new, with an existing parent directory. Every download,
CMake build, C++ executable, and Cargo target goes inside that prefix. No
machine-local upstream checkout or object file is required. A second invocation
verifies the completed prefix and exits; it refuses drift or incomplete prefixes.
After a failed build, retry with a new prefix. No process is stopped by this script.

The two stage upstream pins are PFP++
`1a5f114ae026c18e7c0049ceace1a5eabc8be44a` and r-pfbwt
`1fea5c30ac32cef9574392388005184b23d98d22`. The native AGC library is pinned
to `e67e3fc865a459779118d3d4e9fbdf42c70ba75e`; transitive CMake dependencies
and TeraTools headers plus HTSlib are fixed in `tools/upstream.lock.json`. Cargo uses
`--locked`. PFP++ uses `tools/build_pfp_agc.sh` with
`patches/pfp_agc.patch`; r-pfbwt uses `patches/rpfbwt_emit_tails.patch`.
The patch changes only the merge's endpoint-selection/range filter and adds
`.ssa_t` output (`u64 count`, then `count` native-endian u64 values, little-endian
on this host). It leaves `.ssa` writes unchanged. The tail merge skips empty
temporary streams to avoid setting the output failbit. Upstream's unchanged
head merge has that empty-stream bug; builds smaller than 1 MB use one chunk,
while large builds use 50. Both choices are printed with `--verbose`.

`XSA_PFP`, `XSA_RPFBWT`, `XSA_TOOLS`, `XSA_AGC2FLAT`, and `XSA_PIPELINE`
override tool paths. Tools default to the directory containing the invoked xsa.
The Rust launcher otherwise locates the Python script
relative to the compile-time repository path; shipping a standalone binary
requires installing the script/tools and setting these paths.

Before creating scratch or launching a stage, the pipeline verifies **all eight
stage executables**, resolved overrides included, against the prefix's
`MANIFEST.sha256`. Missing tools/entries, malformed manifests, duplicate entries,
changed pins/patches/sources, or changed binary bytes fail closed. A mismatch
reports the manifest path, line number, exact expected line, actual digest, and
resolved path. `--manifest PATH` explicitly selects another manifest.

`tools/MANIFEST.sha256` records the verified reference build. Binary hashes
depend on the compiler/platform and embedded build paths; `build_all.sh` seals
the complete locally built set in its own prefix manifest and verifies it at the
end. It never regenerates the manifest of an existing prefix. Review source and
pin changes before creating a new distribution. The manifest protects against
accidental desynchronization, not an attacker replacing both tools and manifest.
Inline role comments and logical `bin/`, `repo/`, and `pin/` paths are interpreted
by `python3 tools/tool_manifest.py verify --prefix PREFIX --manifest MANIFEST`.

`--allow-drift` is **debug-only**: it prints a warning and each mismatch, and
records the bypass and actual hashes in the journal. Do not use it for accepted
or production builds. A manifest must still be readable. Each journal begins
with `tool-provenance`: the full manifest, its SHA256, resolved per-tool SHA256,
source/patch/pin hashes, and verification status. The final PASS row binds that
manifest digest to the published SXI path and SHA256. Keep the journal with its
SXI for provenance. Keep the checkout and installed prefix immutable during use.

Exact stages:

1. Prepare the input. AGC defaults to `agc2flat --stdout --revlines --upper --sep 1e
   -o SCRATCH/collection.txt`. Its stdout feeds `collection.fifo`, which PFP reads
   directly. Only `collection.txt.names.tsv` is created: **no collection text file**.
   The sidecar is computed from actual decompressed lengths during that same pass.
   `xsa build --agc ARCHIVE --materialize` retains the legacy text-file path.
   FASTA/FASTQ: stream extracted sequences into the same `collection.txt` path,
   joining wrapped FASTA lines and separating and terminating every record with 0x1E.
   Both then enter exactly the same stages as `--text`.
2. `pfp++ -t TEXT -o PREFIX -w 10 -p 100 -j THREADS --tmp-dir SCRATCH`.
3. `pfp++ -i PREFIX.parse -w 5 -p 11 -j THREADS --tmp-dir SCRATCH` (second level).
4. Patched `rpfbwt --l1-prefix PREFIX --w1 10 --w2 5 --threads THREADS
   --chunks CHUNKS --tmp-dir SCRATCH`.
5. `rpfbwt_endpoints PREFIX fresh.ri4 fresh.head_sa TERMINAL_HEX`: bounded-buffer O(r)
   normalization, remove ten padding rows, remap byte 2 to the input terminal byte (0x1E under the contract), coalesce
   adjacent normalized runs, convert tails to `n-1-SA`, pack the existing v4
   transport. The `.ssa` and `.ssa_t` source files are retained untouched.
6. Committed `slim_dump --slim --resolve-ri4 --dict-stream --ri4 fresh.ri4
   --head-sa fresh.head_sa --parse PREFIX -t THREADS -o fresh.agg`.
   Heads/tails resolve directly; no new Phi, M/b_bwt/w_wt artifacts or LF walks.
   rpfbwt retains its own existing internal second-level structures.
7. `xsa chi-rspace --stream-agg --ri4 fresh.ri4 --agg fresh.agg -o fresh.sA`.
   Gate logged count against file length; enforce `--expect-chi` if supplied.
8. Committed `sxi_write` consumes these fresh endpoint inputs plus chi. It
   checks sample ranges/singletons, run counts, chi range/uniqueness and CRCs.
   `xsa sxi-info` validates the candidate before atomic no-clobber publication.

Each subprocess has its own `/usr/bin/time -v` file and stage log under
`bit6/sxi_logs` (override `--log-dir`). The JSONL journal records argv, return
code, wall seconds, and stage peak RSS. Stages run sequentially except for the AGC streamer and first PFP parse, which run together.
The pipeline respects a lower inherited hard address-space ceiling; otherwise it uses 149 GB. Scratch is intentionally retained for independent
review. Failed stages or gates never publish the requested output path.
FASTA/FASTQ preparation runs in the Python process, recording elapsed time and
process peak RSS in the journal; each completed record logs its identifier,
sequence length and cumulative collection size to stderr and the journal.

`--expect-heads RAW` and `--expect-ri4 FILE` are acceptance-only comparisons,
run **after** constructing fresh endpoints. The latter checks the entire
run table and packed tails. Neither oracle supplies construction inputs.

## Reserved collection separator contract

`xsa build` indexes **one byte string** `T = s1 0x1E s2 0x1E ... sk 0x1E`.
The reserved separator is **0x1E (ASCII record separator, decimal 30)**, both
between records and terminally. It must never appear in sequence content.
The intended suffix structure is cyclic over T, with one byte string in the `.ri4` core transport (`k=1`). SXI header `k`
counts named records instead (9,901 for yeast235). The writer derives k from
validated names; the native loader keeps record count separate from the
core byte-string count. This changes metadata, not BWT/endpoints/chi.

The rationale for this convention is theory-faithful single-string chi semantics: it uses a byte
native to rpfbwt, makes every record boundary uniform, and is pile-compatible:
the web corpus already uses 0x1E separators. Genomic sequence bytes never
contain 0x1E. FASTA/FASTQ and AGC input preparation reject a reserved separator
in sequence content with a `corpus contract` error before PFP/publication.
AGC decodes numeric nucleotide symbols; a literal 30 in that decoded numeric
stream is rejected before the legacy fallback-to-N conversion can hide it.
An archive creator may have discarded invalid source characters already;
preparation cannot recover information absent from the archive.

`--text` passes the supplied raw bytes **as-is**, with no O(n) contract scan,
separator insertion, newline conversion, reversal or uppercasing. The caller
owns the contract: using 0x1E as content is user error. A constant-time final-byte
read only configures endpoint padding normalization. Existing parser/alphabet
limitations still apply (the current backend supports bytes 6..127). Historical
newline-terminated raw pilots retain their legacy transport/gates; they are
outside the new corpus contract. In particular, old BCR newline collection
oracles do not establish correctness for the new T.

`agc2flat --sep <hexbyte>` accepts `1e` or `0x1E` and defaults to 0x1E for
flat, group, reverse and `--revlines` output. `--sep 0a` is an explicit legacy
newline escape hatch for external consumers; `xsa build --agc` always requests
`--sep 1e`. Names sidecar offsets still count one separator byte per record
and retain their previous orientation semantics.

`--fasta` accepts multiple records and wrapped sequence lines. `--fastq`
accepts four-line records, validates the `@`/`+` headers and equal sequence
and quality lengths, and discards quality scores after validation. Extraction
preserves sequence bytes, case, order and orientation, just like `--text`;
unlike AGC's explicit `--revlines --upper` conversion, it does not reverse or
uppercase sequences. Supply the desired pilot orientation. LF and CRLF are
accepted, including a final sequence/quality line without a line terminator.
Sequence lines accept ASCII letters, `*`, `.` and `-`; blanks, whitespace,
control bytes, empty records, missing identifiers, truncated FASTQ records and
invalid quality characters fail before PFP or final publication. FASTQ quality
bytes must be printable ASCII (`!` through `~`); an optional repeated `+`
header must match the identifier or full `@` header.

The names member contains one `identifier<TAB>start<TAB>length` row per record,
in input order. The identifier is the first whitespace-delimited header token;
descriptions are omitted and duplicate identifiers are retained. Headers must
be UTF-8 and at most 1 MiB. As in `agc2flat --revlines`, the coordinate is
`start = total_collection_bytes - 1 - record_stream_offset - sequence_length`.
Extraction and the names metadata spool use bounded 1 MiB buffers on disk;
neither a whole sequence nor the collection's names are retained in RAM.

Reproduction tests:

```sh
python3 bit6/test_endpoint_tap.py
# Separator acceptance (currently fails at the documented downstream blockers):
RAYON_NUM_THREADS=2 cargo run --release --manifest-path agc2flat/Cargo.toml \
  --example reserved_fixture -- /tmp/reserved.agc
python3 bit6/test_sxi_separator.py --work /tmp/new-separator-gates \
  --xsa xsa/target/release/xsa --agc2flat /path/to/new/agc2flat \
  --reserved-agc /tmp/reserved.agc --log-dir bit6/sxi_logs/sep-convention
python3 bit6/test_sxi_build.py
python3 bit6/test_source_battery.py --work /tmp/laneV/new-battery
python3 bit6/test_sxi_input_modes.py --work /tmp/new-input-modes \
  --xsa xsa/target/release/xsa
```

The tiny suffix-array oracle in `test_endpoint_tap.py` is test-only; no such
algorithm is used by the production pipeline. The legacy Phi extractor and
the existing SXI format remain unchanged.

Historical input-modes acceptance (2026-09-27, prior newline convention): FASTA and FASTQ each passed five three-record battery
fixtures (`random-4-2k`, `random-4-20k`, `random-bin-20k`, `random-4-200k`,
`satellite-18k`). Battery bytes 11..14 were mapped to A/C/G/T before wrapping.
All five core SXI members (runs/RLBWT, packed tails, heads, anchors, chi), the
raw RI4/chi files, extracted text and names passed exact byte comparisons.
The multi-record endpoint gates use the equivalent text's freshly constructed
endpoints; this establishes input-mode equivalence, not independent BCR order.
The original binary `random-4-2k --text` passed its existing endpoint/aggregate
oracles; `--agc` matched all six members of the accepted tiny AGC artifact.
The committed format regression, CLI regression, 15 valid parser cases and
25 malformed parser cases passed. Late malformed records published no SXI.
See `sxi_logs/input-modes/acceptance.log`, `format-regression.log` and
`cli-regression.log`. Gate peak RSS was 26,624 KiB (individual pipeline stages:
12,288 KiB), under a 9,000,000 KiB address-space ceiling. The isolated release
build peaked at 208,896 KiB. Binary and scratch were isolated under
`/tmp/sxi-input-modes-T3DUT2`; shared executables, k10 processes and k10 scratch
were untouched, and no commits were made.

An exploratory broader run is retained as `sxi_logs/input-modes/full-battery.log`:
the DNA-transcoded, three-record `HOR-nested` fixture fails the existing
`--text` PFP second parse (`A sequence doesn't have w DOLLAR at the end!`).
No algorithm or existing publication gate was changed to accommodate it.

## Query mode and sampled ground-truth gate

`--mode auto|dna|text` defaults to auto: FASTA/FASTQ/AGC builds mark the
container DNA; `--text` marks it generic text. AGC also records reversed
storage. These flags change only the final container header; source bytes,
producer, adapter and sweep are unchanged. See [SXI_QUERY.md](SXI_QUERY.md).

`--verify-text-sample N` (0 by default, maximum 100000) runs
`sxi_text_audit TEXT fresh.ri4 fresh.agg fresh.sA N` immediately after the
sweep and before publication. It selects up to N evenly spaced emitted chi
witnesses, replays compressed sweep candidates, and compares their shared
context, exact LCP endpoint, and distinct following characters directly
against source bytes. AGC builds fetch these bytes directly from the archive; other
input modes use their existing text input. The reversed/cyclic coordinate map
is `(n - SA) % n`. No dense SA, inverse SA or BWT is constructed.

The audit logs `TEXT_SAMPLE_PASS requested=... verified=... chi=...` or
`TEXT_SAMPLE_FAIL ...`; any failure prevents publication. It uses five bounded sequential streams, 64 KiB direct-text comparison
buffers, and O(N + alphabet) state. Runtime is the compressed sweep plus
the total sampled context lengths. Neither text nor aggregates are mapped
or expanded; memory is independent of corpus size. This is a sampled witness-grounding gate, not a proof of
complete chi-set equality. Legacy newline multi-string convention fails
loudly if this optional cyclic audit is requested.

## AGC streaming and archive audit

The first parse uses one private FIFO (mode 0600) in the new build directory.
The pipeline starts the PFP reader, then opens **one** writer descriptor and
passes it as stdout to **one** agc2flat process. The parent closes its copy
immediately after spawning the producer. There is no second writer, tee,
retry producer, keeper descriptor, or text spool. This discipline matters:
two producers writing one FIFO corrupt the byte stream. The reader-open
handshake times out after 30 seconds; data transfer has no fixed time limit.
Both exit statuses must succeed before the next stage can start. A broken
pipe or failed child aborts the build and prevents publication. On success,
failure, SIGINT, or SIGTERM, the pipeline reaps its own process groups and
unlinks the FIFO; it retains parse artifacts, names, and logs for review.
SIGKILL or host failure cannot run cleanup; any leftover FIFO is confined to
that unique failed scratch directory and is never reused by a later build.

For AGC, `--verify-text-sample N` starts a persistent `agc2flat --serve-ranges
NAMES --revlines --upper --sep 1e` reader of the original archive. It does not
open the `collection.txt` argument. The service checks the names rows against
archive sample/contig order (including repeated contig names across samples),
validates their mirrored offsets, and maps stream offsets to reversed contig
ranges using `get_contig_range`. Separator positions return 0x1E. A request
may cross any number of record boundaries. The service sends an initial
little-endian u64 collection length, accepts pairs of little-endian u64
(offset, length), and returns exactly length bytes (at most 64 KiB per request).
Clean request EOF ends the service; malformed requests, short reads, archive
errors, or nonzero service exit fail the audit before publication.

The audit retains two 64 KiB range buffers, plus the existing compressed
streams and O(N + alphabet) witness state. The range service retains O(record
count) name/coordinate metadata and the AGC decoder's segment working set;
it does not decompress or scan the whole archive for audit. It does not
construct an SA, BWT, or LF walk. Even `--materialize` AGC builds audit the
archive, so both input paths exercise the same independent witness check.

Reproduction and evidence (isolated tools, no edits to running build trees):

```sh
python3 bit6/test_sxi_fifo.py
python3 bit6/test_sxi_stream.py --work /tmp/new-stream-battery \
  --xsa /tmp/sxi-stream/xsa-target/release/xsa --tools /tmp/sxi-stream/tools \
  --log-dir bit6/sxi_logs/stream-mode
python3 bit6/sxi_logs/stream-mode/run_regressions.py
python3 bit6/sxi_logs/stream-mode/run_yeast.py
```

The full yeast script runs stream and fresh file control sequentially, samples
32 archive witnesses in each, checks absence/cleanup of materialized text/FIFO
as applicable, and compares all five core members byte for byte. It records
process-tree peak RSS and enforces a gate below 20 GB. Evidence lives in
`sxi_logs/stream-mode/`; scripts use fresh output directories on a rerun.
