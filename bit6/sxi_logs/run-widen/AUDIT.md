# Run width audit (before edits)

Scope: the `sxi_pipeline.py` path is PFP → `rpfbwt_endpoints` → `slim_dump` → `xsa chi-rspace` → `sxi_write` → Rust `sxi-info`/query. SXI2 transcode/query is included because it consumes the published runs.

| Component | Run-count or run-ID fields and uses | Width before change | On disk? |
|---|---|---|---|
| `rpfbwt_endpoints.cpp` | `r`, `Run::{c,len,h,t}`, scan rows and emitted count | `U` = u64 | `.ri4` header R u64; each **run length** u32 |
| `sxi_write.cpp` | input `r`, `Output::finish(r)`, `Member::count`, all run loops, offsets | `U` = u64, but explicit `r<=UINT32_MAX` | SXI1 header R and member counts u64; run lengths u32 |
| `sxi_format.hpp` | `Container::r`, `Member::count`, member sizes | u64, but explicit `r<=UINT32_MAX` | SXI1 u64 |
| `slim_lce.hpp` + `chi_rspace_dump.cpp` | `Ri4::R`, run/row indices and starts u64; `LfIndex::charRuns` and flat spot-check `charRuns` are u32 run IDs; LF byte accounting assumes 4 bytes | mixed | CRA1 R u64; source run lengths u32 |
| `chi_rspace_dump.cpp` other paths | `Ri4::l` is u32 **run length**; aggregate arrays and calibration indices use u64; indexed BWT helper buffers u32 for text/SA domain | mixed | CRA1 R u64 |
| `xsa/src/sxi.rs` | `Container::r`, `Member::count`, `runs` loops u64; explicit `r<=u32::MAX` | mixed | SXI1/SXI2 header R u64 |
| `xsa/src/main.rs` | `Ri4Header::r` and `Ri4::r`/run indices u64; `Ri4::cruns` u32 run IDs, pushes `x as u32`; `run_len` u32 **run lengths** | mixed | `.ri4` R u64, length u32 |
| `xsa/src/sxi2.rs` | `Phi::r` u64; inverse phi map `Vec<u32>` and `i as u32`; compact run IDs bit-packed with width derived from R; legacy v2 phi record run ID u32 at byte 16 | mixed | SXI2 v2 ID 8 fixed 24-byte records; v3/4 packed width up to 64 |
| `xsa/src/witness.rs` | two witness slots per run (`slots=2R`) u64; `next_block` holds a block ID derived from R and narrows it to u32 | mixed | sidecar header R u64; bitmap bits |
| `xsa/src/product.rs` | `idx.r`, `run_of`, `run_start`, witness slot `2*run`, and product JSON run count inherit u64 run IDs from `Ri4`; matching-statistics `Vec<u32>` stores match lengths, not run IDs | u64 run IDs | no new run field |
| `sxi2_write.cpp` | `r`/loops U; `Edge::run` u32 and `uint32_t(i)`; packed run ID width computed from R | mixed | emits SXI2 v4 packed run IDs |
| `sxi_pipeline.py` | `struct.unpack('<4Q')` RI4 header; Python int for chi count, no narrowing | arbitrary precision | unchanged |
| `rlbwt_sampler.cpp`, `ri4_from_rle.cpp`, `rl_text_extract.cpp`, `sa_decode.cpp` | alternate/legacy run lookups contain u32 run IDs; not invoked by `sxi_pipeline.py` | mixed | alternate inputs and diagnostics |

The u32 run lengths are a per-run length limit, not an R limit. Phrase IDs in `slim_lce.hpp` and PFP vendor code are u32 phrase IDs, not run IDs. CRCs, magic/version, header directory counts and bit widths use u32 for non-run fields. SXI2 v2's fixed record is legacy readable; the current writer emits compact SXI2 v4. The current compact layout can carry 64-bit run IDs without changing bytes for small R. No format bump is planned unless validation finds a wire constraint.

Synthetic test must avoid allocating O(2^33) run records: a sparse header probe can validate the R field and query index arithmetic, but cannot prove a full writer-to-query pipeline at that R within 64 GB. This limitation will be reported explicitly.
