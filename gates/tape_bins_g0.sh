#!/bin/bash
# G0 (TAPE_BINS.md) - GUARDS BEFORE GATES, ONE FIXTURE AT A TIME.
#
# Runs DETACHED (setsid nohup by the caller); every step appends to the
# journal file so a re-read after any death shows exactly where it stopped.
# Smallest fixture first: when the pathological fixture finds the runaway,
# the journal names the bin, class, round and bytes at the moment of
# refusal (the in-process guards), not the kernel OOM killer.
#
# The oracle is brute_sa (gates/brute_sa.cpp): a standalone memory-honest
# comparison sort. The old python oracle materialized c[p:] keys for every
# position at once - O(N^2) bytes, the box OOM at 2026-11-17 22:28.
# The compare STREAMS both files (no materialization).
#
# MEM_GUARD is set LOW on purpose: the fixtures are 6 MB; 16 GiB of anon
# is already 2600x the corpus and any crossing is a runaway by definition.
set -u
BIN=${BIN:-/tmp/bins_sort}
BRUTE=${BRUTE:-/tmp/brute_sa}
FIX=${FIX:-/tmp/tr_p3/fixtures}
GEN=${GEN:-/tmp/parse_gate_adv}
RD=${RD:-/mnt/nvme2n1/erikg/TAPEREADER/bins-g0}
J=$RD/JOURNAL.txt
mkdir -p "$RD"
: > "$J"

say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$J"; }

# one fixture: producer (journals everything) then the brute oracle + the
# streaming compare. XSA_WAVEKEY_VERIFY=1 runs the in-process brute guard
# per bin as a second belt.
run_one() {
  local name=$1 corpus=$2; shift 2
  local rd="$RD/$name"
  rm -rf "$rd"; mkdir -p "$rd/bands" "$rd/merged"
  local log="$rd/bins_sort.log"
  say "START name=$name corpus=$corpus bytes=$(stat -c %s "$corpus" 2>/dev/null)"
  env XSA_MEM_GUARD_MB=16384 "$@" TRIE_TAP=1 XSA_WAVEKEY_VERIFY=1 \
    SS_LCP_SLICE_WINDOW=4096 CROSS_V2_PIECE_WINDOW=1000000 \
    "$BIN" --corpus "$corpus" --out-prefix "$rd/merged/frag" --bands "$rd/bands" \
    --budget-bytes 16000000000 --threads 8 \
    > "$log" 2>&1
  local rc=$?
  if [ $rc -ne 0 ]; then
    say "GATE_FAIL name=$name rc=$rc producer_tail:"
    tail -4 "$log" | sed 's/^/  /' | tee -a "$J"
    G0_FAILS=$((G0_FAILS+1)); G0_RESULTS="$G0_RESULTS $name:rc$rc"
    return
  fi
  if ! grep -q "^SS_DONE " "$log"; then
    say "GATE_FAIL name=$name rc=0 reason=NO_SS_DONE_VOID"
    tail -4 "$log" | sed 's/^/  /' | tee -a "$J"
    G0_FAILS=$((G0_FAILS+1)); G0_RESULTS="$G0_RESULTS $name:VOID"
    return
  fi
  local rows rchar parses rel postlive
  rows=$(grep -o 'n=[0-9]*' "$log" | tail -1 | cut -d= -f2)
  rchar=$(grep -o 'rchar=[0-9]*' "$log" | tail -1 | cut -d= -f2)
  parses=$(grep -c "PARSE_CLASS" "$log")
  rel=$(grep -o 'prefix_relations=[0-9]*' "$log" | tail -1 | cut -d= -f2)
  postlive=$(grep -c "POST_RUN_LIVE bytes=0" "$log")
  say "SS_DONE_OK name=$name rows=$rows rchar=$rchar parse_classes=$parses prefix_rel=${rel:-0} post_run_live_zero=$postlive"
  # the independent reference + the streaming compare
  "$BRUTE" "$corpus" "$rd/ref.sa" >> "$J" 2>&1 || { say "GATE_FAIL name=$name rc=brute"; G0_FAILS=$((G0_FAILS+1)); G0_RESULTS="$G0_RESULTS $name:brute"; return; }
  python3 - "$rd" "$rd/ref.sa" <<'PY' >> "$J" 2>&1
import struct, sys, glob, os
rundir, refp = sys.argv[1], sys.argv[2]
ref = open(refp, 'rb').read()
bands = sorted(glob.glob(rundir + '/bands/band_*.sa'),
               key=lambda x: int(os.path.basename(x).split('_')[1].split('.')[0]))
# STREAM: never hold both sides - the reference streams once, the bands
# stream in order, compared row by row
refv = struct.iter_unpack('<Q', ref)
bi = 0
mism = 0
total = 0
reflen = len(ref) // 8
for f in bands:
    d = open(f, 'rb').read()
    for (w,) in struct.iter_unpack('<Q', d):
        if bi >= reflen:
            mism += 1
            continue
        (r,) = next(refv)   # iter_unpack yields TUPLES - a scalar compare
        if (w & (2**48 - 1)) != r:   # mismatched every row (the spurious
                                     # 7-fixture ORACLE FAIL was this line)
            if mism < 5:
                print(f"MISMATCH at {bi}: band={w & (2**48-1)} ref={r}")
            mism += 1
        bi += 1
total = bi
print(f"ORACLE {'OK' if mism == 0 and total == reflen else 'FAIL'}"
      f" rows={total} expected={reflen} mismatches={mism}")
PY
  if tail -1 "$J" | grep -q "ORACLE OK"; then
    say "GATE name=$name PASS"
    G0_RESULTS="$G0_RESULTS $name:PASS(rows=$rows,parse=$parses,rel=${rel:-0})"
  else
    say "GATE name=$name ORACLE_FAIL"
    G0_FAILS=$((G0_FAILS+1)); G0_RESULTS="$G0_RESULTS $name:ORACLE"
  fi
}

G0_FAILS=0; G0_RESULTS=""
# smallest first: the adversarial collision fixture (20 KB), then the 6 MB
# fixtures one at a time
# The adversarial collision fixture: the parse is forced on so the
# modality slide runs over the disputed prefix-relation classes and
# journals them. The knobs are chosen so BOTH budgeted terms fit: the
# census (rows x (cmp_len+delta) > budget) fires at delta_start=4096 for
# the 4-row classes, and the ids plane (rows x B x 4 + rows x 4 = 1040 B)
# fits the 8 KiB budget - with PLANE_MAX=1024 the new ids-plane guard
# would refuse every class and nothing would be journaled.
"$GEN" "$RD/adv.bin" 20000 >> "$J" 2>&1 || say "GATE_FAIL name=adv rc=gen"
run_one adv "$RD/adv.bin" XSA_PARSE_PLANE_MAX=8192 XSA_EXPAND_DELTA_START=4096
for f in docsep eofrun internsep mixed period2 r0 single; do
  run_one "$f" "$FIX/$f.txt"
done
say "SUMMARY fails=$G0_FAILS results=$G0_RESULTS"
[ $G0_FAILS -eq 0 ] || exit 1
