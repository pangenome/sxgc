#!/usr/bin/env bash
# Producer-only matched cold arms. Invoke from the repository root after release build.
set -eu
sandbox=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
mib=${PROBE_MIB:-224}; [[ "$mib" =~ ^[0-9]+$ ]] && (( mib>=128 && mib<=384 )) || exit 2
r=$(mktemp -d "$sandbox/inline-probe-XXXXXX"); echo "receipt=$r"
for p in 0 16; do
 d=$r/p$p; mkdir "$d"; cp --reflink=never "$sandbox/pile-slice100.bin" "$d/corpus"
 python3 gates/private_cold.py "$d/corpus"
 cap=4194304; if [[ $p == 16 ]]; then cap=${INLINE_PAIRS:-4194304}; fi; echo "prefix=$p pair_cap=$cap producer_mib=$mib carry_pred=${XSA_OVERLAP_CARRY_PRED:-0}" >"$d/config"
 rc=0; systemd-run --user --quiet --collect --wait --pipe -p MemoryMax="${mib}M" -p MemorySwapMax=0 -p CPUQuota=100% -p Nice=10 /usr/bin/env XSA_TEST_OVERLAP_BINNED=1 XSA_OVERLAP_CARRY_PRED="${XSA_OVERLAP_CARRY_PRED:-0}" XSA_OVERLAP_SORT_PAIRS="$cap" XSA_OVERLAP_INLINE_PREFIX="$p" "$PWD/xsa/target/release/overlap_io_probe" "$d/corpus" 6000000 15000000000 >"$d/run.log" 2>&1 || rc=$?
 echo "$rc" >"$d/rc"; tail -2 "$d/run.log"; if [[ $rc != 0 && $rc != 75 ]]; then exit "$rc"; fi
done
python3 - "$r" <<'PY'
import pathlib,re,sys
r=pathlib.Path(sys.argv[1]); arms=[r/f'p{i}' for i in (0,16)]
if any((d/'rc').read_text().strip()!='0' for d in arms):print('read guard: no paired digest verdict');sys.exit(75)
a,b=[re.search(r'PROBE rows=6000000 digest=([a-f0-9]+)',(d/'run.log').read_text())[1] for d in arms];assert a==b,(a,b);print('MATCH six-million-row diagnostic digest',a)
PY
