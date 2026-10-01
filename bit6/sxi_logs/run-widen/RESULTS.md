# Run widening validation

## Format decision

SXI1 R and every member count were already u64 on wire. SXI2 v4 stores run IDs at a bit width derived from R; `R = 2^33 + 17` needs 34 bits and fits that codec. The change widens in-memory run IDs and removes artificial u32 caps. No version bump or wire change was made. Existing SXI1/SXI2 artifacts need no regeneration. Rebuild writer, slim, and Rust binaries before using high-R inputs.

## Gates

- Tiny SXI1: independently compiled pre-edit and post-edit writers produced byte-identical 2,395-byte SXI1; SHA256 `8983acc68d89338579c4609c181fed5c4784d6ef7f855dd6c1ca62eba74ea883` for each.
- Tiny SXI2 v4: pre-edit and post-edit transcoders produced byte-identical 2,792-byte SXI2; SHA256 `6dd59d987ce7110943d1806a443454c66e01f182a9a5aeb3fc540a9ba7a4b5fe` for each.
- Synthetic high-R header: production `Output::finish` wrote R=`8589934609` to SXI1 header and five u64 member counts. C++ header reader recovered exact R and reached the expected sparse-body size refusal. Rust `sxi-info` also reached `run/sample sizes`, rather than rejecting `n/k/r`. Rust unit tests exercise the actual query predecessor helper and compact phi packed decoder with run IDs above `2^32` (see `cargo-test.log`). This **does not** constitute a full writer-to-reader-to-query artifact with 8.6 billion stored runs: even the SXI1 raw run table would be 43 GB and the current query arrays exceed the 64 GB RAM budget. This gate is therefore partial, and the complete high-R end-to-end case remains unproven.
- Full small source battery: `random-4-2k`, `random-4-20k`, and `random-4-200k` each passed fresh `xsa build` with exact `.ri4` and head sidecar gates; each aggregate matched its retained reference byte-for-byte. It passed again using the final packaged release build. See `battery-final.log` and the per-stage JSONL/log files here.
- Format regression: `test_sxi_format.py` passes after correcting its SXI2 v4 directory lookup for chi/phi/escape members. See `format-test.log`.
- Cargo release unit tests: all three run-width tests passed (`cxx_writer_header_round_trip`, `high_run_id_query_predecessor`, `compact_phi_run_id_above_u32`); see `cargo-test.log`.
- Final packaged release build passed with all 44 pinned source hashes matching. See `cargo-final-build.log`; the format regression also passed against this binary (`format-final.log`).
- A real 2k SXI1 artifact was transcoded to SXI2 v4 and queried for a present source byte. Three sampled hits matched byte-for-byte across the two readers (`query-differential.log`).

Representative commands (all scratch outputs under `/tmp/sxi-run-widen`):

```sh
g++ -O2 -std=c++17 bit6/test_sxi_run_width.cpp -o /tmp/sxi-run-widen/test_sxi_run_width
/tmp/sxi-run-widen/test_sxi_run_width /tmp/sxi-run-widen/high-header3.sxi
SXI_RUN_WIDTH_HEADER=/tmp/sxi-run-widen/high-header2.sxi BUILD_JOBS=2 cargo test --locked --release --manifest-path xsa/Cargo.toml --target-dir /tmp/sxi-run-widen/cargo-target
python3 bit6/test_sxi_format.py --writer /tmp/sxi-run-widen/sxi_write.after --sxi2-writer /tmp/sxi-run-widen/sxi2_write --xsa /tmp/sxi-run-widen/cargo-target/release/xsa
```

The three-scale battery ran `xsa build --text` once per source with `--expect-ri4` and `--expect-heads`, then compared `fresh.agg` to the retained aggregate. The exact commands, output and stage paths are recorded in the per-run JSONL files.

## Capacity limitation

The code no longer truncates run IDs in the audited chain, but the existing O(R) resident indexes are too large to construct or query a real R≈4.8×10^11 pile under 64 GB. The source run characters alone require roughly 480 GB if resident. This task validates width and small-scale behavior, not an operational 64 GB construction of that pile. SXI2 v2 remains a legacy u32-run-ID format and explicitly rejects R above `2^32`.
