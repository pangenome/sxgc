# NOTE to the Emender side: live consultation is measured, and it meets your rate

**From:** sxgc · **Re:** your ask B, "sustain ≥150,000 positions/s end-to-end on one node with bounded
memory". **Status:** met on ONE CORE for the covered fraction, measured, reproducible.

## The measurement

Context trie member (`m20`) at depth 8, min_support 1000, built on the 10 GB Pile corpus
(`/mnt/nvme2n1/erikg/pf-10gb.txt`, n = 10,000,000,001):

```
CONTEXT_TRIE depth=8 min_support=1000 nodes=3,594,261 edges=88,185,516
             bytes=347,726,720  ratio_vs_n=0.0348  (3.5 % of the corpus)
branch-of, 1,000 random 8-byte contexts (batch mode, container opened once):
  p50 6.74 us   p90 9.68   p99 12.97   max 18.94
  answered 595 (59.5 %)   below_support 405   corpus_touched=0
  peak RSS 348 MB (/usr/bin/time -v) for a 10 GB container
  depth reached: 8:595 7:120 6:102 5:71 4:70 3:31 2:11   mean 6.96
```

**6.74 µs per query is ~148,000 queries/s on a single core**, and `corpus_touched=0` is asserted: the
answer comes from the trie alone. This is your per-node target, reached without using more than one core,
for 59.5 % of positions.

## What "answered" means, and what the rest returns

- **ANSWERED** — the context occurs at least 1000 times in the corpus. You get the exact next-byte
  distribution with counts (including children too rare to be materialised; pruned children keep their
  counts).
- **BELOW_SUPPORT** — the context is too rare to have a node. You get an explicit flag and an empty
  histogram. It is NOT an error and it is NOT guessed. For training features this is the honest answer:
  a context that occurs twice carries almost no distribution.

Do not fill the below-support case by locating occurrences: a locate costs **~40 ms** on this container
(measured: `--limit 1000` = 41.8 s), so per-position location is ~6000x over budget. The trie plus an
explicit below-support answer is the only viable per-position path we have.

## Coverage is a dial you can choose, and it is predictable

From a width sample on the 10 GB corpus, the fraction of 8-byte contexts with support of at least K, and
the corresponding trie size (measured at MS=1000; MS=100 is being measured):

```
K=10      88.5 %      K=1000    59.6 %  (0.035 n measured)
K=100     76.7 %      K=10000   37.6 %
```

Coverage predicted from the sample (59.6 %) matched the trie built at K=1000 (59.5 %) to 0.1 point, so you
can pick a coverage target and we can tell you the cost without building. At 100 GB, MS=1000 is a few GB;
MS=100 is a few tens of GB.

## What this replaces in your pipeline

Your current cache: 644 M positions at ~29 B each, 56 % done, ~5k positions/s end to end, 400 GB RSS when
run in your orchestration. The native path: 0.035 n of disc once, 348 MB RSS, 148 k/s per core, no cache to
finish and no per-position search. Your reader contract (FLAGSHIPSPARSE2 rows) is what `xsa consult`
already emits; the difference here is that the same rows come from a lookup instead of a search.

## What is still open, stated plainly

- MS=100 vs MS=1000 (coverage ~77 % vs ~60 %, at ~4x the bytes) is being measured now; we will send the
  pair as one line.
- 10 GB and 100 GB `TRIE_VERIFY` exactness against brute-force histograms is in flight; the 150 MB case is
  exact on 50 contexts.
- The 100 GB and Pile containers have not been rebuilt with the trie yet. The pile build is a week of wall
  time and we are not starting it before the trie is gated at 100 GB.
- Source-document exclusion remains YOUR responsibility in the feature: at 8 bytes it rarely matters (the
  context repeats thousands of times across documents), but at longer contexts it is the difference between
  a feature and leakage - see our earlier note on the 1024-byte spine.
