# Parse-memory lane: construction audit

The task's initial 3 × 8-byte-array model is **not** the pinned implementation.
`gsacak` commit `1e533019c29617a496b6d47e929d2639632a5ded` has only
`#if M64` (64-bit) and `#else` (32-bit). Neither `M5` nor `M32` selects a
separate five-byte ABI. Defining `M5` alone would silently choose 32 bits.
Both retained forks and the sealed dependency have this implementation.

`pfp/utils.hpp::gsacak_templated` passes **null LCP and DA** to gsacak.
Only the temporary SA is eight bytes per dictionary symbol. This selects
`gSACA_K`, not the LCP/DA variants. The caller copies its output to packed
`saD` while the temporary array remains live. Later stages build packed
DA, ISA, LCP, and colex structures. At 466, SA and ISA use five bytes;
DA uses four; the completed 466 reference logs **four-byte L1 LCP** and two-byte L2 LCP. LCP width depends on maximum phrase length.

Liveness, in construction order (D means symbols, not bytes for L2):

| Stage | Large live arrays | Change in this lane |
|---|---|---|
| Input/padding | dictionary vector; transient old and new allocation | Reserve exact padded capacity instead of vector doubling |
| gSACA-K | dictionary, boundary rank/select, temporary SA | 32-bit workspace for D and alphabet <= INT32_MAX; 64-bit otherwise |
| SA packing | preceding arrays plus packed final SA | Same final representation; simultaneous-copy peak still exists |
| DA/ISA/LCP | dictionary, packed SA, DA, ISA, LCP | Omit L1 DA; obtain phrase ID from existing boundary rank |
| Colex sort | preceding arrays plus reversed phrase vectors and IDs | Sort phrase IDs against the original dictionary; no copied phrase symbols |
| L2 construction and merge | all retained L1 arrays plus L2 structures | Release L1 ISA once LCP is complete |

The merge uses L1 LCP for adjacent equal-suffix groups and L1 DA only to
identify the phrase containing SA[i]. Thus LCP remains resident. The DA
replacement is exactly `rank_b_d(saD[i]) + b_d[saD[i]] - 1`, including
phrase starts and dictionary terminator. L2 DA, ISA, LCP and colex DA
remain materialized: its PFP query support needs them. The patch changes
all nine L1 DA reads and its constructor assertion. No input-text walk,
full-text SA, or phi reconstruction was added.

Exact reservation improves virtual address requirements. It must **not**
be counted as an automatic D-byte RSS saving: unused vector capacity may
never have been touched. `allocations-*.log` logs pointer lifetimes for
malloc/calloc/realloc/free, including sdsl allocations that operator-new
tracing misses. `LIVE` records report persistent array storage separately
from process high-water RSS. The trace library is a Linux/glibc experiment,
not a production dependency.

## Scope and reproducibility

`tools/build_memory_rpfbwt.py DEPS RPFBWT_BUILD OUTPUT_DIRECTORY` uses
existing pinned dependencies read-only, copies the rpfbwt translation unit
and algorithm header, applies `bit6/patches/rpfbwt_memory.patch`, and
compiles the opt-in `SXI_MEMORY_DICTIONARY` path. Both gSACA-K ABIs are
linked; all defined 32-bit symbols are renamed, including helper globals,
to prevent cross-width symbol interposition. The normal build remains
unchanged pending the full gates and independent review.

The copied pile input is 1,082,130,213 bytes. Measurement slices are decimal
100 MB, 500 MB, and 1 GB **before** stripping bytes 0..5. Stock pfp++ also
rejects bytes >=128 due to its signed `char` comparison. The isolated
measurement probe changes only ParserText's local byte to unsigned char;
it does not remap, drop high bytes, or implement the parallel remap lane.
The measurement includes UTF-8 bytes unchanged. `build_parser_probe.py`
records how the local object is linked into a copied archive. The first
stock-parser failure remains in `measurements.jsonl`.

`contact_supervisor` is absent from the available tool catalog. No request
or delivery through that channel could be made. No retained inputs, shared
upstream checkouts, or staged files were changed.

## Review notes

- Capacity blocker: tested parse regimes do not project below 900 GB. A
  standalone mmap probe does not resolve full-constructor RAM/disk needs.
- Acceptance blocker: K10 four-file comparison and independent review are
  pending; the process/journal continuation is documented in GATES.md.
- Performance tradeoff: direct dictionary colex comparisons perform extra
  boundary-select queries, and derived DA adds rank queries during merges.
  The observed yeast and 100 MB elapsed times increased ~21% and ~24%.
  These were shared-machine observations, not isolated CPU benchmarks.
- Scope boundary: the unsigned-byte parser probe is for this measurement;
  the production remap may change phrase statistics and needs recalibration.
- Correctness review: derived DA matches the previous formula exactly;
  L1 ISA has no merge readers; L2 arrays remain intact. The 32-bit branch
  is bounded by INT32_MAX, not UINT32_MAX, because gSACA-K uses signed
  sentinels internally. Both ABIs have disjoint defined symbols. No new
  correctness blocker was found by code inspection and the completed gates.
