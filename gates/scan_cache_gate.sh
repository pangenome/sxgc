#!/usr/bin/env bash
# Two passes over private banked 100 MB columns. No global cache manipulation.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
r=$(mktemp -d "$s/private-scan-XXXXXX"); echo "receipt=$r"
c=$s/br_measured_recursive_512000000_1795388550845345182/merged/modeb
cp --reflink=auto "$s/pile-slice100.bin" "$r/corpus"
files=("$r/corpus"); for f in dict_refpos.bin dict_len.bin pa.bin pstart.bin; do cp --reflink=auto "$c/$f" "$r/$f"; files+=("$r/$f"); done
# Bounded data shape: 5 read-only inputs plus 8 fixed-size (1 MiB) summaries.
(( $(du -sb "$r" | cut -f1) + 12*1024*1024 < 256*1024*1024 ))
for arm in 384:dictionary 384:physical 64:physical 64:dictionary; do
  mib=${arm%:*}; order=${arm#*:}; out=$r/$mib-$order
  python3 "$repo/gates/private_cold.py" "${files[@]}"
  echo "start $arm"; systemd-run --user --quiet --wait --pipe --collect -p "MemoryMax=${mib}M" -p MemorySwapMax=0 -p CPUQuota=200% -p Nice=10 "$repo/xsa/target/release/scan_replay" "$r" "$order" "$out" | tee "$out.log"
  for pass in 0 1; do cmp "$out.$pass" "$r/384-dictionary.0"; done
done
