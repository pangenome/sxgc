# What actually fits beside the 100 MB epoch producer?

**The historical v8 finish is not an 80 MB consumer.** The [external 100 MB arm](../gates/receipts/external-product-100m/) made the exact 107,519,008-byte, nine-member container in 512.208 s from prepared columns. Its joint space verdict is **FAIL**, not PASS. The original logs, output and SHA stand. This correction does not rerun that payload or change the newer matched 1/10 GB product plans (which explicitly charge run pieces).

`laws.rs::du_dir` skips directories. `scratch_components` scans `merged` and `bands` but not `merged/frag.pieces/p-*`. `SS_SCRATCH_PEAK=74,913,627` is a sampled partial tally. `overlap_all_sets.py` substituted a fixed 80,000,000 B for the downstream files. The reported 199,706,270 B is therefore not a conservative peak. `overlap_product.py` now refuses before opening any inputs. Do not discard an exact index just because its space accounting failed, or use the old receipt to admit another build.

## Lower bound at one instant — not the sum of individual peaks

The postmortem [authenticates the complete container](../gates/overlap_finish_credits.py), the original finish log and its durable m1 release events. Use only full 64-piece batches *during the sweep*. The row sink sends already maximal BWT runs, no seam classes. `read_stream_band` returns writer credit after closing its reader but defers **unlink**. An armed piece cannot be released before `finishAdvance` and fsync of the covering m1 codec blocks. At the first unlink of batch `[512,576)`, all 64 files still exist immediately before that call. At least `576*65536−2` runs have been folded: one pending in each coalescer. Sparse tails are written at 1/1024. The m14 directory gives the actual padded span of each 1024-run block; accumulating those spans up to the last **completed 4 MiB flush** gives the on-file lower bound, not the eventual member size or bytes still in RAM.

| Still live before that release | Logical B |
|---|---:|
| 64 closed but not yet unlinked PRC1 pieces | 49,285,888 |
| PFCK (`tau=22`) | 36,363,864 |
| χ bitmap | 12,500,063 |
| flushed m14 payload | 25,167,168 |
| m11 sparse tails, lower bound | 294,912 |
| **Downstream lower bound** | **123,611,895** |
| Continuously retained old producer input columns | 96,184,365 |
| **Historical joint lower bound** | **219,796,260** |

This is **19,795,254 B over 2N**, without charging an extra SXLS, any later ready piece, in-flight set, log, raw corpus or final output. Conversely it is *not* an exact high water or an IO count. Even the downstream floor alone exceeds the [rolling arm's reservation](../gates/receipts/finite-epoch64-100m/) by **3,169,692 B**. Keeping the whole new posting capacity charged throughout means we cannot simply transplant those controls. It does not follow that the epoch arm crossed its cap: no finish ran there.

## Can credits be shortened without a circular wait?

These are two different clocks: a PRC1 piece covers 65,536 **runs**, an SXLS covers 262,144 **rows**. Tightening either window blindly can deadlock a single writer. For this *same run stream*, m12's 65,536- and 1,024-run row samples give a conservative progress test. Let `y[i]` be the first row of piece `i`, append `y[K]=N+1`. Before reading piece `i`, LCP through the preceding piece can progress independently. Its exclusive end is the start of run `max(0,i*65536−2)`; round its run index **down** to the previous 1024 sample to lower-bound copied rows. Each whole slice opened up through this frontier returns **disk** credit immediately (after close/unlink), even if its RAM copy stays alive.

* To publish piece `i`, the sink must first have published `floor(y[i+1]/262144)` slices. The lookahead row triggering a run change cuts the piece **before** its LCP is appended. At EOF publish the final short slice **before** `cutter.finish`; use `ceil((N+1)/262144)` instead. Subtract credits already returned. Even the optimistic preceding frontier `y[i]` cannot reduce the maximum below **4 slices**, at piece 550. Four also suffice by the conservative test. Largest piece span 1,126,176 rows.
* Once the reader has acknowledged piece `i`, to supply its LCP in full the producer need go no further than `y[i+1]` rounded up to the end of its slice (clamp at EOF). Only internal piece boundaries **strictly below** that exclusive end must have fired. Subtract `i+1` acknowledged pieces. **3 piece credits suffice**; not claimed minimal. The live LCP worker can copy and acknowledge one slice at a time if the main thread is waiting on the following piece. This establishes a schedule of protocol credits, not general process liveness or a wall bound.
* Counterexamples: a huge run can require many slices before the first piece or terminal; dense singleton runs can require many pieces before the first slice. A fixed window valid here is not an unconditional scale rule. `overlap_finish_credits_test.py` exercises 12,276 schedules, including both directions of stall and cuts coincident with SXLS publication.

## One candidate envelope (not admission)

Keep a little distance from both credit minima: 6 writer piece credits, 5 SXLS credits. Reserve `(6+64+8)` full pieces+progress records: at most 63 already covered at the previous release check, unopened codec block (1,024 runs vs a full 65,536-run band), two coalescers, current/next read, terminal and possible in-flight partial, with extra slack. This reasoning requires **one** seam-free maximal-run producer, no resume/spool/debug, the frozen two-slot feed and its after-each-band release check; it is not a bound on arbitrary `pendingPieces`. Credit on copy is not deletion credit for these pieces. Use the proved row slice peak (2,410,736 B incl header) and the actual m14 payload, not an average. Hold 2 MiB extra for controls and bounded transport. M5 EF files begin **after** forced close/release of all pieces; conservatively keep PFCK and bitmap charged even in that phase. Product bytes grow separately. [Parts and arithmetic](../gates/receipts/finish-credits-100m/review.json):

| PFCK spacing | Modeled downstream peak B | Headroom vs 120,442,203 B |
|---:|---:|---:|
|22|149,949,703|−29,507,500|
|64|126,085,943|−5,643,740|
|128|119,835,911|606,292|

Changing spacing affects fingerprint work and readiness: no speedup or identity at 128 measured. This is a candidate for a **guarded v8** test, not permission to allocate the modeled amounts on an unchecked stream. It needs online bounds before payload growth, a verified relay, private stop and joint working-set accounting (including all anonymous memory and cache, not just producer arrays). v10 uses a *different* pinned finish, no m14, a position tap, trie payload and two joins. Do not subtract m14 and call that a v10 proof. No new 100 MB joint timing and no scale authorization.

## Check before forwarding, not after a scratch excursion

[`overlap_finish_row_fence.cpp`](../gates/overlap_finish_row_fence.cpp) is an independent raw verifier with a narrow-output pipe for this v8 sink only. It rejects a high row43 field before narrowing. Each row must prove its predecessor, exact equal prefix and the strict order of the following symbol or exhaustion. Count `N+1` strictly ordered suffixes, then scan the raw text for the final suffix rank. For any earlier suffix `i`, its LCP with the last suffix is no greater than its LCP with suffix `i+1`. This bounds the logical work of the final-rank comparison by the sum of the adjacent LCPs plus at most 65 extra byte comparisons per raw position. Neither number is device IO. The verifier holds one 98,304 B packet, checks STOP/deadline on both nonblocking pipes, and withholds the last packet until EOF, rank and the pinned narrow-row SHA match. A supervisor must still gate publication on *all* successful exits.

For this injectively remapped, seam-free maximal-run input, run heads on the raw predecessor stream are precisely m14's input values. Fence the run count, each growing SXLS (including its 48 B header), and the accumulated m14 spool **before forwarding the packet**. Price closed blocks using a tiny width histogram plus all-ones counts; price the last short block before releasing the tail. This is independent of the producer and uses no reference member at construction time. Prefix errors may leave real partial files; a failed check is not a complete stream. [Fixture and frozen-member receipts](../gates/receipts/finish-row-fence-fixtures/) check byte forwarding and all 37,742 historical block spans. Other finish files, memory, lifetime/launch constraints and the different v10 wire/tap/joins are not certified by the fence. The [PRC1 slot option](overlap_v8_piece_pool.md) separately enforces the 78-payload reservation on the writer before growth, with independent synthetic wire and inode-lifetime tests. The opt-in [checked SXLS writer](overlap_v8_sxls.md) enforces the five disk-slice capacities with 589,824 B of codec arrays; neither file credit nor that array charge bounds the frozen reader's retained RAM copies.
