#!/usr/bin/env bash
# Isolated native-AGC PFP++ build. Never writes /home/erikg/pfp.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
pfp_dir=${PFP_AGC_FORK:-$root/vendor/pfp-agc-fork}
agc_dir=${PFP_AGC_LIBRARY:-$root/vendor/pfp-agc-library}
[[ $(realpath -m "$pfp_dir") != /home/erikg/pfp ]] || { echo 'Refusing protected PFP checkout' >&2; exit 1; }
if [[ ! -d "$pfp_dir/.git" ]]; then
  git clone https://github.com/marco-oliva/pfp.git "$pfp_dir"
  git -C "$pfp_dir" checkout 1a5f114ae026c18e7c0049ceace1a5eabc8be44a
  git -C "$pfp_dir" apply "$root/bit6/patches/pfp_agc.patch"
fi
[[ $(git -C "$pfp_dir" rev-parse HEAD) == 1a5f114ae026c18e7c0049ceace1a5eabc8be44a ]] || { echo 'PFP revision differs from pinned revision' >&2; exit 1; }
git -C "$pfp_dir" apply --reverse --check "$root/bit6/patches/pfp_agc.patch"
if [[ ${PFP_SYNCMER:-0} == 1 ]]; then
  if ! git -C "$pfp_dir" apply --reverse --check "$root/bit6/patches/pfp_syncmer.patch" 2>/dev/null; then
    git -C "$pfp_dir" apply "$root/bit6/patches/pfp_syncmer.patch"
  fi
  git -C "$pfp_dir" apply --reverse --check "$root/bit6/patches/pfp_syncmer.patch"
fi
if [[ ! -d "$agc_dir/.git" ]]; then
  git clone --recursive https://github.com/refresh-bio/agc.git "$agc_dir"
  git -C "$agc_dir" checkout e67e3fc865a459779118d3d4e9fbdf42c70ba75e
  git -C "$agc_dir" submodule update --init --recursive
fi
[[ $(git -C "$agc_dir" rev-parse HEAD) == e67e3fc865a459779118d3d4e9fbdf42c70ba75e ]] || { echo 'AGC revision differs from the tested adapter revision' >&2; exit 1; }
make -C "$agc_dir" -j"${BUILD_JOBS:-16}" libagc
cmake -S "$pfp_dir" -B "$pfp_dir/build" -DPFP_ENABLE_AGC=ON -DAGC_ROOT="$agc_dir" "$@"
cmake --build "$pfp_dir/build" -j"${BUILD_JOBS:-16}" --target pfp++ pfp++64 pfp_source_probe
