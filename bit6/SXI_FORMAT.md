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

### Rank-concat proposal: unresolved membership

Member 5 remains the position-valued Elias–Fano payload described above. It must
not be relabelled as first-occurrence ranks without a format and reader change.
The production sweep's bounded per-symbol active state does not guarantee bounded
reordering storage: active candidates can be replaced by later input, while later
witnesses have already been determined. The executable regression and measured
synthetic lower bound are in
[`CHI_BOUNDED_DEFERRAL.md`](../tools/binbwt/CHI_BOUNDED_DEFERRAL.md).
This is an unresolved proposal, not a new supported encoding.

## Optional chi on the tape route

`xsa build --tape` constructs chi by default; `--no-chi` explicitly disables
construction (`xsa_finish --no-chi` / `--with-chi` are the stage switches).
No sweep advances, witness values or EF streams are built when disabled.
Tape planning and LCP slice production retain their existing defaults. Finish
skips the unused LCP/LCE answers after consuming any supplied slices. Rank
tiling, endpoint ranges, singleton equality, closure and sampled anchor checks
remain active before this skip. The explicit stage-level `--no-lcp-slices`
producer arm can also pair with `xsa_finish --no-chi`.
The directory omits member 5 entirely, flag 64 declares chi absent, and bit 0
(chi complete) is cleared. `CHI_POLICY present=... member=m5` is journalled.
All other members and their construction are unchanged.

Chi is consumed by the m5 Elias–Fano encoder to store the sorted suffixient
witness positions; the full Rust `sxi::Container` validator decodes it for
range/order validation and `sxi-info --chi-out` exports it. The Rust full
reader refuses chi-absent containers at open with an error naming missing
member m5 (chi). The legacy C++ SXI1 conversion/validation reader also
requires chi for its own format, but does not accept SXI2 v6 in either arm. The qlite
reader does not use it: exact count and locate use m1, m10, m11 and the blocked
run directories m12/m13. Qlite checks the chi absence flag against the directory.
The legacy `product::Engine` (MEMs/serve) also reads the chi count for metadata;
`witness::build` decodes chi to map its positions onto BWT run edges for the
`--first --witness-index` query arm, while `forks::load_chi` uses the set for
fork witness extraction. These paths all open through `sxi::Container`, so
they fail at open when m5 is absent.
The claims checker consumes LCE claims and source text, **not chi**; it checks
inputs to the sweep. Witness sample checks and bridge exports require chi and
are refused with `--no-chi`. An absent chi is not an empty, complete chi set.

### Full-width run directories (v6)

New tape builds enable chi by default; `--no-chi` retains the A/B arm.
The bitmap construction below is unchanged.

New m1 codec **114** replaces codec 102's packed directory with 16-byte
entries: `payload_byte_offset u64, head_bits u32, length_bits u32`.
The 2336-byte header and block payload encoding are unchanged; `payload_off`
is now `2336 + 16*nblocks`. Payload offsets no longer stop at 32 GiB.

New m13 codec **115** retains its 72-byte header, per-symbol counts/offsets,
and level-2 directory of `(cumulative_rows u64, run_index u64)` pairs.
Main entries are now 24 bytes: `(cumulative_rows u64, run_index u64, stride u64)`.
The member ends at `entry_offset + 24*entry_count`. No run ID is masked or
narrowed at either level. m12 already stores every row and index as u64;
its codec 112 and layout are unchanged. The query reader accepts old 102/113
and new 114/115 independently, selected by codec, and validates their sizes.
Old readers reject new m1 codec 114 instead of silently misinterpreting it.

Materialized query ranks are always u64; run lengths retain the producer's
checked u32 wire bound. The run columns require 13 bytes/run, plus 16 bytes
per 256 text rows. The pre-allocation memory check includes both terms.
`XSA_FINISH_CHI_BITMAP`, `XSA_FINISH_CHI_PEAK`, `XSA_FINISH_CHI_PUBLISHED`, `XSA_FINISH_PEAK_RSS`,
and the m1/m12/m13 size lines are copied into the top-level build journal.

### Mmap chi construction (v6 finish)

With chi enabled, `xsa_finish` keeps one membership bit per normalized corpus
position in `<prefix>.chi-bitmap`: exactly `ceil(n/8)` bytes, file-backed with
`MAP_SHARED` and `MADV_RANDOM`. The unchanged interval sweep selects witnesses;
test-and-set retains their first emission. The existing uniqueness invariant is
checked before publication (sweep count = bitmap popcount, no duplicate attempts).
After the sweep, an ascending scan emits codec 105 Elias–Fano directly through
two bounded spool buffers. The bitmap is unmapped and unlinked after this scan;
normal exception cleanup also removes it. Chi-disabled builds allocate no bitmap.

No m5 format or reader change is needed: the published set is already a sparse
bitvector, with `24 + ceil(chi*l/8) + ceil(((n>>l)+chi+1)/8)` bytes for nonempty
chi. The bitmap replaces the former vectors, value buckets and external merges,
not the published encoding. `XSA_FINISH_CHI_BITMAP` reports file bytes and the
process peak RSS immediately after mapping, before encoding, and after publication.
`XSA_FINISH_CHI_PEAK peak_bytes=... scope=process_rss stage=before_publish`
records the observed process high-water RSS before assembling m5; it is not
a chi-only allocation estimate. The finish-v2 address-space cap is at least
`max(memory_gb * 1048576, ceil(3*n/1024))` KiB (or a larger explicit
`--finish-as-gb`), because mapped pages count toward RLIMIT_AS even when not
resident. Each PHASE START records the actual `rlimit_as_kb`.
`XSA_FINISH_CHI_PUBLISHED`
reports payload bytes (including its 24-byte header) and bits per entry.
File-backed pages can become resident: mmap does not guarantee a small RSS.
Chi working storage is O(n/8) file-backed pages plus bounded buffers, while other
finish components also contribute to the reported process peak.

Published positions are asserted strictly increasing and unique. Sweep emissions
are **not** suffix-rank monotone: the existing banana regression emits ranks
3,2,0. A rank-order assertion on those emissions is therefore invalid; retaining
the established representatives requires preserving the interval sweep. See
`tools/binbwt/tests/chi_deferral_test.py`. `--selftest-witness-batch` exercises the
bitmap, duplicates, sparse/dense sets, first-occurrence bridge order, partial-byte
boundaries and cleanup; `tools/binbwt/tests/chi_bitmap_gate.py` compares complete
m5 bytes and CRCs with a retained baseline.

### Finish RSS attribution (2026-11-03)

The unexplained 100 GB finish peak is **resident corpus mappings populated by
bulk readiness scans**, not a decoded run column or a chi-owned allocation.
The archived 100 GB run already reports `peak_rss_kb=97067008` when the chi
bitmap contains zero entries. Its actual container is
`/mnt/nvme2n1/erikg/wf-finish-as-100gb-20261102T044713Z/corpus.sxi2`.

Instrumented mainline (`abf2415`) reports `VmRSS` and `RssAnon` separately from
`/proc/self/status` at readiness, repair-state construction, band-array
allocation, first run-band decode, first repaired band, populated chi bitmap,
and published chi. A 100 ms sampler records the largest sampled RSS with the
anonymous RSS from that same sample. This sampled value is not an exact
high-water mark; the existing `getrusage` high-water receipt remains separate.
`XSA_FINISH_RSS_READINESS_ONLY=1` is a diagnostic that joins PFCK and histogram
construction and exits without consuming the run stream or publishing a
container. It removes its PFCK scratch unless `--keep-pfck` is requested.

At 10 GB, readiness measured 9,830,604 KiB RSS and 60,824 KiB anonymous RSS.
An independent `/proc/PID/smaps` snapshot attributed 9,765,644 KiB specifically
to the corpus mapping. At 100 GB, the readiness-only confirmation measured
97,911,280 KiB RSS and 159,148 KiB anonymous RSS. Its peak smaps snapshot
attributed 97,747,984 of 97,938,208 KiB to `wiki/corpus.bin`. This confirms the
approximately one-byte-resident-per-corpus-byte term. PFCK's 36.4 GB file size
at 100 GB is not a 36.4 GB heap allocation; its construction uses bounded
worker buffers. The finish decodes run **bands**, unlike the query reader's
whole-column decode.

`CorpusText::read_block` now reads directly into those existing worker buffers
with `pread`, then applies the same byte remap and virtual-END rule. Both
PFCK and corpus-histogram scans use this method. Random-access corpus reads
retain their existing adapter. No chi algorithm, codec, directory sampling,
or publication arithmetic changes. In particular, published m5 is streamed
through bounded output buffers; its on-disk byte count must not be charged as
resident heap.

This removes the dominant startup term but **does not establish flat total
finish RSS**. The chi bitmap remains `ceil(n/8)` file-backed bytes, and the
m1/m12/m13 directory vectors accumulate sampled run metadata in anonymous
memory. Those are distinct remaining scaling terms. Neither a flat-RSS claim
nor a chi rewrite is justified by the startup measurement alone.

The 128 MiB bulk-read regression compares all remapped bytes, checks virtual
END and zero-length boundaries, and limits RSS growth to 16 MiB. The previous
mapped implementation fails that residency check; the new implementation
measured 5,368 to 5,496 KiB. Full 8 MB and 150 MB containers compare byte for
byte to their baselines, with chi counts 6,544,643 and 44,034,230. Detailed
receipts and the reproducible baseline instrumentation patch are under
`tools/binbwt/evidence/finish-memory/`; unabridged live-run evidence is under
`/mnt/nvme2n1/erikg/finish-memory-evidence/`.

The complete 10 GB baseline attribution is recorded verbatim in
[`evidence/finish-memory/G1.md`](../tools/binbwt/evidence/finish-memory/G1.md).
Its kernel high-water RSS was 12,151,760 KiB; the 100 ms status sampler reached
12,362,120 KiB with 1,388,540 KiB anonymous at that sample. Later file-backed
reclamation explains the decrease at publication; stage deltas are not a sum
of independent persistent allocations.

After the fix, the 150 MB / 10 GB kernel peaks are 131,068 / 4,180,964 KiB;
the status-sampled peaks are 177,084 / 4,356,012 KiB. **G2's flatness target is
not met.** At 10 GB the populated-bitmap stage is 4,311,840 KiB RSS with
3,087,772 KiB anonymous, separating anonymous allocations (including the
accumulated directory vectors) from the approximately 1.25 GB bitmap. Full 10 GB chi is
2,676,929,445; m5 and the entire container are byte-identical to the baseline,
and the query gate passes all 20 positions. The 150 MB query gate passes
100/100 counts and 574/574 positions. Branches, MEM, seeds, the 150 MB count
regression, all 64 Rust unit tests (including three fanout tests), and the
packaged 8 MB end-to-end build pass.

The stashes were inspected without applying or dropping them. `stash@{0}`
contains an untracked row-code experiment note; `stash@{1}` contains chi
residency pacing and marker instrumentation. The pacing was not adopted:
it cannot address a high-water mark already reached with zero bitmap entries,
and its claimed resident-window bound is not established by the algorithm.

The archived 100 GB container directory records m5 as 8,972,091,731 bytes and
m13 as 12,938,616,320 bytes. These are published file sizes, not measurements
of resident allocations. In particular the approximately 9 GB m5 payload is
streamed to the container rather than held in a chi-sized output vector.

G3 was measured past the requested marker: band 57,601 emitted
3,000,425,823 chi entries (the same progress count as the archived run).
The finish's native marker receipt is:

```text
XSA_FINISH_RSS_AT stage=chi_so_far_gt_3e9 rss_kb=18550288 anon_kb=6337620
```

An external status read **after** observing the qualifying progress line
recorded `VmRSS=VmHWM=18550652`, `RssAnon=6337984`, and `RssFile=12212668`,
all in KiB. This is about 17.69 GiB through the marker, not a full-run peak
for a newly published 100 GB container. The monitor then stopped the three
measurement stages; the existing reference container was not changed.
The last nearby smaps snapshot attributes 12,199,276 KiB to the chi bitmap
and only 10,156 KiB to the corpus mapping. **RSS still tracks input size**:
the original corpus-residency term is removed, but the remaining bitmap is
linear in n and the sampled run-directory vectors also grow.

The shared machine reclaimed pages during the run. A separate read-only
helper, with a fixed 16 MiB buffer, periodically warmed the bitmap scratch
until the marker. This qualification is retained with G3; no elapsed-time
performance comparison or flat-memory claim is made. The interrupted 100 GB
measurement's owned scratch was removed after preserving its receipts.

## SXI2 version 7: the incremental (streaming) container

Version 7 is **the product default from 2026-11-09** (owner-authorised). It exists
because v6 cannot be built as the sweep progresses: its 64-byte header plus the
40-byte member directory are a PREFIX whose contents (length, offsets, CRCs) are
only known at close, and m1's own 16-byte-per-block directory sits in FRONT of m1's
payload. Both force the whole encoded run stream to be staged in scratch before a
single container byte can be placed — at Pile scale that term is ~0.277 n.

v7 keeps every member codec, every payload byte and every semantic (n, k, r,
flags, chi, EF, run directories) unchanged. Only the two directory positions and
one m1 head field set change.

### Container layout

| region | v6 | v7 |
|---|---|---|
| header | 64 bytes | 64 bytes (identical fields) |
| member directory | bytes [64, 64+40\*count) | **trailing**: [length-40\*count, length) |
| member bytes | after the directory | from byte 64, **in stream order** |

Header field `hs` (offset 36) still declares `64 + 40*count`; the physical
directory offset is derived as `length - 40*count` (offset 40 carries the exact
file length). Header CRC still covers the 64-byte header (CRC field zeroed)
followed by the 40-byte directory entries, in that order. Members still tile the
member region, 8-byte aligned; in v7 the member region ends where the trailing
directory starts.

Member write order in v7 is m1 first (it is appended during the sweep), then m5,
m4, m7, m10, m11, m12, m13. The directory is sorted by member ID in both versions.
A v7 reader must not assume a physical order; the partition check is by offset.

### m1 codec 116

| bytes | content |
|---|---|
| [0,2048) | 256 u64 C table (prefix sums of the corpus byte histogram) |
| [2048,2304) | 256 u8 canonical Huffman code lengths |
| [2304,2312) | nblocks = **0** (derived by the reader) |
| [2312,2320) | block_runs (1024) |
| [2320,2328) | rNorm = **0** (derived: the member's count) |
| [2328,2336) | payload_off = **2336** (constant in v7) |
| [2336, 2336+P) | payload: one block pair per 1024 runs, each pair 8-byte aligned |
| [2336+P, 2336+P+16\*nblocks) | per-block directory: (payload byte offset u64, head_bits u32, length_bits u32) |

The payload BYTES and the block directory BYTES are identical to codec 114's
(measured: 7,217,520 payload bytes and 123,920 directory bytes byte-for-byte equal
at the 20 MB fixture); only the directory position changes. The head is written
BEFORE the sweep and never patched, which is why nblocks/rNorm are left zero: a
patched member would need its CRC recomputed over the whole member. The reader
derives `nblocks = ceil(count/block_runs)` and `rNorm = count`.

### Reader compatibility

`xsa query` opens v6 (codec 102/114/116) and v7. `sxi::Container` (the full
validator/exporter) opens v1..v7. Old readers reject v7 rather than misreading it.
The v6 layout remains reachable for A/B with `XSA_CONTAINER_STREAM=0` (or
`xsa_finish --container-legacy`).

### Durability and deletion (finish side)

* `CONTAINER_STREAM mode=incremental member=<m> off=<n>` once per member.
* `CONTAINER_STREAM mode=incremental member=m1 off=<off> blocks=<n> payload_bytes=<n>` every 256 blocks.
* `CONTAINER_SYNC why=<reason> member=m1 member_bytes=<n> unsynced_bytes=<n>` per fsync.
* `PIECE_ABSORBED idx=<band> container_off=<n> bytes=<n>` after the covering bytes are fsynced; the piece is unlinked *after* the fsync.
* `CONTAINER_FINAL bytes=<n> sha256=<hex>` over the durable published object.
