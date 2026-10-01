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
| 48 | u64 flags: bit 0 = chi completed; bit 1 = DNA mode; bit 2 = reversed records; bit 3 = record metadata present; bit 4 = byte permutation present; other bits zero |
| 56 | u32 IEEE CRC32 of header+directory, this field zeroed |
| 60 | u32 reserved = 0 |

Directory: five mandatory entries followed by optional names (ID 6) and optional byte permutation (ID 7). Each entry
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
| 7 | Byte permutation (optional) | 256 u8 values, `sigma[original] = indexed`; count = byte length = 256 |

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
index construction retains the original O(R) data structures. SXI1 and
compact SXI2 v3/v4 store R as u64; native and Rust query indexes now retain
run IDs as u64. Per-run lengths remain u32 in `.ri4` and SXI1; this limits an
individual run, not the number of runs. SXI2 v2's legacy phi record stores a
u32 run ID and remains limited to `R <= 2^32`; current SXI2 publication uses
v4 with packed run IDs sized from R. Head positions remain raw u64 in v1;
tails retain their packed width. No wire layout or version changed, so
existing small-scale SXI1/SXI2 artifacts do not require regeneration. New
high-R artifacts require the widened readers; older readers reject them.

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

The byte-permutation member is present exactly when flag bit 4 is set. It
requires bit 3 (explicit metadata). ID 7 may follow ID 5 directly when there
are no names. The table must be a permutation of all 256 values and fix
`0x1E`; this also ensures no other byte maps to the separator. Omission means
identity, and writers omit an explicitly supplied identity table so previous
identity containers remain byte identical. Nonidentity metadata costs 40
directory bytes plus 256 payload bytes and up to 7 alignment bytes. Old
readers reject the new flag.
PFP emits `<prefix>.remap` during its single input read; the build journal
records its 256 entries and SHA256 before the writer consumes it. Observed
source bytes map into 6..255; unobserved bytes may map into reserved codes,
which are absent from the normalized index. DNA complementation uses original bytes; reversing a sequence commutes
with byte mapping. Positions and lengths remain in
original byte units. The table changes lexicographic order, not equality.

## SXI2 v2 experimental publish codec

`bit6/sxi2_write.cpp` transcodes a validated SXI1 publication without
reading text or walking BWT rows. It writes a distinct `SXI2` magic and
version 2, retaining the 64-byte header, 40-byte directory, flags, alignment,
and CRC rules above. It fsyncs a partial file and uses a no-clobber hard link.
The SXI1 input is never changed. Build with `tools/build_sxi_tools.sh` or
`g++ -O3 -std=c++17 bit6/sxi2_write.cpp -o sxi2_write`; invoke
`sxi2_write --sxi input.sxi --output new.sxi2 --validator xsa
[--escape sidecar] [--max-bytes budget]`. The writer validates the partial
container with the supplied dual-format `xsa`, compares every decoded EF chi
value bytewise with the SXI1 source, and only then links the output. A byte
budget can reject an impossible size target before encoding starts.

| ID | Codec | Representation |
|---|---:|---|
| 1 | 101 | 256 LE u64 C values; 256 u8 canonical Huffman code lengths; u64 head bit count; u64 gamma length bit count; head bits then length bits. Both bitstreams pack the first emitted bit into byte bit 0 and require zero padding. Canonical Huffman codes are emitted most significant bit first; each positive run length uses Elias gamma. |
| 2 | 2 | SXI1 packed mirrored run-tail samples, retained for arbitrary-row fallback. |
| 3 | 3 | SXI1 raw run-head SA samples, used to seed backward-search toeholds. |
| 4 | 4 | SXI1 XANC anchors. |
| 5 | 105 | u64 low width `l`, u64 low bit count, u64 upper bit count, then Elias–Fano low values and unary upper bits. Low values are emitted least significant bit first. Upper 1 positions are `(value >> l) + rank`; the upper vector has `(n >> l) + chi + 1` bits. Empty chi uses two zero bit counts. |
| 6, 7 | 6, 7 | Optional SXI1 names and remap, verbatim. |
| 8 | 108 | `R` sorted 24-byte records: u64 tail SA value `u`, u64 next-head SA value `v`, u32 run ID, u32 zero. The successor for an SA value in this cyclic domain is `(v + value - u) mod n`. |
| 9 | 109 | Zero bytes when criterion C certifies every domain. Otherwise the `SXESC3` reference layout from `sxi_logs/sxi2-v3/escape_codec.py`, with one exact successor array per failed run, sorted by run ID. |

The writer requires a supplied exact escape sidecar if frequency gcd is
greater than one and any domain has more than one value. It checks its domain
coverage, bit lengths, padding, and value ranges before publication. The
loader checks directory and member CRCs, decodes and validates the RLBWT and
EF streams, checks phi ordering and unique run IDs, and validates escape
coverage. `xsa sxi-info --chi-out` decodes EF in increasing order with bounded
streaming state. `xsa mems` and `xsa serve` use the same dual-format query
engine. On frequency-gcd-one inputs, backward search carries a head-SA
toehold and subsequent SA rows use the phi successor. Periodic inputs use the
existing LF fallback for the first row and the exact escape-aware phi map
for adjacent rows.

This v4 codec is a working **intermediate implementation** of the requested
compact form. It retains raw head and tail members and stores a 24-byte phi
record per run. It does not yet serialize a compact LF move map or meet the
intended few-bits-per-run space budget. Phi predecessor is binary search over
the sorted records, so its bound is O(log R), not the intended O(log log n).
The large-artifact gates and size targets must pass before treating SXI2 as
the production replacement.

## SXI2 v3 packed interval experiment

Version 3 retains the SXI2 magic and the CRC protected directory but deletes
members 2 and 3. Its required IDs are 1, 4, 5, 8, 9, 10, and 11; optional
names and remap retain IDs 6 and 7. SXI1 and SXI2 version 2 remain readable.

| ID | Codec | Representation |
|---|---:|---|
| 8 | 118 | Five LE u64 fields: SA width, run ID width, Elias–Fano low width, low bit count, high bit count. Then sorted phi domain starts as EF low/high vectors, packed successor starts at SA width, and packed run IDs at run width. Bit fields are LSB first and high padding is zero. |
| 9 | 109 | The version 2 exact escape sidecar, when needed. |
| 10 | 110 | LE u64 SA width followed by packed LF output start for each BWT run. For a row in run `i`, `LF(row) = start[i] + row - run_start[i]`. |
| 11 | 111 | LE u64 stride exponent 10, LE u64 count, then one u64 tail SA value for every 1024th run. |

The phi intervals are sorted by SA domain start. The reader builds a compact
select directory and an in-memory inverse run lookup to recover a run-tail
sample from member 8. It validates every sparse anchor against that member.
The LF starts in member 10 replace the decoded cumulative sum arrays; the
reader still builds symbol-run lists for rank predecessor queries. Phi domain
predecessor remains binary search. This is a working space reduction, but the
packed interval and LF members exceed the desired 5–12 bits per run and are
not the final Nishimoto–Tabei encoding.
