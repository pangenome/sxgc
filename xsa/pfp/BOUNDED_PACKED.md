# Prepared packed columns → bounded row43 stream (fixture implementation)

`overlap_bounded_packed --fixture COLUMNS STOP ARRAY_BYTES ARENA_BYTES [EXPECTED_RANKS]`
(16-byte predecessor control) or
`overlap_bounded_packed --fixture-compact COLUMNS STOP ARRAY_BYTES ARENA_BYTES CURSOR_CAP [EXPECTED_RANKS]`
(8-byte compact MSD candidate)
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
|build lists, PLCP dropped|`B+37m+8+R+4(k+1)` (IDs/SA, starts, aligned/tree, positions/ranks/predecessors = 13m, loc)|
|range order and stream|`B+21m+R+4(k+1)+A` (drop IDs/SA/starts; A is arena budget)|

`phase_bytes` widens counts before products. Parse/plcp refuse unsupported
rank/extent; wide text and dictionary starts do not widen occurrence list
indices. On the 16-byte control, `S=floor((A-16*65536)/(16+20))` is maximum descriptors per
range. The range workspace admits its 65536 2-byte-prefix bins and `16*S`
bytes of tails; heap capacity `min(k,S)` is explicitly reserved before sort.
Equal tails have at most one offset per word (length fixes the offset).
Check class length before pushing, pop before replenishing: no geometric
heap growth. Refuse a bin exceeding S, or >64 distributions, before END.
Not counted: allocator rounding/metadata, kernel internals, stack, fixed-size
I/O buffers, page cache, downstream process. Not an RSS or cgroup guarantee.

Reuse `loc[v]` for forward insertion, checked increment. In descending ID order
require the completed cursor equals the restored upper bound (starting at m),
subtract the known count with checked subtraction, and require the initial zero.

**Compact candidate.** Reserve `C=min(k,CURSOR_CAP)` heap cursors, 20 bytes each,
then `compact::fixed_bytes(P)` for the plan, histogram and explicit fixed radix
stack (64 triples of usize on this host). Remaining space buys 8-byte descriptors.
A whole class remains admitted or refused, even when its cursors do not all fit.
No tie copies. Since the finite length fixes the offset, there is at most one
member per word. Rank-ordered per-word occurrence lists are disjoint in rank:
`continuations` emits END then each noninitial successor once, from certified SA.
For `class.len()>C`, visit integer windows of width C on `[0,m+1)`. Two lower
bounds per list select its subspan. Every active subspan has a distinct first
rank in the window, so heap length never exceeds C; retain `prior` through empty
windows and seams. The extra lower-bound predicate evaluations are bounded by
`oversize_descriptors * ceil((m+1)/C) * 2 * (floor(log2(m))+2)`. Refuse above
100 million. First try substituting *all* descriptors for oversize_descriptors;
if too loose, dry-order once to count the complete oversized classes, before
END. Each of two orders then gets 32 full classifications rather than 64;
unused dry passes are not transferred. Stop at every compact census/distribution
boundary. Neither long key sorting nor a blocked downstream write is cancellable
in the middle. Plan/work failure is not a signal to raise the grants.

For maximum finite length L and U descriptors, either path performs at most
64U classifications (prefix checks up to L bytes); at most two compact local
orders (`17U(L+1)` digit reads apiece plus O(UL) group equality/LCP bytes).
Emission visits n occurrence rows, O(n log(C+1)) heap work and O(n log(m+1))
RMQ work. Charge 8192 bytes for the one active buffered reader/writer plus stdio,
vector headers, decoder maps, main/C stack, libsais internal scratch, allocator
rounding/metadata, executable and page cache outside logical arrays. Only an
actual finite cgroup with headroom bounds the process; arrays do **not** certify RSS.

**Follow-up fixture only:** `--fixture-shortest` accepts the same tiny fixtures and caps as `--fixture-compact`. It can certify the *exact maximum* finite tie from the last nine bytes of each ordinary word, before full finite-key order. See [proof, costs and falsifiable prediction](SHORTEST_TIE.md). `--component-100m` is the banked two-order control path and has **not** been switched to this certificate. The original [100 MB component](../../gates/receipts/bounded-component-100m/) passed all rows but missed the performance requirement.

## Intermediate handoff — metadata preflight, NOT a launch path

`xsa/target/release/overlap_bounded_packed --plan-component 100m` (or `1g`)
opens no columns, models **only** the two banked prepared shapes, and prints
`launch=false`. Source: `gates/receipts/radix-emit-100m/one/resident.log`
and `packed-raw-1g/{resident.log,front-end.json}`; T = encoded dictionary bytes
+ 3k - 1. At 100,000,503 bytes propose 320 MiB array / 64 MiB arena / 8192
cursors; at 1,000,000,665 bytes 3 GiB / 512 MiB / 65536. Minimum distributions
are **lower** bounds, not proof of actual pass, tie, or work fit. Ordinary
fixture producer still refuses both sizes. No grant for larger inputs.

Before adding any executable intermediate path:

1. Separate exact-rung CLI (never relax `--fixture`); require pinned corpus and
   column identities from independent preparation, dimensions/row mass, actual
   array, arena and work checks. Do not authorize an arbitrary same-size input.
   Use new private 0700 dirs, reject links/shared columns, fsync/fadvise +
   `gates/private_cold.py` mincore each private input, never evict banked files.
2. Require dedicated non-root cgroup v2, check effective finite memory/CPU and
   zero swap, including ancestors, before any preparation/read; log exact grants,
   affinity, memory events/peak and per-process physical IO. Proposed producer
   limits: 512 MiB / 4 GiB respectively, 1 CPU; independently capped raw oracle
   140 / 1400 MiB, 1 CPU. Refuse pending resource approval. Poll stop at each
   full pass (including front end and oracle), propagate failure/75 to all peers,
   collect every exit and discard partial evidence. Guard bounded logs, pipes and
   queues with a combined 4 MiB reservation; count all private scratch incl.
   `T+4m` private columns, plus any additional ranks, copies and staging against
   2N over simultaneous lifetimes. Private raw oracle input is immutable input,
   report it separately. Never materialize triples; raw preparation needs its own
   prospective peak check, not a sum of isolated phase timings.
3. Stream *every* row through the independent raw `overlap_row43_oracle.cpp`
   (1g needs a pinned exact-rung verifier switch, NOT its generic fixture flag).
   Require all adjacent SA/LCP/BWT, END, row count and independent final rank.
   For the 100m narrow digest `82747f11a9925bb4d89cc812410d98e411ce80b1767ee1ff4dc22e94e2cd874e`,
   check high fields zero and convert checked row43 to LE u32 triples before hashing;
   never compare different wire formats. For 1g obtain a matching-width full row
   bank or certify a fresh matched resident stream with raw oracle first; product
   SHA is not row SHA. Re-run both from identical cold columns, profiles, CPUs,
   consumers and verifier settings. Compare from prepared open to verified EOF and
   exit for both, not candidate order+merge vs resident emit alone. Raw-to-verified
   is a separate measured arm. No hash-only vs verified speed comparison, inferred
   IO savings, full-product or scale promotion.

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
