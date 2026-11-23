#!/usr/bin/env bash
# Full producer diagnostic, not a certified container. Baseline must finish;
# then compare every common checkpoint even if pressure arm hits its IO guard.
set -eu
sandbox=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
r=$(mktemp -d "$sandbox/full-stream-XXXXXX"); echo "receipt=$r"
for a in reference pressure; do
 if [[ $a == reference && ${BANKED_REFERENCE:-0} == 1 ]]; then
  mkdir "$r/reference"; cp gates/receipts/full-stream-FD0ekD/reference.log "$r/reference/run.log"; echo 'reused full-stream-FD0ekD' >"$r/reference/config"; continue
 fi
 p=0; rounds=0; cap=4194304; mib=384; carry=0
 if [[ $a == pressure ]]; then p=16; rounds=4; cap=${INLINE_PAIRS:-1398101}; mib=192; carry=1; [[ $cap =~ ^[0-9]+$ ]] && ((cap>0 && cap<=1398101)) || exit 2; fi
 d=$r/$a; mkdir "$d"; cp --reflink=never "$sandbox/pile-slice100.bin" "$d/corpus"
 python3 gates/private_cold.py "$d/corpus"; rows=$(($(stat -c%s "$d/corpus")+1)); [[ $rows == 100000504 ]]
 echo "prefix=$p rounds=$rounds pair_cap=$cap carry=$carry phase_advice=${XSA_PROBE_PHASE_ADVICE:-0} producer_mib=$mib rows=$rows" >"$d/config"
 rc=0; systemd-run --user --quiet --collect --wait --pipe -p MemoryMax="${mib}M" -p MemorySwapMax=0 -p CPUQuota=100% -p Nice=10 /usr/bin/env XSA_PROBE_PHASE_ADVICE="${XSA_PROBE_PHASE_ADVICE:-0}" XSA_TEST_OVERLAP_BINNED=1 XSA_OVERLAP_RESTAGE="$rounds" XSA_OVERLAP_CARRY_PRED="$carry" XSA_OVERLAP_SORT_PAIRS="$cap" XSA_OVERLAP_INLINE_PREFIX="$p" "$PWD/xsa/target/release/overlap_io_probe" "$d/corpus" "$rows" 15000000000 >"$d/run.log" 2>&1 || rc=$?
 echo "$rc" >"$d/rc"; tail -3 "$d/run.log"; if [[ $rc != 0 && ( $a == reference || $rc != 75 ) ]]; then exit "$rc"; fi
done
python3 - "$r" <<'PY'
import re,sys,pathlib
r=pathlib.Path(sys.argv[1]); a,b=[(r/x/'run.log').read_text() for x in ('reference','pressure')]; pat=r'^CHECKPOINT rows=(\d+) digest=(\w+)'; ref=dict(re.findall(pat,a,re.M)); obs=dict(re.findall(pat,b,re.M)); assert len(ref)==100 and ref['6000000']=='80bcb54036d8a8a4';assert all(ref[k]==v for k,v in obs.items());print(len(obs),'million-row checkpoints matched');
if (r/'pressure/rc').read_text().strip()=='75': sys.exit(75)
pat=r'^FULL_STREAM rows=100000504 digest=([a-f0-9]{16})$';assert len(obs)==100 and re.search(pat,a,re.M)[1]==re.search(pat,b,re.M)[1];print('MATCH full producer stream (diagnostic digest)')
PY
