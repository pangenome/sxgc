# Sourced by a matched-series launcher, before preparing private inputs.
# Pin Rust, C, Python, scores and the banked references together. Later commits
# in the integration checkout must not switch a binary between timed arms.
overlap_pin_series() {
    local script=$1 root sha snap; shift
    root=$(git rev-parse --show-toplevel); sha=$(git -C "$root" rev-parse --verify HEAD)
    if [[ ${SXGC_PINNED_SOURCE:-} == "$root:$sha" ]]; then
        [[ -z $(git -C "$root" status --porcelain --untracked-files=no) ]] || return 65
        return 0
    fi
    [[ -z $(git -C "$root" status --porcelain --untracked-files=no -- gates xsa) ]] || { echo 'refuse to time uncommitted study code' >&2;return 65; }
    snap=$(mktemp -d /tmp/overlap-study-src-XXXXXX)
    # Local read-only object alternates, private index and target/, no worktree
    # registration in the operator's checkout. Neither setup nor compilation is
    # part of the measured raw or prepared wall time. Preserve exact source.
    git clone --quiet --local --shared --no-checkout "$root" "$snap"
    git -C "$snap" sparse-checkout set gates xsa
    git -C "$snap" checkout --quiet --detach "$sha"
    export SXGC_PINNED_SOURCE="$snap:$sha"
    echo "STUDY_PIN commit=$sha source=$snap"
    cd "$snap" && exec bash "$script" "$@"
}
