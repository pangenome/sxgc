# BCR v2 gate journal (2026-09-30 UTC)

## Implementation and current status

`bit6/bcr_frontend_v2.cpp` replaces the flat v1 run array with 512-segment
blocks in an implicit AVL tree. Each node maintains subtree byte length,
canonical run count, and 256 symbol totals. Rank, row lookup, replacement,
and insertion descend the balanced tree; a block split inserts a new node.
Adjacent equal segments across block boundaries are counted as one output
run. Input is read once, backwards, in 64 KiB blocks. There are no maintained
head/tail SA samples in the dynamic tree.

The final sample writer walks the complete LF cycle from the primary row and
SA=0, writing exact endpoint samples by offset into private output files.
It uses O(1) sample RAM and O(n log r) worst-case time. **This is not the
requested sparse-anchor O(r) endpoint recovery.** Criterion C's gcd-one
observation establishes a primitive corpus and single LF cycle for the
retained corpora; it does not by itself give arbitrary run endpoint SA values
in O(r) time. The [2026 move-structure result](https://arxiv.org/abs/2602.11029)
likewise gives O(n) SA enumeration from an RLBWT. The code makes no O(r)
endpoint-time claim.

The implementation independently follows the blocked rope design described
by [ropebwt2](https://github.com/lh3/ropebwt2) and
[ropebwt3](https://github.com/lh3/ropebwt3); both repositories use MIT
licenses. No upstream source was copied or modified.

The long scale gate launcher is `run_gates.sh`, PID in `gate.pid`; it writes
to `/tmp/bcr-v2-gates-2ec315bd` and records GNU time plus byte comparisons
here. The binary for that launch is `/tmp/bcr_frontend_v2`, SHA-256
`fdebe41f460ac1ac391f523e210a1d12a8e342662c738573fa1a6d0235ae1981`.
The launcher runs yeast first, then remapped pile-frag, stopping at the first
failure. No retained PFP artifact is an output path.

## Completed gates

| Gate | Result | Wall | Peak RSS |
| --- | --- | ---: | ---: |
| Independent cyclic-SA oracle, 33 collections, all four files | PASS | n/a | n/a |
| No-clobber retry, all four files preserved | PASS | n/a | n/a |
| 50,000-byte blocked-tree differential vs banked v1, all four files | PASS | 0.28 s v2 / 5.86 s v1 | 2,048 KiB v2 / 4,096 KiB v1 |
| 9,705-byte PFP cross-producer, all four files | PASS | n/a | n/a |
| 10,005-byte remapped PFP cross-producer, all four files | PASS | n/a | n/a |
| 1,000,000-byte four-symbol synthetic preflight | Completed, with byte equality against earlier v2 build | 7.67 s | 14,336 KiB |
| yeast235 PFP four-file gate | RUNNING; see `scale-gates.log` | pending | pending |
| remapped pile-frag PFP four-file gate | QUEUED after yeast | pending | pending |

The 1 MB preflight is a throughput check, not a claim about either retained
corpus. The 50 KB differential tests many block splits and AVL rotations.
The independent oracle and PFP gates use `bit6/test_bcr_frontend.py`.

## Route accounting

| Route/corpus | Wall | Peak stage RSS | Front-end workspace peak |
| --- | ---: | ---: | ---: |
| BCR v2 yeast235 | pending | pending | pending |
| BCR v2 remapped pile-frag | pending | pending | pending |
| Retained PFP fragment parse + level 2 + rpfbwt | 2,066.3 s | 18,479,248 KiB | not measured as one workspace peak here |
| PFP 8 GB dictionary-SA build (separate observation) | not specified | not comparable to fragment stage | about 200 GB |

The PFP endpoint adapter was another separate retained stage: 390.6 s and
28,257,076 KiB peak RSS. The stage RSS and whole front-end workspace figure
must not be put in the same comparison column.

## Conditional memory projections

Assume the remapped fragment's canonical run density
`397,723,010 / 1,082,130,213 = 0.3675371` holds at larger slices.
The compiled structure has an 8-byte segment, a 2,128-byte node, and
4,120 bytes of pre-reserved segment capacity per block: 6,248 bytes per
block. The table brackets 50% occupancy (256 segments/block) and 73%
occupancy (375 segments/block). It excludes allocator overhead and output
page cache, and assumes few extra same-symbol segments at block boundaries.

| Source | Projected runs | BCR tree at 375/block | BCR tree at 256/block | PFP 25x workspace extrapolation |
| ---: | ---: | ---: | ---: | ---: |
| 100 GB | 36.754 billion | 612 GB | 897 GB | 2.5 TB |
| 1 TB | 367.537 billion | 6.12 TB | 8.97 TB | 25 TB |

The 100 GB tree cannot be certified below 900 GB from this model because the
lower-occupancy case leaves essentially no room for allocator overhead.
The 1 TB monolithic tree exceeds the 900 GB cap. These are modelled working
set values, not measured RSS. A production 1 TB route needs external memory
or sharding even after removing full SA samples.

## Remaining work

The sparse-anchor maintenance and an endpoint recovery method with a proven
O(r) bound are unresolved. The yeast and fragment byte gates are in flight.
The current builder has no checkpoint/restart or atomic publication of its
four output files; an interrupted long gate leaves a partial private prefix.
The parse-free slim rewrite is therefore **not** the only remaining webtext
route item; the builder's endpoint method, billion-byte throughput, and scale
gate results remain to be established.
