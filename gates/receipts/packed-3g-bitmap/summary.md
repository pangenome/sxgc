Phase | seconds | peak RSS (GiB) | read GB | write GB
--- | ---: | ---: | ---: | ---:
lexparse (raw input) | 92.98 | 2.848 | 3.000 | 9.720
load_ids | 4.67 | 4.319 | 4.047 | 0.000
parse_sort | 9.21 | 6.316 | 0.000 | 0.000
check_ranks | 2.19 | 5.029 | 0.000 | 0.000
pack_dictionary | 6.28 | 8.498 | 0.000 | 0.000
aligned_and_lists | 38.15 | 6.637 | 0.000 | 0.000
dictionary_sort | 159.87 | 30.901 | 0.000 | 0.000
owner_map | 0.56 | 31.704 | 0.000 | 0.000
emit | 1311.98 | 31.708 | 0.000 | 0.000
sink (concurrent) | 1315.01 | 0.012 | 0.000 | 0.003
finish (concurrent) | 1351.82 | 1.206 | 3.000 | 5.405

Raw to product: 1667.955 s including 1.185 s private cold transition; raw worker IO bytes {'read_bytes': 10047684608, 'write_bytes': 15127613440}; raw scratch high-water 4907641872 B.

From packed words/IDs to finished product: 1573.783 s; includes streaming to oracle; excludes cold copies, post-run certification, and making the lexparse.
Emit write calls (s): sink 211.450983 audit 303.395246 (inside emit elapsed, not additive to it).
Worker-attributed physical read/write GB: 7.0473 5.408
Oracle separately: CPU s 329.45 read GB 3.0
Peak shared GiB: 38.837
Counted scratch: sampled 4047331439 reserved 4055720047 limit 6000000000
Whole private directory (allocated, sampled): 11475456000 bytes. Includes oracle and output.
Bitmap in private tmpfs: 375000001 B. Includes bitmap once in counted scratch; all-phase sampled disk allocation 14170533888 B.
SHA-256: 3935e8f1d220092d2cd12081938748e8a62f02e882bd021781ec7c3d0cabe7f3
