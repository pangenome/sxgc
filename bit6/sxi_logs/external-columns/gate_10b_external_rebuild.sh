#!/usr/bin/env bash
# 10 GB EXTERNALIZED REBUILD + BYTE-IDENTITY GATE (mandatory items b+c).
# Rebuilds the 10 GB prefix end-to-end with the EXTERNALIZED merge
# (windowed columns + sync-fill pool, serial-cold tree) under a HARD
# ulimit -v 24 GiB — a cap the old resident code could not survive (it
# peaked 332 GB on this input) — and requires byte-identity of every
# produced artifact against the banked 10 GB reference (moved to nvme2n1).
# Disc discipline: scratch on nvme2n1; df gates (>=15% free) before each
# phase; root untouched except for the repo.
set -uo pipefail
BIN=/tmp/extcols/bin
BIN3=/tmp/extcols/bin3
XSA=/home/erikg/sxgc/xsa/target/release/xsa
REF=/mnt/nvme2n1/erikg/extcols-scratch/real10b-ref
SCR=/mnt/nvme2n1/erikg/extcols-scratch/real10b-ext
REMAP=/tmp/extcols/remap-10b.bin
SNAP=$REF/pile-10b-snap.txt
N=10000017258
SNAP_SHA=d440d9e1fe0aeb4c32ebeffb2df02dac009a0806510c2f864a04c2cd92d3864a
CAP_GB=${CAP_GB:-24}
CHUNKS=32

dfgate() { local d=$1; local free=$(df --output=pcent "$d" | tail -1 | tr -dc '0-9');
    [ "$free" -le 85 ] || { echo "DF_GATE_FAIL $d ${free}% used"; exit 1; }; }
dfgate /mnt/nvme2n1

# 0. snap integrity (never re-cut: the banked reference IS this snap)
S=$(sha256sum "$SNAP" | awk '{print $1}')
[ "$S" = "$SNAP_SHA" ] && echo "SNAP_OK n=$N sha256=$S" || { echo SNAP_MISMATCH; exit 1; }

rm -rf "$SCR"; mkdir -p "$SCR/chunks" "$SCR/merged" "$SCR/mwork" "$SCR/finish"
echo "########## 1. chunk_frontend ($CHUNKS chunks, production 10b remap)"
dfgate /mnt/nvme2n1
/usr/bin/time -v -o "$SCR/chunk.time" "$BIN/chunk_frontend" "$SNAP" "$CHUNKS" "$SCR/chunks" "$REMAP" \
    > "$SCR/chunk.log" 2>&1 || { echo CHUNK_FAIL; tail -3 "$SCR/chunk.log"; exit 1; }
grep CHUNKS_PASS "$SCR/chunk.log"

echo "########## 2. externalized merge --tree --emit-pf under ulimit -v ${CAP_GB} GiB"
dfgate /mnt/nvme2n1
( ulimit -v $((CAP_GB * 1048576)); /usr/bin/time -v -o "$SCR/merge.time" \
  "$BIN/cross_lcp_merge" --tree "$SCR/chunks" "$CHUNKS" "$N" "$SCR/merged/frag" \
    --threads 48 --work "$SCR/mwork" --emit-pf > "$SCR/merge.log" 2>&1 ) \
  || { echo MERGE_FAIL; tail -5 "$SCR/merge.log"; exit 1; }
grep -c "CROSS_PAIR" "$SCR/merge.log"
grep "CROSS_PF_EMIT" "$SCR/merge.log" | tail -1

echo "########## 3. byte-identity vs the banked 10 GB reference"
for f in rlebwt rlebwt.meta ssa ssa_t pftext pfck; do
    cmp "$SCR/merged/frag.$f" "$REF/merged/frag.$f" \
      && echo "EXT10B_$f BYTE_IDENTICAL_TO_BANKED" || { echo "EXT10B_$f MISMATCH"; exit 1; }
done

echo "########## 4. external adopt finish under ulimit -v 8 GiB"
dfgate /mnt/nvme2n1
( ulimit -v 8388608
  /usr/bin/time -v -o "$SCR/fin1.time" "$BIN3/rpfbwt_endpoints" "$SCR/merged/frag" \
      "$SCR/finish/ext.ri4" "$SCR/finish/ext.head_sa" \
      --pf-text "$SCR/merged/frag.pftext" --pf-checkpoints "$SCR/merged/frag.pfck" \
      > "$SCR/fin1.log" 2>&1
  /usr/bin/time -v -o "$SCR/fin2.time" "$BIN3/slim_dump" --slim --resolve-ri4 \
      --ri4 "$SCR/finish/ext.ri4" --head-sa "$SCR/finish/ext.head_sa" -t 48 \
      --pf-text "$SCR/merged/frag.pftext" --pf-checkpoints "$SCR/merged/frag.pfck" \
      -o "$SCR/finish/ext.agg" > "$SCR/fin2.log" 2>&1
  /usr/bin/time -v -o "$SCR/fin3.time" "$XSA" chi-rspace --stream-agg \
      --ri4 "$SCR/finish/ext.ri4" --agg "$SCR/finish/ext.agg" -o "$SCR/finish/ext.sA" \
      > "$SCR/fin3.log" 2>&1
) || { echo FINISH_FAIL; exit 1; }
for f in ri4 head_sa agg sA; do
    cmp "$SCR/finish/ext.$f" "$REF/ext.$f" \
      && echo "EXT10B_FINISH_$f BYTE_IDENTICAL_TO_BANKED" || { echo "EXT10B_FINISH_$f MISMATCH"; exit 1; }
done
grep -oE "chi = [0-9]+ \(N=[0-9]+, R=[0-9]+\)" "$SCR/fin3.log"
grep ENDPOINT_PASS "$SCR/fin1.log" | head -1

echo "########## 5. telemetry"
for t in chunk.time merge.time fin1.time fin2.time fin3.time; do
    w=$(grep -E "Elapsed \(wall" "$SCR/$t" | grep -oE "[0-9:.]+$")
    r=$(grep -E "Maximum resident" "$SCR/$t" | grep -oE "[0-9]+$")
    echo "PHASE $t wall=$w peak_rss_kb=$r"
done
df -h /mnt/nvme2n1 | tail -1
echo EXT10B_REBUILD_GATE_DONE
