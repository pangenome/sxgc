Phase | seconds | peak RSS (GiB) | read GB | write GB
--- | ---: | ---: | ---: | ---:
load_ids | 0.49 | 0.425 | 0.330 | 0.000
parse_sort | 2.99 | 0.991 | 0.000 | 0.000
check_ranks | 0.37 | 0.543 | 0.127 | 0.000
pack_dictionary | 5.17 | 2.634 | 1.000 | 0.000
aligned_and_lists | 10.06 | 3.148 | 0.000 | 0.000
dictionary_sort | 51.93 | 10.460 | 0.000 | 0.000
owner_map | 0.19 | 10.731 | 0.000 | 0.000
emit | 476.03 | 10.733 | 0.000 | 0.000
sink (concurrent) | 477.49 | 0.010 | 0.000 | 5.870
finish (concurrent) | 490.15 | 0.494 | 0.000 | 3.791

From IDs/refs to finished product: 561.796 s; includes streaming to oracle; excludes cold copies, post-run certification, and making the lexparse.
Emit write calls (s): sink 32.478717 audit 158.886336 (inside emit elapsed, not additive to it).
Worker-attributed physical read/write GB: 1.457 9.6615
Oracle separately: CPU s 173.37 read GB 1.0
Peak shared GiB: 13.526
Counted scratch: sampled 1123789269 reserved 1597106334 limit 2000001330
Whole private directory (allocated, sampled): 3807813632 bytes. Includes oracle and output.
SHA-256: b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3
