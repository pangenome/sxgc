# SAMPLED SA (option B): is the sampled suffix array the sledgehammer, and what
# would the added constant have to be?

**Status**: Phase 0 (survey + design). Written before any code, on branch
`sampled-sa`, base `pile-week-v1 @ aa1a95c`.

**The operator's question**: *"Maybe we just need to SAMPLE the suffix array.
Wouldn't that be the sledgehammer with an added constant? What would the constant
have to be to make it worth it? Don't we have an r-index anyway?"*

**Short answer, stated up front so the rest can be read as its proof:**

1. The identity is real and it is free: the producer's leaf sorts hold the global
   SA in RAM, in order, at emit time (`bit6/binbwt_slice.h:1808` `emitBin`, and
   `bit6/binbwt_slice.h:2475` `BinBandSink::publishBin`), so emitting every k-th
   `(rank, SA value)` is a sequential side-stream over an array that is already in
   cache order. 8 B/entry, `--sa-sample-k`.
2. **But it buys the finish nothing, at any k.** The finish's 473,695,414 LCE
   queries (1 GB fixture) are *not* SA-order lookups. Both query families are
   `LCP(a,b)` between two suffixes whose **ranks are already known exactly**
   (they are BWT run endpoints, and the pieces carry the run interval). An SA
   sample at density 1/k gives you `SA[j*k]`; it gives you *no* information about
   `LCP(SA[i],SA[j])`. `d(residual)/dk = 0` for every k, so the cost curve has no
   interior optimum: the sampled SA is dominated by "off" for this consumer.
3. **We do have an r-index, and its sample is already in the container.** The
   merge's v2 pieces carry `{char u8, len u32, head SA packed w, tail SA packed w}`
   per run (`bit6/cross_lcp_merge.cpp:3512-3527`, `V2Piece`
   `bit6/cross_lcp_merge.cpp:3599`), i.e. **two SA samples per run**, mean rank
   spacing 1.36 rows at 1 GB. That is a *denser* anchor set than any k >= 2 the
   charter proposes. Sampling the SA again at k in {4..1024} would be strictly
   coarser than what the container already ships.
4. **The constant that does buy it is the LCP array, not the SA.** With ranks
   known, `LCP(SA[i],SA[j]) = min LCP over (i,j]` — an RMQ over the LCP array.
   Family F1 (adjacent ranks) is one LCP array entry. Cost: ~1 B/row
   (`n` bytes; 1.31 TB at pile) versus `8n/k` for the SA sample which returns
   exactly zero. And the amortisation is the whole story: the finish pays
   `12,820,424,563` verified symbols **per 1 GB corpus byte** for these LCPs;
   Kasai builds the same array in `O(n)` total symbol comparisons (~2n), i.e. a
   12.8x reduction *and* a 473.7M-query table lookup.

The rest of this document derives each of those four statements from the code and
from the banked counters of the 1 GB gate, then prices the options in the same
units (bytes on disc, symbols walked, wall).

---

## 1. THE IDENTITY: the leaf sorts already hold the global SA in order

The producer never materialises a persistent SA (charter invariant, and the
`BIN_WORK` journal still prints `full_SA=0 persistent_positions=0`,
`bit6/binbwt_slice.h:2761`). But at *emit* time the global SA **is** in RAM, in
global order, in one contiguous slice per byte-1 bin:

* `bin_order` keeps the running global rank in `U row = 0;`
  (`bit6/binbwt_slice.h:1616`) and hands `(row, position)` to the sink row by row.
* `emitBin` (`bit6/binbwt_slice.h:1808`) walks the bin's finished slice
  `arena.data() + off[c]`, and each word already carries the position:
  `bin_pos_of(bin_word_tail(slice[i]), n, dollar)`.
* The product arm publishes whole bins instead
  (`BinBandSink::publishBin`, `bit6/binbwt_slice.h:2475`), and it computes
  `posOf(i)` for rows it needs anyway; the slice `words[]` it is handed is the
  same ordered array.
* The order in which `emitBin` is called is ascending byte-1 bin and then
  ascending slot, i.e. **ascending global rank, on one thread** — the sink's
  parallel band writers come later and do not touch the slice.

So "emit every k-th entry" is `for (i = i0; i < m; i += k) append(baseRow+i,
pos_of(i))` over an array that the emit pass is about to stream anyway: a
sequential extra read of `m/k` words per bin, one buffered `write()`. It costs
`8n/k` bytes of scratch and *zero* extra comparisons, LCEs or text reads. That is
what "the sledgehammer with an added constant" gets right: the *emission* really
is nearly free.

What it does not get right is the *use* (next section).

## 2. THE MECHANISM: what the finish's expensive walks actually are

The finish's cost centre is `LceWrap` (`bit6/pile_finish.cpp:3075`), a cyclic
wrapper around `SlimFingerprint::lce` (`bit6/slim_lce.hpp:313`): a fibre of
tau-spaced polynomial suffix hashes (`hash_at`, `suffix`, `equal`) plus a direct
symbol verify of the proposed answer. It is exact — every returned length is
re-verified symbol by symbol (`bit6/slim_lce.hpp:336-345`).

It is called from **exactly one place**, the per-band verification job
(`bit6/pile_finish.cpp:3484` `lceJob`):

```cpp
s.topLcp[i]  = gx[i] ? slimLce(prev, first) : 0;      // pile_finish.cpp:3505
s.interior[i]= v.len == 1 ? INF : slimLce(first, tail); // pile_finish.cpp:3506
```

with `first = v.h` (head SA value of run i), `tail = v.t` (tail SA value), and
`prev = nb[i-1].t` (tail SA value of the *previous run*).

Those are the two families, and their ranks are known:

* **F1 `topLcp`**: `prev` is the last row of run i-1 and `first` is the first row
  of run i. Runs are contiguous BWT intervals, so these are **adjacent SA ranks**
  `r-1` and `r`. `topLcp[i]` is *literally LCP array entry r*.
* **F2 `interior`**: `first`/`tail` are the first and last rows of the **same**
  run, i.e. ranks `r` and `r+len-1`, and
  `LCE(SA[r],SA[r+len-1]) = min LCP over (r, r+len-1]`. At 1 GB the mean run
  length is `n/R = 1,000,000,665 / 368,036,610 = 2.72`, so this interval holds
  1.7 LCP entries on average, and it is skipped entirely for `len == 1`.

Measured at 1 GB (`tools/binbwt/RESULTS_FINISH_DECOMP.md:147`, banked):
`queries=473,695,414  verified_symbols=12,820,424,563` → `1.287` queries per run
over `368,036,610` runs, i.e. F1 for every run plus F2 for the 29% of runs with
`len >= 2`. Mean verified prefix **27.1 symbols**, `max_verified` 97k-class.

### 2.1 Why an SA sample cannot shorten either family

Let the anchor set be `A = {(j*k, SA[j*k])}`. A query is a pair of **ranks**
`(i, j)` and wants `LCP(SA[i], SA[j]) = min_{i<t<=j} LCP_array[t]`.

* If `j - i <= k` there is no anchor strictly between them: the sample is silent.
* If an anchor `a` does sit between them, then
  `LCP(SA[i],SA[j]) <= min(LCP(SA[i],SA[a]), LCP(SA[a],SA[j]))` — an *upper*
  bound, and obtaining either side costs a full LCE walk. The bound is worthless
  as a walk-shortener (it cannot certify a lower bound, which is what lets you
  skip symbols).
* Concretely: the sample tells you `SA[a]`; the *answer* is a property of the
  LCP column. `d(verified_symbols)/dk = 0`, so `cost(k) = base + 8n/k` is
  monotone decreasing in k and the optimum is `k = infinity`, i.e. the knob
  should be off. There is no constant that makes it worth it for *this* consumer.
* The "snap to the nearest anchor and walk the residual" mechanism in the charter
  is the right mechanism **for a different query**: for a *placed* pattern
  (a suffix whose rank you must discover), or for `SELECT(rank) -> SA value`
  (which the r-index does with an LF walk). It is the mechanism of a *locate*
  engine, not of an *LCP* engine. The finish is not a locate engine — it is
  handed the SA order by the merge.

This is also why the producer's own anchoring trick does not transfer: inside
`bin_sort_rec` (`bit6/binbwt_slice.h:1146`) the "snap" works because a node
carries a **certified depth** — every row in it is known to share `depth`
symbols, so the leaf sort starts at `depth` and re-walks nothing. A rank-space
anchor carries no certified depth. **Depth is the currency; rank is not.**

## 3. THE R-INDEX AUDIT: what we already have

The ancestral machinery is the HPRC-era r-index
(`/home/erikg/sxgc/sA/suff-set-src/rindex_build.cpp`, and
`r-index-toehold/README.md`). Its sample is:

```cpp
// rindex_build.cpp:73-79 - the run loop
if (c != b) { run_char.push_back(b); run_len.push_back(cnt);
              sa_sample.push_back(prev_sa);   // SA value at each run's LAST row
              b = c; cnt = 1; }
```

| property | r-index (ancestral) | uniform-k SA sample (this option) |
|---|---|---|
| what is stored | `run_char u8`, `run_len u32`, `sa_sample` packed to `bits(n)` (`rindex_build.cpp:100-110`) | `SA[j*k]`, 8 B/entry |
| entries | `R` (one per BWT run) | `n/k` |
| at the 1 GB fixture | `R = 368,036,610` = **0.368 n** | `n/k`; k=4 → 0.25n, k=2 → 0.5n |
| recovery of `SA[rank]` | O(run distance) via LF/toehold, or a within-run walk for the run-local case | O(k) walk inside an anchor interval |
| what it accelerates | *locate*: toehold lemma → all occurrences per MEM, **no pattern matching** | nothing beyond `SELECT` |

Three facts decide this option:

1. **The r-index's sample is already in the pile container, twice over.** The v2
   pieces carry `head SA` and `tail SA` per run, packed to `w = bits(n+9)`
   (`bit6/cross_lcp_merge.cpp:3514`). At the 1 GB fixture that is 736,073,220
   sampled SA values, one every 1.36 ranks on average. **Every uniform k in the
   charter's sweep {4, 16, 64, 256, 1024} is sparser than that.** Producing a
   second, coarser copy cannot add information.
2. **The container is exactly the "sampled at run boundaries" form.** The r-index
   audit therefore answers itself: we are not missing the SA sample; we are
   missing the LCP column (and the r-index does not have one either — its
   `LF/toehold` reconstruction is for occurrences, not LCPs).
3. **Census-adaptive sampling does not rescue it either.** The charter's
   "dense anchors where the depth census lives" would place dense anchors where
   LCE answers are deep. But deep answers cost deep walks in *both* families and
   the anchors still carry no depth. Adaptive sampling changes `8n/k` to
   `8n/k_eff` and the residual stays exactly the full LCE. The adaptive form's
   benefit is real for *locate* workloads (deep repeats = many occurrences per
   anchor); it is zero here.

## 4. THE CONSTANT: pricing the three structures honestly

Units: `n` = corpus bytes, `R` = BWT runs, at the 1 GB fixture
`n = 1,000,000,665`, `R = 368,036,610` (r/n = 0.368), finish query census
`Q = 473,695,414`, verified symbols `V = 12,820,424,563` (12.8 per corpus byte).
Pile scale: `n_pile = 1.31e12` (1.31 TB), `w = 41` bits; `R` projected at the
measured 1 GB rate is 4.82e11, labelled a projection.

### 4.1 Option A — uniform-k SA anchors (what was chartered)

Disc: `8n/k` B (or `w/8 * n/k` packed). Benefit: **0 verified symbols saved**.

| k | 1 GB fixture, disc | pile (n=1.31e12), disc | verified symbols saved |
|---|---|---|---|
| 4 | 2000 MB | 2624 GB | 0 |
| 16 | 500 MB | 656 GB | 0 |
| 64 | 125 MB | 164 GB | 0 |
| 256 | 31 MB | 41 GB | 0 |
| 1024 | 7.8 MB | 10.2 GB | 0 |

The row that matters is the last column: it is zero for every k, so the table's
optimum is "off". No added constant makes it worth it. **That is the honest
answer to the operator's question for the SA sample as such.**

### 4.2 Option B — the run-boundary LCP column (what the finish actually needs)

Per run, one `topLcp` value (F1, 77% of queries) and one intra-run minimum (F2,
29% of queries, only for `len >= 2`). With `1 B` for `lcp < 255` plus an escape
list for the deep tail (max verified 97k-class, measured; the escape list is
`o(n)` on any real corpus), the column is `~2 B/run = 0.74n` B:

| | 1 GB fixture | pile (r/n = 0.368) |
|---|---|---|
| LCP column bytes | 736 MB | ~0.97 TB |
| finish verified symbols | 12.82e9 → **0** | 1.68e13 → 0 |
| finish LCE wall (measured 154.8 s of a 737 s end-to-end) | → ~0 | → ~0 |

And where does the column come from? Not from 473M LCE queries moved to the
producer (that would only relocate the 12.8n symbol cost). It comes from the
amortisation the finish does not have: **Kasai's construction computes the whole
LCP array in `O(n)` symbol comparisons total** (the invariant `lcp[rank[p+1]] >=
lcp[rank[p]] - 1` pays for the re-walk), i.e. ~2n reads instead of 12.8n. The
producer can run it over each window's finished bin slices because it already has
the positions in SA order; what it needs is the *inverse* permutation `rank[p]`
for the window, which is the only real cost (`n*w` bits if held for the whole
corpus; per window it is `window_cap * w` bits — at 1 GB one window, 3.75 GB;
at pile, one window of the derived cap).

Ratio, both ways:

```
SA sample (option A) : adds 8n/k bytes,  removes 0            -> dominated by off
LCP column (option B): adds ~0.74n bytes, removes 12.8n symbol reads
                       (plus removing 473.7M query dispatches)
```

`0.74n` bytes once, against `12.8n` symbol reads *per finish run*. The constant
that makes sampling worth it is therefore **not a k — it is a different column**.
Option A is a sledgehammer aimed one bin to the left.

### 4.3 What the SA sample *is* worth (do not throw the survey away)

Two consumers would take it, and neither is in the finish:

* `SELECT(rank)` for the *r-index locate* path
  (`r-index-toehold/README.md`, "external SA toehold"): the r-index needs the SA
  value at a run boundary to start the toehold lemma, and the pile container
  already supplies it. No new structure needed.
* Any future *search* consumer (pattern → interval) needs SA values at interval
  boundaries, again already present in m1.

So the correct engineering output of this survey is: **keep the emission hook
(it is nearly free and it is the only way to get `SA[rank]` out of the producer
without a persistent SA), but do not build a placement path on it.**

## 5. What this branch implements and measures

Scope is set by the audit, not by the charter's phase order:

* **Phase 1 (landed)**: the producer's anchor side-stream, `--sa-sample-k`
  (default `0` = OFF; the product path is byte-identical when off) and
  `--sa-sample-out`. Sequential over the ordered slice; journalled
  (`sa_sample_k`, `sa_samples`, `sa_sample_bytes`).
* **Phase 3 (landed)**: a new brute-force oracle for the anchor stream — the
  anchors must be *exactly* `SA[j*k]` of the true brute SA, at adversarial k
  (1, 2, 3, 7, 64, n) on adversarial texts (all-equal, periodic,
  separators+repeats, random), including `k = 1` which must reproduce the whole
  SA (the sledgehammer) and `k >= n` which must emit exactly one anchor.
* **Phase 4 (landed, measured)**: the k-sweep at the 100 MB and 1 GB fixtures:
  anchor disc cost, emission cost delta (producer counter + wall), the
  recovered-`SA[rank]` walk curve, and the byte-identity of the product with the
  knob on and off.
* **NOT implemented, priced instead**: the LCP column (§4.2). It is a
  producer-side `O(n)` pass plus one new piece column and a finish-side swap of
  `slimLce(...)` for a table read. That is a charter of its own; this document
  is its cost model, and the measurement here (12.8n symbol reads for values the
  producer could compute in 2n) is the case for commissioning it.

## 6. Residual risks of the audit itself

* The `R`-rate at pile is a projection from the 1 GB fixture (r/n = 0.368). Deep
  repeats make `R` smaller, not larger, so the LCP column's cost is an
  over-estimate on a repetitive pile - the direction that helps.
* The `2 B/run` LCP column assumes a `u8` fast path + escape list. The 1 GB
  `max_verified` is 97k-class; if the pile's census has a heavier deep tail the
  escape list grows, but it grows with the *distinct* deep values, not with n.
* The finish's F2 family is reduced to an intra-run minimum only if the run
  interval is contiguous in rank space - true by construction (a run *is* a BWT
  interval), and asserted by the piece format.
* Anything measured on this box is measured next to the live 100 GB rung
  (`/mnt/nvme2n1/erikg/ladder100`, 96 threads). Every wall in this branch's
  results is labelled with the load it saw.
