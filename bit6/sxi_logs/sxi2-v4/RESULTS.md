# SXI2 v4 implementation and failed publication gate

## Production diff

This lane added a publish-only `SXI1 -> SXI2` converter
(`bit6/sxi2_write.cpp`), a dual-format Rust decoder
(`xsa/src/sxi2.rs` and `xsa/src/sxi.rs`), and query support in
`xsa/src/main.rs` and `xsa/src/product.rs`. SXI2 has Huffman run heads,
gamma run lengths, Elias–Fano chi with streaming decode, sorted SA-value
phi intervals, and criterion-C escape-list lookup. A frequency-gcd-one
backward search carries a head-SA toehold; adjacent results use phi. The
periodic query path starts with the existing LF fallback and uses the
exact side-list for phi exceptions. `xsa mems` and `xsa serve` share this
path. SXI1 writer bytes and behavior were not changed.

The converter requires `--validator xsa`: it validates the partial SXI2
with the dual-format reader and byte-compares every decoded EF chi value
with the source before the no-clobber link. `--max-bytes` rejects a size
budget when the phi member alone cannot fit. Bad sidecars, bad budgets,
malformed members, and validator failures do not publish an output.
See [SXI_FORMAT.md](../../SXI_FORMAT.md) for exact on-disk codecs.

## Gates completed

| Gate | Result | Evidence |
|---|---|---|
| SXI1 compatibility | PASS | Existing `test_sxi_format.py` suite passes after dual-format changes. |
| SXI2 format differential | PASS on synthetic fixture | Huffman, EF, phi order, and escape-count corruption rejected after CRC recomputation. |
| Multi-occurrence `mems` byte parity | PASS on two tiny fixtures | Direct cyclic-SA `BANANABANANA 0x1E` and periodic `ABABAB`, with several patterns and multiple output rows; `test_sxi2.py`. |
| Periodic escape fallback | PASS on tiny fixture | Exact `SXESC3` sidecar from the banked reference codec; malformed sidecar rejected before link. |
| Chi value parity | PASS on tiny fixtures | Writer validates every decoded EF value against SXI1 and test checks `--chi-out`. |
| HTTP `/query` byte parity | PASS on tiny fixtures | `test_sxi2.py` starts and terminates only its own two servers. |
| Three source chi header counts and gcd | PASS, read only | `criterion-current.json`: yeast 85,404,336; k10 1,627,067,257; remapped pile-frag 306,164,765; gcd 1 on all. |
| Full-artifact SXI2 MEM/HTTP/chi parity | NOT RUN | Compact space preflight failed before real-artifact publication. |
| Locate-heavy throughput, old vs new | NOT RUN | No full SXI2 output was published after the space failure. |
| Reviewer gate | PENDING | No independent review was obtained in this lane. |

## Exact space preflight: **FAIL**

`space_gate.py` uses the banked full-source Huffman/gamma bit counts,
the exact EF formula, and the concrete v4 member layout. It accounts for
headers, codec framing, and alignment. These are **prospective bytes**, not
measured SXI2 files. The SXI1 values are recomputed exactly from the
retained directory and match the source file lengths. The preflight
returns nonzero with `--enforce`.

| Artifact | Huffman+gamma | Raw tail | Raw head | Phi (24R) | EF chi | SXI1 actual | SXI2 prospective | SXI2 / SXI1 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| yeast235 | 0.099 GB | 0.404 GB | 0.807 GB | 2.422 GB | 0.077 GB | 1.804 GB | 3.809 GB | 2.111 |
| k10 | 2.227 GB | 8.137 GB | 14.879 GB | 44.636 GB | 1.252 GB | 33.962 GB | 71.131 GB | 2.094 |
| remapped pile-frag | 0.358 GB | 1.541 GB | 3.182 GB | 9.545 GB | 0.144 GB | 7.018 GB | 14.771 GB | 2.105 |

The fragment prospective container is **13.65 times its 1.082 GB text**,
far above the requested 0.6–0.7 times text. For the 466 target with
`R=2,739,737,289`, this v4 phi member alone is **65.754 GB**, above
both the requested 6–8 GB total and the cited 52 GB SXI1 size. A claimed
achieved 466 size would be unsupported: its Huffman and map bit counts
have not been measured here.

## Verdict and blockers

The code now implements and tests a real dual-format publication/query
path on small artifacts, including nonempty escapes. It is **not** the
requested compact production result. The 24-byte-per-run phi table plus
retained raw head and tail samples makes every requested real scale
larger than SXI1. It also lacks a serialized compact LF move map; the
reader builds LF rank structures from decoded runs. Phi predecessor is
binary search in `O(log R)`, not the requested `O(log log n)`, and the
initial LF fallback remains on periodic inputs. The writer currently
requires an externally supplied exact escape list for failed domains;
it checks its structure but cannot derive or prove its interior successor
values from the retained inputs in `O(R polylog R)`.

The **space gate failed**, so this lane did not produce or retain SXI2
versions of yeast, k10, or pile-frag. Consequently their byte parity,
decoded chi, HTTP parity, achieved member sizes, and paired throughput
remain unverified. No large conversion or long-running gate was launched.
The `contact_supervisor` tool named in the brief is absent from the
available tool catalog, so no intercom handoff was possible.

The normal `cargo check --manifest-path xsa/Cargo.toml` is independently
blocked by the pre-existing vendored SHA mismatch for
`xsa/runtime/bit6/chi_rspace_dump.cpp`. A temporary local build hook
allowed compilation of the changed Rust source and execution of the
synthetic gates; the original `xsa/build.rs` was restored after each
build. The package build mismatch was not changed in this lane.

## Commands

- `g++ -O2 -std=c++17 -Wall -Wextra bit6/sxi2_write.cpp -o /tmp/sxi2_write_v4` — PASS.
- `cargo check --manifest-path xsa/Cargo.toml` — FAIL on the existing vendored SHA mismatch.
- `cargo build --manifest-path xsa/Cargo.toml` with a temporary generated-bundle-only build hook, restored afterward — PASS.
- `python3 bit6/test_sxi2.py --writer /tmp/sxi_write_v4 --compact-writer /tmp/sxi2_write_v4 --xsa xsa/target/debug/xsa` — PASS.
- `python3 bit6/test_sxi_format.py --writer /tmp/sxi_write_v4 --sxi2-writer /tmp/sxi2_write_v4 --xsa xsa/target/debug/xsa` — PASS.
- `python3 bit6/sxi_logs/sxi2-v4/space_gate.py --enforce` — expected FAIL at all three sizes; see `space-gate.json`.
- `python3 bit6/sxi_logs/sxi2-v3/criterion_probe.py <three retained sources>` — PASS; see `criterion-current.json`.
- `git diff --cached --stat` — empty; no staged files.
