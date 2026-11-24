Phase | seconds | peak RSS (GiB) | read GB | write GB
--- | ---: | ---: | ---: | ---:
lexparse (raw input) | 33.92 | 0.953 | 1.000 | 3.261
load_ids | 1.72 | 1.466 | 1.369 | 0.000
parse_sort | 3.09 | 2.150 | 0.000 | 0.000
check_ranks | 0.37 | 1.701 | 0.127 | 0.000
pack_dictionary | 2.44 | 2.881 | 0.000 | 0.000
aligned_and_lists | 14.61 | 2.241 | 0.000 | 0.000
dictionary_sort | 56.17 | 10.460 | 0.000 | 0.000
owner_map | 0.23 | 10.732 | 0.000 | 0.000
emit | 178.76 | 10.743 | 0.000 | 0.000
sink (concurrent) | 179.95 | 0.010 | 0.000 | 0.001
finish (concurrent) | 194.45 | 0.493 | 1.000 | 1.796

Raw to product: 308.244 s including 0.773 s private cold transition; raw worker IO bytes {'read_bytes': 3496820736, 'write_bytes': 5057646592}; raw scratch high-water 1635681177 B.

From packed words/IDs to finished product: 273.547 s; includes streaming to oracle; excludes cold copies, post-run certification, and making the lexparse.
Emit write calls (s): sink 98.004684 audit 75.546639 (inside emit elapsed, not additive to it).
Worker-attributed physical read/write GB: 2.4965 1.7968
Oracle separately: CPU s 83.12 read GB 1.0
Peak shared GiB: 13.347
Counted scratch: sampled 1375646741 reserved 1400704518 limit 2000001330
Whole private directory (allocated, sampled): 3598462976 bytes. Includes oracle and output.
Bitmap in private tmpfs: 125000084 B. Includes bitmap once in counted scratch; all-phase sampled disk allocation 4837924864 B.
SHA-256: b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3
