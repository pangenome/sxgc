# Skiplist-over-anchors — REPORT

Lane: Durbin's run-length-compressed skiplist idea applied to the lite
container's sparse-anchor locate. Date: 2026-10-02.
Artifacts read read-only; all outputs in this directory; nothing staged.

Corpora:
- **yeast235** v6-3 (`/mnt/nvme3n1/erikg/sxi2-v6-3-3eae0415/yeast235.sxi2`):
  n=3,336,986,759, R=100,905,045 (n/R=33.1), member 11 = 98,541 anchors
  (stride 1024). **Lite artifact = 177,179,983 B** (container minus member 8).
- **pile-frag** v5 (`/mnt/nvme3n1/erikg/sxi2-v5/pile-frag.sxi2`):
  n=1,082,130,213, R=397,723,010 (n/R=2.7, text regime). Lite = 2,046,770,460 B.

## 1. Baseline: the ACTUAL flat-walk distribution (exact, all n rows)

The anchor->next-anchor walk segments partition the single LF cycle
(sum(delta)=n asserted, `CYCLE-OK` in sweep logs), so the numbers below are
the exact row-weighted distribution over ALL rows, not a sample.

Termination semantics (member 11 alone): land exactly on the tail row of an
anchored run; SA = (anchor + steps) mod n. Landing *inside* an anchored run
does NOT resolve SA (within-run SA values are not consecutive; BWT "annb$aa"
run {a,a} has SA {4,2}).

| corpus / stride | mean | p50 | p90 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|---:|
| yeast235 / 1024 (member 11) | **221,573** | 83,950 | 560,641 | 1,991,444 | 3,884,030 | **6,516,845** |
| yeast235 / 256 | 113,767 | 27,238 | 278,042 | 1,419,147 | 2,849,422 | 4,233,376 |
| yeast235 / 128 | 93,292 | 15,946 | 219,488 | 1,343,538 | 2,785,799 | 4,233,376 |
| yeast235 / 32 | 69,353 | 6,120 | 156,499 | 1,132,165 | 2,506,625 | 4,229,509 |
| pile-frag / 1024 | **3,051** | 1,971 | 6,904 | 16,041 | 41,057 | 134,859 |

Context: sampled distance to first *entry into an anchored run* — the
banked "~1024 steps" model — measures mean 1,022.8 (yeast) / 1,195.7
(pile-frag). That is the *entry* distance, a lower bound; the correct
locate must reach the anchored **tail row**, which costs the numbers above.
**The banked "worst-case ~1024" understates the lite locate by ~217x on
yeast (mean 221.6k, max 6.5M) and ~3x on pile-frag.**

Where the cost lives (yeast, stride 1024, contribution to the mean by
segment size): 87.5% of the mean comes from just 2,705 segments >= 262k
steps (satellite/mega-run class, 44.7% of rows); 12.4% from 16k..262k
segments. pile-frag: 83.7% of the mean from ordinary 1k..16k segments —
the satellite tail is a DNA-specific phenomenon.

## 2. Design: what "Durbin-style levels over the anchors" can and cannot be

Durbin's Rskip works because the searched order is the LINEAR run sequence
and level edges carry counts, so each hop spans many runs by construction.
The lite locate is a walk along the **LF permutation orbit** — there is no
linear order to search, and the only free primitive is the single LF step
(LF(row) = start[run]+off, member 10 derived free). Two consequences:

1. A skip-pointer chain *between anchors* (anchor a -> next anchor along the
   walk, with step counts, plus higher fan-out levels over that chain) is
   useless for single-position locate: the walk from a row must still reach
   the FIRST anchor one step at a time; the chain only serves rows already
   on anchor rows. Measured chain data = the delta table above (that IS the
   level-0 chain), and it is not navigable from arbitrary rows.
2. The only reusable jump is one **safe for every row of a run at once**.
   Because LF maps a whole run's rows to a contiguous row block, a jump of
   d steps is parallel-safe for all offsets o in [0,L_r) iff the offset-0
   walk's bands stay inside single runs for k<=d, and termination-safe iff
   no band contains an anchored tail row. This is the run-length-backbone
   insight of Durbin's structure, transplanted to the LF orbit: **safe-jump
   pointers at run granularity** (levels = clearance thresholds 2^l;
   fan-out shrinks ~x0.5 per level, measured in jumps*.log; encoding
   dest-row u32 + dist, 8 B/pointer + presence bitmap, ~6 B/pointer packed).

## 3. Prototype: flat vs skiplist on the same rows, text-verified

`verify` reconstructs the indexed text by inverse BWT (anchor-segment
walks), byte-compares it against the source corpus, then locates sampled
rows both ways. Gate results (yeast235):
- reconstructed text == `syng/yeast.fa` with **each record reversed**,
  0x1E separators — byte-exact over all 3,336,986,759 bytes.
- 4096 (+2048) sampled rows, all recovered positions text-verified
  (window 128 BWT chars vs source bytes at the recovered coordinate):
  **0 mismatches**, flat == skiplist anchor and step count on every row,
  for every configuration below.
- derived stride-32 anchor values: 3,153,283 values, 0 holes, all 98,541
  member-11 slots cross-checked equal (sweep-derive.log), then located and
  text-verified (verify-stride32-*.log).

### THE TABLE (yeast235; 48-thread runs; latency = mean per locate; speedup = within-run ratio)

| configuration | extra space (% of 177.2MB lite) | walk mean (LF steps) | mean hops | locate latency | speedup (within run) |
|---|---:|---:|---:|---:|---:|
| flat, member 11 (stride 1024) | 0 | 229,430 | 0 | 244.1 ms | 1x |
| + jumps d>=65536 | 7.7% | 229,430 | 0.95 | 336.3 ms* | 0.94x |
| + jumps d>=16384 | 10.1% | 229,430 | 3.1 | 192.7 ms | 1.39x |
| + jumps d>=4096 | 16.8% | 229,430 | 7.8 | 124.3 ms | 2.22x |
| + jumps d>=512 | 47.1% | 229,430 | 30.4 | 19.9 ms | 5.84x |
| + jumps d>=128 | 86.8% | 229,430 | 82.2 | 18.9 ms | 11.06x |
| + jumps d>=2 (all 49.3M) | 229.8% | 229,430 | 789.9 | 4.15 ms | **58.9x** |
| stride-32 anchors (derived), flat | 14.2% | 75,486 | 0 | 76.8 ms | (3.0x fewer steps; 3.6x in the clean 4-thread run) |
| stride-32 + jumps d>=1024 (s32-safe) | 14.2+13.2% | 75,486 | 10.4 | 16.7 ms | 3.07x |
| stride-32 + jumps d>=2 (s32-safe, all) | 14.2+221.4% | 75,486 | 257.0 | 1.37 ms | **59.3x** |

Cross-run caveat: run-to-run flat latency varies ~±40% with machine load;
within-run ratios and all step/hop counts are exact and reproducible.

*below noise: pointers almost never fire. Clean 4-thread runs give
~557 ns/LF-step in this prototype (untuned run_of); absolute latencies
scale with the production per-step cost, the step/hop ratios do not.

Level/fan-out table (band-safe against stride-1024 termination,
jumps65536.log): d>=2: 49.3M runs; >=64: 23.3M; >=512: 8.85M; >=4096: 2.15M;
>=16384: 670k; >=65536: 134k (censored 0.13%). Mean pointer distance 1015
(stride-1024 safety) vs 175 (stride-32 safety — denser anchors shrink safe
jumps; the two improvements couple through the band-termination check).

## 4. Honest verdict

1. **The banked weakness was understated by ~200x.** The lite locate on
   yeast is not ~1024 steps worst case: it is mean 221.6k / p99 2.0M / max
   6.5M LF steps. On text-like corpora (pile-frag) it is mean ~3.1k steps —
   there the flat walk is genuinely fine.
2. **O(log) navigation over the sparse anchors does not exist at "a few
   percent" space.** The LF orbit has no monotone order to binary-search;
   every accelerated step must be bought as a run-granularity pointer. The
   measured exchange is roughly win ~ space^1.25: ~10% of the lite artifact
   buys only 1.4-2.3x; ~47% buys 5.8x; the full pointer set (2.3x the lite
   artifact, ~33 bits/run — half of member 8's ~59 bits/run, which buys
   O(1)-ish phi locate instead) buys 58.9x (790 hops, still not O(log)).
3. **What a few percent DOES buy:** the safe-jump structure crushes the
   satellite tail specifically (87.5% of the mean lives in 2,705 mega
   segments). At 10% space the *tail* is fixed but the yeast bulk (16k-262k
   segments) needs mid-level pointers that are inherently numerous (half of
   R runs have d>=2 clearances). A budget of ~27% total (stride-32 anchor
   values + d>=1024 safe-jumps) delivers ~7.6x vs the member-11 flat
   (3.0x fewer steps from denser anchors, then 3.1x within-run from jumps)
   and is the practical knee of the measured curve.
4. **Sample compression is untouched** (per the falsification rulings);
   this lane only accelerates the sample-free lite form's locate.

## Files

- `skiplist_locate.cpp` — the tool (modes: info/sweep/entry/verify/jumps)
- `sweep1024.log`, `strides.log`, `sweep-derive.log`, `pile-sweep1024.log` — exact distributions
- `entry.log`, `pile-entry.log` — anchored-run-entry distances (the ~1024 model)
- `jumps.log`, `jumps-s32.log` — pointer level/fan-out tables
- `verify-*.log` — sampled locate gates (text verification, flat vs skiplist)
- `dist*.bin`, `vals32.bin`, `jumps*.bin`, `text-cache.bin` — derived data
- `README.md` — method + Durbin extraction

Machine: 256-proc host, runs capped at 48 worker threads (<64 procs rule),
peak RSS ~16 GB per process (<100 GB rule). No retained artifact modified.
