# Block-compressed m1 + fast query: measured

Container: block-256 run-length BWT with a per-block offset directory and a global Huffman
codebook; the reader decodes the run stream once into per-run columns (interleaved len/rank,
huge pages) and does O(1) rank. Product unchanged: chi = 44,034,230.

## Scoreboard (150 MB corpus, 100 low-count patterns, single thread)
| metric | pre-block (contiguous m1) | block-64 first cut | block-256 + columns |
|---|---|---|---|
| open | 505 ms | 8.8 ms | **~9 ms** (mmap 0.05 ms, decoded_bytes=0 at open) |
| per pattern | 1.1 ms | 33.6 ms | **3.1 ms** |
| rank | 0.5 us/step | 8.5 us/step | **0.49 us/step** |
| container | 0.49 n | 0.81 n | **0.71 n** |
| RSS | ~51 GB at 10 GB corpus | small | 511 MB at 150 MB (columns in heap) |
| chi | 44,034,230 | 44,034,230 | **44,034,230** |
| positions | exact | exact | **100/100 exact** |

## The 10 GB numbers this replaces
open 25.8 s and RSS 51 GB (eager decode of the whole run stream) -> now mmap-only open.
The heap columns are ~8 B/run: 456 MB at 150 MB, ~28 GB at 10 GB, ~250 GB at 90 GB,
~3.7 TB at pile. So 90 GB is servable with this design; the pile needs the SAME columns
stored in the container and mmap'd (scheduled follow-up, not a redesign).

## Cost accepted
container 0.49 n -> 0.71 n: the price of random access. Queries no longer pay to inflate
the file at startup; the file carries the access structure instead.

## Reproduce
    xsa build --tape --input <corpus> --scratch <dir> --threads N --timeout S
    xsa query <container> --patterns <0x1E-separated records> -j N    # id<TAB>offset

## 10 GB certificate on mainline (9852494, binary rebuilt, verified)
    REMAP       source=corpus present=243 order_preserving=1 bijection=1
    PEAK        peak_intermediate_over_n = 2.4576   (24.6 GB, on DISC)
    XSA_BUILD   chi = 2,676,929,445  (the virtual-END value; pre-END was 2,676,929,428)
    CONTAINER   6,178,186,696 B = 0.62 n
    QLITE_OPEN  mmap_ms=0.06 decoded_bytes=0 stages=none      (open was 25.8 s before block-m1)
    QUERY       12 low-count patterns: positions 12/12 EXACT, per pattern 1.9 ms
    WALL        3.37 s total; first answer 3.09 s (kernel build)
    RSS         34,358,488 kB = 33.6 GB  <-- THE DEFECT: per-run heap columns, 3.4 n

## The one open item: query RSS
The query holds ~8 B/run in heap: 511 MB @150 MB, 33.6 GB @10 GB, ~250 GB @90 GB, ~3.7 TB @pile.
This violates the memory law (memory must be bounded and FLAT in n). Fix in progress: mmap the
container, keep a FIXED block cache (64 MB, compile-time constant), pread blocks into it, prefetch
along the LF walk. Acceptance: RSS <= 1 GB @150 MB and <= 2 GB @10 GB, FLAT between them, rank
<= 2 us/step, chi unchanged, positions exact. Trade accepted: 0.49 us/step -> ~1-2 us/step.
