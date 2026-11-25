#!/bin/bash
# Snapshot the frozen trie writer/reader verbatim. Compile and run a tiny
# generated doc-aware fixture only; no original corpus/member open here.
set -euo pipefail;cd "$(dirname "$0")/..";umask 077;exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75;python3 -BE gates/overlap_timing_idle.py
d=$(mktemp -d /tmp/m20-cut-tools-XXXXXX);echo "M20_CUT_TOOLS=$d";trap 'rc=$?;echo launcher=$rc >>"$d/exits.log";if ((rc));then touch "$d/stop.requested";fi' EXIT
mkdir "$d/src" "$d/tmp";cp gates/{overlap_m20_cut_reader.rs,overlap_m20_cut_fixture.py,overlap_m20_prefix_cuts.py,overlap_prefix_vocab_bounds.py} "$d/src/";git rev-parse HEAD >"$d/base.log"
python3 -BE - "$d" <<'PY'
from pathlib import Path
from hashlib import sha256
from subprocess import check_output
import sys
p=Path(sys.argv[1]);ref='6b87aa1620505cff02bc93e6c008483d0d895a83:xsa/src/ctx.rs';s=check_output(['git','show',ref]);part=s[s.index(b'pub const END: u16 = 0;'):s.index(b'// --------------------------------------------------------------- SA from')];assert sha256(s).hexdigest()=='d6767913a9a809c125b8ab68d3b1fd0646bc58af852919fe1fd6a11952dd4866' and sha256(part).hexdigest()=='1031f0e49558e3fc2d0ba3dbd627822ec39f8f6bcc885db8e260c0df5dc1263d' and b'pub const TRIE_VERSION: u64 = 3;' in s
(p/'src/frozen.rs').write_bytes(b'use super::die;\npub const TRIE_VERSION: u64 = 3;\n'+part);(p/'frozen.log').write_text(f'{ref} {sha256(s).hexdigest()}\nverbatim_section {sha256(part).hexdigest()}\n')
PY
(cd "$d/src";sha256sum ./*)>"$d/sources.sha";cc=$(rustup which rustc);echo "d3a664c970a9fd8361b64194861bebc1ae37b9054e5ee3400dc1c9e691797eea  $cc" >"$d/compiler.sha";sha256sum -c "$d/compiler.sha"
one=$(python3 -BE -c 'import os;print(min(os.sched_getaffinity(0)))');end=$(( $(date +%s)+100 ));cd "$d"
u=(systemd-run --user --quiet --pipe --wait --collect --expand-environment=no -p CPUQuota=100% -p MemorySwapMax=0 -p Nice=10 -p IOAccounting=yes -p LimitCORE=0 -p LimitSTACK=8M -p LimitFSIZE=16M -p UMask=0077 -p TimeoutStopSec=3s)
phase(){ local name=$1 rc=0;shift;"$@" >"$d/$name.log" 2>&1 || rc=$?;echo "$name=$rc" >>"$d/exits.log";return "$rc"; }
phase compile "${u[@]}" -p "WorkingDirectory=$d" -p MemoryMax=512M -p RuntimeMaxSec=45s taskset -c "$one" env "TMPDIR=$d/tmp" "$cc" --edition=2021 -D warnings -C opt-level=2 -C codegen-units=1 "$d/src/overlap_m20_cut_reader.rs" -o "$d/probe"
(cd "$d/src";sha256sum -c "$d/sources.sha");sha256sum "$d/probe" >"$d/tool.sha";phase setup "${u[@]}" -p "WorkingDirectory=$d" -p MemoryMax=256M -p RuntimeMaxSec=10s taskset -c "$one" /usr/bin/python3 -BE "$d/src/overlap_m20_cut_fixture.py" setup "$d/fixture"
rc=0;"${u[@]}" -p "WorkingDirectory=$d/fixture" -p MemoryMax=256M -p RuntimeMaxSec=20s taskset -c "$one" "$d/probe" fixture "$end" >"$d/fixture/dump" 2>"$d/observer.log" || rc=$?;echo "observe=$rc" >>"$d/exits.log";(( rc==0 )) || exit "$rc"
phase check "${u[@]}" -p "WorkingDirectory=$d" -p MemoryMax=256M -p RuntimeMaxSec=20s taskset -c "$one" /usr/bin/python3 -BE "$d/src/overlap_m20_cut_fixture.py" check "$d/fixture"
