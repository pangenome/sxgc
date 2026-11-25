# What does a second parse buy on a saved Pile ID tape?

Prepared-ID measurement only, not a recursive v10 build. At source `56dda458`, the bundle already contains `pfp++` and `rpfbwt` ([upstream pins](../../../xsa/runtime/tools/upstream.lock.json)); the [genomic pipeline](../../../xsa/runtime/bit6/sxi_pipeline.py) invokes `pfp++ -i` with a five-ID window and modulus 11. This measures that existing parser, **not** a new trigger or a setting chosen for `build-stream`.

Input: `$S/parse-ranks-WjSNyk/parse.u32` (`$S=/mnt/nvme2n1/erikg/TAPEREADER/sandbox`), saved zero-based lexical tape from the 100,000,503-byte prefix, 3,170,531 IDs, 2,689,467 distinct IDs, SHA-256 `603e39e31da8fb51ad8838a114ee432dc03bb483e3e9f95f523b072d82e7f418`. The raw corpus was not opened. Output/log directory `/tmp/xsa-rpfp-id-census-suFfhH`; bundle `f32d97460abc3a20a7e2aabad402da88ebfe66604734b8b7742307efe044cc11`.

| Single invocation on prepared input | Output | Wall (s) | Process max RSS (KiB) |
|---|---|---:|---:|
| `pfp++ -i INPUT.u32 -o DIR/l2 -w 5 -p 11 -j 1 --tmp-dir DIR` | 288,655 P2 IDs; 286,106 distinct phrases | 0.44 | 62,532 |
| `stream_rank inverse p1.u64 p1.inverse 1 268435456` | 3,170,531 one-based P1 suffix ranks | 0.24 | 125,532 |
| `stream_rank inverse p2.u64 p2.inverse 1 268435456` | 288,655 one-based P2 suffix ranks (**not P1 ranks**) | 0.02 | 12,920 |

Tape length falls **10.98×**. But `l2.dict` = 19,484,036 B and `l2.parse` = 1,154,620 B: together **1.63×** the 12,682,124-byte u32 source tape. 284,570 of 286,106 dictionary entries occur only once. Sum of `(phrase length in IDs − 5)` is 3,154,372 over distinct entries and 3,170,532 over occurrences (includes one upstream start marker): **99.49%** of the owned ID positions remain as dictionary work before grouping equal representatives. This is not a count of distinct representative strings or an IO measurement. A dictionary by reference need not write 19 MB of symbols, but its source must remain available during induction.

Checks, outside timing: every `l2.parse` ID indexes a dictionary entry; entries are strictly lexically ordered by numeric IDs. Every occurrence, with five-ID overlaps, matches the corresponding slice of `[2] + [x+10 for x in P1] + [2]*5`, with exact final extent. `p1.inverse` equals every saved `work/inverse.u32` rank after widening. `p2.inverse` is a permutation and gives strictly increasing `(P2[i],rank[i+1])` in rank order, END rank zero. No recursive lift or byte rows were computed here.

One pinned logical CPU, 1 GiB address-space limit, no separate RAM/swap cgroup, `/usr/bin/time`, one observation per command, uncontrolled cache. Rank inputs widened from u32 to LE u64 outside timing (`p2.u64` retains upstream's one-based lexical labels). Times include temporary file close, not publication, fsync or postchecks. No device/attributed IO or physical scratch peak collected. L2 parse closed 2026-11-25 23:31:58 UTC / 17:31:58 Memphis; rank commands completed 23:34:01 UTC / 17:34:01 Memphis. No raw-to-product time or performance promotion.

**Implication:** use the upstream parser as a control. The useful product component to qualify is `P1 -> rank(P1)` *via* P2, including the full upward induction, with the same inverse-rank contract as `stream_rank`. Comparing just 0.02 s to 0.24 s would omit both the parse and the lift.
