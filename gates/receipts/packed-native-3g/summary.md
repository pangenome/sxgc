Phase | seconds | peak RSS (GiB) | read GB | write GB
--- | ---: | ---: | ---: | ---:
lexparse (raw input) | 93.28 | 2.848 | 3.000 | 9.720
load_ids | 4.66 | 4.319 | 4.047 | 0.000
parse_sort | 9.18 | 6.316 | 0.000 | 0.000
check_ranks | 2.14 | 5.029 | 0.000 | 0.000
pack_dictionary | 6.15 | 8.498 | 0.000 | 0.000
aligned_and_lists | 37.07 | 6.639 | 0.000 | 0.000
dictionary_sort | 161.01 | 30.901 | 0.000 | 0.000
owner_map | 0.56 | 31.704 | 0.000 | 0.000
emit | 1342.36 | 31.708 | 0.000 | 0.000
sink (concurrent) | 1345.66 | 0.012 | 0.000 | 0.003
finish (concurrent) | 1382.91 | 1.206 | 3.000 | 21.909

Raw to product: 1699.108 s including 1.162 s private cold transition; raw worker IO bytes {'read_bytes': 10047348736, 'write_bytes': 31631425536}; raw scratch high-water 4907641872 B.

From packed words/IDs to finished product: 1604.660 s; includes streaming to oracle; excludes cold copies, post-run certification, and making the lexparse.
Emit write calls (s): sink 246.928876 audit 298.860610 (inside emit elapsed, not additive to it).
Worker-attributed physical read/write GB: 7.0473 21.9118
Oracle separately: CPU s 324.5 read GB 3.0
Peak shared GiB: 38.836
Counted scratch: sampled 4047331439 reserved 4055720047 limit 6000000000
Whole private directory (allocated, sampled): 11764989952 bytes. Includes oracle and output.
SHA-256: 3935e8f1d220092d2cd12081938748e8a62f02e882bd021781ec7c3d0cabe7f3
