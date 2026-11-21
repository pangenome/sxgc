# Sort kernels (AMENDMENT W7)

- ips4o: In-place Parallel Super Scalar Samplesort (IPS4o),
  https://github.com/ips4o/ips4o, BSD 2-Clause, (c) 2017 Michael
  Axtmann / Daniel Ferizovic / Sascha Witt. Vendored 2026-11-21
  (include/ tree only; the SEQUENTIAL ips4o::sort is the kernel used -
  the band pool already fans the interiors). Keys at every converted
  call site are UNIQUE BY CONSTRUCTION (the position bits break the
  ties) - the kernel's instability is legal there and the byte-identity
  gate proves it.
- ips2ra: In-place Parallel Super Scalar Radix Sort (IPS2Ra),
  https://github.com/ips4o/ips2ra, BSD 2-Clause, same authors.
  Vendored for future integer-key interiors; NO call site qualifies
  today (the pairs' keys are byte strings, read via the corpus
  references - ips4o's comparator interface is the correct kernel).
