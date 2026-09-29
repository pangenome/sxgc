#!/bin/bash
set -euo pipefail
cd /tmp/sxgc-laneV
D=/tmp/sxgc-dist-final/.build/deps
B=/tmp/sxgc-dist-final/.build/rpfbwt-build
OUT=bit6/sxi_logs/rpfbwt-64/seam-policy-tools
for target in rpfbwt_endpoints slim_dump slim_lce_test; do
 case "$target" in
 rpfbwt_endpoints) src=bit6/rpfbwt_endpoints.cpp;;
 slim_dump) src=bit6/chi_rspace_dump.cpp;;
 slim_lce_test) src=tools/slim_lce_test.cpp;;
 esac
 g++ -DM64=1 -O2 -std=c++17 -I bit6/pfp_ds_vendor -I "$D/pfp_ds/include" -I "$D/spdlog/include" -I "$D/gsacak" -I "$D/teratools/src/include" -I "$D/sdsl/include" -I "$B/_deps/divsufsort-build/include" "$src" "$B/gsacak64.o" -o "$OUT/$target" "$B/_deps/sdsl-build/lib/libsdsl.a" "$B/_deps/divsufsort-build/lib/libdivsufsort.a" "$B/_deps/divsufsort-build/lib/libdivsufsort64.a" -pthread
 done
