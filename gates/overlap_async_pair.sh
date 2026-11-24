#!/usr/bin/env bash
# Optional same-binary cold 1 GB full products with the verifier attached (no hash-sink).
set -euo pipefail
[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1<=1795531169 && $1>$(date +%s) )) || exit 75
cd "$(dirname "$0")/..";source gates/overlap_pin.sh;overlap_pin_series gates/overlap_async_pair.sh "$1"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75
end=$1;repo=$PWD;s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox;d=$(mktemp -d "$s/async-pair-1g-XXXXXX");t=$d/tools;echo "ASYNC_SERIES receipt=$d"
py=/usr/bin/python3;pair=$repo/gates/overlap_async_pair.py;worker=$repo/gates/overlap_resident_gate.py
"$py" -BE "$pair" admit "$d" "$end";mkdir "$t"
comp(){ local n=$((end-$(date +%s)));((n>0)) || return 75;timeout -k 2 "$n" "$@"; }
unset XSA_BUILD_DICT_OPENMP CFLAGS RUSTFLAGS CC CARGO_ENCODED_RUSTFLAGS PYTHONOPTIMIZE;export CARGO_TARGET_DIR=$(mktemp -d /tmp/async-product-build-XXXXXX)
{
 comp cargo build --locked --offline --release -j1 --manifest-path xsa/Cargo.toml -p pfp --bin pfp_sort_r --bin overlap_resident
 cp "$CARGO_TARGET_DIR/release/"{pfp_sort_r,overlap_resident} "$t/"
 comp cc -O2 -Wall -Wextra -Werror -shared -fPIC -pthread gates/finish_stop_monitor.c -o "$t/monitor.so"
 comp cc -O2 -std=c11 -Wall -Wextra -Werror -shared -fPIC gates/finish_bitmap_tmpfs.c -o "$t/bitmap.so"
 for tool in oracle lexparse;do f=gates/overlap_prefix_oracle.cpp; [[ $tool == oracle ]] || f=gates/overlap_lexparse.cpp
    comp g++ -O2 -Wall -Wextra -Werror -std=c++17 "$f" -o "$t/$tool";done
 comp "$py" -BE gates/finish_bitmap_test.py "$t/bitmap.so";comp "$py" -BE gates/overlap_radix_test.py "$t/overlap_resident"
 comp "$py" -BE xsa/pfp/tests/overlap_async_audit.py "$t/overlap_resident";comp "$py" -BE gates/overlap_async_pair_test.py
 (cd "$t" && sha256sum bitmap.so monitor.so oracle lexparse pfp_sort_r overlap_resident >manifest.sha)
} >"$d/tools-and-fixtures.log" 2>&1
run=(systemd-run --user --quiet --collect --wait --pipe -p MemorySwapMax=0 -p Nice=10 -p LimitCORE=0 -p LimitNOFILE=8192 -p IOAccounting=yes)
for a in sync async;do
    dir=$d/$a;echo "AUDIT_ARM_START $a";(cd "$t" && sha256sum -c manifest.sha)
    "${run[@]}" -p MemoryMax=192M -p CPUQuota=100% "$py" -BE "$pair" prepare "$dir" "$t" "$a" "$end"
    "$py" -BE gates/overlap_timing_idle.py;set +e
    "${run[@]}" -p MemoryMax=20G -p CPUQuota=400% "$py" -BE "$worker" build-audit "$dir" "$t" "$a" "$end" | \
        "${run[@]}" -p MemoryMax=1400M -p CPUQuota=100% "$py" -BE "$worker" oracle-audit "$dir" "$t" "$a" "$end"
    rc=("${PIPESTATUS[@]}");set -e;printf 'build=%s oracle=%s\n' "${rc[@]}" |tee "$dir/exits.log";[[ ${rc[*]} == '0 0' ]]
    # Time the separate post-service certificate invocation, not a second row pass.
    /usr/bin/time -f 'cert_process_wall_s=%e cpu_user_s=%U cpu_system_s=%S' -o "$dir/certify.log" "$py" -BE "$worker" certify "$dir" packed-radix-4-batch
    "$py" -BE "$pair" single "$dir";echo "AUDIT_ARM_VERIFIED $a";done
"$py" -BE "$pair" score "$d";echo "AUDIT_SERIES_COMPLETE receipt=$d"
