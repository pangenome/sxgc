Phase | seconds | peak RSS (GiB) | read GB | write GB
--- | ---: | ---: | ---: | ---:
load_ids | 0.52 | 0.425 | 0.330 | 0.000
parse_sort | 2.98 | 0.991 | 0.000 | 0.000
check_ranks | 0.40 | 0.543 | 0.127 | 0.000
pack_dictionary | 5.32 | 2.539 | 1.000 | 0.000
aligned_and_lists | 10.38 | 3.054 | 0.000 | 0.000
dictionary_sort | 47.35 | 6.021 | 0.000 | 0.000
owner_map | 2.44 | 10.365 | 0.000 | 0.000
emit | 381.91 | 10.367 | 0.000 | 0.000
sink (concurrent) | 383.39 | 0.010 | 0.000 | 5.870
finish (concurrent) | 395.79 | 0.476 | 0.000 | 3.420

From IDs/refs to finished product: 465.669 s; includes streaming to oracle; excludes cold copies, post-run certification, and making the lexparse.
Emit write calls (s): sink 33.599641 audit 77.334195 (inside emit elapsed, not additive to it).
Worker-attributed physical read/write GB: 1.457 9.2904
Oracle separately: CPU s 84.09 read GB 1.0
Peak shared GiB: 13.142
Counted scratch: sampled 1119529213 reserved 1597106334 limit 2000001330
Whole private directory (allocated, sampled): 3797041152 bytes. Includes oracle and output.
SHA-256: b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3
