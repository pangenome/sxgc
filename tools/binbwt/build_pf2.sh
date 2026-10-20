#!/usr/bin/env bash
# Build the two binaries the prefetch charter's arms use, exactly the way the
# live probes are built (xsa/src/tools.rs: -O3 C++17 + libatomic), plus the SD
# sdsl header the finish's slim fingerprint includes.
#
#   bash tools/binbwt/build_pf2.sh OUT_DIR
#
# pile_finish needs sdsl's headers and ram_fs.cpp (the slim fingerprint's
# ram_fs_initializer); binbwt (cross_lcp_merge) needs neither.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT="${1:?usage: bash tools/binbwt/build_pf2.sh OUT_DIR}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
SDSL_INC="${SDSL_INC:-}"
if [ -z "$SDSL_INC" ]; then
  SDSL_INC=$(find /mnt/nvme2n1/erikg -path "*stages-*/deps/sdsl/include/sdsl" -type d -printf "%h\n" 2>/dev/null | head -1)
fi
[ -n "$SDSL_INC" ] || { echo "build_pf2: no sdsl headers found; set SDSL_INC" >&2; exit 1; }
SDSL_LIB="$(dirname "$SDSL_INC")/lib/ram_fs.cpp"
[ -f "$SDSL_LIB" ] || { echo "build_pf2: missing $SDSL_LIB" >&2; exit 1; }
echo "+ binbwt (cross_lcp_merge)"
c++ -O3 -std=c++17 -pthread bit6/cross_lcp_merge.cpp -latomic -o "$OUT/binbwt"
echo "+ pile_finish"
c++ -O3 -std=c++17 -pthread -I"$SDSL_INC" bit6/pile_finish.cpp "$SDSL_LIB" -latomic -o "$OUT/pile_finish"
echo "+ oracle"
c++ -O2 -std=c++17 -pthread tools/binbwt/oracle.cpp -latomic -o "$OUT/oracle"
echo "build_pf2: $OUT"
