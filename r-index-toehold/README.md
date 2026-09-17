# r-index-toehold — Phase 5 spec: MEM anchors → all occurrences, GAF out

**Goal**: enumerate **all** text occurrences of every MEM found by the sA layer.
The sA returns, per MEM, `(pattern_start, mem_length, one_anchor)` — the anchor
is fed to an r-index as its **toehold**, skipping pattern matching entirely.

## Pipeline

```
read ──▶ sA (banded, χ-bounded) ──▶ all MEMs + 1 anchor each
                                        │  anchor = SA position (toehold)
                                        ▼
              r-index (banded, same shards) ──▶ toehold lemma: enumerate all
                                        │       occurrences via LF-steps,
                                        ▼       O(occ·log n/log w) per MEM
              tools/mappos.py ──▶ every occurrence -> sample#contig:offset
                                        ▼
              GAF seed records (compose with impg/giraffe workflows)
```

## Why it's fast

- The sA layer pays the search cost (binary search + oracle LCP walks).
- The r-index toehold lemma: an SA value inside a run + LF-mapping enumerates
  the full occurrence set **without any pattern matching** — the expensive part
  is already done. Startup is O(1)-ish per MEM; enumeration is output-sensitive.
- Both indexes are built over the **same shard texts** with the **same
  `.names.tsv` sidecars** — no coordinate translation layer needed.

## r-index source options

| Option | Notes |
|---|---|
| grlBWT + `ri` pipeline | the WABI 2025 (Eskandar/Paten/Sirén) route; proven on HPRC v2 whole-genome (19 h unidirectional); RLE BWT output feeds the r-index directly |
| Prezza-style r-index (`nicolaprezza/r-index`) | classic toehold locate; minor patch to accept an **external** toehold (skip its own pattern-matching phase) |
| move-r | modernized, parallel construction; Rust-adjacent |

Requirement: locate-all given an external SA toehold (all candidates support
this with a small patch — the toehold lemma is the same in each).

## GAF output

Seed records per read, one line per occurrence:

```
<read>  <len>  0  <mem_len>  +  <sample>#<contig>  <contig_len>  <offset>  <offset+mem_len>  ...  cg:Z:*
```

(seed/PAF-like GAF: path = PanSN name from the AGC sidecar, span = MEM length.)
Composes with impg/giraffe-style seed consumers. Reads with no MEMs emitted as
unmapped records.

## Footprint (HPRC v2 scale)

| | |
|---|---|
| sA banded (already building) | ~10–14% of shard text |
| r-index banded | scales with BWT runs (HPRC v2: 2.53 B runs whole-genome → ~5–8 GB class banded) |
| AGC archive (random access, Phase 1) | 3.3 GB, unchanged |

## Acceptance

- yeast235: every MEM anchor expanded; occurrence sets byte-verified against
  brute-force pattern search on the shard texts.
- HPRC v2 3-sample smoke: all-occurrence counts per MEM reported; GAF emitted;
  spot-verified via mappos + AGC extraction.
- Capability contract updated: sA = "all MEMs + one anchor"; toehold layer =
  "all occurrences per MEM".

## Status

- [x] Spec (this file)
- [ ] r-index banded build over smoke shards (source selection: start with
      grlBWT+`ri` on the fork's shard texts)
- [ ] external-toehold patch (skip r-index's own matching)
- [ ] GAF emitter + mappos all-occurrences mode
- [ ] yeast235 acceptance run
