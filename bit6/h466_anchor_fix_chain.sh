#!/bin/bash
# h466_anchor_fix_chain.sh — the sentinel-erasure FUNDAMENTAL FIX chain at 466.
# Anchors: string-start rows as extra sample anchors; two-case s_at.
# Stage 1: build the 0x0A-row -> S anchor table (samples + search, no 37h walk)
# Stage 2: p39/p197 anchored rerun + AGC precision spot-check (early signal)
# Stage 3: full 200-pattern anchored query + pilot_verify vs AGC -> VERDICT
set -e
cd /mnt/nvme3n1/erikg/sxgc-pilot/k466
X=/home/erikg/sxgc/xsa/target/release/xsa
AGC=/home/erikg/hprcv2/HPRC_r2_assemblies_0.6.1.agc
TOOLS=/home/erikg/sxgc/tools

echo "=== H466 ANCHOR FIX start $(date -Is)"
echo "=== stage 1: build-anchors $(date -Is)"
$X build-anchors --ri4 h466.ri4 --flat h466_rl.txt \
  --sidecar h466_rl.txt.names.tsv --output h466.anchors
ls -la h466.anchors

echo "=== stage 2: p39/p197 anchored rerun $(date -Is)"
$X query --ri4 h466.ri4 --patterns p39p197.fa --anchors h466.anchors \
  --output p39p197.anch.occs
python3 - <<'EOF'
# spot-check: (a) occurrence COUNT unchanged (intervals are exact, only locate changes)
#             (b) none of the 5 known-bad garbage positions appear anymore
old = set(); new = set()
for line in open('p39p197.trace.occs'):
    if line.startswith('>') or line.startswith('TRACE'): continue
    old.add(int(line.split()[0]))
for line in open('p39p197.anch.occs'):
    if line.startswith('>') or line.startswith('TRACE'): continue
    new.add(int(line.split()[0]))
KNOWN_BAD = {520750459960, 1283123177160, 1087202055600, 54375641120, 1087202023795}
still = KNOWN_BAD & new
print(f"p39/p197: old={len(old)} new={len(new)} same-count={len(old)==len(new)}")
print(f"known-bad in OLD run (sanity, expect 5): {len(KNOWN_BAD & old)}")
print(f"known-bad garbage still present: {len(still)} (must be 0)")
print("p39/p197 SPOT-CHECK:", "GREEN" if len(old)==len(new) and not still else "RED")
EOF
echo "=== stage 3: full 200-pattern anchored query $(date -Is)"
/usr/bin/time -f "query wall %e s, maxRSS %M KB" \
  $X query --ri4 h466.ri4 --patterns patterns.fa --anchors h466.anchors \
  --output h466_occs_anchored.txt
echo "=== stage 4: verify vs AGC $(date -Is)"
python3 $TOOLS/pilot_verify.py --agc "$AGC" --sidecar h466_rl.txt.names.tsv \
  --patterns patterns.fa --truth patterns.fa.truth.tsv \
  --occs h466_occs_anchored.txt 2>&1 | tail -3
echo "=== H466 ANCHOR FIX CHAIN COMPLETE $(date -Is)"
