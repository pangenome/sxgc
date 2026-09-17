# sa-agc — Suffixient-Array Indexing of AGC Genome Collections

Integration workspace for building **suffixient-array (sA) indexes** directly on
**AGC-compressed genome collections** (via `ragc`), with all query results mapped
into the **sample/contig name space**.

## Status

| Milestone | Status |
|---|---|
| yeast235 (235 strains, 3.34 Gbp) end-to-end | ✅ **validated** — χ=85.4 M, 975/975 banded indexes, 1000/1000 verified locate hits |
| AGC random-access oracle (`oracle-agc`) | 📋 spec'd (Phase 1) |
| HPRC v2 smoke (3/10 samples) | 📋 Phase 2 |
| HPRC v2 full (466 samples, 1.4 Tbp) | 📋 Phase 3, **disk-gated** |

## Quickstart (validated on yeast235)

```bash
# 1. AGC -> flat text + name-space sidecar
agc2flat/target/release/agc2flat yeast235.agc -o yeast/yeast235.txt

# 2. Shard by contig (each shard < 2^31 bytes for the lz77 oracle path)
tools/shard_by_contig.py yeast235.txt yeast235.txt.names.tsv yeast/shards/

# 3. Banded index builds (48-way on 256 cores; 975 shards in 236 s)
cd /path/to/suffixient-array/build
find shards -name "*.txt" ! -name "*.samples.txt" | sort | \
  xargs -P 48 -I{} sh -c 'python3 suffixient-array-index.py --build-index "{}"'

# 4. Queries + name-space mapping
./sA-index-src/locate -i shards/chrIV.txt -t suffixient-array -o lz77 -p patterns.fa
python3 tools/mappos.py shards/chrIV.txt.names.tsv --occs patterns.fa.occs
```

## Components

| Path | What | Language |
|---|---|---|
| `agc2flat/` | AGC → flat text + `names.tsv` (name-space sidecar); forbidden-byte validation; `--upper` | Rust (`ragc-core`) |
| `tools/shard_by_contig.py` | Per-contig sharding with shard-relative offset sidecars | Python |
| `tools/mappos.py` | flat-offset → `sample#contig:offset` mapper (occs/mems aware) | Python |
| `oracle-agc/` | **AGC random-access oracle** for the sA toolchain (Phase 1) | C++ ↔ ragc-core FFI |
| `ARCHITECTURE.md` | **Design document** — measured constants, scaling model, phase plan, decision log | — |

## Why this exists

The suffixient-array toolchain (`regindex/suffixient-array`) has no AGC integration,
no name-space mapping, and three scaling blockers we hit and characterized on real
data: the 2³¹ lz77-oracle limit, the O(n)-RAM in-RAM index step, and the
single-threaded χ scan. This workspace integrates the glue that works around the
first two today, and specs the AGC-backed oracle that eliminates the 2³¹ problem
entirely while cutting the queryable footprint ~10×.

External dependencies: `ragc` (github.com/ekg/ragc), `suffixient-array`
(github.com/regindex/suffixient-array). See `ARCHITECTURE.md` for the full
picture, measured constants, and phase gates.
