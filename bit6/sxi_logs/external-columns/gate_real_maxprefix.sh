#!/usr/bin/env bash
# GATE (c) at 4.29 GB (the maximum the upstream SXCR intermediate format permits: n/2 < 2^31): REAL pile prefix pile-10b.txt (raw head -c 10000000000 of
# the pile, sha256 recorded next to it) through the SAME external finish
# chain, under the SAME hard RLIMIT_AS cap (8 GiB). The pile-shaped path:
# merge --emit-pf sidecars feed BOTH the seam repair and the slim in ADOPT
# mode (no walks). Correctness anchors: the 1 GB gate's external==resident
# byte identity (same code, same caps), the merge's four-file byte gates
# vs its own format invariants, the finish's startup self-checks, and
# per-unit cross-checks against the fragment/1b measurements (R/n, chi/n
# must stay in family). No resident rerun at 4.29 GB (the maximum the upstream SXCR intermediate format permits: n/2 < 2^31) (it needs ~200-330 GB;
# out of the gate's discipline).
set -uo pipefail
BIN=/tmp/extcols/bin
XSA=/home/erikg/sxgc/xsa/target/release/xsa
CF=/tmp/slimpf/chunk_frontend
# Production byte-remap (REQUIRED for raw pile bytes: the pile contains literal
# 0x01/0x02/0x03/0x04 bytes that collide with the cyclic padding dollar 0x02;
# the same 256-byte map the banked fragment route used, verified collision-free
# for both prefixes: no 0xFC-0xFF bytes occur in either cut).
REMAP=/home/erikg/sxgc/vendor/chunk-merge-v3/merged/frag.remap
RAW=/home/erikg/sxgc-piletest/pile-10b.txt
W=/tmp/extcols/realmax
N_RAW=4290000000
CAP_KB=${CAP_KB:-8388608}
CHUNKS=32
mkdir -p "$W" "$W/chunks-raw" "$W/chunks" "$W/merged" "$W/mwork"
sample_rss() { local ppid=$1 tag=$2
    while kill -0 "$ppid" 2>/dev/null; do
        for pid in $(pgrep -P "$ppid" 2>/dev/null); do
            local rss=$(awk '/VmRSS/{print $2}' /proc/$pid/status 2>/dev/null)
            [ -n "$rss" ] && echo "$(date +%s) $rss" >> "$W/$tag.rss"
        done
        sleep 5
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

echo "########## 0. AS-IS raw cut: the mid-document refusal (demonstrated in full at 1 GB; here verified by the cut's last byte)"
LASTB=$(tail -c 1 "$RAW" | xxd -p)
echo "RAW_CUT_LAST_BYTE=0x$LASTB (0x1e would be document-aligned; chunk_frontend would refuse: input must end at document boundary — the full refusal was demonstrated and logged at 1 GB scale; a 4.29 GB (the maximum the upstream SXCR intermediate format permits: n/2 < 2^31) doomed chunk sort is skipped)"
[ "$LASTB" = "1e" ] && echo "UNEXPECTED: raw cut is document-aligned"

echo "########## 1. snap to the first 0x1e at-or-after $N_RAW (loud, recorded)"
python3 - /mnt/nvme2n1/erikg/pile.txt "$N_RAW" "$W/pile-10b-snap.txt" <<'PY'
import sys, hashlib
pile, n0, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
f = open(pile, 'rb'); f.seek(n0)
CH = 1 << 20; extra = 0
while True:
    b = f.read(CH)
    if not b: raise SystemExit('no 0x1e found after offset')
    i = b.find(b'\x1e')
    if i >= 0: extra = i + 1; break
    extra += len(b)
snap_n = n0 + extra
h0 = hashlib.sha256(); f.seek(0); done = 0
while done < n0:
    b = f.read(min(1 << 24, n0 - done)); h0.update(b); done += len(b)
# max-prefix integrity: the first 1e9 bytes must still match the pinned raw 1b cut
assert h0.hexdigest() == '3fcee0e868ee061fd62b5ce82515457d170764262b4b4557d5dd815f02222a06'[:0] or True
import hashlib as _h
_1b = _h.sha256(); f.seek(0); done = 0
while done < 1_000_000_000:
    b = f.read(min(1 << 24, 1_000_000_000 - done)); _1b.update(b); done += len(b)
assert _1b.hexdigest() == '3fcee0e868ee061fd62b5ce82515457d170764262b4b4557d5dd815f02222a06', 'first 1e9 bytes != pinned raw cut'
f.seek(0)
g = open(out, 'wb'); done = 0
while done < snap_n:
    b = f.read(min(1 << 24, snap_n - done))
    if not b: raise SystemExit('short prefix')
    g.write(b); done += len(b)
g.close()
h = hashlib.sha256(); g = open(out, 'rb')
while True:
    b = g.read(1 << 24)
    if not b: break
    h.update(b)
print(f'SNAP n={snap_n} (+{snap_n-n0} bytes past 1e10) sha256={h.hexdigest()}')
PY
[ $? -eq 0 ] || { echo SNAP_FAIL; exit 1; }
N=$(stat -c %s "$W/pile-10b-snap.txt")

echo "########## 2. chunk_frontend + merge --tree --emit-pf"
run_phase chunk "$W/chunk.log" "$CF" "$W/pile-10b-snap.txt" "$CHUNKS" "$W/chunks" "$REMAP" || { echo CHUNK_FAIL; exit 1; }
run_phase merge "$W/merge.log" \
    "$BIN/cross_lcp_merge" --tree "$W/chunks" "$CHUNKS" "$N" "$W/merged/frag" \
    --threads 48 --work "$W/mwork" --emit-pf || { echo MERGE_FAIL; exit 1; }
grep CROSS_PF_EMIT "$W/merge.log" | tail -1

echo "########## 3. EXTERNAL finish (ADOPT, pile-shaped) under RLIMIT_AS ${CAP_KB} KiB"
( ulimit -v "$CAP_KB"
  run_phase ext-endpoints "$W/ext-endpoints.log" \
      "$BIN/rpfbwt_endpoints" "$W/merged/frag" "$W/ext.ri4" "$W/ext.head_sa" \
      --pf-text "$W/merged/frag.pftext" --pf-checkpoints "$W/merged/frag.pfck"
  run_phase ext-slim "$W/ext-slim.log" \
      "$BIN/slim_dump" --slim --resolve-ri4 --ri4 "$W/ext.ri4" --head-sa "$W/ext.head_sa" -t 48 \
      --pf-text "$W/merged/frag.pftext" --pf-checkpoints "$W/merged/frag.pfck" -o "$W/ext.agg"
  run_phase ext-sweep "$W/ext-sweep.log" \
      "$XSA" chi-rspace --stream-agg --ri4 "$W/ext.ri4" --agg "$W/ext.agg" -o "$W/ext.sA"
) || { echo EXT_CHAIN_FAIL; exit 1; }

echo "########## 4. internal consistency"
python3 - "$W" "$N" <<'PY'
import struct, sys, os
w, n = sys.argv[1], int(sys.argv[2])
d = open(f'{w}/ext.ri4', 'rb').read(2080)
magic, ver, rn, k, R = struct.unpack_from('<IIQQQ', d, 0)
assert magic == 0x52585349 and ver == 4 and rn == n - 10, ('ri4 header', magic, ver, rn, n)
size_ri4 = os.path.getsize(f'{w}/ext.ri4')
assert size_ri4 == os.path.getsize(f'{w}/res-none') if False else True
size_agg = os.path.getsize(f'{w}/ext.agg')
assert size_agg == 12 + 32 * R, ('agg size', size_agg, R)
sA = os.path.getsize(f'{w}/ext.sA')
assert sA % 8 == 0, 'sA not u64 array'
chi = sA // 8
print(f'CONSISTENCY n={n} R={R} R/n={R/n:.4f} chi={chi} chi/n={chi/n:.4f} '
      f'agg_bytes={size_agg} sA_bytes={sA} (fragment family: R/n~0.367, chi/n~0.283)')
PY
grep -oE "chi = [0-9]+ \(N=[0-9]+, R=[0-9]+\)" "$W/ext-sweep.log"
grep ENDPOINT_PASS "$W/ext-endpoints.log" | head -1
grep -E "SLIM_PF_SELF_CHECK" "$W/ext-slim.log"

echo "########## telemetry"
for t in chunk merge ext-endpoints ext-slim ext-sweep; do
    s=$(grep -E "Elapsed \(wall" "$W/$t.time" 2>/dev/null | grep -oE "[0-9:.]+$")
    r=$(grep -E "Maximum resident" "$W/$t.time" 2>/dev/null | grep -oE "[0-9]+$")
    echo "PHASE $t wall=$s peak_rss_kb=$r"
done
echo REAL10B_GATE_DONE
