# The 100 GB pile-head rehearsal: RESULTS

One command, the full stack, the exact dress rehearsal for the pile run.
Corpus: `/mnt/nvme2n1/erikg/pile-head100.txt` (100,000,000,000 B, sha256 prefix
`de79fe96…`). Map: `/mnt/nvme2n1/erikg/pilemap100` (16,119,767 docs, exact-corpus).
Container: `/mnt/nvme2n1/erikg/PILE100-REHEARSAL/pile-head100.sxi2`.

```
xsa build --tape --input pile-head100.txt --scratch PILE100-REHEARSAL \
          --threads 96 --memory-gb 96 --scratch-budget-gb 100 \
          --trie-depth 8 --trie-min-support 1000 --docs-map pilemap100
```

## Build

| metric | value |
|---|---|
| χ | **24,185,339,204** — identical to the banked 100 GB run: determinism at 100 GB with the full new stack |
| wall | ~12 h (102 waves in the first run; the rehearsal completed with the trie tap and m15 embedding riding along) |
| peak scratch | 118 GB against the 100 GB budget (the documented one-class borrow headroom), **zero backpressure pauses** |
| container | 65,533,578,480 B = **0.655 n** (0.632 n base + trie + m15) |
| trie (m20) | 21,707,038 nodes · 551,286,102 edges · 2,333,923,624 B = **0.0233 n**, boundary_aware=1, built in-build from the band tap |
| docstarts (m15) | present, 16,119,767 docs, v10, rank/select verified against the map and an independent C oracle |

## Certificates (all green)

- counts from mmap, 75 ms each: `"the "` → **704,942,163** ✓ banked; `"Harper Reed"` → **55** ✓ banked
- search, law on, `--corpus` byte-verify: 55/55 matches verified, zero crossings kept, zero refused
- wide patterns honestly REFUSED past `--max-occ 65536` — the refusal line itself carried
  `occurrences=4,785,152` for `" function "`, equal to the banked count: the refusal is a certificate
- `xsa trie-info` / `xsa docstarts info` confirm both members, boundary policy `aware=1`

## Findings (what the rehearsal was for)

1. **The map must match the corpus for embedding.** The full 1.31 TB map was REFUSED in
   40 ms for the 100 GB head (its document 16,119,767 starts past the head's end).
   Superset maps remain consult-side conveniences only; `--docs-map` embeds require the
   exact-corpus map. Failure mode: fast, loud, clean teardown.
2. **The old consult layer is locate-bound regardless of RAM.** Measured at three budgets
   (8 GB → 8.5 pos/s; 64 GB → grind; 96 GB → 13.7 pos/s average, RSS plateau ~31 GB of
   the 65 GB container, wedged twice on single width-8192 items). Locating one row walks
   a stride-1024 LF chain over the whole container's blocks; caching half of it does not
   halve the tail. RAM is not the knob.
3. **The banked 682,496 rows are the layer-mix census at 100 GB** (the design input for
   the χ-proportional consult rewrite):

   | layer | rows | share | new-design cost |
   |---|---:|---:|---|
   | width = 1 (forced, answer in the window) | 601,293 | **88.1 %** | 0 locates |
   | single branch, width ≤ 16 | 68,435 | 10.0 % | 0 locates (one-step width verification) |
   | single branch, width > 16 | 4,150 | 0.6 % | 0 locates (verification) or marked |
   | wide histogram (marked fallback) | 4,995 | 0.7 % | as today (marked, journaled) |
   | genuine forks ≤ 8 branches | 3,614 | 0.5 % | a few locates each |

   With the one-step verification (`width(C·b) == width(C)` ⇒ forced to b — exact,
   never wrong, at worst spuriously conservative into a tiny enumeration) ~99.5 % of
   rows need zero locates. Consult's cost becomes proportional to χ, the project thesis,
   measured rather than predicted.

## Pile-readiness verdict

The exact command shape is certified at 100 GB. The pile run is the same command with
`pile.txt` (N = 1,307,910,802,540), `pilemap_full` (matches pile.txt by construction),
and a 200–300 GB scratch budget. Free disc at launch: 8.2 TB. Expected wall: ~7 days.
