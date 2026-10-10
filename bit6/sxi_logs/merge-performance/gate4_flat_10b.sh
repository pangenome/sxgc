#!/usr/bin/env bash
# MERGE PERFORMANCE lane - GATE 4: 10 GB FLAT k-way rebuild + byte-identity
# vs the banked 10 GB reference (real10b-ref; NEVER modified - the gate only
# reads it). One 32-way merge straight from referenced chunks through the
# consolidated `xsa build --input`, under the banked cap discipline
# (ulimit -v 24 GiB per phase, scratch on nvme2n1, df gates). This is the
# deliverable-table row that prices the pile at its I/O floor.
set -uo pipefail
W=/home/erikg/worktrees/sxgc/pi-worktree-721d9f35-fd8d-4651-85a3-3995854b08f7-s0-0
XSA=$W/xsa/target/release/xsa
SNAP=/mnt/nvme2n1/erikg/extcols-scratch/real10b-ref/pile-10b-snap.txt
SNAP_SHA=d440d9e1fe0aeb4c32ebeffb2df02dac009a0806510c2f864a04c2cd92d3864a
REF=/mnt/nvme2n1/erikg/extcols-scratch/real10b-ref
SCR=/mnt/nvme2n1/erikg/extcols-scratch/mergperf-10b-flat
J=$W/bit6/sxi_logs/merge-performance
CAP_GB=${CAP_GB:-24}
CHUNKS=${CHUNKS:-32}
KWAY=${KWAY:-32}
THREADS=${THREADS:-48}
TIMEOUT_S=${TIMEOUT_S:-86400}
mkdir -p "$J"
dfgate() { local d=$1; local free=$(df --output=pcent "$d" | tail -1 | tr -dc '0-9');
    [ "$free" -le 85 ] || { echo "DF_GATE_FAIL $d ${free}% used"; exit 1; }; }
dfgate /mnt/nvme2n1

echo "=== gate start $(date -Is) pid=$$ caps: ulimit -v ${CAP_GB} GiB, timeout ${TIMEOUT_S}s (10GB FLAT kway=${KWAY})"
echo "=== xsa provenance: $(sha256sum "$XSA" | cut -c1-16)... (bundle: sharded emission + referenced chunks + flat k-way + parallel anchors)"
S=$(sha256sum "$SNAP" | awk '{print $1}')
[ "$S" = "$SNAP_SHA" ] && echo "SNAP_OK $(stat -c %s "$SNAP") sha256=$S" || { echo SNAP_MISMATCH; exit 1; }
rm -rf "$SCR"; mkdir -p "$SCR"

timeout "$TIMEOUT_S" "$XSA" build --input "$SNAP" --scratch "$SCR" --snap-1e \
    --memory-gb "$CAP_GB" --threads "$THREADS" --chunks "$CHUNKS" --kway "$KWAY" \
    > "$J/gate4-10b.driver.log" 2>&1
rc=$?
echo "=== xsa build rc=$rc $(date -Is)"
[ "$rc" -ne 0 ] && { echo BUILD_FAIL; tail -20 "$J/gate4-10b.driver.log"; exit 1; }

echo "=== byte-identity vs the banked 10 GB reference"
pass=0; fail=0
for f in rlebwt rlebwt.meta ssa ssa_t pftext pfck; do
    if cmp "$SCR/merged/frag.$f" "$REF/merged/frag.$f"; then
        echo "TENGB_FLAT_MERGE_$f BYTE_IDENTICAL_TO_BANKED"; pass=$((pass+1))
    else echo "TENGB_FLAT_MERGE_$f MISMATCH"; fail=$((fail+1)); fi
done
for f in ri4 head_sa agg sA; do
    if cmp "$SCR/finish/frag.$f" "$REF/ext.$f"; then
        echo "TENGB_FLAT_FINISH_$f BYTE_IDENTICAL_TO_BANKED"; pass=$((pass+1))
    else echo "TENGB_FLAT_FINISH_$f MISMATCH"; fail=$((fail+1)); fi
done
[ "$fail" -eq 0 ] || { echo "BYTE_IDENTITY_FAIL pass=$pass fail=$fail"; exit 1; }
grep -h "chi = " "$SCR/sweep.log" | tail -1 | tee -a "$J/gate4-10b.cmp.log"
grep -qF 'chi = 2676929448' "$SCR/sweep.log" \
    && echo "TENGB_FLAT_CHI_2676929448_OK" || { echo CHI_CHECK; grep -oE "chi = [0-9]+" "$SCR/sweep.log"; }

echo "=== flat telemetry"
grep -E "CROSS_KWAY" "$SCR/merge.log" | tail -1 | tee -a "$J/gate4-10b.cmp.log"
grep -E "CROSS_PHASE|POOL_STATS|XMERG_TIMING" "$SCR/merge.log" | tee -a "$J/gate4-10b.cmp.log"
grep -E "^(XSA_BUILD START|SNAP_OK|PHASE .* (START|END)|MERGE cross|CROSS_PF_EMIT|PEAK_DISC|XSA_BUILD_DONE)" \
    "$SCR/xsa-build.log" | tee -a "$J/gate4-10b.cmp.log"
echo "DISC: chunk artifacts $(du -sb "$SCR/chunks" | cut -f1)B; scratch peak below"
df -h /mnt/nvme2n1 | tail -1
echo "MERGPERF_GATE4_DONE pass=$pass fail=$fail"
