# Packed parse order: isolated signed-wide prototype

`src/overlap_wide_parse.rs` is compiled only by `tests/overlap_wide_parse.rs`.
Input is an already decoded slice of little-endian `parse.u32` IDs and the
packed dictionary's lexical alphabet size `k`; order on IDs is lexical phrase
order. This is not a reader, recursive SXLS kernel, PLCP or product pipeline.
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
  `m as i32`; a separate wide exact PLCP and SA lifetime plan are needed. The
  rank-only return here deliberately does not supply those arrays. Per-word
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
Only tiny arrays are sorted; limit cases use the arithmetic planner alone.
No 100 GB census, fit, IO saving, or speed result is established here.
