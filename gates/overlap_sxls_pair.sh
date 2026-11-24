#!/usr/bin/env bash
# Bounded local writer A/B, not a full-container or media bandwidth test.
set -euo pipefail
cd "$(dirname "$0")/..";repo=$PWD;s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
[[ -f gates/receipts/radix-emit-100m/result.json ]];d=$(mktemp -d "$s/sxls-pair-100m-XXXXXX");t=$(mktemp -d /tmp/overlap-sxls-XXXXXX);ram=$(mktemp -d /dev/shm/sxls-pair-XXXXXX);echo "receipt=$d tools=$t ram=$ram"
(cd xsa && cargo build --release -p pfp --bin overlap_resident && cargo test --release -p pfp --bin pfp_sort_r --no-run --message-format=json >"$t/build.jsonl")
codec=$(python3 - "$t/build.jsonl" <<'PY'
import sys,json
xs=[json.loads(x) for x in open(sys.argv[1])]; paths=[x['executable'] for x in xs if x.get('executable') and x['target']['name']=='pfp_sort_r' and x['profile']['test']];assert len(paths)==1;print(paths[0])
PY
);cp "$codec" "$t/codec";cp xsa/target/release/overlap_resident "$t/overlap_resident"
"$t/codec" --exact unit::sxls_batch_test::escape_publications_are_byte_identical |tee "$d/fixtures.log"
{ git rev-parse HEAD;git status --porcelain;sha256sum "$t/"{codec,overlap_resident}; } >"$d/binaries.sha"
mkdir "$d/in";cp --reflink=never "$s/dict-adj-7B0aj9/columns/"{parse.u32,dictionary.refs} "$d/in/";cp --reflink=never "$s/pile-slice100.bin" "$d/in/corpus";python3 gates/private_cold.py "$d/in/"*;echo 'private source columns and corpus: zero resident pages' >"$d/cold.log"
run=(systemd-run --user --quiet --collect --wait --pipe -p MemorySwapMax=0 -p Nice=10)
"${run[@]}" -p MemoryMax=2G -p CPUQuota=400% /usr/bin/python3 "$repo/gates/overlap_radix_stage.py" verify "$d" "$t/overlap_resident" 4 | \
"${run[@]}" -p MemoryMax=192M -p CPUQuota=100% /usr/bin/python3 "$repo/gates/overlap_sxls_pair.py" meter "$d" "$t/codec" "$ram"
rmdir "$ram";python3 gates/overlap_sxls_pair.py score "$d";echo "completed receipt=$d"
