#!/bin/bash
set -euo pipefail;cd "$(dirname "$0")/..";umask 077
if [[ ${1:-} == compile ]];then
 d=$2;[[ $d == /tmp/rolling-ladder-tools-* && $d == "$(realpath "$d")" && -O $d && ! -e $d/sink ]];mkdir "$d"/{tmp,old};export TMPDIR=$d/tmp CARGO_TARGET_DIR=$d/build CARGO_BUILD_JOBS=2 CARGO_INCREMENTAL=0
 cargo build --locked --offline --release --manifest-path xsa/Cargo.toml -p pfp --bin pfp_sort_r;cp "$d/build/release/pfp_sort_r" "$d/sink"
 git archive 6b87aa1620505cff02bc93e6c008483d0d895a83 xsa/src | tar -xC "$d/old";export CARGO_MANIFEST_DIR=$d/old/xsa CARGO_PKG_VERSION=0.1.0 OUT_DIR=/tmp/v10-fixture-1001-1206931/tools/xsa
 echo "59966de4415cc35ca393006c1c6b33f337f0dd1769d289e0552e7d702589bc5a  $OUT_DIR/bundled_tools.rs" |sha256sum -c -
 python3 -BE - "$d" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1])/'old/xsa/src/main.rs';s=p.read_text();assert s.count('fn main() {')==1;p.write_text(s.replace('fn main() {',Path('gates/rolling_ladder_join.rs').read_text()+'\nfn main() {\nif std::env::args().nth(1).as_deref()==Some("rolling-join"){rolling_join();return;}'))
PY
 deps=$HOME/sxgc/xsa/target/release/deps;flags=();for k in flate2 memmap2 rayon serde_json sha2 tempfile tiny_http;do a=("$deps"/lib"$k"-*.rlib);[[ ${#a[@]} == 1 ]];flags+=(--extern "$k=${a[0]}");done
 rustc --crate-name xsa --edition=2021 -C opt-level=2 -C codegen-units=4 -L "dependency=$deps" "${flags[@]}" "$d/old/xsa/src/main.rs" -o "$d/join";sha256sum "$d"/{sink,join} >"$d/tools.sha";exit
fi
[[ $# == 0 && $(git rev-parse HEAD) == "$(git rev-parse origin/main)" ]];source gates/overlap_pin.sh;overlap_pin_series gates/rolling_ladder_tools.sh
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75;python3 -BE gates/overlap_timing_idle.py;d=$(mktemp -d /tmp/rolling-ladder-tools-XXXXXX);echo "LADDER_TOOLS=$d";echo "$SXGC_PINNED_SOURCE" >"$d/source.log";trap 'rc=$?;echo launcher=$rc >>"$d/exits.log"' EXIT
cores=$(python3 -BE -c 'import sys;sys.path.insert(0,"gates");from overlap_parse64_study import cores;print(",".join(map(str,cores()[:2])))');rc=0
systemd-run --user --quiet --pipe --wait --collect --expand-environment=no -p CPUQuota=200% -p MemoryMax=1G -p MemorySwapMax=0 -p Nice=10 -p LimitCORE=0 -p LimitFSIZE=192M -p RuntimeMaxSec=300s -p TimeoutStopSec=3s taskset -c "$cores" env PATH="$HOME/.cargo/bin:/usr/bin:/bin" bash "$PWD/gates/rolling_ladder_tools.sh" compile "$d" >"$d/build.log" 2>&1 || rc=$?;echo compile=$rc >>"$d/exits.log";exit "$rc"
