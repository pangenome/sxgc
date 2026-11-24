Phase | seconds | peak RSS (GiB) | read GB | write GB
--- | ---: | ---: | ---: | ---:
load_ids | 0.52 | 0.425 | 0.330 | 0.000
parse_sort | 3.01 | 0.991 | 0.000 | 0.000
check_ranks | 0.37 | 0.543 | 0.127 | 0.000
pack_dictionary | 5.20 | 2.634 | 1.000 | 0.000
aligned_and_lists | 10.49 | 3.148 | 0.000 | 0.000
dictionary_sort | 50.57 | 10.460 | 0.000 | 0.000
owner_map | 0.19 | 10.731 | 0.000 | 0.000
emit | 344.74 | 10.733 | 0.000 | 0.000
sink (concurrent) | 345.73 | 0.010 | 0.000 | 0.001
finish (concurrent) | 358.56 | 0.494 | 0.000 | 3.295

From IDs/refs to finished product: 429.375 s; includes streaming to oracle; excludes cold copies, post-run certification, and making the lexparse.
Emit write calls (s): sink 23.096896 audit 80.301872 (inside emit elapsed, not additive to it).
Worker-attributed physical read/write GB: 1.457 3.2962
Oracle separately: CPU s 87.19 read GB 1.0
Peak shared GiB: 13.522
Counted scratch: sampled 1119200658 reserved 1597106334 limit 2000001330
Whole private directory (allocated, sampled): 3922644992 bytes. Includes oracle and output.
SHA-256: b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3
