# m14 — the BWT runs' LCP column, persisted (SXI2 **v8**, member codec **117**)

Why it exists: the suffix tree *is* the LCP-interval tree. A node is a distinct
context, its children are contiguous in SA order, and a node's children are
delimited by the ranks where LCP drops below the node's depth. Until v8 the finish
**computed** that column (it is an input to the chi sweep) and **discarded** it, so
nothing in the container could be navigated as a tree. m14 persists it.

```
lcpRuns[i] = LCP[r_i] = lce(SA[r_i - 1], SA[r_i]),   r_i = sum(len[0..i-1])
```

One value per **run**, in run order, at the run's **head row** (the left-neighbour
LCP). `lcpRuns[0] = 0` is the array's conventional sentinel (row 0 has no left
neighbour). r-space is the point: `R` is ~3.2x smaller than `n` at the Pile
(31,381,597,694 vs 100,093,934,467) and it is the space m1/m12/m13 already use.

**Not** persisted: the LCP of the rows *inside* a run. A run's interior carries its
own drops (an all-equal text is ONE run whose interior is the whole tree), so this
member delimits children at **run granularity**. The finish computes that interior
minimum for chi; it is not part of m14.

## Format

```
member m14, codec 117:
  [0,40)   head: block_runs u64 | n u64 | payload_off u64 (=40) |
                  count u64 (0 = derived = the member count) |
                  enc_version u32 (=1) | codec u32 (=117)
  [40,..)  payload: one independently decodable block per `block_runs` (=1024) runs
  [..,end) directory: 16 B per block = payload off u64 | span u32 | escapes u32
```

The directory is **trailing** (the v7 rule: a directory the writer cannot know
before the bytes exist cannot be a prefix), so `nblocks = ceil(R/block_runs)` and
it starts at `member.bytes - 16*nblocks`. Nothing in m14 is patched after it is
written: `count = 0` means "derived from the member directory".

A block, `mode u8` then:

```
mode 0  every value is zero                     -> 1 byte (padded)
mode 1  u8 array, value < 255 inline, 255 = escape
mode 2  width u8 (1..=32), values packed little-endian at that width,
        the ALL-ONES field = escape
then    esc_count u32, then esc_count * { u16 index, u64 value }   (sorted by index)
then    zero padding to an 8-byte multiple
```

The writer tries both schemes from a width histogram and keeps the cheaper one
**per block**; the mode is inside the block, so the reader never needs the writer's
reasons. Escaped values are full u64, so nothing is truncated: a Pile LCP of 3 and
a fixture whose whole text is one run (LCP ≈ n) live in the same format.

**Random access is the layout's whole point:** one 16-byte directory entry
(`dir[i/1024]`) + one bit slice (or one byte) at a computed offset + a binary search
in that block's escape list. Never a scan over the block, never a walk over
preceding blocks. `xsa lcp` reports `bytes_per_answer` (16 + the block's span).

## Reader

```
xsa lcp <container> --run I | --runs A:B | --sample N --seed S | --all [--audit]
      run<TAB>lcp, plus LCP_OPEN / LCP_AUDIT / LCP_ACCESS
xsa lcp-verify <container> --corpus FILE [--sample N] [--deep K] [--deep-windows W]
      every sampled run's STORED value vs a brute-force lce over the corpus
```

`--audit` is the structural pass: the directory must tile the payload exactly, the
escape counts must agree with the blocks, and every escape list must be **sorted**
(a binary search over an unsorted list is a wrong answer that looks like a right
one). It reads the directory and every escape list, so it is the verification
path, not the open path: `xsa query` still opens at `decoded_bytes=0`.

## Compatibility

v6 and v7 containers keep opening (`xsa query`), and m14 is **purely additive**:
building the same corpus with `--no-lcp-runs` (or `XSA_LCP_RUNS=0`) yields v7 with
all 8 other members byte-identical (measured at 150 MB, sha per member). A container
without m14 is refused **by name** by `xsa lcp`, never answered around.

## What m14 uniquely enables (and what it does NOT)

m14 is **opt-in**: `xsa build --tape --with-lcp-runs`. The default arm writes the
v7 layout with no m14, and `xsa lcp` refuses such a container **by name**.

The operations design (MEMORY_OPS.md) answers contexts from two backends: a
support-bounded context **trie** (a lookup, no scan) for WIDE contexts, and
`seek` + SA **locate-and-scan** (cost O(width)) for NARROW ones. m14 buys the third
thing: **LCP-interval topology** — a node's depth, its children's delimiters, MEM
and interior enumeration — i.e. the middle of that split, plus the ability to walk
a node's interval when you deliberately want every occurrence in order.

How thick is that middle band? Measured on the 10 GB Pile slice: 1000 byte-weighted
contexts per depth (the CONTEXT_DEPTH.md shape, seed 20261110), each context's
occurrence count taken from the container's own `xsa query --count`:

| depth | median width | w==1 | 2..999 | 1e3..1e5 | 1e5..1e6 | >=1e6 |
|---:|---:|---:|---:|---:|---:|---:|
| 8  | 2,815 | 4.3 % | 36.1 % | 45.7 % | 11.3 % | 2.5 % |
| 16 | 3     | 39.9 % | 53.8 % | 5.1 % | 0.0 % | 1.1 % |
| 24 | 1     | 73.8 % | 24.6 % | 0.9 % | 0.1 % | 0.5 % |
| 32 | 1     | 85.7 % | 13.6 % | 0.4 % | 0.0 % | 0.2 % |

At the depths where the information actually lives (16..32 bytes) the middle band
is **0.4..5.1 %** of contexts, the wide tail (>=1e6) is 0.2..1.1 % and the narrow
bulk is handled by locate-and-scan. A support-bounded trie at depth 8 / support
1000 covers 59.6 % of positions directly for 0.062 n. So m14 is a **quarter of `n`
spent on a thin band**: a deliberate, opt-in purchase, not a default tax.

When to turn it on: (a) you want node depths/child counts from the artifact rather
than from a fresh sweep; (b) you want MEM/interior enumeration with exact interval
bounds; (c) you know your workload sits in 1e3..1e6-occurrence contexts at shallow
depth (the depth-8 row above is the one place that band is thick).

**What it does not give you, stated plainly:** children at the TOP of the tree
cannot be enumerated by scanning, because those intervals are billions of rows
wide; they must be materialised (the trie lane builds them from the ordered sweep).
And the member is run-granular, so a child boundary that falls strictly INSIDE a run
is not in it. At the top of the tree that is exactly why the trie exists.

## Depth: what the member costs, and what a depth-limited form would save

The member stores LCP at FULL depth (exact u64). The tree operations we want are
shallow — the median context that repeats anywhere in 100 GB of Pile is 18 bytes,
entropy is 0.50 bits by depth 16 and 0 by depth 32 (`CONTEXT_DEPTH.md`) — so the
question "would a depth-limited member be cheaper?" is a measurement, and the
writer journals it: `LCP_RUNS_DEPTH`.

At the 150 MB fixture (`/dev/shm/t150m.txt`):

```
LCP_RUNS_DEPTH runs=57279194 b0=202 b1_8=20940129 b9_16=29338374 b17_32=6479869
  b33_64=414910 b65_255=98241 b256_64k=7468 b64k_up=1 deep_runs_gt64=52106
  deep_escape_bytes=521060 escape_bytes_total=2492520
  bytes_if_clamped_at_64=39924500 frac_le64=0.999090
```

At the 10 GB corpus the same accounting reads `frac_le64=0.998924`,
`deep_escape_bytes=37,395,570` of `2,556,383,072` (**1.46 %**), 5.89 bits/run.

99.909 % of runs are already ≤ 64 deep (150 MB), and depth above 64 costs 521,060 B of the
member's 40,445,560 B — **1.29 %**, an upper bound because clamping would not
refund the field it replaces. Total cost is 5.65 bits/run. So the codec is already
depth-adaptive in the only sense that matters (a shallow value costs ~1 byte, a
deep one 10 bytes) and a depth-limited format would buy ~1 % of the member while
deleting the ability to tell a depth-64 node from a deeper one. **Keep it exact.**

## Manifest (measured)

| corpus | container | runs | m14 bytes | ratio vs u32 | bits/run | wall |
|---|---|---|---|---|---|---|
| arm | container | runs | m14 bytes | ratio vs u32 | bits/run | wall |
|---|---|---|---|---|---|---|
| 150 MB, **default (no m14)** | 118,891,088 B v7 | 57,279,194 | 0 | - | - | 97 s |
| 150 MB, `--with-lcp-runs` | 159,336,688 B v8 | 57,279,194 | 40,445,560 | 0.176528 | 5.65 | 92 s |
| 10 GB, `--with-lcp-runs` | 9,560,930,192 B v8 | 3,474,883,891 | 2,556,383,072 | 0.183919 | 5.89 | finish 29:24, total 29:44 |
| 100 GB, **default (no m14)** | see `gates/lcp_runs_g4_100g.sh` | 31,381,597,694 (projected) | 0 | - | - | |

Gates: `gates/lcp_runs_g1_150m.sh` (build + oracle + additivity),
`gates/lcp_runs_g2_verify.sh` (the 124-value correctness gate),
`gates/lcp_runs_g3_10g.sh`, `gates/lcp_runs_g4_100g.sh`.
