# The context trie: what it cost, and the architecture it forces

Status: **negative result at the parameters first tried**, with the correction that matters. Measurements
from the m20 (`CONTEXT_TRIE`) builder on the 150 MB fixture, 2026-11-10.

## What we measured

```
CONTEXT_TRIE depth=16 min_support=10 nodes=15,986,374 edges=70,052,969
             bytes=1,904,270,232  ratio_vs_n=13.49  (119 B/node, 0.107 nodes per corpus byte)
```

Scaled linearly that is 1.35 TB at 100 GB and ~17 TB at the Pile. Unusable.

## The mistaken premise, stated plainly

The design argued: information is in the first ~16-32 bytes (`CONTEXT_DEPTH.md`), therefore only the top of
the tree must be materialised, therefore the structure is small. **The conclusion does not follow.** The top
of the tree is small in DEPTH and enormous in NODE COUNT: a 150 MB corpus has 150 M suffixes, and 16 M of
them begin a context that repeats at least ten times. Node count is governed by *support*, not by depth.

For reference, the depth measurement that motivated the attempt is sound and stands:

```
support at depth K      p50    p90     p99
K=2                      18 B   38 B   2082 B
K=100                    12 B   22 B     96 B
at exactly 1024 bytes:   1.4 % of positions have any repeat elsewhere
entropy of next byte:    3.89 bits at depth 1, 1.29 at 8, 0.50 at 16, 0 at 32
```

## The architecture this forces: high-support trie + `seek` fallback

```
trie       contexts occurring >= min_support times (min_support ~ 10^3-10^4), depth 8-16
           answers extend/branch-of for COMMON text, which is where the information is
seek       everything rarer: backward search on the container, exact today, microseconds at these
           depths (472 us at depth 1 through 7 ms at depth 16, measured)
relax      ascends by re-descending shorter prefixes; cheap at depth <= 32
project    seek + document-map lookup on the located positions (seps.bin/meta.bin), not a per-node
           document list
```

The budget that makes this checkable rather than assumed: `ratio_vs_n <= 0.05`, i.e. the trie may spend at
most 5 % of n. That is 5.2 GB at 100 GB and 65 GB at the Pile - affordable, and small enough to live beside
a 0.63 n container.

## What is still open

- The D x min_support curve at 150 MB and 10 GB, to pick the smallest parameterisation that keeps the
  operations useful. Until that exists, the ratio target is a hope, not a result.
- Record size: 119 B/node is fat. The 64-register HLL plus top-1 document id is likely the largest single
  term, and `project` no longer needs it if it answers via `seek` + the document map.
- No 10 GB or 100 GB trie has been built yet. No operation (`branch-of`, `relax`, `project`) has been timed
  against the 150k positions/s target.

## The locate cost, measured (and why the "narrow" backend is not cheap)

```
10 GB v8 container, 16-byte context "version of the s", one CLI process per measurement
  --count          0.075 s                     (no location)
  --limit 100      5.05 s    ~50 ms / position
  --limit 1000    41.82 s    ~42 ms / position
  --limit 5000   153.15 s    ~39 ms / position (3926 found)
```

A locate walks LF from a sampled anchor at stride 1024, i.e. up to 1024 decode-touching steps, and each
step lands on a different BWT block, so it is a cache miss per step. **Per-position location is ~40 ms**,
which is ~6000x the 6.7 us/position budget at 150k positions/s.

Consequences, none optional:
- "the narrow case is cheap because there are few rows" is false: it is few rows at ~40 ms each.
- Sampling k rows to estimate a histogram per position is also dead (k x 40 ms).
- The two-backend rule reduces to ONE viable backend: a stored structure with no locate. Either the trie
  (with TOP-K branches stored, since a 256-bin histogram is not needed and costs bytes) or a reversed
  companion index (rank differences, no location, but O(sigma) work for a full histogram).
- Any future claim about consult rates must state the locate cost explicitly; the earlier 2.3k positions/s
  and 4.9k positions/s figures were dominated by depth and by location, not by the index.

## Why this counts as progress

Each of the three studies this week ended by killing a plausible design cheaply: witnesses are not context
nodes, query-side extension is not index-side, m7 is a permutation and not LCP, and now the shallow tree is
not a small tree. The cost of each was hours of measurement; the cost of acting on any of them would have
been weeks.
