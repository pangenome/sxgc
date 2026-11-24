# Prepared packed columns → bounded row43 stream (fixture implementation)

`overlap_bounded_packed --fixture COLUMNS STOP ARRAY_BYTES ARENA_BYTES [EXPECTED_RANKS]`
reads lexical `dictionary.packed` and little-endian `parse.u32`, never corpus,
refs or text SA/LCP. Stdout is binary; require successful exit, and discard
**all** output on stop (75) or error. Optional expected one-based text-order
parse ranks are read sequentially. No dispatcher, front end, container,
concurrent verifier or gate changes. Hard limits: 64-bit host, 1 MiB text,
2^20 occurrences, 16 MiB tape, 200,000,000 array bytes. Wide kernel even for
tiny fixtures. Stop checks at phase boundaries, before END and every 4096 rows;
FFI, comparisons, a distribution and blocked writes are not interruptible.

## Why the rows are exact

The immutable input contract is an overlapping lexical parse with prefix-free
owned tails (8-byte overlap). Check strict header order `(raw, eof)`, spans,
unique final EOF, dense occurring IDs, every boundary overlap, and the shared
byte kernel's 254-symbol limit. Validate before compacting the original tape
forward in place (two passes; EOF=1, separator=0, data>=2). Keys are borrowed
finite slices. No second payload or per-phrase header table. Parse recurrence
certificate → PLCP → aligned byte LCP = equal-token strides plus common bytes
of first differing words. END is rank zero. Insert occurrences in successor
rank order into per-word lists with **text-order** starts and predecessor from
the previous word's last owned byte (position zero uses 256).

Emit global `(n,0,last byte)` once. The range-sort callback gets one whole
finite-key class and its exact LCP with the previous class, even across
batches. For the first member emit that LCP; subsequent members follow the
min-heap's continuation rank order, with `len(key)-8+RMQ(rank1-1,rank2-1)`:
subtract the overlap because both successors start at the last trigger. An
EOF-bearing key cannot tie. For each descriptor add offset to the occurrence
start; use the decoded in-word predecessor for nonzero offset. Adjacent
proper-prefix classes are refused rather than treating finite-key order as
sufficient on an invalid parse.

## Preallocation / lifetime (logical payload bytes)

Let `T` = actual packed file bytes (capacity remains T after compaction), `m`
= occurrences, `k` = unique words, `B=T+8(k+1)+8k` = tape, dictionary starts,
counts; `R=16 ceil(m/16)` = BlockMin tree. Admit the maximum, **not** the sum:

|phase|simultaneously live upper bound|
|---|---|
|wide parse, no tape yet|`20m+48k` (u32 IDs, i64 kernel input, i64 m+6k slots; input dropped before inverse)|
|pack, align PLCP|`B+28m+8` (IDs, inverse/PLCP, compact SA, text starts and aligned)|
|build lists, PLCP dropped|`B+37m+8+R+8(k+1)` (IDs/SA, starts, aligned/tree, positions/ranks/predecessors = 13m, loc+cur)|
|range order and stream|`B+21m+R+4(k+1)+A` (drop IDs/SA/starts/cur; A is arena budget)|

`phase_bytes` widens counts before products. Parse/plcp refuse unsupported
rank/extent; wide text and dictionary starts do not widen occurrence list
indices. At emit, `S=floor((A-16*65536)/(16+20))` is maximum descriptors per
range. The range workspace admits its 65536 2-byte-prefix bins and `16*S`
bytes of tails; heap capacity `min(k,S)` is explicitly reserved before sort.
Equal tails have at most one offset per word (length fixes the offset).
Check class length before pushing, pop before replenishing: no geometric
heap growth. Refuse a bin exceeding S, or >64 distributions, before END.
Not counted: allocator rounding/metadata, kernel internals, stack, fixed-size
I/O buffers, page cache, downstream process. Not an RSS or cgroup guarantee.

**Arithmetic only, not a grant.** Scale the archived 10 GB `m,k,D` linearly
by ten: 3,181,644,880; 2,344,727,230; 112,395,446,120. Derive
`T=D+3k-1`. `m+6k=17,250,008,260`. Parse / align / lists / emit with a
128 GiB arena = 164.08 / 229.13 / 276.24 / 348.09 GiB: *array* fit under a
hypothetical 384 GiB. It does not prove actual bin fit, number of scans, or
actual census. At the width extreme `m=k=2^32-2`, all words distinct, 100 GB
text gives `T=n+12k-8`; **list construction alone is 389.13 GiB**, and emit
before arena is 309.13 GiB. A safe follow-up for that case: use loc itself as
the forward insertion cursor, restore starts by shifting completed exclusive
ends one slot right and setting `loc[0]=0` (cross-check against counts).
Eliminating independent cur saves 16 GiB at this extreme. Separately reduce
arena below 74 GiB and recheck the bin/skew/pass census, never assume fit.
No integer-width change justified past the u32 limits.

**Replacement seam:** `overlap_range_order::order`'s distribution arena and
`Dictionary::key` lookup are where to replace the 16-byte native `Tail` with
a checked `(word:u32, offset:u32)` pair. Keep distribution index in usize,
dictionary starts u64, prefix/range admission in both counts and rows, and
pass borrowed groups of packed descriptors to the same merge (widen **one**
pair at lookup, no converted group vector). Change both the slot equation and
`order`'s size accounting together. A bin still too large needs deeper prefix
refinement without splitting equal keys, or refusal; packing does not fix a
single huge tie class.

Cost from prepared columns: sequential input `T+4m`, no producer scratch,
`12(n+1)` output bytes. Range census plus up to 64 whole resident dictionary
walks; comparisons may reread arbitrarily long common prefixes, and pointer
lookups/RMQ/list access may miss cache. Not a physical IO or timing estimate.
No raw-input timing, matched producer/consumer benchmark, CPU/RAM/swap grant,
or performance promotion here. Do not raise caps or choose defaults on these
results. Reproduce from repository root:

```
cargo build --offline --release --manifest-path xsa/pfp/Cargo.toml --bin overlap_bounded_packed --bin overlap_resident
python3 gates/overlap_bounded_packed_test.py
cargo test --offline --release --manifest-path xsa/pfp/Cargo.toml --bin overlap_bounded_packed --test overlap_wide_parse --test overlap_range_order
```
