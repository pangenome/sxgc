# Full-width run-index audit

Base: `83cb20f`. Worktree: `/home/erikg/wc-u64`.
Evidence root: `/mnt/nvme2n1/erikg/wc-u64`.

The audit searched `uint32_t`, `int32_t`, `uint16_t`, `u32`, `0xffffffff`,
`0xffff_ffff`, unsigned index declarations and narrowing casts in
`bit6/xsa_finish.cpp`, `bit6/binbwt_slice.h`, `xsa/src/qlite.rs`, and the
m1/m12/m13 consumers. Raw search results are `audit-{finish,slice,reader}.txt`
in the evidence root. Native and packaged producer/finish sources are identical.

Changed sites:

- `SeamClasses::rebuild` occurrence sorting: permutation vector, initialization
  and comparator arguments now use u64, removing a separate 2^32-entry limit.
- m1 writer: codec 114; directory width and payload offset calculation are
  16 bytes/block; byte offsets are full u64 rather than packed u32 word offsets.
  This removes the separate 32 GiB payload limit. Per-block bit counts are u32.
- m13 writer: codec 115; main entries are three u64 words (cumulative rows,
  run index, stride), size closure/journal use 24 bytes; removed u32 refusal,
  packing and mask. Level-2 run indices were already written as full u64.
- qlite m1 reader: codec-specific directory width/shape and full-width byte
  offset decoding; still accepts legacy codec 102 explicitly.
- qlite m13 reader: codec-specific main-entry width/shape, full-width run and
  stride decoding, and removal of the level-2 run-index mask. Legacy codec 113
  main entries still decode with their documented packed width.
- qlite materialized ranks: removed the packed u32 rank representation entirely;
  every cumulative row rank uses u64, even on small inputs. Lengths decode into
  their own column directly, avoiding a temporary u64 column. Allocation
  estimate is 13 bytes/run plus 16 bytes per 256 text rows.
- `binbwt_slice.h`: compile-time assertion that its `U` type is unsigned u64;
  all suffix positions, ranks, run counters, firstRun/lastRun, band IDs,
  anchors and exception-list indices already use U. The enclosing native
  program defines U as uint64_t. No producer wire change was needed.
- Finish journal: explicit final `getrusage` peak RSS; chain journal forwards
  bitmap bytes, published chi bytes/bits per entry, peak RSS and member sizes.
- Tape CLI: chi defaults on; `--no-chi` disables it; expectation validation and
  CLI regression tests retain explicit opt-out semantics.

Retained narrow fields, with reasons:

- `binbwt_slice.h`: SA magic/version/header size; BandRun length and on-wire
  length columns (checked before narrowing); packed RLE fragments, symbol
  values and continuation flags. They are not run/position/rank indices.
  The 257-element scheduling `rank` array is a permutation of alphabet bins,
  not a suffix or BWT rank.
- `xsa_finish.cpp`: file magic/version/width; shard IDs and shard loops;
  on-wire u32 run lengths (including bridge/debug records); four-byte RLE
  word unpacking; SIMD four-lane mask; 256-symbol heap positions and counters.
  The sampled ISA reader accepts old 4-byte and 8-byte files, promoting
  values immediately to u64; the tape route does not use that legacy input.
  m13 in-memory sampling strides are bounded by PF_RUNDIR_OCC=256, and use
  u16 storage; they are emitted and consumed as full u64 in codec 115.
- `qlite.rs`: Huffman/gamma lookup tables, bit counts/shifts, bounded decoded
  lengths, magic/version/member ID/codec/CRC, and directory entry bit lengths.
  Narrow masks remain only in explicitly selected legacy codecs. Native
  platform indices use usize on the package's required x86_64 target.
- m12: header fields, offsets, counts, L1/L2 row samples and reader arithmetic
  already use u64 throughout. No layout change needed.
- m10/m11: sample ranks/positions and anchor counts already use u64.

Regression tests cover run IDs 2^32-1, 2^32, 19,973,766,926 and 2^48;
byte offsets just below/at 32 GiB and at 2^48; legacy 102/113 decoding;
and a materialized rank of 19,973,766,927. End-to-end fixture validation
covers writer/reader agreement and byte-identical m5 publication.

Compatibility: new readers accept old blocked containers. Old readers reject
new codec 114. Run lengths remain limited by their pre-existing checked u32
wire format; this change widens indices and ranks, not run-length encoding.
The 100 GB query reader still materializes run columns and therefore needs
substantially more memory than construction.
