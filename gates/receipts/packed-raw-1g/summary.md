Phase | seconds | peak RSS (GiB) | read GB | write GB
--- | ---: | ---: | ---: | ---:
lexparse (raw input) | 29.17 | 0.953 | 1.000 | 3.261
load_ids | 1.68 | 1.466 | 1.369 | 0.000
parse_sort | 2.92 | 2.150 | 0.000 | 0.000
check_ranks | 0.39 | 1.701 | 0.127 | 0.000
pack_dictionary | 2.23 | 2.881 | 0.000 | 0.000
aligned_and_lists | 12.17 | 2.244 | 0.000 | 0.000
dictionary_sort | 50.90 | 10.460 | 0.000 | 0.000
owner_map | 0.22 | 10.731 | 0.000 | 0.000
emit | 342.56 | 10.733 | 0.000 | 0.000
sink (concurrent) | 343.70 | 0.010 | 0.000 | 0.001
finish (concurrent) | 356.57 | 0.492 | 1.000 | 3.420

Raw to product: 457.572 s including 1.026 s private cold transition; raw worker IO bytes {'read_bytes': 3496706048, 'write_bytes': 6681427968}; raw scratch high-water 1635681177 B.

From packed words/IDs to finished product: 427.373 s; includes streaming to oracle; excludes cold copies, post-run certification, and making the lexparse.
Emit write calls (s): sink 23.466811 audit 80.800230 (inside emit elapsed, not additive to it).
Worker-attributed physical read/write GB: 2.4965 3.4205
Oracle separately: CPU s 87.69 read GB 1.0
Peak shared GiB: 13.332
Counted scratch: sampled 1369355285 reserved 1394413062 limit 2000001330
Whole private directory (allocated, sampled): 3845918720 bytes. Includes oracle and output.
SHA-256: b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3
