#!/usr/bin/env bash
# X2 - THE COMBINED 10 GB BUILD: the full no-cross stack in ONE build.
#
#   xsa build --tape --input pf-10gb.txt --scratch ... \
#         --trie-depth 8 --trie-min-support 1000 \
#         --docs-map /tmp/map10g \
#         --scratch-budget-gb 3 --chi-expect 2676925006
#
# Checks and reports:
#   - the build exits 0 and journals chi == 2676925006 (the banked value)
#   - the container: m14 ABSENT (it is opt-in), m15 and m20 PRESENT
#   - the m20 member says boundary_aware=1 (the no-cross law by construction)
#   - the in-build trie policy line says the map fed the tap
#   - peak scratch vs the 3 GB budget, the pause/resume counts, the wall
#
# usage: gates/no_cross_x2_10g.sh [WORKDIR]
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
X=${X:-$ROOT/xsa/target/release/xsa}
D=${1:-/mnt/nvme2n1/erikg/no-cross/x2}
CORPUS=${CORPUS:-/mnt/nvme2n1/erikg/pf-10gb.txt}
MAP=${MAP:-/tmp/map10g}
CHI=${CHI:-2676925006}
THREADS=${THREADS:-48}
MEMGB=${MEMGB:-24}

fail=0
say() { echo "X2_$*"; }
bad() { echo "X2_FAIL $*"; fail=1; }

mkdir -p "$D"
echo "X2_BOX $(uptime)"
echo "X2_CORPUS $(ls -l "$CORPUS")"
echo "X2_MAP $(ls -l "$MAP"/seps.bin)"

if [ ! -d "$MAP" ] || [ ! -f "$MAP/seps.bin" ]; then
  bad "the document map $MAP (seps.bin) is missing"
fi

rm -rf "$D/scratch"
echo "X2_BUILD_START $(date -u +%FT%TZ) flags=--tape,--trie-depth-8,--trie-min-support-1000,--docs-map=$MAP,--scratch-budget-gb-3,--chi-expect-$CHI"
/usr/bin/time -f "X2_WALL %e s peak_rss_kb %M" \
  "$X" build --tape --input "$CORPUS" --scratch "$D/scratch" \
     --threads "$THREADS" --memory-gb "$MEMGB" \
     --chi-expect "$CHI" \
     --trie-depth 8 --trie-min-support 1000 \
     --docs-map "$MAP" \
     --scratch-budget-gb 3 > "$D/build.log" 2>&1
rc=$?
echo "X2_BUILD_END rc=$rc $(date -u +%FT%TZ)"
if [ $rc -ne 0 ]; then
  bad "the build exited $rc (see $D/build.log)"
  tail -30 "$D/build.log"
  exit 1
fi

C=$D/scratch/pf-10gb.sxi2
if [ ! -f "$C" ]; then
  bad "no container at $C"
  find "$D/scratch" -maxdepth 1
  exit 1
fi

# ---- chi, members, the trie's boundary bit, scratch, pauses, wall -----------
grep -E "CHI_ASSERT|chi_count" "$D/build.log" | tail -2
grep -E "^X2_WALL" "$D/build.log"
grep -E "CONTEXT_TRIE_BOUNDARY_POLICY|CONTEXT_TRIE_BOUNDARY " "$D/build.log" | head -3
grep -E "CONTEXT_TRIE_PUBLISHED|CONTEXT_TRIE_BUILD_WALL" "$D/build.log"
grep -E "DOCSTARTS " "$D/build.log" | tail -1
grep -E "PEAK_DISC_USED_BYTES|SCRATCH_BUDGET|SCRATCH_BACKPRESSURE" "$D/build.log" | tail -4
grep -cE "state=pause" "$D/build.log" | sed 's/^/X2_PAUSES /'
grep -E "^XSA_BUILD_DONE" "$D/build.log" | tail -1

python3 - "$C" <<'EOF'
import struct, sys, os
p = sys.argv[1]
b = open(p, 'rb').read(64)
assert b[:4] == b'SXI2', 'not SXI2'
ver = struct.unpack_from('<I', b, 4)[0]
n = struct.unpack_from('<Q', b, 8)[0]
count = struct.unpack_from('<I', b, 32)[0]
flags = struct.unpack_from('<Q', b, 48)[0]
L = os.path.getsize(p)
f = open(p, 'rb'); f.seek(L - 40 * count)
members = []
for i in range(count):
    d = f.read(40)
    members.append((struct.unpack_from('<I', d, 0)[0], struct.unpack_from('<I', d, 4)[0],
                    struct.unpack_from('<Q', d, 8)[0], struct.unpack_from('<Q', d, 16)[0]))
ids = [m[0] for m in members]
print("X2_CONTAINER path=%s bytes=%d version=%d n=%d flags=%d" % (p, L, ver, n, flags))
print("X2_MEMBERS %s" % " ".join("m%d(codec%d,%dB)" % (m[0], m[1], m[3]) for m in members))
assert 14 not in ids, "m14 must be absent by default"
assert 15 in ids and 20 in ids, "m15 and m20 must be present"
# the m20 head: version at 0..8, boundary_aware at 168..176
m20 = [m for m in members if m[0] == 20][0]
f.seek(m20[2])
head = f.read(192)
mver = struct.unpack_from('<Q', head, 0)[0]
aware = struct.unpack_from('<Q', head, 168)[0]
print("X2_M20 member_version=%d boundary_aware=%d depth=%d min_support=%d nodes=%d edges=%d"
      % (mver, aware, struct.unpack_from('<Q', head, 8)[0],
         struct.unpack_from('<Q', head, 16)[0],
         struct.unpack_from('<Q', head, 40)[0], struct.unpack_from('<Q', head, 48)[0]))
assert mver == 3, "member format version must be 3"
assert aware == 1, "the trie must be boundary-aware"
m15 = [m for m in members if m[0] == 15][0]
print("X2_M15 bytes=%d" % m15[3])
print("X2_CHECK members=ok m14=absent m15=present m20=present boundary_aware=1")
EOF
[ $? -eq 0 ] || bad "container checks failed"

say "SHA256 $(sha256sum "$C")"
say "RESULT $([ $fail -eq 0 ] && echo PASS || echo FAIL)"
exit $fail
