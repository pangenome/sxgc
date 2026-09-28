#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
DEPS=${SXI_DEPS:?set SXI_DEPS to build_all.sh dependency directory}
RPFBWT_BUILD=${SXI_RPFBWT_BUILD:?set SXI_RPFBWT_BUILD to isolated rpfbwt build}
gcc -O2 -DM64 -c "$DEPS/gsacak/gsacak.c" -o "$RPFBWT_BUILD/gsacak64.o"
g++ -DM64=1 -O2 -std=c++17 \
 -I bit6/pfp_ds_vendor \
 -I "$DEPS/pfp_ds/include" -I "$DEPS/spdlog/include" \
 -I "$DEPS/gsacak" -I "$DEPS/teratools/src/include" \
 -I "$DEPS/sdsl/include" -I "$RPFBWT_BUILD/_deps/divsufsort-build/include" \
 "${2:-bit6/chi_rspace_dump.cpp}" "$RPFBWT_BUILD/gsacak64.o" \
 -o "${1:?output path required}" \
 "$RPFBWT_BUILD/_deps/sdsl-build/lib/libsdsl.a" \
 "$RPFBWT_BUILD/_deps/divsufsort-build/lib/libdivsufsort.a" \
 "$RPFBWT_BUILD/_deps/divsufsort-build/lib/libdivsufsort64.a" -pthread
