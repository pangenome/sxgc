# SXI1, internal version 1

All integers are little endian. Readers detect magic, independent of suffix;
`.ri4` v4 remains supported. Fresh source construction is described in
[SXI_BUILD.md](SXI_BUILD.md); checked seam-repair evidence is in
[sxi_logs/sep-convention/repair2/ACCEPTANCE.md](sxi_logs/sep-convention/repair2/ACCEPTANCE.md).

The 64-byte header is:

| Offset | Field |
|---|---|
| 0 | four bytes `SXI1` |
| 4 | u32 version = 1 |
| 8, 16, 24 | u64 n, named record count k, run count R |
| 32, 36 | u32 member count, total header+directory bytes |
| 40 | u64 exact file length |
| 48 | u64 flags: bit 0 = chi completed; bit 1 = DNA mode; bit 2 = reversed records; bit 3 = record metadata present; other bits zero |
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
Names feed all SXI query/MEM coordinates automatically. TSV rows contain
`name<TAB>fstart<TAB>length`; fstart is mirrored, so
`stream_start = n - 1 - fstart - length`. Rows must partition the stream in
order, including one reserved separator byte after each named record.
The sorted array `stream_start + length` is the 0x1E boundary array.
A predecessor/rank search gives the record ID; offsets exclude separators.
For reversed storage, offsets are converted back to the original record.

**Records versus byte strings:** `k` is the number of named records, e.g.
9,901 for yeast235. The RLBWT still indexes ONE cyclic byte string
`T = s1 0x1E ... sk 0x1E`; `.ri4` transport k remains the core string count.
The native SXI-to-core loader separates these counts. A names-free text
container retains its transport k and queries use the synthetic name `text`.
No text scan or sequence-content rewrite is introduced by this metadata fix.

Writers set bit 3 and validate k against the names rows. The source pipeline
sets bit 1 for FASTA/FASTQ/AGC, clears it for text, and sets bit 2 for AGC's
reversed storage. Build `--mode dna|text` overrides the automatic mode.
Legacy flags 0/1 remain readable: the loader derives record k from names,
without modifying the file; legacy auto mode is text/forward because the old
header does not encode provenance. Use `--mode dna --revlines` for a legacy
AGC container. New flags require upgraded readers; old readers fail closed.
The five core members and verbatim names bytes are unchanged.

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

Source build and query interfaces:

```sh
xsa build --agc archive.agc -o out.sxi --verify-text-sample 32
xsa build --fasta refs.fa -o out.sxi
xsa build --text collection.txt -o out.sxi
xsa mems --sxi out.sxi --reads reads.fq.gz -j 4 --min-len 20
xsa serve --sxi out.sxi -j 4 --bind 127.0.0.1:7331
```

See [SXI_QUERY.md](SXI_QUERY.md) for the query/server contract and limitations.
`xsa stats --sxi FILE` labels k as records, validates names and reports exact
embedded chi and member sizes. Repacking yeast235 with the new writer reports
k=9901 while all six members retain their original hashes. Published legacy
artifacts are not edited.
