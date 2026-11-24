#!/usr/bin/env bash
# From raw rung1 input, physically write IDs + distinct word bytes. The decoder
# has no corpus argument. No full SA/LCP/tape of reconstructed text is written.
set -euo pipefail
cd "$(dirname "$0")/.."; root=/mnt/nvme2n1/erikg/TAPEREADER
D=$(mktemp -d "$root/sandbox/packed-parse-XXXXXX"); T=$(mktemp -d /tmp/packed-parse-XXXXXX); echo receipt="$D"
for f in overlap_lexparse overlap_packed_check;do g++ -O2 -std=c++17 -Wall -Wextra -Werror "gates/$f.cpp" -o "$T/$f" -lcrypto;done
cp --reflink=never "$root/climb/rung1/pile-slice1g.bin" "$D/corpus"
cp --reflink=never "$root/climb/rung1/pile-slice1g.bin" "$D/oracle-corpus"
python3 gates/private_cold.py "$D/corpus" "$D/oracle-corpus"
run=(systemd-run --user --quiet --collect --wait --pipe -p MemorySwapMax=0 -p CPUQuota=100% -p Nice=10)
meter=(/usr/bin/time -f 'wall_s=%e peak_rss_kib=%M exit=%x')
"${run[@]}" -p MemoryMax=192M "${meter[@]}" -o "$D/pack.time" "$T/overlap_lexparse" "$D/corpus" "$D/columns" --packed >"$D/pack.log"
[[ ! -e "$D/columns/dictionary.refs" && ! -e "$D/columns/dict-adj.u32" ]]
# Check the physical ID string byte-for-byte against the separately banked run.
cmp "$D/columns/parse.u32" "$root/sandbox/dict-adj-kIPyZL/columns/parse.u32"
python3 gates/private_cold.py "$D/columns/parse.u32" "$D/columns/dictionary.packed"
"${run[@]}" -p MemoryMax=3G "${meter[@]}" -o "$D/decode.time" "$T/overlap_packed_check" decode "$D/columns" "$D/stop.requested" >"$D/decode.log"
"${run[@]}" -p MemoryMax=2G "${meter[@]}" -o "$D/oracle.time" "$T/overlap_packed_check" oracle "$D/oracle-corpus" "$D/stop.requested" >"$D/oracle.log"
cmp <(grep '^CHECK ' "$D/decode.log") <(grep '^CHECK ' "$D/oracle.log")
grep -q '^CHECK n=1000000665 phrases=31784026 ' "$D/oracle.log"
printf 'PASS parse.u32 identical; packed-only reconstruction, overlap, all occurrence-boundary predecessors plus END verified; no container attempted\n' |tee "$D/verdict.log"
