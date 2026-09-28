#!/usr/bin/env bash
# A prefix is created once. Repeated invocations verify, never silently re-bless it.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
prefix=$(realpath -m "${1:?usage: tools/build_all.sh NEW_PREFIX}")
case "$prefix/" in
  /home/erikg/sxgc/tools-built/*|/home/erikg/sxgc/vendor/*|/home/erikg/sxgc-466/*)
    echo "refusing protected build prefix: $prefix" >&2; exit 1 ;;
esac
if [[ -e "$prefix/MANIFEST.sha256" ]]; then
  exec python3 "$root/tools/tool_manifest.py" verify --prefix "$prefix" --manifest "$prefix/MANIFEST.sha256"
fi
# Reject partial/existing trees; retry in a new prefix after a failed build.
mkdir "$prefix"
export BUILD_JOBS=${BUILD_JOBS:-4}
export CARGO_BUILD_JOBS=$BUILD_JOBS
export SXI_DEPS="$prefix/.build/deps"
export SXI_RPFBWT_BUILD="$prefix/.build/rpfbwt-build"
mkdir -p "$SXI_DEPS"
python3 - "$root" "$prefix" <<'PY'
import json, pathlib, subprocess, sys
root, prefix = map(pathlib.Path, sys.argv[1:])
pins = json.loads((root / 'tools/upstream.lock.json').read_text())
for name, pin in pins.items():
    dest = prefix / '.build/deps' / name
    subprocess.run(['git', 'clone', '--no-checkout', pin['url'], str(dest)], check=True)
    subprocess.run(['git', '-C', str(dest), 'checkout', '--detach', pin['commit']], check=True)
    if name in ('agc', 'sdsl', 'htslib'):
        subprocess.run(['git', '-C', str(dest), 'submodule', 'update', '--init', '--recursive'], check=True)
for name, patch in [('pfp', 'bit6/patches/pfp_agc.patch'),
                    ('rpfbwt', 'bit6/patches/rpfbwt_emit_tails.patch')]:
    subprocess.run(['git', '-C', str(prefix / '.build/deps' / name),
                    'apply', str(root / patch)], check=True)
PY
flags=(-DCMAKE_BUILD_TYPE=Release)
for dep in "$SXI_DEPS"/*; do
  name=$(basename "$dep")
  flags+=("-DFETCHCONTENT_SOURCE_DIR_${name^^}=$dep")
done
make -C "$SXI_DEPS/htslib" -j "$BUILD_JOBS" libhts.a
flags+=("-DHTSlib_INCLUDE_DIR=$SXI_DEPS/htslib/htslib"
        "-DHTSlib_LIBRARY=$SXI_DEPS/htslib/libhts.a")
PFP_AGC_FORK="$SXI_DEPS/pfp" PFP_AGC_LIBRARY="$SXI_DEPS/agc" \
  bash "$root/tools/build_pfp_agc.sh" "${flags[@]}"
cp "$SXI_DEPS/pfp/build/pfp++" "$prefix/pfp++"
cmake -S "$SXI_DEPS/rpfbwt" -B "$SXI_RPFBWT_BUILD" "${flags[@]}"
cmake --build "$SXI_RPFBWT_BUILD" -j "$BUILD_JOBS" --target rpfbwt
cp "$SXI_RPFBWT_BUILD/rpfbwt" "$prefix/rpfbwt"
bash "$root/tools/build_sxi_tools.sh" "$prefix"
python3 "$root/tools/tool_manifest.py" create --prefix "$prefix" --manifest "$prefix/MANIFEST.sha256"
python3 "$root/tools/tool_manifest.py" verify --prefix "$prefix" --manifest "$prefix/MANIFEST.sha256"
echo "Built verified SXI toolset in $prefix"
