#!/bin/bash
# Integration on the banked 6.3 MB synthetic v10 text only. No Pile run or finished index.
set -euo pipefail
cd "$(dirname "$0")/.."; umask 077
sandbox=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
front=$sandbox/pos64-front-10g-XTNbSv/front; rank=$sandbox/raw64-p1-10g-TyJqjC/parse64
emit=/tmp/epoch-array-fence-PtDPqy/emit; ref=/tmp/native-v10-p4oy1a/final/ref/corpus.sxi2
if [[ $# == 2 && $1 == tools ]];then
    d=$2; [[ $d == /tmp/ctx-rowpipe-* && $d == "$(realpath "$d")" && -O $d && $(stat -c %a "$d") == 700 && ! -e $d/xsa ]]
    cg=/sys/fs/cgroup$(awk -F'::' '$1=="0"{print $2}' /proc/self/cgroup)
    [[ $(<"$cg/memory.max") == 1073741824 && $(<"$cg/memory.swap.max") == 0 && $(<"$cg/cpu.max") == '200000 100000' ]]
    # Compile the current Rust CLI without replacing the installed xsa or rebuilding native stages.
    # The bundle include list from the original fixture points at its immutable cache. No bundled executable runs here.
    export OUT_DIR=/tmp/v10-fixture-1001-1206931/tools/xsa CARGO_MANIFEST_DIR=$PWD/xsa CARGO_PKG_VERSION=0.1.0
    printf '%s  %s\n' 59966de4415cc35ca393006c1c6b33f337f0dd1769d289e0552e7d702589bc5a "$OUT_DIR/bundled_tools.rs" | sha256sum -c -
    deps=$PWD/xsa/target/release/deps; flags=();for k in libc flate2 memmap2 rayon serde_json sha2 tempfile tiny_http;do files=("$deps"/lib"$k"-*.rlib); [[ ${#files[@]} == 1 ]]; flags+=(--extern "$k=${files[0]}"); done
    rustc --version; rustc --crate-name xsa --edition=2021 -C opt-level=1 -C codegen-units=4 -L "dependency=$deps" "${flags[@]}" xsa/src/main.rs -o "$d/xsa"
    g++ -std=c++20 -O2 -Wall -Wextra -Werror gates/overlap_row43_oracle.cpp -o "$d/oracle"; exit 0
fi
[[ $# == 0 ]];exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75
python3 -BE gates/overlap_timing_idle.py
printf '%s  %s\n' 463199aa720822e702082d34edfd290a6d508e0b27cffad03e8063f8cb2de5f6 "$front" 668f1a3a861fbca4c578fa643db73f4204be1e5a2e1f9e864131bb5586dafefe "$rank" 109d1562dce50ea42c4c6165db9a1e37e6d33de18209132b0415f31b73652a5b "$emit" | sha256sum -c -
d=$(mktemp -d /tmp/ctx-rowpipe-XXXXXX);echo "ROWPIPE_FIXTURE=$d";git rev-parse HEAD >"$d/base";sha256sum xsa/src/{ctx,ctx_row43_pipe}.rs xsa/Cargo.{toml,lock} gates/overlap_{v10_pipe_fixture.sh,v10_pipe_fixture.py,row43_oracle.cpp} >"$d/sources.sha"
trap 'rc=$?;echo launcher=$rc >>"$d/exits.log"; if ((rc));then touch "$d/stop.requested";fi' EXIT
cores=$(python3 -BE -c 'import sys;sys.path.insert(0,"gates");from overlap_parse64_study import cores;print(",".join(map(str,cores()[:2])))')
u=(systemd-run --user --quiet --wait --pipe --collect --expand-environment=no -p MemorySwapMax=0 -p CPUQuota=200% -p LimitCORE=0 -p Nice=10 -p IOAccounting=yes -p TimeoutStopSec=3s)
phase(){ local tag=$1; shift;local rc=0; "$@" >"$d/$tag.log" 2>&1 || rc=$?;echo "$tag=$rc" >>"$d/exits.log";return "$rc"; }
phase tools "${u[@]}" -p MemoryMax=1G -p RuntimeMaxSec=150s -p LimitFSIZE=128M taskset -c "$cores" /usr/bin/env -u BASH_ENV -u ENV PATH="$PATH" /bin/bash --noprofile --norc "$PWD/gates/overlap_v10_pipe_fixture.sh" tools "$d"
sha256sum "$d"/{xsa,oracle} "$front" "$rank" "$emit" >"$d/tools.sha"
phase stream "${u[@]}" -p MemoryMax=256M -p RuntimeMaxSec=110s -p LimitFSIZE=16M taskset -c "$cores" /usr/bin/env OMP_DYNAMIC=FALSE OMP_STACKSIZE=1M OMP_WAIT_POLICY=PASSIVE GOMP_SPINCOUNT=0 /usr/bin/python3 -BE "$PWD/gates/overlap_v10_pipe_fixture.py" "$d/run" "$front" "$rank" "$emit" "$d/xsa" "$d/oracle" "$ref"
sha256sum -c "$d/sources.sha"; echo "ROWPIPE_COMPLETE=$d"
