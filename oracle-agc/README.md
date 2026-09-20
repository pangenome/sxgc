# oracle-agc — Phase 1 specification + skeleton

> **Status: PRODUCTION — this is the sA query oracle.** The `-o agc` oracle
> (`agc_text_oracle` + `ragc-ffi`, 64 KiB window LRU) is the production text
> path for the sA layer; queries are byte-verified against it (Bit 5:
> 500/500; yeast whole-collection: 50/50). The Phase-1 checklist below is
> the original spec's plan, retained unedited.

**Goal**: replace the sA toolchain's lz77 random-access text oracle with reads
served directly from the AGC archive, eliminating the 2³¹ text limit and the
12%-of-text lz77 files.

## Interface

The toolchain selects text oracles via `-o lz77|rlz|bitpacked`; each implements a
random-access read interface over the indexed text. We add `-o agc`:

```
AgcRandomAccess:
  open(archive_path, names_tsv)     // archive + name-space sidecar
  read(T, pos, k) -> bytes          // k bases at flat-text offset pos
  close()
```

`pos` is a flat-text offset (same coordinate system as `.occs`/`.mems` and
`names.tsv`).

## Implementation plan

1. **ragc-core as cdylib**: expose partial extraction (segment granularity) over
   a C ABI. The FFI surface partially exists (`ffi/segment_helpers.rs`,
   `test_get_part.cpp`, `test_cpp_ffi.rs`). Key requirement: substring reads
   *within* a contig — decompress only the segments overlapping `[pos, pos+k)`.
   (Do NOT use `get_sample` — that decompresses whole 3 Gbp contigs per read.)
2. **C++ oracle class** (`AgcRandomAccess`) implementing the toolchain oracle
   interface; linked into `locate`/`mems` alongside the existing variants.
   Flat offset → (contig, contig-offset) via binary search on `names.tsv`
   (same logic as `tools/mappos.py`; load once at open, ~50 MB for HPRC v2).
3. **LRU segment cache** (configurable, default ~256 MB): MEMs/locate access is
   per-read-window and cache-friendly; decompression cost amortizes. Track hit
   rate in the query log for tuning.
4. **Construction-free**: zero build cost — the archive *is* the oracle.

## Acceptance (Phase 1 gate)

- yeast235 banded queries via `-o agc`: 1000/1000 byte-verified vs flat text.
- HPRC v2 3-sample smoke: MEMs + mapped positions with **zero lz77 files**.
- Cache hit-rate ≥ 90% on a realistic read workload (10k reads, 1 Mbp window each).

## Status

- [x] Spec (this file)
- [ ] ragc-core cdylib C-ABI partial-reads
- [ ] C++ `AgcRandomAccess` + `-o agc` wiring in `build_store_sA_index`/`locate`/`mems`
- [ ] LRU cache
- [ ] yeast235 acceptance run
