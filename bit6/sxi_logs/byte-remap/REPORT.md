# Byte-remap acceptance evidence

All requested functional gates passed. Independent reviewer approval remains pending. No commits or staged files. The intercom contact_supervisor tool was unavailable in this session.

## Measurement and design
[ALPHABET.md](ALPHABET.md) records all four measurements; [alphabet.jsonl](alphabet.jsonl) contains all 256-bin histograms. [DESIGN.md](DESIGN.md) explains the online permutation, proof of stable observed mappings, container change, and escape tradeoffs.

Fragment: 1,082,130,213 bytes, 177,753 separators, 231 distinct values; forbidden frequencies `[0,130,29,3,67,0]`. Three 256 MiB pile slices contain 219, 219 and 230 values; the union of measured alphabets has 237 values. No full-pile alphabet claim is made.

Fragment mapping: `01 -> ff`, `02 -> fd`, `03 -> fe`, `04 -> fc` (paired swaps with unobserved source values). 0x1E is fixed. The normalized index preserves all 256 expected byte frequencies; see [pile-alphabet-conservation.json](pile-alphabet-conservation.json).

## Checked gates
- Packaged offline release build and isolated tools/build_pfp_agc.sh build: PASS (build.log, fork-build.log).
- Mapper property test: 1,000 shuffled maximal alphabets, frozen observed mappings, identity and exhaustion: PASS (bit6/test_byte_remap.cpp).
- Small fresh binary pipeline: PASS; MEMs equal an independent brute-force oracle, C++ SXI aggregates and Rust chi match RI4, 250-code capacity succeeds, all 256 bytes fail without publishing, identity metadata is omitted, and HTTP agrees (small-gates.log).
- Format checks: PASS for optional names with/without remap, identity byte equality, invalid permutations/separator/flags/sizes, plus existing packed widths 2..64 and corruption cases (cpp-format.log). Remap cases are checked by both C++ and Rust readers.
- Chunk boundary: PASS; >2 MiB identity parse/dictionary equal the old parser; a displaced high byte immediately after a 1 MiB seam maps correctly (chunk-gate.log, chunk-commands.json).
- Pile-frag build -> SXI: PASS with a verified tool manifest and no --allow-drift. n=1,082,130,213; runs=397,723,010; chi=306,164,765; SXI bytes=7,017,802,200.
- Pile audit: TEXT_SAMPLE_PASS requested=128 verified=128 chi=306164765 compared_bytes=1965.
- Pile native MEMs: precision 179/179; slice recall 123/123; planted 7/7 across seven 256 KiB windows. Queries include original bytes 01, 02 and 03. All reported hits were byte- and maximality-checked against the original fragment (pile-query-gate.log, pile-query-commands.json, pile-patterns.json, pile-slices.json).
- Yeast235: parse/dictionaries/names/heads/RI4/aggregates/chi/final SXI are byte-identical to accepted outputs. Chi=85,404,336. Audit 32/32 passes (yeast-identity.json, yeast-identity.log).

The legacy seven-fixture sxi_gate.py harness fails at duplicates-600k.sxi.ms because its byte splitlines pattern generator creates an empty p1 query. The same command fails with the pre-change binary with exactly the same error. This pre-existing harness failure is retained in legacy-format.log and legacy-format-baseline.log; the new remap differential and format tests pass. No production code was changed to accommodate the obsolete harness.

## Build artifacts and costs

### pile-frag
- Output: `/home/erikg/worktrees/sxgc/pi-worktree-e7286239-b261-424c-8585-f2604e5d83e0-s0-0/vendor/byte-remap-gates/pile-frag.sxi`
- SHA256: `db35e331d1837dcbd47d985ef5a08d2ae820ef2f000dee18265501b01c452a90`
- Completed stage wall time total: 4247.507 seconds.
- Largest stage peak RSS: 28,257,076 KiB.

### yeast235
- Output: `/home/erikg/worktrees/sxgc/pi-worktree-e7286239-b261-424c-8585-f2604e5d83e0-s0-0/vendor/byte-remap-gates/yeast235.sxi`
- SHA256: `764f3dc7cb589223e0935d5bf0177c64f67835ab8857cb2408bdefbf776e5e11`
- Completed stage wall time total: 1562.200 seconds.
- Largest stage peak RSS: 8,821,556 KiB.

Full stage commands, timings, return codes and binary provenance are retained in the two *-xsa-build-*.jsonl journals. [gate-stages.json](gate-stages.json) is the compact machine-readable summary. All parsing and indexing outputs live under vendor/byte-remap-gates. No remapped corpus file or full-text transform was created; shared inputs were opened read-only.

## Review and residual limits
- SXI1 member 7/flag 0x10 costs 256 payload bytes, 40 directory bytes and at most 7 padding bytes. Identity adds zero container bytes. Old readers fail closed on the new flag.
- More than 249 distinct nonseparator bytes cannot fit. The parser fails explicitly; escapes are not implemented. All 256 original bytes require boundary-aware escapes or a wider parser alphabet, encoded-to-original coordinate mapping, and changes to MEM/MS/chi semantics. A 256-entry byte table alone cannot represent variable-width escapes.
- Only the fragment was built; the complete 1.31 TB pile was sampled, not indexed. The existing 32-bit run-count limit is unchanged. The fragment index itself is 7.02 GB, so full-pile resource feasibility is a separate gate.
- Existing query framing remains: JSON HTTP accepts UTF-8 strings and escaped controls; binary FASTA can carry other byte values subject to FASTA framing. Raw RI4 does not carry the remap; use the SXI container for original-byte queries.
- Local code review found no new blockers; required independent reviewer sign-off has not been performed by this agent.

## Diff and reproduction
[changes.patch](changes.patch) contains tracked edits and both new test sources; reverse applicability and git diff --check passed. [changed-files.json](changed-files.json) records the 23 source/doc/test paths and final SHA256 values. All 42 packaged source hashes match their manifest. [no-staged-files.json](no-staged-files.json) records the final root and fork checks.

Core reruns from the worktree:
```sh
BUILD_JOBS=4 cargo build --offline --locked --release --manifest-path xsa/Cargo.toml
c++ -std=c++17 -O2 -I vendor/pfp-agc-fork/include bit6/test_byte_remap.cpp -o /tmp/byte-remap-unit
/tmp/byte-remap-unit
python3 bit6/test_byte_remap.py
python3 bit6/sxi_logs/byte-remap/verify-yeast.py
python3 bit6/sxi_logs/byte-remap/verify-pile.py
```
Fresh builds refuse existing outputs; use a new output and scratch directory to rerun the full build commands recorded in the journals. Count-only measurements can be repeated with measure.cpp and the offsets/lengths recorded in alphabet.jsonl.
