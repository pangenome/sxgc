#!/usr/bin/env bash
# Emission scalability at matched 2 GiB/100 MB, 4 CPU, no swap. No triples file.
set -euo pipefail
cd "$(dirname "$0")/.."; repo=$PWD; s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
t=$(mktemp -d /tmp/overlap-radix-XXXXXX); d=$(mktemp -d "$s/radix-emit-100m-XXXXXX"); echo "receipt=$d tools=$t"
(cd xsa && cargo build --release -p pfp --bin overlap_resident); cp xsa/target/release/overlap_resident "$t/"
python3 gates/overlap_radix_test.py "$t/overlap_resident" |tee "$d/seam-fixtures.log"
g++ -O2 -Wall -Wextra -Werror -std=c++17 gates/overlap_prefix_oracle.cpp -o "$t/oracle"
{ git rev-parse HEAD;git status --porcelain;sha256sum "$t/"{oracle,overlap_resident}; } >"$d/binaries.sha"
run=(systemd-run --user --quiet --wait --pipe --collect -p Nice=10 -p MemorySwapMax=0)
for arm in verify one four two repeat-one; do
    a=$d/$arm;mkdir -p "$a/in";cp --reflink=never "$s/dict-adj-7B0aj9/columns/"{parse.u32,dictionary.refs} "$a/in/";cp --reflink=never "$s/pile-slice100.bin" "$a/in/corpus";input=("$a/in/"*)
    if [[ $arm == verify ]];then cp --reflink=never "$a/in/corpus" "$a/oracle-corpus";input+=("$a/oracle-corpus");fi
    python3 gates/private_cold.py "${input[@]}"; echo 'private arm inputs: zero resident pages' >"$a/cold.log"
    case $arm in verify|one|repeat-one) n=1;; four) n=4;; two) n=2;; esac
    job=("${run[@]}" -p MemoryMax=2G -p CPUQuota=400% /usr/bin/python3 "$repo/gates/overlap_radix_stage.py" "$arm" "$a" "$t/overlap_resident" "$n")
    if [[ $arm == verify ]]; then
        "${job[@]}" | "${run[@]}" -p MemoryMax=140M -p CPUQuota=100% /usr/bin/time -v -o "$a/oracle.time" "$t/oracle" "$a/oracle-corpus" >"$a/oracle.log"
    else "${job[@]}";fi
done
python3 gates/overlap_radix_stage.py score "$d";echo "completed receipt=$d"
