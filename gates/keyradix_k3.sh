#!/bin/bash
# K3, resized to FINISH, with NO TIMEOUT anywhere.
#
# Each arm: posix_fadvise(POSIX_FADV_DONTNEED) on the corpus, then the sorter,
# untouched, to its own exit. The receipt names its own denominator - bands the
# sorter actually completed, the rows in them, the bytes the kernel says it read -
# so a truncated run is impossible to mistake for a measurement (and no run here
# can be truncated: there is no timeout to truncate it).
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-/mnt/nvme2n1/erikg/KEYRADIX}
cd "$WORK"
OUT=$WORK/k3sized.receipt
: > "$OUT"
echo "K3_POLICY timeout=none every run ends by its own exit; a deliberate stop is logged as void" >> "$OUT"
arm() { # tag bin corpus
  local tag=$1 bin=$2 corpus=$3
  local d=$WORK/k3sized/$tag
  rm -rf "$d"; mkdir -p "$d/scratch"
  echo "K3_MEM tag=$tag $(grep -E 'MemTotal|MemAvailable|^Cached' /proc/meminfo | tr -s ' ' | tr '\n' ' ')" >> "$OUT"
  $WORK/stage/fadvise "$corpus" || { echo "K3 tag=$tag VOID fadvise failed" >> "$OUT"; return; }
  local t0; t0=$(date +%s)
  setsid "$bin" --corpus "$corpus" --out "$d/out.sa" --lcp-out "$d/out.lcp" \
      --scratch "$d/scratch" --threads 12 --budget-bytes 48000000000 \
      --scratch-budget-bytes 60000000000 --memory-gb 48 --scratch-cap-gb 60 \
      > "$d/run.log" 2>&1 &
  local jpid=$!
  local spid=""
  for i in $(seq 1 240); do spid=$(pgrep -f -- "--out $d/out.sa" | head -1); [ -n "$spid" ] && break; sleep 0.5; done
  : > "$d/io.samples"
  while kill -0 "$jpid" 2>/dev/null; do
    [ -n "$spid" ] && [ -r "/proc/$spid/io" ] && awk '/^(rchar|read_bytes):/{printf "%s ", $2}' "/proc/$spid/io" >> "$d/io.samples" && echo >> "$d/io.samples"
    sleep 2
  done
  wait "$jpid"; local rc=$?
  local wall=$(( $(date +%s) - t0 ))
  local rows bands rchar rb
  rows=$(grep -o 'SS_BAND band=[0-9]* rows=[0-9]*' "$d/run.log" | sed 's/.*rows=//' | awk '{s+=$1} END{printf "%d", s+0}')
  bands=$(grep -c 'SS_BAND band=' "$d/run.log")
  rchar=$(tail -1 "$d/io.samples" 2>/dev/null | awk '{print $1}'); rchar=${rchar:-0}
  rb=$(tail -1 "$d/io.samples" 2>/dev/null | awk '{print $2}'); rb=${rb:-0}
  local done_line; done_line=$(grep -o 'SS_DONE.*' "$d/run.log" | tail -1)
  if [ "$rc" -ne 0 ] || [ -z "$done_line" ]; then
    echo "K3 tag=$tag VOID rc=$rc no-SS_DONE wall=${wall}s bands_completed=$bands rows_completed=$rows" >> "$OUT"
    return
  fi
  echo "K3 tag=$tag corpus=$corpus corpus_bytes=$(stat -c %s "$corpus") rc=0 completed=1 wall=${wall}s bands_completed=$bands rows_completed=$rows read_bytes=$rb rchar=$rchar read_bytes_per_row=$(awk -v b="$rb" -v r="$rows" 'BEGIN{if(r)printf "%.4f",b/r; else print "NA"}') rchar_per_row=$(awk -v b="$rchar" -v r="$rows" 'BEGIN{if(r)printf "%.1f",b/r; else print "NA"}') sa=$(sha256sum "$d/out.sa"|cut -d' ' -f1) $done_line" >> "$OUT"
}
arm base "$WORK/stage/base_scatter_sort" "$WORK/c3g.txt"
arm new  "$WORK/stage/scatter_sort"      "$WORK/c3g.txt"
arm base10g /tmp/pre-keyradix/scatter_sort /mnt/nvme2n1/erikg/pf-10gb.txt
echo K3SIZED_DONE >> "$OUT"
