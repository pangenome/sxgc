# SXI1, internal version 1

All integers are little endian. Readers detect magic, independent of suffix;
`.ri4` v4 remains supported. This implementation is a container/consumer
bridge; fresh endpoint construction is blocked as recorded in
[SXI_CONSTRUCTION_BLOCKER.md](SXI_CONSTRUCTION_BLOCKER.md).

The 64-byte header is:

| Offset | Field |
|---|---|
| 0 | four bytes `SXI1` |
| 4 | u32 version = 1 |
| 8, 16, 24 | u64 n, string count k, run count R |
| 32, 36 | u32 member count, total header+directory bytes |
| 40 | u64 exact file length |
| 48 | u64 flags: bit 0 = chi completed; other bits must be zero |
| 56 | u32 IEEE CRC32 of header+directory, this field zeroed |
| 60 | u32 reserved = 0 |

Directory: five mandatory entries followed by optional names. Each entry
is 40 bytes: u32 ID, u32 codec, u64 offset, u64 byte length, u64 item count,
u32 IEEE CRC32 of stored member bytes, u32 reserved=0. ID and codec are
identical in v1. Entries occur in ID order. Each member starts at the next
8-byte boundary after the preceding member (zero padding written). No
trailing data is accepted. CRC is accidental-corruption detection, not
cryptographic authentication.

| ID | Member / codec | Stored representation |
|---|---|---|
| 1 | RLBWT run table | 256 u64 cumulative-before C entries; R u8 symbols; R u32 lengths |
| 2 | Tail samples | SDSL int_vector: u64 bit count, u8 width, LE u64 packed words; R mirrored samples `n-1-SA[tail]` |
| 3 | Head samples | R raw u64 `SA[head]`, in run order |
| 4 | Anchors | XANC: u32 `0x434e4158`, u64 count, `(u64 row,u64 mirrored sample)` pairs, increasing row order |
| 5 | Chi set | sorted unique witness positions, unsigned LEB128 gaps, starting from zero |
| 6 | Names (optional) | existing UTF-8 names TSV, verbatim; count = byte length |

SA samples lie in `[0,n)`. Chi witnesses use the existing `.sA` coordinate
convention, including the virtual end at **n**; allowed range is `[0,n]`.
The chi codec preserves the set, not the sweep's emission order. Normal
`chi-rspace` output retains its existing byte order; `sxi-info --chi-out`
exports the sorted set. Duplicate witnesses are rejected. A stage container
contains all five mandatory members, with empty chi and flag bit 0 clear;
a completed empty chi set is distinguished by that flag being set. Empty
anchors are legal, but cannot resolve interior erased-sentinel rows.

Writer refuses an existing output or partial and publishes with an atomic
no-clobber hard link. It validates runs/C, sample ranges, anchors, chi
uniqueness and sizes. CRC validation and section bounds precede consumer
use. Rust validates all member semantics, including canonical varints.
Native C++ slim maps the embedded head member directly, reads embedded
anchors and tails, and does not need an extracted `.ri4` or head sidecar.
Optional names feed Rust plain-query coordinate decoding automatically.

Cost: writer O(R + chi log chi + names bytes), O(chi) working memory plus
bounded I/O buffers. Sorting is over witness positions, never text positions.
Rust validation uses bounded streaming buffers plus O(k) anchors; query
index construction retains the original O(R) data structures. All readers
currently restrict R to u32 because the existing query index uses u32 run
IDs. Head positions remain raw u64 in v1; tails retain their packed width.

Build:

```sh
bash tools/build_sxi_tools.sh /tmp/laneU/sxi-tools
```

The writer is C++ (`bit6/sxi_write.cpp`); Rust consumes SXI directly.
`xsa stats --sxi FILE` validates the container and reports its embedded chi,
all member sizes, delta/raw chi ratio and exact file size. `--ri4` remains
accepted, with magic detection determining the actual format.

The final source-build interface is recognized:

```sh
xsa build --agc archive.agc -o out.sxi
xsa build --fasta refs.fa -o out.sxi
xsa build --text file.txt -o out.sxi --verbose
```

These commands currently **fail preflight without writing output**. The
Rust module is a CLI boundary only; it does not implement the missing
endpoint construction or the requested internal stage orchestration/chi
gate. This must not be presented as a working source-build UX.

Endpoint-ready development pipeline (distinct from a fresh source build):

```sh
python3 bit6/sxi_pipeline.py --ri4 input.ri4 --heads input.head_sa \
  --parse input_pfp --names input.names.tsv --output output.sxi \
  --writer /tmp/laneU/sxi-tools/sxi_write --dump /tmp/laneU/sxi-tools/slim_dump
```

This writes a stage container, builds slim aggregates using its embedded
heads, sweeps chi to `output.sxi.sA`, and writes the completed container.
It retains logs and intermediates and refuses overwrites. Child address
space is capped at 149 GB; mmap-heavy stages can hit that conservative cap
before physical RAM does. No process outside this tree is controlled.
`--source PATH --output output.sxi` explicitly fails preflight until the
endpoint dependency is resolved; it is **not** an implemented G2.

AGC input boundary: `agc2flat ARCHIVE.agc --samples samples.txt --revlines
-o SOURCE` is the existing streaming collection-text producer. The inspected
`--revlines` branch writes `-o` directly and ignores `--stdout`; use a named
FIFO at SOURCE with a reader already running to avoid storing the flat text.
The names file is written as `SOURCE.names.tsv` at EOF. It emits reversed
contigs in archive order, newline-terminated, plus names metadata. Preserve
those exact bytes for the PFP parse and endpoint producer; substituting the
single-string `h10ss` conversion changes the index. The missing normalized
endpoint producer must be supplied before this stream can be wired to the
source entry point. The running pfp466 process is never a pipeline target.
