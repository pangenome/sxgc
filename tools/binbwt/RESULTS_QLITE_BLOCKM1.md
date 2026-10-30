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
