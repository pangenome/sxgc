# Chunk merge round 2 journal

The requested stable run interleave is blocked by an exact cyclic BWT
counterexample. The updated [round 1 report](../chunk-merge/REPORT.md)
contains the byte-level prefix proof. The gate here finds the same obstruction
in 7/20 deterministic separator-aligned synthetic pairs. No full merge
builder or merge-speed claim is made.

## Work completed

- `bit6/chunk_frontend.cpp` reads the source once, applies the 256-byte
  permutation in-stream, cuts at document boundaries, sorts each doubled
  chunk with banked libsais, resolves periodic rotation ties by source
  position, and writes versioned `SXCR` RLE runs with head/tail SA samples.
- `test_chunk_frontend.py` passes an independent cyclic-SA oracle on all
  254 binary-alphabet strings of length 1..7 (plus `1e`), 20 synthetic and
  two periodic collections, a nonidentity byte remap, output no-clobber,
  and a refused mid-document cut. The buffered and initial
  serializer produced byte-identical artifacts on all 16 fragment chunks.
- The buffered 16-chunk fragment run passed contiguous header and file-size
  checks: n=1,082,130,213, local runs summed=420,913,952, chunk lengths
  67,236,273..67,702,549 bytes. Per-chunk sort wall summed to 212.189 s,
  range 12.679..15.152 s. Total including one source read and output was
  3:57.26, peak RSS 2,309,920 KiB. The initial unbuffered serializer took
  31:29.40 with byte-identical output; it is retained separately.
- The fresh monolithic parser applied the exact reported swaps
  `01↔ff`, `02↔fd`, `03↔fe`, `04↔fc` and fixed `1e`. Raw padded
  `rpfbwt` yielded n=1,082,130,223 and r=397,723,016; the fresh endpoint
  adapter reported normalized n=1,082,130,213 and r=397,723,010. The new
  remap SHA256 `b4f387767707f3095a0981148a1753f8da5885c30c19753b3b93887153e611be`
  matches the retained byte-remap journal exactly.
- Reference Gate 0 **passed**: the new slim aggregate and sweep printed
  `chi = 306164765 (N=1082130213, R=397723010)`. The witness file has
  exactly 306,164,765 eight-byte entries (2,449,318,120 bytes). The six
  monolithic stage walls sum to 4,789.07 s at eight threads; this is a sum
  of stage measurements, not a separate end-to-end timer.
- The first sweep attempt hit a pre-existing source guard that rejected raw
  ri4 version 4 because it tested `version>=2` as if every input were an
  SXI container. `xsa/src/main.rs` now applies that guard only to actual
  containers. `sweep-prepatch-failure.log` records the diagnostic; the
  rebuilt sweep passed the exact chi gate.

## Timings to date

| Stage | Wall | Peak RSS KiB |
| --- | ---: | ---: |
| Monolithic parse | 0:43.23 | 2,401,824 |
| Monolithic parse L2 | 0:02.61 | 195,204 |
| Monolithic rpfbwt | 25:59.32 | 18,472,252 |
| Monolithic endpoints | 7:23.06 | 28,251,580 |
| Monolithic slim | 44:25.45 | 13,384,660 |
| Monolithic chi sweep | 1:15.40 | 6,144 |
| Buffered 16-chunk frontend | 3:57.26 | 2,309,920 |

There is no measured merge wall, complete chunk-merge wall, or 100x contrast.
The scale arithmetic is 27 chunks at a strict 50 GB maximum (26.2 nominal),
depth 5, versus 1,310 chunks at 1 GB, depth 11; no runtime projection follows.

## Fragment verdict

Reference regeneration: **PASS**. Sixteen local chunk builds and their
sampled RLE artifacts: **PASS**. Exact balanced pairwise merge, final
byte-identity to the regenerated reference, and chi from a merged structure:
**NOT RUN**. The specified stable run interleave cannot be exact for the
repository's cyclic text convention, as the fixed and synthetic gates show.
An independent reviewer has not approved a replacement merge operation.

## Reproduction

The exact stage commands and resource readings are in `monolithic-*.time`,
`chunk-sort-buffered.time`, and their adjacent logs. Tool hashes are in
`tool-hashes.txt`; `reference-hashes.txt` records the four regenerated
front-end files plus normalized ri4 and witnesses. `measurements.json`
aggregates the eight stage records and all 16 chunk rows. Focused commands:

```sh
python3 bit6/sxi_logs/chunk-merge-v2/test_interleave_obstruction.py
CHUNK_FRONTEND="$PWD/vendor/chunk-merge-v2/chunk_frontend_buffered" python3 bit6/sxi_logs/chunk-merge-v2/test_chunk_frontend.py
python3 bit6/sxi_logs/chunk-merge-v2/verify_chunks.py vendor/chunk-merge-v2/chunks-buffered 16 1082130213
```

All generated corpus artifacts are private under `vendor/chunk-merge-v2/`.
`changed-files.txt` and `no-staged-files.log` record the final worktree
inventory. No upstream file was changed. The independent reviewer gate
remains open.
