# tag-array — Phase 6 spec: sequence-space → graph-space projection

**Goal**: project seeds/matches from haplotype-sequence coordinates
(`sample#contig:offset`) into **pangenome-graph coordinates** (node, offset,
strand) — lossless, haplotype-aware, with cross-haplotype deduplication at
query time. Reference implementation: "Lossless Pangenome Indexing Using Tag
Arrays" (Eskandar, Paten, Sirén — WABI 2025, LIPIcs 344.8;
github.com/parsaeskandar/pangenome-index).

## The three-tier stack this completes

```
Tier 1  sA (banded, χ-bounded)          all MEMs + 1 anchor          ~10-14% of shard text
Tier 2  r-index toehold (banded)        ALL occurrences              ~BWT-runs (5-8 GB @ HPRC v2)
Tier 3  tag array (banded per-chr)      occurrences -> GRAPH coords  ~86 GiB @ HPRC v2 (measured)
        AGC archive (Phase 1)           random access to text        3.3 GB
```

Tier 3 answers: *where does this seed sit in the variation graph?* — returning
the set of **unique graph locations** (merging equivalent hits across
haplotypes — the dedup FM-indexes can't do natively), valid across all 464
embedded haplotype paths, enabling vg/giraffe-style graph downstream and
coordinate translation between haplotypes via graph paths.

## What it adds over Tiers 1-2

| Capability | Tiers 1-2 (sequence space) | + Tier 3 (graph space) |
|---|---|---|
| MEMs + anchors | ✅ `sample#contig:offset` | ✅ + (node, offset, strand) |
| All occurrences | ✅ via toehold | ✅ **deduplicated to unique graph locations** |
| Haplotype-aware mapping | per-haplotype sequence coords | graph coords valid on all embedded paths |
| Coordinate translation between haplotypes | ❌ | ✅ (graph paths + tag array = bidirectional mapping) |
| Graph-round-trip (vg/giraffe) | ❌ | ✅ GAF in graph coordinates |

## Measured costs (WABI 2025, HPRC v2.0 graph, 256-core/2 TiB nodes)

| | v1.1 | v2.0 |
|---|---|---|
| Haplotypes / sequence / sequences | 90 / 257 Gbp / 30,640 | 464 / 1,317 Gbp / 94,554 |
| BWT runs (whole-genome MSBWT) | 3.89 B | 2.53 B |
| Tag runs / final tag array | 9.86 B / **80 GiB** | 11.1 B / **86 GiB** |
| grlBWT per chromosome (16 t) | ≤35 min | ≤75 min |
| Per-chr tag arrays (incl. BWT) | 5.3 h | 16.1 h |
| Whole-genome r-index (merge input) | 5.3 h | 19 h |
| Merge step | 7 h | 30 h |
| FMD bidirectional variant | 75 h (33 h parallel) | 287 h (**133 h** parallel) |
| Peak memory | 199 GiB | ~500 GiB uni / 1.04 TiB FMD |

Sublinearity highlight: v2's tag-run count stayed flat vs v1.1 despite 5×
sequence — shared paths coalesce; average tag-run length up to 225 bp.
~90% of tags are derivable from unique k-mer anchoring + graph extension alone.

## Inputs sxgc does not yet carry

- The **graph**: `hprc-v2.0-mc-chm13.gbz` (minigraph-cactus; paths + .bed
  sidecars already local: pansn/traversal/covering_set). AGC sequences alone
  are insufficient — tags annotate graph nodes.
- Tools: `grlBWT` (pangenome), `gbz-extract`, `build-tags`/`merge-tags`
  (parsaeskandar/pangenome-index), `ri` for the whole-genome r-index.

## sxgc integration shape

1. **Banded per-chromosome orchestration** (our Makefile): per-chr jobs in
   parallel (their 133 h FMD wall -> our 256-core box runs the same jobs
   48-96-way), each job bounded by the shard contracts we already enforce.
2. **Sidecar extension**: `names.tsv` gains graph-node ranges per
   `sample#contig` (from the .gbz paths) so `mappos.py` emits **both**
   sequence and graph coordinates.
3. **GAF emitter**: seed records in graph coordinates for vg/giraffe consumers;
   sequence-space GAF (Tiers 1-2) unchanged.
4. Query: BWT interval (from r-index pattern match) -> two rank queries on the
   RLE tag array (Elias-Fano run starts) -> distinct tags = unique graph
   locations.

## Acceptance

- yeast: n/a (no graph) — acceptance is on HPRC v2 smoke subsets with the
  .gbz chroms.
- HPRC v2 3-sample: tags built over the smoke's chr graphs; seeds from Tier 2
  resolve to graph coords; spot-verified against vg paths.
- Scaling checkpoint: v2 tag runs vs our measured per-shard runs (validate the
  86 GiB figure on the full collection).

## Status

- [x] Spec (this file)
- [ ] .gbz ingestion + per-chr extraction for the 3-sample smoke
- [ ] grlBWT + build-tags banded jobs
- [ ] sidecar graph-range extension + dual-coordinate mappos
- [ ] GAF graph-coordinate emitter
