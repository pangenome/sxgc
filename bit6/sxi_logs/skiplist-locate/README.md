# Skiplist-over-anchors: the lite container's locate accelerator

Lane task: Durbin's run-length-compressed skiplist idea (bioRxiv
10.64898/2026.03.26.714584, "A run-length-compressed skiplist data structure
for dynamic GBWTs...", R. Durbin) applied to the **lite container** locate
path (member 11 sparse anchors + derived LF starts, member 8 dropped).

Everything here reads the retained artifacts read-only:
- yeast235: `/mnt/nvme3n1/erikg/sxi2-v6-3-3eae0415/yeast235.sxi2` (v5, 923,770,376 B)
- pile-frag: `/mnt/nvme3n1/erikg/sxi2-v5/pile-frag.sxi2` (5,196,757,368 B)
No container was written. All outputs are in this directory. Nothing staged
or committed.

## What Durbin's paper actually provides (extracted from the v2 full text)

- Rskip = Pugh skiplist over the **run-length backbone**: one column per
  run; `access()` walks right at the top level accumulating partial sums
  (run lengths live in the level edges as counts) and drops levels when
  the next hop would overshoot; expected O(log R).
- Level discipline: column heights from a geometric distribution
  (mean height 1.6, sampled while the draw is < 3/8, "close to the optimum
  1/e"). Static nodes are five 32-bit integers: right, sRight, down
  pointers plus sum and sSum. rank() embeds a per-symbol skiplist in the
  same columns (sRight + sCount) for expected O(log R_s).
- Why O(log) works there: the searched order is the **linear run sequence**,
  and each level edge spans many runs *by construction* (counts). The
  skiplist replaces array rank/select over runs — it does not accelerate
  LF-orbit walks.

The key translation question for OUR problem: the lite locate is a walk
along the **LF permutation orbit** of a row, not a search in a linear order.
The only free primitive per step is `LF(row) = start[run] + off` (member 10,
derived free from member 1). Any jump of k>1 steps must be *stored*
somewhere, and it is only reusable across rows if it is safe for a whole
run's rows at once.

## Correct lite-locate semantics (member 11 alone)

Member 11 stores one raw u64 **tail-SA value** for every 1024th run
(checked against member 8's phi by `xsa`; anchors are runs 0, 1024, 2048, ...).
Landing **in** an anchored run does not resolve SA: within a run, SA values
are not consecutive (e.g. BWT "annb$aa": the {a,a} run has SA {4,2}).
So the flat lite locate must walk until the current row **is** the tail row
of an anchored run:

    SA[origin] = (anchor_value + steps) mod n        (LF decrements SA by 1)

The banked "worst-case ~1024 LF steps" model matches the distance to
*entry into an anchored run* (measured mean 1022.8 on yeast), which is a
lower bound only. The real termination needs the anchored *tail row*.

## Tool

`skiplist_locate.cpp` (C++17, no deps; `-O3 -march=native`):
- decodes SXI2 codec-101 runs (Huffman heads, MSB-first, + gamma lengths),
  validates C table and sum == n;
- derives LF starts and row->run checkpoints (1 per 1024 rows);
- modes: `info`, `sweep` (exact all-rows walk-length distribution: the
  anchor->next-anchor segments partition the single LF cycle, sum(delta)==n
  is asserted), `entry` (sampled run-entry distance), `verify` (text
  reconstruction + source byte-compare + sampled locate flat vs skiplist
  with per-row text verification), `jumps` (safe-jump pointer build).

## Results

See **REPORT.md** for the full table and verdict. One-paragraph summary:
the correct member-11 locate (anchored *tail-row* termination) costs mean
221,573 / p99 1.99M / max 6.52M LF steps on yeast235 (the banked "~1024
worst case" is the anchored-run *entry* distance, mean 1022.8, and
understates the locate by ~217x); pile-frag text costs mean ~3.1k. A
Durbin-style skip hierarchy over the anchors cannot navigate the LF orbit
from arbitrary rows; the transplantable piece of his idea is run-granular
*safe-jump* pointers (parallel-band jumps), which at ~10% of the 177.2MB
lite artifact buy 1.4-2.3x, at ~47% buy 5.8x, and at 2.3x-the-lite-artifact
(full pointer set) buy 57x — not O(log). O(log)-class locate at few-percent
space is not achievable in this family; it needs the phi/Move class
(member 8, ~66 bits/run). All recovered positions text-verified; source
matched byte-exact (yeast.fa, records reversed).
