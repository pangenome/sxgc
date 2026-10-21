# ENTROPY RULE — radix while the next byte holds more than half a bit

Branch `entropy-rule`, base `pile-week-v1 @ 88a7cb0`. Status: DESIGN, written
before any code. The measurements this document predicts are in
`tools/binbwt/RESULTS_ENTROPY_RULE.md`; where the built thing differs from what
is written here, the results document says so and why.

The operator's rule, verbatim:

> A radix level on bucket b costs ~b staged window reads (1/row) and saves
> ~b*H comparisons x 2 reads, where H = the entropy of the next-byte histogram
> (bits). Take the level iff H > threshold, where threshold =
> cost_per_level_row / (2*reads_per_comparison) = ~0.5 bits, CALIBRATED from the
> measured constants. The counting pass and the entropy evaluation are the SAME
> operation — the level's histogram tells you whether to take the level;
> degenerate buckets (H below threshold) exit to the existing LCP-carrying
> comparison sort immediately.

---

## 0. The one paragraph

`bin_sort_rec` decides nothing about cost. Its rule is "split until the node
holds <= target rows", i.e. a fixed row bound stands in for a cost comparison
that the tree can compute for free: the level's own counting pass IS the
next-byte histogram, and the entropy of that histogram IS the number of
comparison-bits the level buys. So the stopping rule is not a tuning knob, it is
arithmetic: **take the level iff H > θ**, where H is the Shannon entropy of the
next-byte histogram in bits and θ = (cost of one level row) / (cost of one
comparison row) is a number the census already contains. The rule changes WHEN a
bucket stops splitting — never WHAT the tree produces, because a radix level and
an LCP-carrying comparison sort are both exact total orders. That is what makes
it safe to arm, and what makes the oracle's byte-identity gate the right gate.

---

## 1. THE COST MODEL, with the measured constants

Two numbers decide everything. Both are already measured on this tree, and
neither is a guess:

| symbol | meaning | value | source |
|---|---|---|---|
| `w` | cost of one 64-byte corpus window read (one random text read, memory-latency bound) | **57 ns** | `FP_LCE_DESIGN.md` §0: `leaf_s 1296.1 CPU-s / 22.63e9 windows` at 1 GB |
| `r_c` | window reads per leaf COMPARISON (staged leaf) | **1.0681** | `FP_LCE_DESIGN.md` §1 (`BIN_CMP_HIST windows_per_cmp`) |
| `c_cmp` | cost of one comparison row | **67.1 ns** (1 GB), **82.2 ns** (10 GB) | `leaf_s / comparisons` |
| `r_l` | window reads per LEVEL row (count pass + scatter pass) | **2** | derived below |
| `c_lvl` | cost of one level row | **112.1 ns** (1 GB), **96.8 ns** (10 GB) | `(build_cpu_s - leaf_s) / level_elems` |

`c_cmp = leaf_s / comparisons` and `c_lvl = (build_cpu_s - leaf_s) / level_elems`
are the tree's own counters, not a microbenchmark; the two 1 GB/10 GB arms are:

```
1 GB  (ar-1b-ladder.log, n=1,000,000,665, 96t, target=AUTO)
  level_elems 2548479275  comparisons 21555811697  leaf_s 1445.822
  build_cpu_s 1731.634 -> c_cmp 67.07 ns  c_lvl 112.15 ns
10 GB (ar-10b-default.log, n=10,000,000,000, 96t, target=AUTO, dcap AUTO)
  level_elems 41021242629  comparisons 214876750387  leaf_s 17657.506
  build_cpu_s 21626.977 -> c_cmp 82.18 ns  c_lvl  96.77 ns
```

**Why `r_l = 2` and not 1.** A level is two passes over the node: the counting
pass (`count[bin_code(...)]++`) and the scatter pass (`dst[fill[bin_code(...)]++]`).
Both code the SAME byte at the SAME address per row. They would be one read if
the node's byte set fit cache; a node stays in the tree only while it holds more
than `target` (1M by default) rows, i.e. its touched cache lines are ~1M x 64 B =
64 MB of distinct lines — L3 sized at best, so the second pass is not free at the
scale the rule decides at. The arithmetic above confirms it independently:
`(build_cpu_s - leaf_s) / level_elems = 112 ns = 1.96 w` at 1 GB, and the same
ratio at 10 GB is `1.7 w`. The two-pass reading of the level is what the machine
does.

**And `r_c = 1.0681`, not 2.** The operator's "saves ... comparisons x 2 reads"
is the RAW shape of a comparison (two 64-byte windows, one per side). The census
then measured how many windows are actually issued per comparison on the shipped
staged leaf: 1.0681, because (a) the leaf stage answers one or both sides from a
bulk pread the leaf already paid for, and (b) 99.33% of comparisons diverge
inside the first window. So the comparison is CHEAPER than 2 reads, which pushes
the threshold UP relative to the operator's analytic form.

---

## 2. THE DERIVATION OF THE THRESHOLD

A node of `m` rows that would otherwise be comparison-sorted costs `m log2(m)`
comparisons; taking one radix level partitions it into children of sizes `c_i`
and costs `m` level rows, after which the residue costs `sum_i c_i log2(c_i)`.
The saving is exactly the partition's information

```
m log2(m) - sum_i c_i log2(c_i) = m * H,        H = -sum_i (c_i/m) log2(c_i/m)
```

so, in ns per row:

```
take the level  <=>  m * H * c_cmp  >  m * c_lvl        <=>   H > c_lvl / c_cmp
```

**θ = c_lvl / c_cmp.** The `2` in the operator's `2*reads_per_comparison` is the
raw comparison shape and the `1` in "1/row" is the level's counting pass alone;
in the same unit of 64-byte window reads the ratio is `r_l / r_c`:

| arm | `c_lvl` | `c_cmp` | **θ (bits)** |
|---|---|---|---|
| operator's analytic form (`r_l=1`, `r_c=2`) | 1 w | 2 w | **0.50** |
| census, measured (`r_l=2`, `r_c=1.0681`), 1 GB | 112.1 ns | 67.1 ns | **1.672** |
| census, measured, 10 GB | 96.8 ns | 82.2 ns | **1.178** |

The measured pair brackets **θ ∈ [1.18, 1.67]**; both ends are over-estimates of
`c_lvl` (the non-leaf CPU also contains the sweep, the allocator and the wait),
so the truth is at the low end of that bracket. The shipped default is
**θ = 1.5 bits**, and `--bin-entropy-min` takes any value in bits, so the
calibration curve is one flag away (§7). The analytic 0.5 remains the number for
the regime where the level's scatter pass IS free — a node small enough to be
cache-resident, which is exactly the leaf (PART 2 below), not the tree.

**Sanity check of the crossover in the same numbers.** H = 1.5 bits is a split
that concentrates ~74% of the mass in one bucket (or 4 equal buckets is H = 2,
which pays; 8 equal buckets is H = 3, which pays well). A node whose next byte is
dominated is exactly the node a comparison sort should take: the comparison
resolves the first differing byte in ONE window (census F1), while the radix
spends two windows per row to learn the same byte.

### 2.1 THE SEPARABILITY ARGUMENT (per-bucket decisions compose)

The decision is taken per node, independently, and the total cost is the sum of
the per-node costs (each node's level passes touch only its own rows; the leaves
are disjoint). So the greedy per-node rule is optimal for a fixed tree shape: no
global choice exists that a per-node argmin does not find, because the children's
sub-decisions are independent of the parent's (the parent's choice only sets the
`depth` certificate the children start from, worth at most one window per
comparison and identical in both arms). A node's decision CANNOT change another
node's decision, so the rule needs no global pass, no ordering, and no state: it
is a few lines on top of the histogram the level already computed.

### 2.2 THE H COST ITSELF (the one thing the rule adds to the histogram)

H is computed from `count[]`, which the level already has, in ONE pass over the
`codes` bins — at most 257 adds and 257 `log2` evaluations per LEVEL, i.e. a few
hundred flops against a pass that already touched `m` random bytes. The rule's
own cost is `O(codes)` per node and **zero per row**; the assertion the results
must carry is `H evaluations == level_passes` and that the H term does not appear
in the wall. (`log2` is evaluated as `log(x) * 1/ln2` on the bin probabilities;
`codes <= 257` for the product's 1-byte levels.)

---

## 3. WHERE IT PLUGS IN (the tree)

`bit6/binbwt_slice.h` `bin_sort_rec` (and its dispatched twin `bin_task_run`: the
same node loop, one copy per thread) already does, in order: count, scatter,
no-progress detection, periodic probe, recurse per child. The rule is inserted
exactly at the split decision, after the histogram and after the periodic probe:

```
count[]   <- the level's counting pass         (already there, unchanged)
H         <- -sum_c p_c log2 p_c  over c >= 1   (NEW: a few hundred flops)
if H <= theta   ->  the node is COMPARISON-BOUND:
                    keep the scatter (it is already paid for, and it certifies
                    one byte of depth for the children), then hand EVERY child
                    with count > 1 to the LCP-carrying comparison sort at the
                    certified depth, and do not recurse
else            ->  recurse as today
```

The one-shot shape is deliberate: with `H <= θ` the mass is concentrated in one
child, and re-deriving H for that child one byte later costs a full level to
learn the same thing. Taking the scatter and then finishing each child by
comparison is strictly better than exiting the parent before the scatter, since
it certifies `depth + level_bytes` for each child for free (the counts are in
hand) and it keeps the arena's child ordering, which the emit pass requires to be
canonical.

**The backstop does not move.** `m <= targetSize` is still a leaf; `depth >= n`
is still a structural leaf; the periodic probe and the no-progress sanity bound
are untouched (§4). What changes is only the branch "m > targetSize and the split
progressed" — it used to be an unconditional recurse and is now `H > θ`.
With the flag absent (`--bin-entropy-min` unset) `theta == 0` and the comparison
is never taken, so the product path executes the same instructions it does today
and every existing oracle case, counter and byte stays identical.

---

## 4. THE H = 0 CASE IS THE EXISTING PERIODIC TIE-BREAK — the rule GENERALIZES it

`biggest == m` is the special case `H = 0`: the split put every row in one
bucket, so the level coded one symbol and bought ZERO bits, and H = 0 <= θ always.
The tree already answers that case exactly, and it answers it better than the
comparison sort: `bin_node_period` proves the node's rotations identical and
`bin_periodic_sort` orders them by ascending source position with ZERO
comparisons. So the rule ORDER is:

1. `biggest == m` -> the periodic probe (unchanged cadence, and when the rule is
   armed the probe also runs unconditionally, because the rule is about to take
   the node and one `O(m)` gcd pass per no-progress node is cheaper than the
   comparisons it replaces);
2. periodic proven -> `bin_periodic_sort`, zero comparisons, counters unchanged
   (`periodic_leaves` / `periodic_rows` / `periodic_period_max`);
3. periodic NOT proven and the rule armed -> the node is comparison-bound: it
   exits to the LCP-carrying comparison sort and is journalled
   (`entropy_exits` / `entropy_exit_rows` / `entropy_no_progress_exits`). This is
   the byte-0x20-monster regime that "ground 9 h at 100 GB" under the size-only
   rule: a 879-byte-deep chain of no-progress levels now costs ONE level plus one
   leaf sort instead of 879 levels;
4. periodic not proven and the rule OFF -> today's path, unchanged: keep
   subdividing, refuse loud at `BIN_NO_PROGRESS_SANITY_BOUND`.

So the rule does not replace the periodic tie-break, it CONTAINS it: the
zero-entropy case is the one point of the spectrum the exact probe owns, and the
rule's exit is what a no-progress node takes when exactness by periodicity does
not apply.

---

## 5. THE IN-LEAF RULE (PART 2 — the same rule inside the leaf)

A leaf is `m <= targetSize` rows, and the leaf stage has already put its text in
RAM (`bin_stage_leaf`: one bulk pread of the densest span, `stage_hits`). Inside
that cache-resident leaf the SAME arithmetic applies with DIFFERENT constants:
a level row is now two staged reads (a buffer hit, not a 57 ns window), so
`c_lvl` collapses by roughly one to two orders of magnitude and θ falls with it.
The in-leaf rule is therefore:

```
bin_leaf_order(src, dst, m, depth):        (armed)
   if m <= 1: return src
   count[] <- the in-RAM counting pass over the leaf's position array
   H       <- entropy of that histogram
   if biggest == m   -> comparison sort (the H=0 case; exact by tie-break)
   if H <= theta_leaf -> comparison sort at `depth`  (the residue)
   else scatter into dst, recurse per child (and merge back)
```

with `theta_leaf = theta * (c_lvl_staged / c_lvl_cold)`, both constants measured
(§7 journals `stage_s / stage_bytes` so the staged read cost is a measured number,
not a belief). With the flag off, `bin_leaf_order` is exactly today's leaf: stage,
then `std::sort` with `bin_tail_cmp` (the code is factored so that the OFF arm is
the same bytes, in the same order, with the same counters).

Correctness is the tree's correctness: a radix level is exact, the residue is
ordered by the same total-order comparator the leaf already used, and the
`biggest == m` bail keeps a cyclic corpus of identical rotations from grinding to
depth `n` inside a leaf (it ends at the comparator's own ascending-position
tie-break, exactly as the periodic tie-break does at tree scale).

---

## 6. THE POLICY KNOBS

| flag | default | meaning |
|---|---|---|
| `--bin-entropy-min BITS` | **absent = OFF** | arms the rule; the level is taken iff `H > BITS` |
| `--bin-target-size ROWS` | 0 = auto | when the rule is armed this becomes the **BACKSTOP**: the largest serial stretch a leaf may cost, no longer the driver of the shape |
| `--bin-depth-cap` | 0 = auto | unchanged, oracle only |
| `BIN_ENTROPY_LEAF_MIN` (env) | derived | the in-leaf threshold, for the calibration sweep only |
| `BIN_ENTROPY_H=1` (env) | off | prints the H distribution journal (min/mean/max per depth) |

With the flag absent nothing changes: `theta = 0` disables both the tree arm and
the in-leaf arm and no counter moves.

---

## 7. THE COUNTERS (the rule's own instrument, all exact)

`BinWork` (journalled on `BIN_WORK`, per bin on the hot-bin anatomy):

```
entropy_evals          histograms the tree rule looked at
entropy_taken          levels it kept (H > theta): levels the tree was RIGHT to take
entropy_exits          buckets handed to the comparison sort (non-degenerate exits)
entropy_exit_rows      their rows
entropy_noprog_exits   exits taken on a no-progress (H = 0, non-periodic) node
entropy_leaf_evals     in-leaf levels the rule looked at
entropy_leaf_taken     in-leaf levels taken
entropy_leaf_exits     in-leaf residues handed to the comparator
entropy_leaf_rows      their rows
```

plus `BIN_ENTROPY_H`, printed only when armed: per depth `min / mean / max` of H
and the count of evaluations, so the H distribution the rule actually saw is a
measurement and not an assumption. The check the results must carry:
`entropy_evals >= level_passes` is FALSE by construction (evals happen only on
nodes the rule is armed for, i.e. above the target) but **`entropy_evals +
leaf_reaches == level_passes + ...`** is not needed either — the invariant that
matters is that `entropy_taken + entropy_exits + entropy_noprog_exits ==
entropy_evals` and that every exit either becomes a leaf (`leaf_sorts` rises by
exactly `entropy_exits`) or is absorbed by an enclosing leaf. The results
document carries the identity it actually satisfies.

---

## 8. THE RISKS, honestly

1. **θ is load- and cache-sensitive.** `c_cmp` and `c_lvl` both move with the
   machine: at 10 GB the level row is CHEAPER (96.8 ns vs 112.1) and the
   comparison is dearer (82.2 vs 67.1), so θ drops from 1.67 to 1.18. A threshold
   calibrated at one scale is a heuristic at another, and the honest answer is the
   flag plus the sweep (§7), not a claim of a universal constant.
2. **The level's scatter pass may be partly free** for nodes that fit cache, which
   is why the in-leaf threshold is a separate, much lower number and why the
   tree's θ is an over-estimate at small `m`. The tree only sees `m > target`,
   which bounds the error but does not remove it.
3. **H's own cost**: a few hundred flops per node. Asserted to be invisible in the
   wall by the A/B (the same run with the rule armed at `--bin-entropy-min 0`
   takes every level it would have taken and must be within noise of today).
4. **The rule cannot make the product wrong**, only slower: every stopping
   decision hands the residue to an exact comparator. The gate that proves this is
   byte identity at fixture and bank scale (§ gates), which is the strongest gate
   the tree has.
5. **The monster-class thesis is falsifiable and may fail.** If degenerate buckets
   exit before burning levels, `comparisons` and `level_passes` DROP for them and
   the producer wall drops with them. If they do not (e.g. because the deepest
   nodes are already bounded by `target` before H collapses), the counters will
   say so and the report will say so.

---

## 9. What lands where

| item | file |
|---|---|
| the rule, tree + leaf, the counters, the H journal | `bit6/binbwt_slice.h` |
| `--bin-entropy-min`, `BIN_ENTROPY_LEAF_MIN`, `BIN_ENTROPY_H` | `bit6/cross_lcp_merge.cpp`, `bit6/binbwt_slice.h` |
| the entropy oracle family (byte identity, adversarial exits) | `tools/binbwt/oracle.cpp` |
| the A/B driver (100 MB / 1 GB / 10 GB, rule ON vs OFF) | `tools/binbwt/entropy_ab.sh` |
| the measurements | `tools/binbwt/RESULTS_ENTROPY_RULE.md` |
