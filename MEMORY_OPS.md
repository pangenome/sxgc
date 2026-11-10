# Memory operations over a suffix tree — what we are building

This page exists because the project has drifted into naming structures instead of naming
*operations*. It states the goal, the smallest set of operations, what each needs, what is missing
today, and the gates. If a sentence here cannot be checked against a measurement, it does not belong.

## The goal, in one paragraph

A corpus index that a controller can *interrogate* while it writes or remembers: given the text I am
working on, which documents are the strongest match; what can come next in this exact context; how
much support is there; restrict all of that to a set of documents. Not retrieval bolted in front of a
model, and not an embedding approximation — the answers come from exact shared text. The unit is a
**context node**: a distinct string, its occurrences, the documents containing it, and the bytes that
follow it.

## Why a tree, and why not a fixed window

A 1024-byte window is a property of a *batch cache*, not of the data. Common text supports very long
contexts; novel text dies after a few bytes. The tree expresses that directly: descend while the
branch stays shared, ascend when support is thin. Depth becomes a measured property of each context
rather than a constant nobody can justify.

## The operations (from RESEARCH_MEMORY_OPS.md, cut to what is load-bearing)

| operation | question it answers | needs |
|---|---|---|
| `seek(s)` | where does this string occur, in which documents | suffix array interval + document map |
| `extend(node, b)` | what contexts follow this one | child edges (the tree) |
| `relax(node, min_support)` | ascend until the context is supported enough | tree parents + node mass |
| `project(node, docset)` | the same answer restricted to these documents | document map + occurrence/dc lists |
| `select(candidates, weight)` | rank by rarity-weighted match mass | document frequency per node |
| `matches(doc)` | the document's maximal exact matches against the corpus | tree + document map |
| `read(node)` | the stored text/continuation summary for a node | tree + corpus text |

Build order: `extend`, `relax`, `project`. `seek` exists today. `select` exists in embryo as the
rarity-weighted fanout; `matches` is the cats→domestication experiment; `read` is free once contexts
are addressable.

## What each operation needs from the container

| member | status | notes |
|---|---|---|
| suffix array as intervals (m1 BWT runs, m12/m13 rank directories) | **have** | rank arithmetic works; `xsa query --count` is exact on 10^11 bytes |
| sampled SA values (m11, 30.6 M) | **have** | enough to locate, not enough for tree topology |
| chi witness positions (m5, 24.19 G) | **have** | these are positions, NOT context nodes — a study corrected this |
| document map (offsets → document → component) | **in flight** | deterministic from the shards; needed by `project`/`select` |
| **LCP over runs** | **in flight — the gate** | without it there is no tree: children are delimited by where LCP drops |
| child edges (explicit, for wide nodes only) | **not started** | children are contiguous in SA order, so narrow nodes can be enumerated by scanning LCP |
| stateless query core | **have** | read-only mmap, per-worker arenas, no shared cache; scaling measured (58 % at 96 threads) |

## What the data says about depth (CONTEXT_DEPTH.md, 1,000 sampled positions, 100 GB Pile)

The single most design-relevant measurement so far:

```
support at depth K        p50     p90     p99
K=2  (one repeat)         18 B    38 B   2082 B
K=10                      16 B    28 B    314 B
K=100                     12 B    22 B     96 B

at exactly 1024 bytes, positions with >=2 occurrences elsewhere: 1.4 %  (7.5 % counting the source)

repeating contexts:  depth 1  H=3.89 bits  top1=0.23   |   depth 8   H=1.29  top1=0.72
                     depth 16 H=0.50       top1=0.93   |   depth 32  H=0     top1=1.00
```

Three consequences, none of them optional:

1. The useful context length is ~8-16 bytes, not 1024. A 1024-byte window is a hundred times deeper
   than any signal in the data, which is why it is a cache artefact rather than a design choice.
2. Past ~32 bytes a repeating context is deterministic, i.e. it is boilerplate or duplicated text, and
   in the rare case where a long context does occur twice, answering from it returns the source
   document's own continuation - leakage, not information. Source-document exclusion must happen
   before any maximality or support test.
3. Cost is linear in depth and the information is not: `xsa branches` costs 472 us at depth 1 and
   51 ms at depth 1024. Going shallow is therefore both correct and ~60x cheaper, which is what makes
   the 150k positions/s target plausible at all.

Design consequence for the operations: `relax` thresholds belong around support 10-100 at depth
10-16; `extend` rarely needs to go past ~32 bytes; and document-level matching (`matches`, `project`,
`select`) carries the semantics, not context depth.

## What we measured, and what it says

- Build: 100 GB Pile in 12 h 35 m, container 0.632 n, scratch 0.789 n unbounded; with a scratch cap a
  10 GB build completes inside 6 GB and pauses/resumes correctly.
- Query correctness: exact against brute force on 10^11 bytes (counts, positions, a 192-offset
  discrepancy adjudicated in the index's favour — `grep -o` undercounts overlapping matches).
- Query speed today: 2,304 positions/s at 96 threads for a full next-byte histogram; 4,957/s for the
  bounded batch path. The consumer wants 150,000/s. The gap is per-position work (256 symbol lookups
  over decoded runs), not thread count, which is what the tree is for.
- Prior art: Infini-gram (2024) already does continuation queries over 10^12 tokens from suffix
  arrays; suffix-tree document clustering (1998) and context-tree weighting (1995) cover much of the
  theory. **Our claim is not novelty of continuations** — it is the operation set, the exact-match
  memory shaping, and the cost at this scale.

## Gates — no victory without all of these

1. `LCP_RUNS` member present at 150 MB, 10 GB and the 100 GB Pile head, with **50/50 sampled values
   verified against brute force from the corpus**. A wrong LCP builds a tree that answers wrongly
   while looking right.
2. Child enumeration at run granularity equals a brute-force next-byte histogram on ≥50 real
   contexts, at 10 GB and 100 GB.
3. Document map exact: document count equals the 0x1E count in the same byte range, and ≥100 sampled
   documents byte-identical to their shard records.
4. `extend`, `relax`, `project` implemented over the stateless core, with a descent measured per
   position; report positions/s/core and the node-depth distribution actually observed.
5. 10 GB and 100 GB indexes both rebuilt with the layer present, byte-identical answers for existing
   queries, and `origin/main` equal to local.

- **`0x1E` is NOT unique in the Pile.** Scanning `pile.txt` for the separator overcounts documents:
  in the first 100 GB it finds 16,126,745 boundaries against 16,119,766 records, i.e. **6,979 internal
  `0x1E` bytes inside document texts** (0.043 %). Document offsets must come from the shard record
  boundaries, and any check on a byte-range scan must use the identity
  `separators == records + internal_0x1e`, never a raw equality. The convention "0x1E is conventionally
  absent" holds for enwiki, not for arbitrary web text.
## Known traps (each one has already cost time)

- Two chi values are only comparable **on the same corpus**. Comparing across corpora produced a
  false "regression" and nearly deleted a load-bearing pass.
- χ is witness positions, `xsa mem` extends on the *query's* byte, and m7 is a byte permutation —
  not context nodes, not index-side extension, not LCP.
- A generic "quiet for N minutes" watchdog kills healthy builds; the census is silent for ~35 min at
  the pile. Health checks must measure journal freshness AND CPU AND scratch deltas (`HEARTBEAT.md`).
- Container members cannot be appended in the old layout; streaming is a format change (SXI2 v7) and
  the legacy layout stays readable.
