Phase | seconds | peak RSS (GiB) | read GB | write GB
--- | ---: | ---: | ---: | ---:
lexparse (raw input) | 34.12 | 0.954 | 1.000 | 3.261
load_ids | 1.78 | 1.466 | 1.369 | 0.000
parse_sort | 3.20 | 2.150 | 0.000 | 0.000
check_ranks | 0.39 | 1.701 | 0.127 | 0.000
pack_dictionary | 2.34 | 2.881 | 0.000 | 0.000
aligned_and_lists | 13.87 | 2.238 | 0.000 | 0.000
dictionary_sort | 55.28 | 10.460 | 0.000 | 0.000
owner_map | 0.22 | 10.732 | 0.000 | 0.000
emit | 516.41 | 10.733 | 0.000 | 0.000
sink (concurrent) | 517.59 | 0.010 | 0.000 | 0.001
finish (concurrent) | 532.88 | 0.493 | 1.000 | 1.797

Raw to product: 645.252 s including 0.781 s private cold transition; raw worker IO bytes {'read_bytes': 3496882176, 'write_bytes': 5058629632}; raw scratch high-water 1635681177 B.

From packed words/IDs to finished product: 610.347 s; includes streaming to oracle; excludes cold copies, post-run certification, and making the lexparse.
Emit write calls (s): sink 26.863438 audit 141.497353 (inside emit elapsed, not additive to it).
Worker-attributed physical read/write GB: 2.4966 1.7977
Oracle separately: CPU s 153.97 read GB 1.0
Peak shared GiB: 13.335
Counted scratch: sampled 1369355285 reserved 1394413062 limit 2000001330
Whole private directory (allocated, sampled): 3896057856 bytes. Includes oracle and output.
Bitmap in private tmpfs: 125000084 B. Includes bitmap once in counted scratch; all-phase sampled disk allocation 4828344320 B.
SHA-256: b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3
