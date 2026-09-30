# BCR web text front end: prototype result and production gates

## Transform and byte contract

The existing PFP producer builds the cyclic BWT of the **one concatenated byte
string** `doc1 0x1e doc2 0x1e ... docN 0x1e` followed by ten internal `0x02`
padding bytes. The separators are ordinary ordered bytes, not distinct
endmarkers. A generalized collection BWT with per-document endmarkers would
not match `.rlebwt`, even if it represented the same documents. The BCR route
must preserve this exact cyclic order and the PFP parser's 256-byte remap.

`bcr_frontend.cpp` reads fixed blocks of the source file from end to beginning,
exactly once per source byte. This is one in-tool read without text
materialization; the OS seeks to each block. At each prepended character `c`,
it replaces the old primary BWT row by `c` and inserts `0x02` at
`C_old[c] + rank_old(c, primary)`. The new primary is that insertion row.
Starting with the ten padding rotations in increasing source-position order
reproduces the PFP tie convention. Runs carry head and tail SA positions in
**final source coordinates**; existing positions do not shift on a prepend.
When a run splits, the newly exposed endpoint samples are recovered by LF
walking to an existing endpoint before the mutation. The initial all-padding
tie is handled directly. The output includes the exact 4-byte RLE words,
4,112-byte metadata, and counted 64-bit `.ssa`/`.ssa_t` columns.

The prototype uses one flat `std::vector<Run>` (`sizeof(Run)=32` here). It is
deliberately capped at one million runs by default. Rank, row lookup, LF
sample recovery, and insertion are linear in the current run count; the
prototype is therefore a correctness reference **and cannot run the yeast235
or 1.082 GB fragment gates at useful wall time**. The cap fails loudly.
`capacity * 32` bytes is reported after construction; temporary split vectors
are small. Peak capacity can approach twice the live run count. Source blocks
add 64 KiB. No dictionary, suffix array, parse, or resident source text is
built.

## Gates and measurements

The independent test computes cyclic rotations and their SA for 33 small
collections, including duplicates, near duplicates, periodic runs, distinct
records, and random records. It compares all four serialized files. A separate
9,705-byte collection and a 10,005-byte collection whose PFP remap changes 12
byte values are built through the two-level PFP/rpfbwt producer and the BCR
prototype; all four files are byte identical in each gate. This **does not imply**
the requested yeast235 or fragment gates passed.

| Same corpus / route | BWT + metadata + head/tail sample gate | Wall | Peak RSS |
| --- | --- | ---: | ---: |
| 9,705-byte synthetic, BCR flat-run prototype | 4/4 byte identical to PFP | 0.30 s | 2,048 KiB |
| 9,705-byte synthetic, PFP | 4/4 reference files produced | not timed as one route | not timed |
| 10,005-byte remapped synthetic, BCR flat-run prototype | 4/4 byte identical to PFP | 0.24 s | 2,048 KiB |
| yeast235, BCR | **not run**: flat-run algorithm is quadratic | not measured | not measured |
| 1,082,130,213-byte remapped fragment, BCR | **not run**: flat-run algorithm is quadratic | not measured | not measured |
| fragment, retained PFP parse + level 2 + rpfbwt | retained producer output, no BCR comparison | 2,066.3 s | 18,479,248 KiB |

The retained fragment metadata has padded `n=1,082,130,223` and
`r=397,723,016`. Removing ten padding rows and normalizing the seam yields
the user's `n=1,082,130,213`, `R=397,723,010`. The retained raw `.rlebwt`
is 1,590,892,064 bytes; each raw sample column is 3,181,784,136 bytes.
The PFP endpoint adapter is a separate retained stage: 390.6 s and
28,257,076 KiB. The 200 GB at an 8 GB slice is the task's distinct
dictionary-SA measurement; it must not be confused with this retained
fragment rpfbwt RSS.

### Memory projections, not measurements

Assume fragment-like `r/n=0.367537`, decimal corpus sizes, and constant run
density. PFP uses the task's 25x peak ratio from the 8 GB dictionary-SA
measurement. A compact BCR working set needs at least 4 bytes per encoded
run plus two packed SA values: 5 bytes per value at these scales, or 14r,
**before** dynamic rank/select directories, block slack, and a document/batch
buffer. The existing on-disk uncompressed `.ssa` columns take 16r by
themselves. The prototype's 32-byte runs take 32r live and up to 64r at
vector capacity; this is not a viable production layout.

| Corpus | PFP 25x extrapolation | BCR compact lower bound `14r` | Flat prototype live `32r` | 900 GB gate |
| ---: | ---: | ---: | ---: | --- |
| 8 GB | 200 GB (task measurement) | 41.2 GB | 94.1 GB | compact may fit |
| 100 GB | 2.5 TB | 514.6 GB | 1.176 TB | compact leaves ~385 GB for directories/slack |
| 1 TB | 25 TB | 5.146 TB | 11.76 TB | external/sharded construction required |

These figures are bounds/projections, **not BCR RSS observations**. The
in-memory final RLE alone is roughly `4r`, but head/tail SA sample maintenance
dominates this web regime. The claim “final BWT + one document” needs that
sample cost explicitly added. A 100 GB production BCR implementation must
prove peak below 900 GB, including allocator, rank directories, and document
buffer; a 1 TB monolithic build cannot satisfy that cap with 2r samples.

## Production front end work

1. Replace the flat vector with a block-based dynamic RLE tree. Store packed
   `(symbol,length)` plus packed head/tail samples in blocks; keep cumulative
   symbol counts and row counts in an internal tree. Batch LF/rank probes and
   compact blocks periodically. Use 64-bit row, run, and word indices
   throughout; `R > 2^32` occurs well before 1 TB at fragment density. The
   4-byte *length* field in the wire format is separate from the run count.
2. Maintain endpoint samples atomically with each run mutation. Validate
   arbitrary split boundary samples against a tiny SA oracle, including
   periodic text and padding ties. The present LF walk is a correctness path;
   production needs bounded sampled locate or an auxiliary Phi/toehold
   structure to avoid long LF walks. Validate remap and cyclic tie handling.
3. Add resumable block and sample checkpoints with source offset, primary
   row, counts, checksum, and versioned remap. Reopen the source at the saved
   offset without rereading committed bytes. Flush outputs to private files,
   verify counts/checksums, then publish with no-clobber rename.
4. Gate exact raw bytes against retained yeast235 and remapped fragment
   `.rlebwt`, `.rlebwt.meta`, `.ssa`, and `.ssa_t`; then gate `.ri4` and
   `head_sa` from `rpfbwt_endpoints`. Record `/usr/bin/time -v` wall/RSS.
   Benchmark 8, 25, and 100 GB slices before committing to a 1 TB plan.

## Parse-free slim design

`chi_rspace_dump.cpp` currently requires `--parse PREFIX` in argument
validation (lines ~526, 549), even in `--slim --resolve-ri4 --dict-stream`
mode. In that mode it delegates to `slim_dump` in `slim_lce.hpp` (line ~563).
The latter consumes `PREFIX.dict` through `SlimDict` (optionally page cached
via `--dict-stream`), scans phrase delimiters and newline metadata, reads
`PREFIX.parse` as 32-bit phrase IDs, expands phrase lengths to text positions,
builds sparse `bd`/`bp` phrase-boundary bitvectors, and builds two
`SLIM_FP` hash layers over dictionary bytes and parse IDs. Its
`collection_lce` joins dictionary-tail, parse, and dictionary-head LCEs,
with a cyclic seam cap for `0x1e` corpora. The query loop resolves run SA
positions from `.ri4` and optional `--head-sa` before asking four LCE-derived
values per run. In the BCR route none of `.dict`, `.parse`, phrase IDs, or
phrase boundaries exists.

Use a narrow `LCE` interface returning `collection_lce(i,j)` and a checked
text length/boundary policy. Keep `.ri4`, head samples, row resolver, aggregate
serialization, and chi sweep unchanged. Build a BWT-native backend from
`.ri4` plus run endpoint samples: LF/rank, Phi or inverse-Phi/toehold for
position-to-row access, and bounded substring extraction. The r-index
literature supplies O(r)-space locate/access/LCP building blocks; it does
**not** make the current PFP phrase hashes magically available. Reconstruct
text order once from the built BWT (no corpus reread), constructing sparse
`SLIM_FP` polynomial-hash checkpoints in O(r)-target space. Compare substring
hashes by binary/exponential search and verify proposed matches against
bounded BWT-native extraction, using the existing refusal/work-budget policy
for collision or excessive work. Explicitly preserve cyclic LCE semantics
across the one concatenated `0x1e` string; legacy newline strings need their
own boundary metadata.

Bounded implementation sequence: (1) factor `SlimLCE` behind an interface
without changing aggregate bytes; (2) implement BWT-native random access and
fingerprint checkpoint construction with an O(r)-space ledger; (3) tiny
independent SA/LCE oracle and all existing `SLIM_FP`/cyclic seam tests;
(4) exact `.agg`, chi, SXI1, and banked SXI2 gates on synthetic and yeast;
(5) fragment wall/RSS and byte gates. Refuse the BCR route if its checkpoint
space or verification work exceeds explicit bounds.

References: [BCR paper](https://arpi.unipi.it/retrieve/handle/11568/728474/440830/BCR_TCS_2013_postPrint.pdf),
[ropebwt3 implementation and paper](https://github.com/lh3/ropebwt3),
[r-index samples](https://github.com/nicolaprezza/r-index), and
[BWT-runs bounded suffix structures](https://arxiv.org/abs/1809.02792).
