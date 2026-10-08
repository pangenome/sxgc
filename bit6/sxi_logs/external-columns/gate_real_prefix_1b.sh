#!/usr/bin/env bash
# GATE (c) REPLACEMENT — REAL PILE BYTES: the external finish chain on a real
# 1 GB prefix of /mnt/nvme2n1/erikg/pile.txt (the supervisor-cut raw prefix
# /home/erikg/sxgc-piletest/pile-1b.txt, n=1,000,000,000,
# sha256=3fcee0e8...; NO manipulation).
#
# Order of operations, per directive:
#   0. AS-IS: run chunk_frontend on the RAW cut unchanged. The frontend
#      requires 0x1e-terminated input; a mid-document cut must FAIL LOUD.
#      The refusal is recorded as a FINDING, not repaired quietly.
#   1. SNAP (loudly reported): cut to the first 0x1e document boundary
#      at-or-after 1e9 bytes; sha256 recorded; chain runs on that.
#   2. chunk_frontend -> cross_lcp_merge --tree --emit-pf (real merge
#      sidecars for the adopt-mode lever).
#   3. EXTERNAL finish under a hard RLIMIT_AS of 8 GiB (the resident path
#      needs ~19-33 GB at this scale): endpoints -> slim -> sweep.
#   4. RESIDENT reference chain (uncapped; fits this box's RAM) on the same
#      merged files: byte-identity of ri4/head_sa/agg/sA + chi equality.
#   5. ADOPT chain under the same cap (the merge-time sidecars replace both
#      walks): byte-identity vs the walk-mode outputs.
# Per-phase RSS/disk/wall telemetry throughout. Real corpus bytes are only
# ever READ (from the pile prefix); all outputs land under /tmp/extcols.
set -uo pipefail
BIN=/tmp/extcols/bin
BASE=/tmp/extcols/base
XSA=/home/erikg/sxgc/xsa/target/release/xsa
CF=/tmp/slimpf/chunk_frontend
# Production byte-remap (REQUIRED for raw pile bytes: the pile contains literal
# 0x01/0x02/0x03/0x04 bytes that collide with the cyclic padding dollar 0x02;
# the same 256-byte map the banked fragment route used, verified collision-free
# for both prefixes: no 0xFC-0xFF bytes occur in either cut).
REMAP=/home/erikg/sxgc/vendor/chunk-merge-v3/merged/frag.remap
RAW=/home/erikg/sxgc-piletest/pile-1b.txt
W=/tmp/extcols/real1b
N_RAW=1000000000
CAP_KB=${CAP_KB:-8388608}
CHUNKS=16
mkdir -p "$W" "$W/chunks-raw" "$W/chunks" "$W/merged" "$W/mwork"
sample_rss() { local ppid=$1 tag=$2
    while kill -0 "$ppid" 2>/dev/null; do
        for pid in $(pgrep -P "$ppid" 2>/dev/null); do
            local rss=$(awk '/VmRSS/{print $2}' /proc/$pid/status 2>/dev/null)
            [ -n "$rss" ] && echo "$(date +%s) $rss" >> "$W/$tag.rss"
        done
        sleep 2
    done
}
run_phase() { local tag=$1 log=$2; shift 2
    echo "=== $tag start $(date +%s) cap_kb=$(ulimit -v)" >> "$W/phases.txt"
    /usr/bin/time -v -o "$W/$tag.time" "$@" > "$log" 2>&1 &
    local pid=$!; sample_rss "$pid" "$tag" & local spid=$!
    wait "$pid"; local rc=$?; kill "$spid" 2>/dev/null
    echo "=== $tag end $(date +%s) rc=$rc" >> "$W/phases.txt"
    return $rc
}

echo "########## 0. AS-IS raw cut: expect fail-loud at chunk_frontend"
run_phase chunk-raw "$W/chunk-raw.log" "$CF" "$RAW" "$CHUNKS" "$W/chunks-raw"
RC=$?
echo "RAW_CUT_CHUNK_RC=$RC (expected nonzero: mid-document cut)"
tail -2 "$W/chunk-raw.log"
if [ $RC -eq 0 ]; then echo "UNEXPECTED: raw cut accepted"; fi

echo "########## 1. snap to the first 0x1e at-or-after $N_RAW (loud, recorded)"
python3 - /mnt/nvme2n1/erikg/pile.txt "$N_RAW" "$W/pile-1b-snap.txt" <<'PY'
import sys, hashlib
# The snap extends the RAW 1 GiB cut INTO THE PILE (read-only) to the first
# 0x1e document boundary at-or-after offset 1e9; the pile file is only read,
# and the first 1e9 bytes are verified to BE the raw cut (sha256 pin).
pile, n0, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
f = open(pile, 'rb')
f.seek(n0)
CH = 1 << 20
extra = 0
while True:
    b = f.read(CH)
    if not b: raise SystemExit('no 0x1e found after offset')
    i = b.find(b'\x1e')
    if i >= 0:
        extra = i + 1
        break
    extra += len(b)
snap_n = n0 + extra
h0 = hashlib.sha256()
f.seek(0); done = 0
while done < n0:
    b = f.read(min(1 << 24, n0 - done))
    h0.update(b); done += len(b)
assert h0.hexdigest() == '3fcee0e868ee061fd62b5ce82515457d170764262b4b4557d5dd815f02222a06', 'pile prefix != raw-cut sha'
f.seek(0)
g = open(out, 'wb')
done = 0
while done < snap_n:
    b = f.read(min(1 << 24, snap_n - done))
    if not b: raise SystemExit('short prefix')
    g.write(b); done += len(b)
g.close()
import hashlib
h = hashlib.sha256()
g = open(out, 'rb')
while True:
    b = g.read(1 << 24)
    if not b: break
    h.update(b)
print(f'SNAP n={snap_n} (+{snap_n-n0} bytes past 1e9) sha256={h.hexdigest()}')
PY
[ $? -eq 0 ] || { echo SNAP_FAIL; exit 1; }
N=$(stat -c %s "$W/pile-1b-snap.txt")

echo "########## 2. chunk_frontend + merge --tree --emit-pf (real sidecars)"
run_phase chunk "$W/chunk.log" "$CF" "$W/pile-1b-snap.txt" "$CHUNKS" "$W/chunks" "$REMAP" || { echo CHUNK_FAIL; exit 1; }
run_phase merge "$W/merge.log" \
    "$BIN/cross_lcp_merge" --tree "$W/chunks" "$CHUNKS" "$N" "$W/merged/frag" \
    --threads 24 --work "$W/mwork" --emit-pf || { echo MERGE_FAIL; exit 1; }
grep CROSS_PF_EMIT "$W/merge.log" | tail -1
grep -E "Elapsed|Maximum resident" "$W/merge.time" | grep -v aver

echo "########## 3. EXTERNAL finish under RLIMIT_AS ${CAP_KB} KiB"
( ulimit -v "$CAP_KB"
  run_phase ext-endpoints "$W/ext-endpoints.log" \
      "$BIN/rpfbwt_endpoints" "$W/merged/frag" "$W/ext.ri4" "$W/ext.head_sa" \
  && run_phase ext-slim "$W/ext-slim.log" \
      "$BIN/slim_dump" --slim --resolve-ri4 --ri4 "$W/ext.ri4" --head-sa "$W/ext.head_sa" -t 48 -o "$W/ext.agg" \
  && run_phase ext-sweep "$W/ext-sweep.log" \
      "$XSA" chi-rspace --stream-agg --ri4 "$W/ext.ri4" --agg "$W/ext.agg" -o "$W/ext.sA"
) || { echo EXT_CHAIN_FAIL; exit 1; }
grep ENDPOINT_PASS "$W/ext-endpoints.log" | head -1
grep -oE "chi = [0-9]+ \(N=[0-9]+, R=[0-9]+\)" "$W/ext-sweep.log"

echo "########## 4. RESIDENT reference chain (uncapped) on the same merged files"
run_phase res-endpoints "$W/res-endpoints.log" \
    "$BASE/rpfbwt_endpoints" "$W/merged/frag" "$W/res.ri4" "$W/res.head_sa" || { echo RES_FAIL; exit 1; }
run_phase res-slim "$W/res-slim.log" \
    "$BASE/slim_dump" --slim --resolve-ri4 --ri4 "$W/res.ri4" --head-sa "$W/res.head_sa" -t 48 -o "$W/res.agg" || { echo RES_FAIL; exit 1; }
run_phase res-sweep "$W/res-sweep.log" \
    "$XSA" chi-rspace --stream-agg --ri4 "$W/res.ri4" --agg "$W/res.agg" -o "$W/res.sA" || { echo RES_FAIL; exit 1; }

echo "########## 5. cross-checks: external vs resident"
for f in ri4 head_sa agg sA; do
    cmp "$W/ext.$f" "$W/res.$f" && echo "REAL1B_${f^^}_BYTE_IDENTICAL_EXTERNAL_VS_RESIDENT" \
        || { echo "REAL1B_$f MISMATCH"; exit 1; }
done
echo "chi external: $(grep -oE 'chi = [0-9]+' "$W/ext-sweep.log")  resident: $(grep -oE 'chi = [0-9]+' "$W/res-sweep.log")"

echo "########## 6. ADOPT chain (merge sidecars, both walks skipped) under the cap"
( ulimit -v "$CAP_KB"
  run_phase adopt-endpoints "$W/adopt-endpoints.log" \
      "$BIN/rpfbwt_endpoints" "$W/merged/frag" "$W/adopt.ri4" "$W/adopt.head_sa" \
      --pf-text "$W/merged/frag.pftext" --pf-checkpoints "$W/merged/frag.pfck"
  run_phase adopt-slim "$W/adopt-slim.log" \
      "$BIN/slim_dump" --slim --resolve-ri4 --ri4 "$W/ext.ri4" --head-sa "$W/ext.head_sa" -t 48 \
      --pf-text "$W/merged/frag.pftext" --pf-checkpoints "$W/merged/frag.pfck" -o "$W/adopt.agg"
) || { echo ADOPT_FAIL; exit 1; }
cmp "$W/adopt.ri4" "$W/ext.ri4" && cmp "$W/adopt.head_sa" "$W/ext.head_sa" && echo "REAL1B_ADOPT_RI4_HEADSA_BYTE_IDENTICAL"
cmp "$W/adopt.agg" "$W/ext.agg" && echo "REAL1B_ADOPT_AGG_BYTE_IDENTICAL"

echo "########## telemetry"
for t in chunk merge ext-endpoints ext-slim ext-sweep res-endpoints res-slim res-sweep adopt-endpoints adopt-slim; do
    s=$(grep -E "Elapsed \(wall" "$W/$t.time" 2>/dev/null | grep -oE "[0-9:.]+$")
    r=$(grep -E "Maximum resident" "$W/$t.time" 2>/dev/null | grep -oE "[0-9]+$")
    echo "PHASE $t wall=$s peak_rss_kb=$r"
done
echo REAL1B_GATE_DONE
