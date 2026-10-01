#!/usr/bin/env bash
# Supervisor-owned chunk-merge v3 gate: chunks -> batch-BCR merge -> monolithic reference -> four-file + chi gates -> table.
# Rebuilt from the reaped lane's banked scripts/commands. Runs immune to any lane sandbox teardown.
set -euo pipefail
cd /home/erikg/sxgc
J=bit6/sxi_logs/chunk-merge-v3
V=vendor/chunk-merge-v3
T=/home/erikg/.cache/xsa/81c4a8c0271c4096ec4c983e33b22ba23ccdfaac12bdc5db9a5bf1b5ef867f28
SRC=/home/erikg/sxgc-piletest/pile-frag.txt
mkdir -p "$V/reference" "$V/chunks" "$V/merged"
echo "[adopt] building binaries $(date -u +%H:%M:%S)"
cc -O3 -I bit6/third_party/libsais -c bit6/third_party/libsais/token-libsais.c -o /tmp/cm3_libsais.o
c++ -O3 -std=c++17 bit6/chunk_frontend.cpp /tmp/cm3_libsais.o -o "$V/chunk_frontend_buffered"
c++ -O3 -std=c++17 bit6/chunk_bcr_merge.cpp -o "$V/chunk_bcr_merge"
echo "[adopt] stage 1: monolithic reference (parse, l2, rpfbwt) $(date -u +%H:%M:%S)"
/usr/bin/time -v -o "$J/monolithic-parse.time" "$T/pfp++" -t "$SRC" -o "$V/reference/parse" -w 10 -p 100 -j 8 --tmp-dir "$V/reference" > "$J/monolithic-parse.log" 2>&1
/usr/bin/time -v -o "$J/monolithic-parse-l2.time" "$T/pfp++" -i "$V/reference/parse.parse" -w 5 -p 11 -j 8 --tmp-dir "$V/reference" > "$J/monolithic-parse-l2.log" 2>&1
/usr/bin/time -v -o "$J/monolithic-rpfbwt.time" "$T/rpfbwt" --l1-prefix "$V/reference/parse" --w1 10 --w2 5 --threads 8 --chunks 50 --tmp-dir "$V/reference" > "$J/monolithic-rpfbwt.log" 2>&1
echo "[adopt] stage 2: 16 chunks $(date -u +%H:%M:%S)"
/usr/bin/time -v -o "$J/chunk-sort.time" "$V/chunk_frontend_buffered" "$SRC" 16 "$V/chunks" "$V/reference/parse.remap" > "$J/chunk-sort.log" 2>&1
python3 bit6/sxi_logs/chunk-merge-v2/verify_chunks.py "$V/chunks" 16 1082130213 > "$J/chunk-header-gate.log" 2>&1 || { echo "[adopt] CHUNK HEADER GATE FAILED"; exit 1; }
echo "[adopt] stage 3: batch-BCR merge (16 batches, ~5.6s/batch-MB) $(date -u +%H:%M:%S)"
/usr/bin/time -v -o "$J/merge.time" "$V/chunk_bcr_merge" "$V/chunks" 16 1082130213 "$V/merged/frag" > "$J/merge.log" 2>&1
echo "[adopt] stage 4: finish gate (four-file cmp + endpoints + slim + chi) $(date -u +%H:%M:%S)"
bash "$J/finish_gate.sh" > "$J/finish-gate.log" 2>&1 || { echo "[adopt] FINISH GATE FAILED - see $J/finish-gate.log"; exit 1; }
python3 "$J/summarize.py" > "$J/summarize.log" 2>&1 || { echo "[adopt] SUMMARIZE FAILED"; exit 1; }
echo "[adopt] ALL GATES COMPLETE $(date -u +%H:%M:%S)"
cat "$J/four-file-gate.log" 2>/dev/null
tail -3 "$J/finish-gate.log" 2>/dev/null
