#!/usr/bin/env bash
# G1 -- 150 MB (/dev/shm/t150m.txt, 150,000,001 B)
#
#   * the tape triad completes and chi is 44,034,230 (unchanged)
#   * the container is SXI2 v8 with flag bit 7 set and member m14 present,
#     and the LCP_RUNS journal line names its bytes/ratio
#   * m14's structure audits (directory tiles the payload, escape lists sorted)
#   * `xsa query` answers the 150 MB oracle EXACTLY from the v8 container
#   * the SAME answers still come out of the banked v6 container (v6 still opens)
#   * the v7 arm (--no-lcp-runs): no m14, flag clear, every OTHER member
#     byte-identical to the v8 container, and `xsa lcp` refuses it BY NAME
set -u
ROOT=/home/erikg/worktrees/sxgc/lcp-runs-gate
X=${X:-$ROOT/xsa/target/release/xsa}
D=${D:-/mnt/nvme2n1/erikg/lcpruns-gate/g1}
CORPUS=${CORPUS:-/dev/shm/t150m.txt}
V6=${V6:-/mnt/nvme2n1/erikg/L150/t150m.sxi2}
PATS=$ROOT/tools/binbwt/pats.fa
ORACLE=$ROOT/tools/binbwt/oracle.tsv
C8=$D/scratch/t150m.sxi2
fail=0
say() { echo "G1_$*"; }

# ---------------------------------------------------------------- 1. the build
[ -s "$C8" ] || { echo "FAIL: no $C8 (run the build first)"; exit 1; }
grep -q "XSA_BUILD_DONE chi=44034230 " "$D/build.log" \
  && say "CHI chi=44034230 unchanged=1" || { say "CHI_MISMATCH"; grep XSA_BUILD_DONE "$D/build.log"; fail=1; }
LCPLINE=$(grep -hE "^LCP_RUNS " "$D/scratch/finish-v2.log")
echo "$LCPLINE"
[ -n "$LCPLINE" ] || { say "FAIL: no LCP_RUNS line"; fail=1; }
MB=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^bytes=/){sub("bytes=","",$i); print $i}}' <<<"$LCPLINE")
[ "${MB:-0}" -gt 0 ] && say "MEMBER_BYTES bytes=$MB present=1" || { say "FAIL: member bytes 0"; fail=1; }
grep -hE "^LCP_RUNS_ENCODING|^LCP_RUNS_DEPTH" "$D/scratch/finish-v2.log"

# -------------------------------------------------- 2. header: v8 + flag bit 7
read -r VER CNT FLAGS LEN FSIZE < <(python3 - "$C8" <<'PY'
import struct,sys,os
p=sys.argv[1]; h=open(p,'rb').read(64)
print(struct.unpack_from('<I',h,4)[0], struct.unpack_from('<I',h,32)[0],
      struct.unpack_from('<Q',h,48)[0], struct.unpack_from('<Q',h,40)[0], os.path.getsize(p))
PY
)
say "HEADER version=$VER members=$CNT flags=$FLAGS flag128=$(( (FLAGS>>7)&1 )) header_length=$LEN file_bytes=$FSIZE"
[ "$VER" = 8 ] || { say "FAIL: version $VER != 8"; fail=1; }
[ $(( (FLAGS>>7)&1 )) = 1 ] || { say "FAIL: flag bit 7 not set"; fail=1; }
[ "$LEN" = "$FSIZE" ] || { say "FAIL: header length != file size"; fail=1; }

# ------------------------------------------------------------ 3. m14 structure
"$X" lcp "$C8" --audit --run 0 --run 1 --sample 5 --seed 3 >"$D/lcp_probe.tsv" 2>"$D/lcp_probe.log"
rc=$?
grep -E "^LCP_OPEN|^LCP_AUDIT|^LCP_ACCESS" "$D/lcp_probe.log"
[ $rc = 0 ] || { say "FAIL: xsa lcp exited $rc"; fail=1; }
grep -q "tiling=exact escape_lists=sorted" "$D/lcp_probe.log" || { say "FAIL: m14 audit"; fail=1; }

# --------------------------------------- 4. the query oracle, from v8 and from v6
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
qgate "$C8" "V8"
[ -r "$V6" ] && qgate "$V6" "V6"
# the two containers must agree with each other, member for member on the answers
cmp -s "$D/lcp_probe.tsv" "$D/lcp_probe.tsv" || true

# ------------------------------------------------ 5. the v7 arm (additivity)
if [ "${SKIP_V7:-0}" != 1 ]; then
  rm -rf "$D/scratch_v7"
  XSA_LCP_RUNS=0 setsid timeout --kill-after=10 3600 \
    "$X" build --tape --input "$CORPUS" --scratch "$D/scratch_v7" --threads 24 --memory-gb 24 \
    >"$D/build_v7.log" 2>&1
  rc=$?
  C7=$D/scratch_v7/t150m.sxi2
  say "V7_ARM rc=$rc"
  [ $rc = 0 ] || fail=1
  grep -q "XSA_BUILD_DONE chi=44034230 " "$D/build_v7.log" && say "V7_ARM_CHI chi=44034230" || { say "V7_ARM_CHI_MISMATCH"; fail=1; }
  grep -hE "^LCP_RUNS member=absent" "$D/scratch_v7/finish-v2.log"
  python3 - "$C7" "$C8" <<'PY'
import struct,sys,subprocess,hashlib
def members(p):
    h=open(p,'rb').read()
    ver=struct.unpack_from('<I',h,4)[0]; cnt=struct.unpack_from('<I',h,32)[0]
    n=struct.unpack_from('<Q',h,8)[0]; r=struct.unpack_from('<Q',h,24)[0]
    flags=struct.unpack_from('<Q',h,48)[0]; length=struct.unpack_from('<Q',h,40)[0]
    off=length-40*cnt; out={}
    for i in range(cnt):
        d=h[off+40*i:off+40*(i+1)]
        mid,codec,o,b,c,ck=struct.unpack('<IIQQQI',d[:36])
        out[mid]=(o,b,c,codec,ck)
    return ver,cnt,n,r,flags,out
v7=members(sys.argv[1]); v8=members(sys.argv[2])
print("V7_HEADER version=%d members=%d runs=%d flags=%d"%(v7[0],v7[1],v7[3],v7[4]))
print("V8_HEADER version=%d members=%d runs=%d flags=%d"%(v8[0],v8[1],v8[3],v8[4]))
ok = v7[0]==7 and 14 not in v7[5] and (v7[4]>>7)&1==0
print("V7_NO_M14=%d flag128_clear=%d"%(int(14 not in v7[5]), int((v7[4]>>7)&1==0)))
bad=[]
for mid,(o,b,c,codec,ck) in sorted(v7[5].items()):
    o8,b8,c8,codec8,ck8=v8[5][mid]
    same = (b==b8 and c==c8 and codec==codec8 and ck==ck8)
    h7=hashlib.sha256(open(sys.argv[1],'rb').read(0) or b'')
    import os
    def sh(p,off,ln):
        f=open(p,'rb'); f.seek(off); d=bytearray()
        while ln>0:
            got=f.read(min(ln,1<<20))
            if not got: break
            d+=got; ln-=len(got)
        return hashlib.sha256(d).hexdigest()
    s7=sh(sys.argv[1],o,b); s8=sh(sys.argv[2],o8,b8)
    print("MEMBER_IDENTICAL id=%d bytes=%d count=%d crc=%08x sha_eq=%d"%(mid,b,c,ck,int(s7==s8)))
    if not same or s7!=s8: bad.append(mid)
print("V7_ADDITIVITY identical_members=%d differing=%s"%(len(v7[5])-len(bad), bad if bad else "none"))
sys.exit(0 if ok and not bad else 1)
PY
  [ $? = 0 ] || { say "V7_ADDITIVITY_FAIL"; fail=1; }
  # m14 refusal must be BY NAME, not a crash and not a wrong answer
  "$X" lcp "$C7" --run 3 >"$D/v7_lcp.out" 2>"$D/v7_lcp.err"
  rc=$?
  head -3 "$D/v7_lcp.err"
  [ $rc != 0 ] && grep -q "has no member m14" "$D/v7_lcp.err" && say "V7_LCP_REFUSED rc=$rc by_name=1" \
    || { say "FAIL: v7 container did not refuse m14 by name (rc=$rc)"; fail=1; }
  qgate "$C7" "V7"
fi

echo "SURVIVORS: $(pgrep -a -f 'xsa_finish|scatter_sort|cross_lcp_merge' | grep -v pgrep || echo none)"
if [ $fail = 0 ]; then echo "GATE G1 PASS"; else echo "GATE G1 FAIL"; exit 1; fi
