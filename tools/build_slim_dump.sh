#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
SD=/home/erikg/TeraTools/src/thirdparty
SDSL=$SD/sdsl-lite
g++ -DM64=1 -O2 -std=c++17 \
 -I bit6/pfp_ds_vendor \
 -I /home/erikg/r-pfbwt/build/_deps/pfp_ds-src/include \
 -I /home/erikg/r-pfbwt/build/_deps/spdlog-src/include \
 -I /home/erikg/r-pfbwt/build/_deps/gsacak-src \
 -I "$SD/include" -I /home/erikg/TeraTools/src/include \
 -I "$SDSL/include" -I "$SDSL/build/include" \
 -I "$SDSL/build/external/libdivsufsort/include" \
 "${2:-bit6/chi_rspace_dump.cpp}" /tmp/laneY/gsacak64.o -o "${1:-/tmp/laneQ/slim_dump}" \
 -L "$SDSL/build/lib" -L "$SD/lib" -lsdsl -pthread
