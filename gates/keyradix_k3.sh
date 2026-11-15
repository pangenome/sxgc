#!/bin/bash
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd); WORK=${WORK:-/mnt/nvme2n1/erikg/KEYRADIX}; cd "$WORK"
CORPUS=${CORPUS:-/mnt/nvme2n1/erikg/pf-10gb.txt}
for pair in "new:$PWD/stage/scatter_sort" "base:/tmp/pre-keyradix/scatter_sort"; do
  tag=${pair%%:*}; bin=${pair##*:}
  d=$PWD/runs/k3cold_$tag
  rm -rf "$d"; mkdir -p "$d/scratch"
  $PWD/stage/fadvise "$CORPUS" || exit 1
  /usr/bin/time -f "K3_COLD_WALL tag=$tag %e s INBLOCK %I" \
    setsid timeout --kill-after=10 7200 "$bin" --corpus "$CORPUS" \
      --out "$d/out.sa" --lcp-out "$d/out.lcp" --scratch "$d/scratch" \
      --threads 12 --budget-bytes 48000000000 --scratch-budget-bytes 60000000000 \
      --memory-gb 48 --scratch-cap-gb 60 > "$d/run.log" 2>&1
  rows=$(grep -o 'SS_BAND band=[0-9]* rows=[0-9]*' "$d/run.log" | sed 's/.*rows=//' | awk '{s+=$1} END{printf "%d", s+0}')
  echo "K3_COLD tag=$tag rows=$rows $(grep -oE 'K3_COLD_WALL.*' "$d/run.log" | tail -1) sa=$(sha256sum "$d/out.sa" 2>/dev/null|cut -d' ' -f1)" >> $PWD/glogs/k3cold.out
done
echo K3COLD_DONE >> $PWD/glogs/k3cold.out
