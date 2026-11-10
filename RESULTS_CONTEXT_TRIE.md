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

## The monster key class is whitespace, and it is 0.76 % of the corpus

The 100 GB Pile build under a 30 GB scratch cap refuses in the sorter with:

```
SS_WAVE_POINT_MASS_PROOF depth=16 class_rows_sampled=763,350,000 tail_slice_rows=0 mismatches=96
SS_WAVE_POINT_MASS        class_rows=763,350,000 wave_goal=377,119,027 provable=0
```

Measured directly on `/mnt/nvme2n1/erikg/pile-head100.txt` (first 100 GB, exact counts):

```
'                '  16 spaces        762,642,500      <- the class
'----------------'                   206,413,738
'================'                    32,490,771
'****************'                    12,364,953
'0000000000000000'                     2,262,669
```

So the class is **sixteen consecutive spaces**: code indentation, padding and whitespace runs. Indentation
continues past sixteen bytes far more often than random text repeats, which is why the class has no
splitting byte at the depth cap, why it is not a single-symbol tail run (position slicing would corrupt the
BWT: `mismatches=96`), and why it must be sorted as one unit or in chunked runs.

At 90 B/row the class is 68.7 GB: too large for a wave derived from a 30 GB scratch budget, comfortably
inside a 96 GB memory budget. So the correct handling is to borrow memory for that single wave (journal
`SS_WAVE_BORROW`), with chunk+merge inside the class as the fallback when even --memory-gb cannot hold it.

Two consequences worth carrying elsewhere:
- any build under a scratch cap must state that a point-mass wave may exceed the cap by up to one class;
- the Pile's single largest suffix class is whitespace, so whitespace is the natural stress case for the
  sorter, not a synthetic one (we had been using 'ab'*N, which is a period-2 artefact no real corpus has).

## The operation layer, measured: 3.84 us per branch-of (independent run)

Context trie m20 at depth 8 / min_support 1000, 150 MB corpus, 999 random 8-byte contexts drawn from the
text (0x1E-separated batch mode, so container open is amortised and the measurement is the operation):

```
BATCH_BENCH op=branch-of n=999 p50_us=3.84 p90_us=7.09 p99_us=10.66 max_us=26.88
            answered=178 below_support=821 corpus_touched=0
BATCH_DEPTH reached_depth_hist=1:16 2:78 3:133 4:159 5:168 6:151 7:116 8:178 mean_depth=5.194
```

16-byte contexts: p50 3.86 us, answered 161/999, mean depth reached 5.00. Cost is flat in context length
because the descent stops where support ends - asking for 16 bytes costs the same as asking for 8 when the
trie only holds 5.

Reading:
- **3.84 us per query is ~260,000 queries/s single-threaded**, five times the 150k positions/s target before
  any parallelism, and `corpus_touched=0` is asserted by the tool.
- The descent histogram is the variable-order behaviour working: mean depth 5.19, most queries stopping
  between depth 3 and 7 rather than pretending a longer context is supported.
- **Coverage here is 17.8 %, and that is a corpus-size artefact**: a context needs >=1000 occurrences, which
  on a 150 MB corpus is rare. The same measurement on 10 GB gave 59.6 % of contexts at >=1000 occurrences,
  so coverage at 10 GB and 100 GB is the number that matters and must be measured there.
- Latency was previously unmeasurable through the CLI: per-invocation cost is ~50 ms of process start and
  container open, so the batch mode (journal `BATCH_BENCH`) is the only valid instrument for this target.

## Why this counts as progress

Each of the three studies this week ended by killing a plausible design cheaply: witnesses are not context
nodes, query-side extension is not index-side, m7 is a permutation and not LCP, and now the shallow tree is
not a small tree. The cost of each was hours of measurement; the cost of acting on any of them would have
been weeks.
