#!/bin/bash
# battery_chain.sh — full-chain battery gate for the r-space chi/sA
# construction. Per text: grlBWT -> rle -> 5-byte -> TeraLCP -> lcp_index;
# teralcp_chi --triples --samples -o oracle (the PRODUCTION machine =
# ground truth); pfp++; chi_rspace_dump (r-space aggregates);
# xsa chi-rspace (Rust machine). GATES:
#   G1: dump aggregates == teralcp_chi --triples per-run values
#   G2: Rust chi/witness-set == oracle chi/witness-set (sorted u64 compare)
set -e
GRL=/home/erikg/grlBWT/build/grlbwt-cli
RLE=/home/erikg/grlBWT/build/grlbwt2rle
TLC=/home/erikg/TeraTools/src/TeraLCP/TeraLCP
CHI=/tmp/grl_gate/teralcp_chi
DUMP=/tmp/laneY/dump
XSA=/tmp/sxgc-laneY/xsa/target/release/xsa
W=/tmp/laneY/bat
mkdir -p $W
PASS=0; FAIL=0
for T in "$@"; do
  T="$(readlink -f "$T")"
  name=$(basename "$T" .txt)
  echo "=== [$name] ==="
  cd $W
  rm -f $name.rl_bwt ${name}t.syms ${name}t.len ${name}t.bwt.heads ${name}t.bwt.len ${name}t.scratch ${name}t.lcp_index.lcp_index $name.ri4 $name.agg $name.rs.out $name.oracle.sA $name.triples
  cp "$T" $W/${name}_src.txt 2>/dev/null || true
  if [ "$(readlink -f "$T")" != "$W/$name.txt" ]; then cp -f "$T" "$W/$name.txt"; fi
  $GRL $name.txt -t 8 -T $W > /dev/null 2>&1
  $RLE $name.rl_bwt ${name}t 2>/dev/null
  python3 - <<PYEOF
import struct
syms = open('$W/${name}t.syms','rb').read()
lens4 = open('$W/${name}t.len','rb').read()
R = len(syms)
vals = struct.unpack(f'<{R}I', lens4[:R*4])
open('$W/${name}t.bwt.heads','wb').write(syms)
with open('$W/${name}t.bwt.len','wb') as f:
    for v in vals: f.write(v.to_bytes(5,'little'))
open('$W/${name}t.scratch','wb').close()
PYEOF
  $TLC -f rlbwt -i ${name}t -t ${name}t.scratch -oindex ${name}t.lcp_index -othresholds ${name}_thr --thr-pfp > /dev/null 2>&1
  python3 -c "import os; n=os.path.getsize('$name.txt'); open('$name.sc','w').write('s0\t0\t'+str(n-1)+'\n')"
  $CHI ${name}t.lcp_index.lcp_index --rlbwt ${name}t --sidecar $name.sc --samples $name.ri4 -o $name.oracle.sA --triples > $name.triples 2> $name.chi.log
  grep "chi:" $name.chi.log
  /home/erikg/pfp/build/pfp++ -t $name.txt -o ${name}_pfp -w 10 -p 100 > /dev/null 2>&1
  /usr/bin/time -f "dump wall %e s" $DUMP --ri4 $name.ri4 --parse ${name}_pfp --lcp-index ${name}t.lcp_index.lcp_index -o $name.agg --flat $name.txt -t 16 2>&1 | grep -E "flat spot|aggregates|dump wall|FATAL|calibration"
  # G1: aggregates vs --triples (char top interior first last len)
  python3 - <<PYEOF
import struct, sys
R = None
with open('$W/$name.triples') as f:
    rows = [tuple(map(int, l.split())) for l in f]
R = len(rows)
d = open('$W/$name.agg','rb').read()
magic, rr = struct.unpack('<IQ', d[:12])
assert magic == 0x31415243 and rr == R, (magic, rr, R)
def col(off):
    return struct.unpack(f'<{R}Q', d[12+off*R*8:12+(off+1)*R*8])
top, sf, sl, im = col(0), col(1), col(2), col(3)
INF = (1<<64)-1
bad = 0
for i, (c, t_top, t_int, t_first, t_last, t_len) in enumerate(rows):
    if top[i] != t_top: bad += 1; print(f"G1 topLCP run {i}: {top[i]} != {t_top}") if bad<4 else None
    if sf[i] != t_first: bad += 1; print(f"G1 saFirst run {i}: {sf[i]} != {t_first}") if bad<4 else None
    if sl[i] != t_last: bad += 1; print(f"G1 saLast run {i}: {sl[i]} != {t_last}") if bad<4 else None
    a = im[i] if im[i] != INF else INF
    b = t_int if t_int != 18446744073709551615 else INF
    if a != b: bad += 1; print(f"G1 interiorMin run {i}: {a} != {b}") if bad<4 else None
print(f"G1 [$name]: {'GREEN' if bad==0 else 'RED'} ({bad} mismatches / {R} runs)")
sys.exit(0 if bad==0 else 1)
PYEOF
  if [ $? -ne 0 ]; then FAIL=$((FAIL+1)); continue; fi
  # G2: Rust machine vs oracle
  $XSA chi-rspace --ri4 $name.ri4 --agg $name.agg -o $name.rs.out 2> $name.rs.log
  python3 - <<PYEOF
import struct, sys
def rd(p):
    d = open(p,'rb').read()
    return sorted(struct.unpack(f'<{len(d)//8}Q', d))
o = rd('$W/$name.oracle.sA')
r = rd('$W/$name.rs.out')
same = (o == r)
print(f"G2 [$name]: oracle={len(o)} rust={len(r)} seteq={'YES' if same else 'NO'}")
if not same:
    miss = sorted(set(o)-set(r))[:5]; spur = sorted(set(r)-set(o))[:5]
    print(f"  missing {len(set(o)-set(r))}: {miss}  spurious {len(set(r)-set(o))}: {spur}")
sys.exit(0 if same else 1)
PYEOF
  if [ $? -ne 0 ]; then FAIL=$((FAIL+1)); else PASS=$((PASS+1)); fi
done
echo "BATTERY: PASS=$PASS FAIL=$FAIL"
