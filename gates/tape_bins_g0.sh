#!/bin/bash
# G0 (TAPE_BINS.md): 7 fixtures + the parse-collision adversarial case, each
# vs the brute-force SA oracle, 0 mismatches, prefix_relations journaled.
# No timeout crutch: the in-process guards (step counters, MEM_GUARD) make
# silent spins and unbounded growth impossible; the harness kills only on
# journal quietness (a slow-but-progressing run finishes and reports its
# true wall). A run without SS_DONE is VOID.
set -u
FIX=${FIX:-/tmp/tr_p3/fixtures}
BIN=${BIN:-/tmp/bins_sort}
GEN=${GEN:-/tmp/parse_gate_adv}
QUIET_S=${QUIET_S:-180}
RD=${RD:-/tmp/bins_g0}
mkdir -p "$RD"
fails=0; total_rows=0; results=""

oracle() {
  python3 - "$1" "$2" <<'PY'
import struct, sys, glob, os
corpus, rundir = sys.argv[1], sys.argv[2]
c=open(corpus,'rb').read(); N=len(c)+1
suf=sorted(range(N), key=lambda p: c[p:]+b'\x00')
sa=[]
for f in sorted(glob.glob(rundir+'/band_*.sa'),
                key=lambda x:int(os.path.basename(x).split('_')[1].split('.')[0])):
    d=open(f,'rb').read()
    for i in range(0,len(d),8):
        w=struct.unpack('<Q',d[i:i+8])[0]
        sa.append(w & (2**48-1))
if len(sa)!=N:
    print(f"ORACLE_FAIL rows={len(sa)} expected={N}"); raise SystemExit
bad=sum(1 for i,p in enumerate(sa) if p!=suf[i])
print(f"ORACLE_OK rows={N} mismatches={bad}" if bad==0
      else f"ORACLE_FAIL rows={N} mismatches={bad}")
PY
}

run_one() {
  local name=$1 corpus=$2; shift 2
  local rd="$RD/$name"
  rm -rf "$rd"; mkdir -p "$rd/bands" "$rd/merged"
  local log="$rd/bins_sort.log"
  env "$@" TRIE_TAP=1 SS_LCP_SLICE_WINDOW=4096 CROSS_V2_PIECE_WINDOW=1000000 \
    "$BIN" --corpus "$corpus" --out-prefix "$rd/merged/frag" --bands "$rd/bands" \
    --budget-bytes 16000000000 --threads 8 \
    > "$log" 2>&1 &
  local pid=$!
  local kill_reason=clean last_size=-1 last_change now start
  start=$(date +%s); last_change=$start
  while kill -0 $pid 2>/dev/null; do
    sleep 5
    local sz=$(stat -c %s "$log" 2>/dev/null || echo 0)
    now=$(date +%s)
    if [ "$sz" != "$last_size" ]; then last_size=$sz; last_change=$now; fi
    if [ $((now - last_change)) -ge $QUIET_S ]; then
      kill_reason="watchdog: journal quiet ${QUIET_S}s"
      kill -9 $pid 2>/dev/null; break
    fi
  done
  wait $pid; local rc=$?
  local wall=$(( $(date +%s) - start ))
  if [ "$kill_reason" != clean ]; then
    echo "GATE_FAIL name=$name rc=KILLED wall=${wall}s reason=$kill_reason"
    tail -1 "$log" | cut -c1-200
    fails=$((fails+1)); results="$results $name:KILLED"; return
  fi
  if [ $rc -ne 0 ]; then
    echo "GATE_FAIL name=$name rc=$rc wall=${wall}s"
    tail -3 "$log" | cut -c1-200
    fails=$((fails+1)); results="$results $name:rc$rc"; return
  fi
  if ! grep -q "^SS_DONE " "$log"; then
    echo "GATE_FAIL name=$name rc=0 wall=${wall}s reason=NO_SS_DONE_VOID"
    fails=$((fails+1)); results="$results $name:VOID"; return
  fi
  local orc=$(oracle "$corpus" "$rd")
  local n=$(grep -c "PARSE_CLASS" "$log")
  local rel=$(grep -o 'prefix_relations=[0-9]*' "$log" | tail -1 | cut -d= -f2)
  local rows=$(grep -o 'n=[0-9]*' "$log" | tail -1 | cut -d= -f2)
  local rchar=$(grep -o 'rchar=[0-9]*' "$log" | tail -1 | cut -d= -f2)
  total_rows=$((total_rows + rows))
  echo "GATE name=$name rc=0 wall=${wall}s parse_classes=$n prefix_rel=$rel rchar=$rchar $orc"
  case "$orc" in *ORACLE_OK*) ;; *) fails=$((fails+1)); results="$results $name:ORACLE"; return;; esac
  results="$results $name:ok(wall=${wall}s,rows=$rows,parse=$n,rel=$rel)"
}

for f in docsep eofrun internsep mixed period2 r0 single; do
  run_one "$f" "$FIX/$f.txt"
done
# THE ADVERSARIAL PREFIX-RELATION FIXTURE (constructed): the parse is forced
# on (XSA_PARSE_PLANE_MAX is a knob, journaled) so the modality slide runs
# over the disputed prefix-relation classes and journals them.
"$GEN" "$RD/adv.bin" 20000 || { echo "GATE_FAIL name=adv rc=gen"; fails=$((fails+1)); }
run_one adv "$RD/adv.bin" XSA_PARSE_PLANE_MAX=1024

echo "SUMMARY fixtures_rows=$total_rows fails=$fails$results"
[ $fails -eq 0 ] || exit 1
