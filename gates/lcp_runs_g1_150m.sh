#!/usr/bin/env bash
# G1 -- 150 MB (/dev/shm/t150m.txt, 150,000,001 B). BOTH ARMS.
#
#   * the tape triad completes in both arms and chi is 44,034,230 (unchanged)
#   * DEFAULT ARM (`xsa build --tape`, no flag): v7, 8 members, NO m14, header
#     flag bit 7 clear, and `xsa lcp` refuses it BY NAME (m14 is opt-in)
#   * m14 ARM (`--with-lcp-runs`): SXI2 v8, 9 members, flag bit 7 set, the
#     LCP_RUNS journal line, a member that audits structurally, and `xsa query`
#     answering the 150 MB oracle EXACTLY
#   * ADDITIVITY: every one of the other 8 members is byte-identical between the
#     two arms (sha256 per member), so m14 costs its own bytes and changes nothing
#   * `xsa query` still answers the oracle from the banked v6 container
#
# usage: gates/lcp_runs_g1_150m.sh
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
X=${X:-$ROOT/xsa/target/release/xsa}
D=${D:-/mnt/nvme2n1/erikg/lcpruns-gate/g1}
CORPUS=${CORPUS:-/dev/shm/t150m.txt}
V6=${V6:-/mnt/nvme2n1/erikg/L150/t150m.sxi2}
PATS=$ROOT/tools/binbwt/pats.fa
ORACLE=$ROOT/tools/binbwt/oracle.tsv
mkdir -p "$D"
fail=0
say() { echo "G1_$*"; }

build_arm() { # $1 = scratch dir, $2 = log, $3... = extra flags
  local dir=$1 log=$2; shift 2
  rm -rf "$dir"
  /usr/bin/time -f "G1_WALL %e s  RSS %M kB" setsid timeout --kill-after=10 3600 \
    "$X" build --tape --input "$CORPUS" --scratch "$dir" --threads 24 --memory-gb 24 "$@" \
    >"$log" 2>&1
  echo "$*" >"$log.flags"
  grep -E "^G1_WALL|XSA_BUILD_DONE" "$log"
}
header() { # $1 = container -> "version members flags length filesize"
  python3 - "$1" <<'PY'
import struct,sys,os
p=sys.argv[1]; h=open(p,'rb').read(64)
print(struct.unpack_from('<I',h,4)[0], struct.unpack_from('<I',h,32)[0],
      struct.unpack_from('<Q',h,48)[0], struct.unpack_from('<Q',h,40)[0], os.path.getsize(p))
PY
}
member_table() { python3 - "$1" <<'PY'
import struct,sys,hashlib
p=sys.argv[1]; h=open(p,'rb').read()
cnt=struct.unpack_from('<I',h,32)[0]; length=struct.unpack_from('<Q',h,40)[0]
off=length-40*cnt
for i in range(cnt):
    d=h[off+40*i:off+40*(i+1)]
    mid,codec,o,b,c,ck=struct.unpack('<IIQQQI',d[:36])
    print(mid, codec, o, b, c, "%08x"%ck, hashlib.sha256(h[o:o+b]).hexdigest())
PY
}
qgate() { # $1 container  $2 tag
  local c=$1 tag=$2 t; t=$(mktemp -d)
  timeout 900 "$X" query "$c" --patterns-fasta "$PATS" -j 16 >"$t/pos.tsv" 2>"$t/err.txt"
  local rc=$?
  grep -E "QLITE_OPEN|QUERY_DONE" "$t/err.txt"
  awk -F'\t' '{cnt[$1]++} END {for (k in cnt) print k"\t"cnt[k]}' "$t/pos.tsv" | sort -k1,1n >"$t/counts.mine"
  awk -F'\t' 'NR>1{print $1"\t"$2}' "$ORACLE" | sort -k1,1n >"$t/counts.oracle"
  sort -k1,1n -k2,2n "$t/pos.tsv" >"$t/pos.mine"
  awk -F'\t' 'NR>1{n=split($3,a,","); for(i=1;i<=n;i++) printf "%d\t%s\n",$1,a[i]}' "$ORACLE" | sort -k1,1n -k2,2n >"$t/pos.oracle"
  if [ $rc = 0 ] && diff -q "$t/counts.mine" "$t/counts.oracle" >/dev/null; then
    say "QUERY_${tag}_COUNTS exact=100/100"
  else say "QUERY_${tag}_COUNTS_MISMATCH rc=$rc"; diff "$t/counts.mine" "$t/counts.oracle" | head -5; fail=1; fi
  if diff -q "$t/pos.mine" "$t/pos.oracle" >/dev/null; then
    say "QUERY_${tag}_POSITIONS exact=$(wc -l <"$t/pos.mine")/574"
  else say "QUERY_${tag}_POSITION_MISMATCH"; diff "$t/pos.mine" "$t/pos.oracle" | head -5; fail=1; fi
  rm -rf "$t"
}

# ------------------------------------------------------- ARM A: the DEFAULT
if [ "${SKIP_BUILD:-0}" != 1 ]; then
  build_arm "$D/arm_default" "$D/build_default.log"
  build_arm "$D/arm_m14" "$D/build_m14.log" --with-lcp-runs
fi
CD=$D/arm_default/t150m.sxi2
CM=$D/arm_m14/t150m.sxi2
grep -q "XSA_BUILD_DONE chi=44034230 " "$D/build_default.log" \
  && say "DEFAULT_CHI chi=44034230 unchanged=1" || { say "DEFAULT_CHI_MISMATCH"; fail=1; }
grep -q "XSA_BUILD_DONE chi=44034230 " "$D/build_m14.log" \
  && say "M14_CHI chi=44034230 unchanged=1" || { say "M14_CHI_MISMATCH"; fail=1; }
read -r DV DC DF DL DS < <(header "$CD"); read -r MV MC MF ML MS < <(header "$CM")
say "DEFAULT_HEADER version=$DV members=$DC flags=$DF flag128=$(( (DF>>7)&1 ))"
say "M14_HEADER version=$MV members=$MC flags=$MF flag128=$(( (MF>>7)&1 ))"
[ "$DV" = 7 ] && [ "$DC" = 8 ] && [ $(( (DF>>7)&1 )) = 0 ] || { say "FAIL: default arm is not the v7 layout"; fail=1; }
[ "$MV" = 8 ] && [ "$MC" = 9 ] && [ $(( (MF>>7)&1 )) = 1 ] || { say "FAIL: m14 arm is not v8 with flag 128"; fail=1; }
grep -q "lcp_runs=absent" "$D/arm_default/xsa-build.log" && say "PLAN_DEFAULT lcp_runs=absent" || { say "FAIL: plan line does not say absent"; fail=1; }
grep -q "lcp_runs=present" "$D/arm_m14/xsa-build.log" && say "PLAN_M14 lcp_runs=present" || { say "FAIL: plan line does not say present"; fail=1; }
grep -hE "^LCP_RUNS_POLICY" "$D/arm_m14/finish-v2.log" | head -1

# ------------------------------------------------- the m14 arm's journal + audit
LCPLINE=$(grep -hE "^LCP_RUNS " "$D/arm_m14/finish-v2.log")
echo "$LCPLINE"
[ -n "$LCPLINE" ] || { say "FAIL: no LCP_RUNS line"; fail=1; }
MB=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^bytes=/){sub("bytes=","",$i); print $i}}' <<<"$LCPLINE")
[ "${MB:-0}" -gt 0 ] && say "MEMBER_BYTES bytes=$MB" || { say "FAIL: member bytes 0"; fail=1; }
grep -hE "^LCP_RUNS_ENCODING|^LCP_RUNS_DEPTH" "$D/arm_m14/finish-v2.log"
"$X" lcp "$CM" --audit --run 0 --run 1 --sample 5 --seed 3 >"$D/lcp_probe.tsv" 2>"$D/lcp_probe.log"
rc=$?; grep -E "^LCP_OPEN|^LCP_AUDIT|^LCP_ACCESS" "$D/lcp_probe.log"
[ $rc = 0 ] || { say "FAIL: xsa lcp exited $rc"; fail=1; }
grep -q "tiling=exact escape_lists=sorted" "$D/lcp_probe.log" || { say "FAIL: m14 audit"; fail=1; }
# the default arm must REFUSE m14 by name
"$X" lcp "$CD" --run 3 >"$D/default_lcp.out" 2>"$D/default_lcp.err"
rc=$?; head -3 "$D/default_lcp.err"
[ $rc != 0 ] && grep -q "has no member m14" "$D/default_lcp.err" && say "DEFAULT_LCP_REFUSED rc=$rc by_name=1" \
  || { say "FAIL: the default arm did not refuse m14 by name (rc=$rc)"; fail=1; }

# --------------------------------------------------------------- ADDITIVITY
member_table "$CD" >"$D/members_default.txt"
member_table "$CM" >"$D/members_m14.txt"
# The OFFSETS legitimately move (m14 sits between m11 and m12), so additivity is
# asserted on the CONTENT: id, codec, byte length, count, member CRC and the
# sha256 of the member's bytes. An offset is a placement, not a value.
diff <(awk '{print $1,$2,$4,$5,$6,$7}' "$D/members_default.txt" | sort -k1,1n) \
     <(grep -v "^14 " "$D/members_m14.txt" | awk '{print $1,$2,$4,$5,$6,$7}' | sort -k1,1n) \
     >"$D/members.diff" 2>&1
if [ -s "$D/members.diff" ]; then say "ADDITIVITY_FAIL"; cat "$D/members.diff"; fail=1
else say "ADDITIVITY identical_members=8 (id,codec,bytes,count,crc,sha256) offsets_move_for_m12_m13=yes"; fi
awk '{printf "MEMBER id=%s codec=%s bytes=%s count=%s crc=%s sha=%s\n",$1,$2,$4,$5,$6,substr($7,1,16)}' "$D/members_m14.txt"

# ------------------------------------------------------------------- queries
qgate "$CM" "M14"
qgate "$CD" "DEFAULT"
[ -r "$V6" ] && qgate "$V6" "V6"

echo "SURVIVORS: $(pgrep -a -f 'xsa_finish|scatter_sort|cross_lcp_merge' | grep -v pgrep || echo none)"
if [ $fail = 0 ]; then echo "GATE G1 PASS"; else echo "GATE G1 FAIL"; exit 1; fi
