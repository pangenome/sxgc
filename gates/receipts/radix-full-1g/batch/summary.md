Phase | seconds | peak RSS (GiB) | read GB | write GB
--- | ---: | ---: | ---: | ---:
lexparse (raw input) | 33.65 | 0.953 | 1.000 | 3.261
load_ids | 1.71 | 1.466 | 1.369 | 0.000
parse_sort | 3.14 | 2.150 | 0.000 | 0.000
check_ranks | 0.39 | 1.701 | 0.127 | 0.000
pack_dictionary | 2.41 | 2.881 | 0.000 | 0.000
aligned_and_lists | 13.54 | 2.242 | 0.000 | 0.000
dictionary_sort | 54.95 | 10.460 | 0.000 | 0.000
owner_map | 0.23 | 10.732 | 0.000 | 0.000
emit | 176.73 | 10.743 | 0.000 | 0.000
sink (concurrent) | 178.15 | 0.010 | 0.000 | 0.001
finish (concurrent) | 192.74 | 0.493 | 1.000 | 1.796

Raw to product: 304.062 s including 0.769 s private cold transition; raw worker IO bytes {'read_bytes': 3496615936, 'write_bytes': 5057654784}; raw scratch high-water 1635681177 B.

From packed words/IDs to finished product: 269.634 s; includes streaming to oracle; excludes cold copies, post-run certification, and making the lexparse.
Emit write calls (s): sink 91.915313 audit 78.704345 (inside emit elapsed, not additive to it).
Worker-attributed physical read/write GB: 2.4965 1.7968
Oracle separately: CPU s 84.96 read GB 1.0
Peak shared GiB: 13.348
Counted scratch: sampled 1375646741 reserved 1400704518 limit 2000001330
Whole private directory (allocated, sampled): 3598462976 bytes. Includes oracle and output.
Bitmap in private tmpfs: 125000084 B. Includes bitmap once in counted scratch; all-phase sampled disk allocation 4822794240 B.
SHA-256: b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3
