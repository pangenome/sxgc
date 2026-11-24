Phase | seconds | peak RSS (GiB) | read GB | write GB
--- | ---: | ---: | ---: | ---:
lexparse (raw input) | 369.85 | 9.328 | 10.190 | 30.875
load_ids | 23.36 | 15.804 | 13.216 | 0.000
parse_sort | 39.33 | 22.219 | 0.000 | 0.000
check_ranks | 8.00 | 18.163 | 0.000 | 0.000
pack_dictionary | 34.25 | 29.407 | 0.000 | 0.000
aligned_and_lists | 191.40 | 25.414 | 0.000 | 0.000
dictionary_sort | 783.60 | 103.350 | 0.000 | 0.000
owner_map | 2.16 | 105.967 | 0.000 | 0.000
emit | 8581.05 | 105.978 | 0.000 | 0.000
sink (concurrent) | 8600.46 | 0.011 | 0.000 | 0.014
finish (concurrent) | 8739.55 | 3.639 | 10.638 | 17.748

Raw to product: 10198.880 s including 4.081 s private cold transition; raw worker IO bytes {'read_bytes': 34044002304, 'write_bytes': 48637231104}; raw scratch high-water 15090631800 B.

From packed words/IDs to finished product: 9824.947 s; includes streaming to oracle; excludes cold copies, post-run certification, and making the lexparse.
Emit write calls (s): sink 2790.913559 audit 1051.664832 (inside emit elapsed, not additive to it).
Worker-attributed physical read/write GB: 23.8537 17.7626
Oracle separately: CPU s 1330.11 read GB 10.0
Peak shared GiB: 124.962
Counted scratch: sampled 13215620732 reserved 13224009340 limit 20000000000
Whole private directory (allocated, sampled): 41003700224 bytes. Includes oracle and output.
Bitmap in private tmpfs: 1250000001 B. Includes bitmap once in counted scratch; all-phase sampled disk allocation 41003700224 B.
SHA-256: 26cfb2de0cccab8a8712a332956d58693dcbbb34373a9031a7bbaaa08cc504c9
