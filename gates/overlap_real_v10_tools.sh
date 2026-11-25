#!/bin/bash
# Compile/check once. Synthetic empty/partial pipes only; no corpus build in this service.
set -euo pipefail
cd "$(dirname "$0")/..";umask 077
if [[ $# == 2 && $1 == compile ]];then
 d=$2;[[ $d == /tmp/real-v10-tools-* && $d == "$(realpath "$d")" && -O $d && $(stat -c %a "$d") == 700 && ! -e $d/witness ]]
 cg=/sys/fs/cgroup$(awk -F'::' '$1=="0"{print $2}' /proc/self/cgroup);[[ $(<"$cg/memory.max") == 1073741824 && $(<"$cg/memory.swap.max") == 0 && $(<"$cg/cpu.max") == '200000 100000' ]]
 mkdir -m700 "$d"/{old,tmp,tests};export TMPDIR=$d/tmp CARGO_TARGET_DIR=$d/build CARGO_BUILD_JOBS=2 CARGO_INCREMENTAL=0
 ref=6b87aa1620505cff02bc93e6c008483d0d895a83;git archive "$ref" xsa/src | tar -xC "$d/old";export CARGO_MANIFEST_DIR=$d/old/xsa CARGO_PKG_VERSION=0.1.0 OUT_DIR=/tmp/v10-fixture-1001-1206931/tools/xsa
 printf '%s  %s\n' 59966de4415cc35ca393006c1c6b33f337f0dd1769d289e0552e7d702589bc5a "$OUT_DIR/bundled_tools.rs" | sha256sum -c -
 python3 -BE - "$d" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1])/'old/xsa/src/main.rs';s=p.read_text();assert s.count('fn main() {')==1;p.write_text(s.replace('fn main() {',Path('gates/overlap_real_v10_join.rs').read_text()+'\nfn main() {\nif std::env::args().nth(1).as_deref()==Some("real-v10-join"){real_v10_join();return;}'))
PY
 deps=$HOME/sxgc/xsa/target/release/deps;flags=();for k in flate2 memmap2 rayon serde_json sha2 tempfile tiny_http;do a=("$deps"/lib"$k"-*.rlib);[[ ${#a[@]} == 1 ]];flags+=(--extern "$k=${a[0]}");done
 rustc --version;rustc --crate-name xsa --edition=2021 -C opt-level=2 -C codegen-units=4 -L "dependency=$deps" "${flags[@]}" "$d/old/xsa/src/main.rs" -o "$d/xsa"
 cargo build --locked --offline --release --manifest-path xsa/Cargo.toml -p pfp --bin pfp_sort_r
 cp -- "$CARGO_TARGET_DIR/release/pfp_sort_r" "$d/sink";g++ -std=c++20 -O2 -Wall -Wextra -Werror gates/overlap_finish_row_fence.cpp -lcrypto -o "$d/witness"
 python3 -BE gates/overlap_real_v10_tools_test.py "$d/sink" "$d/tests"
 sha256sum "$d"/{sink,xsa,witness} >"$d/tools.sha";exit
fi
[[ $# == 0 ]];exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75;python3 -BE gates/overlap_timing_idle.py
[[ $(df -B1 --output=avail /tmp|tail -1) -ge 2147483648 ]];d=$(mktemp -d /tmp/real-v10-tools-XXXXXX);echo "REAL_V10_TOOLS=$d";trap 'rc=$?;echo launcher=$rc >>"$d/exits.log";if ((rc));then touch "$d/stop.requested";fi' EXIT
git rev-parse HEAD >"$d/base";sha256sum xsa/pfp/src/{main,unit,v8_sxls,v8_piece_pool}.rs gates/overlap_real_v10_{tools.sh,join.rs,tools_test.py} gates/overlap_finish_row_fence.cpp gates/overlap_finish_{row_fence,fold_fence}.hpp >"$d/sources.sha"
cores=$(python3 -BE -c 'import sys;sys.path.insert(0,"gates");from overlap_parse64_study import cores;print(",".join(map(str,cores()[:2])))');rc=0
systemd-run --user --quiet --pipe --wait --collect --expand-environment=no -p MemoryMax=1G -p MemorySwapMax=0 -p CPUQuota=200% -p Nice=10 -p IOAccounting=yes -p LimitCORE=0 -p LimitFSIZE=192M -p RuntimeMaxSec=240s -p TimeoutStopSec=3s taskset -c "$cores" env PATH="$HOME/.cargo/bin:/usr/bin:/bin" bash "$PWD/gates/overlap_real_v10_tools.sh" compile "$d" >"$d/build.log" 2>&1 || rc=$?
echo compile=$rc >>"$d/exits.log";(( rc == 0 )) || exit "$rc";sha256sum -c "$d/sources.sha";echo "REAL_V10_TOOLS_VERIFIED=$d"
