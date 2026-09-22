#!/bin/bash
# pfpbwt_pilot_k10.sh — k=10 head-to-head: pfp++/rpfbwt (PFP-BWT) vs grlBWT
# on the SAME single-string text. Gates the candidate BWT run sequence
# against the grlBWT reference modulo the pinned convention (10-row internal
# 0x02 padding at the front + '\n'->0x02 terminator remap), and records the
# time/RAM/disk head-to-head. Thread-capped to coexist with the 466 chain.
set -euo pipefail
W=/mnt/nvme3n1/erikg/sxgc-pilot/k10
cd "$W"
export TMPDIR="$W/tmp"; mkdir -p "$W/tmp"
PT=8   # pilot threads (96-core policy is spoken for by the 466 chain)

echo "=== PILOT STAGE ss-text: single-string k10 text ($(date -Is))"
if [ ! -f h10ss.txt ]; then
  python3 - <<'EOF'
import os
# h10ss = '!'-joined revlines lines + trailing '\n' (single string)
with open('h10_rl.txt','rb') as f, open('h10ss.txt','wb') as o:
    first = True
    for line in f:
        if line.endswith(b'\n'): line = line[:-1]
        if not first: o.write(b'!')
        o.write(line)
        first = False
    o.write(b'\n')
print('h10ss.txt:', os.path.getsize('h10ss.txt'))
EOF
fi

echo "=== PILOT STAGE grlbwt reference ($(date -Is))"
if [ ! -f h10ss_rl.rl_bwt ]; then
  /usr/bin/time -f "grlbwt-ref wall %e s, maxRSS %M KB" \
    /home/erikg/grlBWT/build/grlbwt-cli h10ss.txt -t $PT -T "$TMPDIR" > grl_ref.log 2>&1 \
    || { echo "GRLBWT REF FAILED"; tail -3 grl_ref.log; exit 1; }
fi
/home/erikg/grlBWT/build/grlbwt2rle h10ss_rl.rl_bwt h10ssr
grep -E "Number of runs" grl_ref.log | tail -1 || true

echo "=== PILOT STAGE pfp++ parse ($(date -Is))"
if [ ! -f h10ss_pfp.parse ]; then
  /usr/bin/time -f "pfp++ wall %e s, maxRSS %M KB" \
    /home/erikg/pfp/build/pfp++ -t h10ss.txt -o h10ss_pfp -w 10 -p 100 > pfp.log 2>&1 \
    || { echo "PFP FAILED"; tail -3 pfp.log; exit 1; }
fi
/usr/bin/time -f "pfp++ L2 wall %e s, maxRSS %M KB" \
  /home/erikg/pfp/build/pfp++ -i h10ss_pfp.parse -w 5 -p 11 >> pfp.log 2>&1

echo "=== PILOT STAGE rpfbwt ($(date -Is))"
/usr/bin/time -f "rpfbwt wall %e s, maxRSS %M KB" \
  /home/erikg/r-pfbwt/build/rpfbwt --l1-prefix h10ss_pfp --w1 10 --w2 5 \
    --threads $PT --tmp-dir "$TMPDIR" > rpf.log 2>&1 \
  || { echo "RPFBWT FAILED"; tail -3 rpf.log; exit 1; }

echo "=== PILOT STAGE gate ($(date -Is))"
python3 - <<'EOF'
import struct
d = open('h10ss_pfp.rlebwt','rb').read()
recs = []
i = 0
while i + 4 <= len(d):
    v = struct.unpack_from('<I', d, i)[0]
    recs.append((v & 0xFF, v >> 8))
    i += 4
# strip leading terminator-preamble records (chars <= 2 or 0x0a)
k = 0
while k < len(recs) and (recs[k][0] <= 2 or recs[k][0] == 0x0A):
    k += 1
stripped, pads = recs[k:], k
tail = [((0x0A if c == 2 else c), L) for (c, L) in stripped]
syms = open('h10ssr.syms','rb').read()
lens = open('h10ssr.len','rb').read()
R = len(syms)
rl = struct.unpack(f'<{R}I', lens[:R*4])
grl = [(syms[j], rl[j]) for j in range(R)]
print(f"rpfbwt runs={len(recs)} (preamble {pads} records) grlBWT runs={R}")
if tail == grl:
    print("PILOT GATE GREEN: ordered run sequence identical")
else:
    print("PILOT GATE MISMATCH")
    n = min(len(tail), len(grl))
    for i in range(n):
        if tail[i] != grl[i]:
            print(f"diff at run {i}: {tail[i]} vs {grl[i]}"); break
    else:
        print("common prefix identical; lengths differ", len(tail), len(grl))
    exit(1)
EOF

echo "=== PILOT COMPLETE $(date -Is)"
