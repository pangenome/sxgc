# Packed parse order: isolated signed-wide prototype

`src/overlap_wide_parse.rs` is compiled only by `tests/overlap_wide_parse.rs`.
Input is an already decoded slice of little-endian `parse.u32` IDs and the
packed dictionary's lexical alphabet size `k`; order on IDs is lexical phrase
order. This is not a reader, recursive SXLS kernel or product pipeline.
For nonempty packed input require `0 < k <= m <= UINT32_MAX` and each ID in
`[0,k)` (including zero). The caller still checks dictionary membership/density,
EOF placement and overlap. No sentinel is appended or subtracted from IDs.

The old resident path rejects `m+6*k > INT32_MAX` even if `m` and `k` fit.
Select `SixK` for the vendor's recommended workspace or explicitly select
`FourK` to save `16*k` caller bytes. Neither silently falls back. Zero fs is
API-legal but may move bucket allocation into the kernel; it is not offered
as free memory. On a 64-bit host the bounded dimensions imply all sizes and
byte products fit signed 64 bits. Before **any wrapper allocation**, check the
shape and grant, then scan ID bounds. This grant counts logical array bytes,
not RSS or an all-in production grant:

| simultaneously live | bytes |
|---|---:|
| original u32 IDs, writable i64 text, native i64 SA including `fs` | `20*m + 8*fs` |
| IDs, full SA allocation, u32 certificate/output after dropping text | `16*m + 8*fs` |
| returned inverse ranks and caller's IDs | `8*m` |

No shrink/realloc peak is assumed. Add other caller capacity, decoder/file
buffers, packed tape and word index, allocator/page overhead and internal C
stack/heap separately. Insufficient grant is refused, not a prediction of OS
allocation success. See `vendor/libsais64/README.md` for the pinned v2.10.4
serial archive. `libsais64_long` may mutate text; own a private widened copy,
check return code before relying on restoration, compare every ID on success,
and certify against the original. `libsais64_main_32s_recursion` can dispatch
to `libsais_int` when `n` and adjusted workspace permit: small tests exercise
the real i64 ABI and returned SA slots, not necessarily the native-width inner
loop. The naive oracle is independent of that dispatch.

The first `m` SA slots must be `m` distinct indices in `[0,m)`, hence a full
permutation. Write inverse ranks `1..=m`; zero denotes only the absent suffix
after the last ID. Check strict increase of `(ID[i],rank[i+1])` in SA order,
using END=0. This certifies lex order: equal heads defer to successors until
a mismatch or END, and all successor chains terminate. No hashing/sampling.
`m=UINT32_MAX` itself fits this rank convention; `m=UINT32_MAX+1` does not,
even though a zero-based index could still fit. Only ranks escape certification.

## P1: from certified ranks to resident alignment and lists

This second stage deliberately requires `0 < m < UINT32_MAX`; the order-only
API above retains its original range. On the original IDs pass the **owned**
one-based inverse to `ParsePlcp::build`. It builds one u32 position per rank,
checking permutation and strict recurrence again before overwriting the inverse
in place with text-order **token** PLCP. No i64 SA crosses the sort boundary.
The inverse need not be saved: on visiting text position `p` only its own rank
is read, and the compact SA already locates the lexical predecessor.

Kasai bound: if `q` precedes `p` and they match `z>1` tokens, then `q+1`
is below `p+1` and shares at least `z-1` tokens. Every suffix between them
shares that prefix, including the immediate predecessor of `p+1`. Start the
next comparison at `max(0,z-1)`, extend only within both suffixes, and assign
zero to the lexically first suffix. A first suffix cannot inherit a positive
lower bound. Thus total successful extensions are linear (the carried length
falls by at most one per step), with at most one failed comparison per suffix.
This covers arbitrarily long equal runs and proper-prefix suffixes without an
out-of-bounds sentinel read. It says nothing about cache traffic.

Integration sequence (not enabled):

1. Charge the sort phase above. Retire wide text and the full SA/workspace;
   keep IDs and inverse (`8*m`). Validate the independent PLCP/rank ceiling
   and `plcp_array_bytes(m)` **before** allocating the one extra `4*m` compact
   SA. PLCP construction then needs `12*m` total logical parse bytes, including
   IDs, not a second inverse or an 8-byte/row predecessor column.
2. Keep the existing verified dictionary keys, `s` (owned-stride cumulative
   starts), unique EOF ID, and per-word occurrence counts. Use `fill_aligned`
   with `common(dict.key(x),dict.key(y))` and the eventual **u64 RMQ leaves**
   as its output; it makes no array allocations. Rank one has no predecessor
   and gets zero. In an adjacent pair `(i,j)` a common token prefix cannot end
   at either text end: it would include the unique final ID at two different
   positions. At the first unequal token, verified lexical IDs satisfy `x<y`.
   The matched bytes of the equal tokens are their *owned strides*, not their
   full overlapping lengths; add the common bytes of the two unequal keys,
   not their owned strides. This uses the resident packed-key/overlap invariant;
   it does not establish that a caller-supplied dictionary is correct. Checked
   extent/sum failures invalidate the output. Core live bytes here: `12*m +
   8*(m+1) + 8*m` (parse, starts, aligned); add dictionary/loc/etc separately.
3. `into_order()` drops token PLCP. Move the aligned vector into `BlockMin`
   (no copy; add `16*ceil(m/16)` tree bytes). Iterate `order.continuations()`
   directly into the existing per-ID insert closure. Last occurrence gets END
   zero; each other occurrence `j` appears once from SA position `j+1`, with
   its one-based rank. Filtering the stream by word ID preserves continuation
   order. List-build core: IDs+order `8*m`, starts `8*(m+1)`, aligned/tree,
   list position/rank/pred `13*m`, `loc`+`cur` `8*(k+1)`; add other retained
   columns (dictionary, `last_owned`, etc.) and capacity/overhead. Drop order
   and starts after insertion. No materialized predecessor or continuation pairs.
4. Preflight each **whole** phase against its grant before integration. The
   sort/PLCP grants count only the named parse arrays; neither covers caller
   capacity slack, allocator, dictionary packing, starts, lists, or RMQ.
   Recheck EOF/overlap, checked prefix sums, final cursors and all existing
   independent row/product gates. This is an interface, not a promoted rung.

## Integration blockers, not relaxed gates

- `bin/overlap_resident.rs` currently checks measured `n,m,k`, packed byte cap,
  grants and 10 GB census. It reads the entire parse and converts to `i32`;
  replacing only the sort call will not handle IDs above `INT32_MAX`. Derive
  `k=max_id+1` in a wide type; validate file multiple of four, `k<=m`, tape
  headers, count/order and ID bounds before indexing. `packed_words` stores
  a tape-relative `Word.pos` (needs wide coordinates for large tapes); the
  u32 span's high bit means EOF, leaving 31 bits for a word. Dictionary starts
  are u64; text starts `s` and aligned LCP need wide coordinates independently.
- `aligned_and_lists` still calls signed `libsais_plcp_int` on parse/SA with
  `m as i32` until the above replacement is integrated and verified. Per-word
  occurrence counts, prefix starts `loc`, insertion cursors `cur`, list ends
  and heap occurrence indices are u32: exclusive ends can equal **m**, not
  `m-1`. Validate checked totals and cursor increments before conversion;
  widen the whole chain if `m > UINT32_MAX`, including continuation ranks.
  `RankStarts` counts dictionary starts in u32 (checked at construction),
  independent of u64 byte offsets. Per-word offsets, row43's `<2^43` text/LCP
  fields and byte alphabet (two reserved codes) have independent limits.
- The front end `gates/overlap_lexparse10.cpp` bounds occurrence and word IDs,
  uses u32 assignment rows and a reserved missing ID. The older independent
  `gates/overlap_packed_check.cpp` decoder has u32 tape starts and text extent,
  and explicit 1 GB-era caps. Neither a wider sort nor row43 upgrades readers,
  front-end grants, independent verification, or corpus gates.

Reproduce with `cargo test --offline --release --manifest-path xsa/pfp/Cargo.toml --test overlap_wide_parse`.
Tests compare every token PLCP against naive slices and serial `libsais_plcp_int`;
on small overlapping-word fixtures they check aligned byte LCP against raw
suffixes and the narrow formula, and continuation insertion order against both
raw suffixes and narrow SA. Synthetic starts exercise a >u32 aligned sum
without allocating the corresponding text. Only tiny arrays are sorted; limit cases use the arithmetic planner alone.
No 100 GB census, fit, IO saving, or speed result is established here.
