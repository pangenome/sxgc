# Fresh-source SXI build

Build tools (no commits and no upstream edits):

```sh
bash tools/build_rpfbwt_tap.sh /home/erikg/r-pfbwt /tmp/rpfbwt-sxgc
bash tools/build_sxi_tools.sh /tmp/laneV/tools
xsa/target/release/xsa build --text collection.txt -o collection.sxi \
  --threads 8 --scratch /path/on/source/filesystem --verbose
xsa/target/release/xsa build --agc collection.agc -o collection.sxi --verbose
xsa/target/release/xsa build --fasta oriented.fa -o collection.sxi --verbose \
  --expect-heads accepted.head_sa --expect-ri4 accepted.ri4
xsa/target/release/xsa build --fastq oriented.fq -o collection.sxi --verbose \
  --expect-heads accepted.head_sa --expect-ri4 accepted.ri4
```

The copy destination must not exist. `build_rpfbwt_tap.sh` copies upstream
including the cached dependency sources, applies `patches/rpfbwt_emit_tails.patch`,
and uses its own CMake build at `/tmp/rpfbwt-sxgc/build-sxgc/rpfbwt`.
The patch changes only the merge's endpoint-selection/range filter and adds
`.ssa_t` output (`u64 count`, then `count` native-endian u64 values, little-endian
on this host). It leaves `.ssa` writes unchanged. The tail merge skips empty
temporary streams to avoid setting the output failbit. Upstream's unchanged
head merge has that empty-stream bug; builds smaller than 1 MB use one chunk,
while large builds use 50. Both choices are printed with `--verbose`.

`XSA_PFP`, `XSA_RPFBWT`, `XSA_TOOLS`, `XSA_AGC2FLAT`, and `XSA_PIPELINE`
override tool paths. The Rust launcher otherwise locates the Python script
relative to the compile-time repository path; shipping a standalone binary
requires installing the script/tools and setting these paths.

Exact stages:

1. Prepare the input. AGC: committed `agc2flat --revlines --upper -o SCRATCH/collection.txt`.
   It streams one reversed contig per line and writes `collection.txt.names.tsv`.
   The text is materialized on the same filesystem as the archive.
   FASTA/FASTQ: stream extracted sequences into the same `collection.txt` path,
   joining wrapped FASTA lines and terminating every record with newline.
   Both then enter exactly the same stages as `--text`.
2. `pfp++ -t TEXT -o PREFIX -w 10 -p 100 -j THREADS --tmp-dir SCRATCH`.
3. `pfp++ -i PREFIX.parse -w 5 -p 11 -j THREADS --tmp-dir SCRATCH` (second level).
4. Patched `rpfbwt --l1-prefix PREFIX --w1 10 --w2 5 --threads THREADS
   --chunks CHUNKS --tmp-dir SCRATCH`.
5. `rpfbwt_endpoints PREFIX fresh.ri4 fresh.head_sa`: bounded-buffer O(r)
   normalization, remove ten padding rows, remap byte 2 to newline, coalesce
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
code, wall seconds, and stage peak RSS. Stages run sequentially with a 149 GB
address-space ceiling. Scratch is intentionally retained for independent
review. Failed stages or gates never publish the requested output path.
FASTA/FASTQ preparation runs in the Python process, recording elapsed time and
process peak RSS in the journal; each completed record logs its identifier,
sequence length and cumulative collection size to stderr and the journal.

`--expect-heads RAW` and `--expect-ri4 FILE` are acceptance-only comparisons,
run **after** constructing fresh endpoints. The latter checks the entire
run table and packed tails. Neither oracle supplies construction inputs.

Collection text must already have the pilot orientation and end in newline.
Do not replace inter-contig newlines by `!` or use the old single-string pilot
as a collection oracle. PFP's suffix order over concatenated newline text can
still differ from BCR's independent-string ordering; endpoint gates must
establish equivalence for each collection. The adapter does not repair that
ordering. For more than one string, both `--expect-heads` and `--expect-ri4`
must pass; otherwise publication is refused. General oracle-free multi-contig
AGC construction is therefore still unavailable with this front end.

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
python3 bit6/test_sxi_build.py
python3 bit6/test_source_battery.py --work /tmp/laneV/new-battery
python3 bit6/test_sxi_input_modes.py --work /tmp/new-input-modes \
  --xsa xsa/target/release/xsa
```

The tiny suffix-array oracle in `test_endpoint_tap.py` is test-only; no such
algorithm is used by the production pipeline. The legacy Phi extractor and
the existing SXI format remain unchanged.

Acceptance (2026-09-27): FASTA and FASTQ each passed five three-record battery
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
