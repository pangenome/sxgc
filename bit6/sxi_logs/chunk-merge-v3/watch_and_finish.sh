#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
journal=bit6/sxi_logs/chunk-merge-v3

for stage in monolithic-rpfbwt merge; do
    while ! rg -q 'Exit status:' "$journal/$stage.time"; do
        sleep 30
    done
    if ! rg -q 'Exit status: 0$' "$journal/$stage.time"; then
        printf 'FAILED %s\n' "$stage" > "$journal/pipeline.status"
        exit 1
    fi
done

if ! bash "$journal/finish_gate.sh" > "$journal/finish-gate.log" 2>&1; then
    printf 'FAILED four-file-or-chi-gate\n' > "$journal/pipeline.status"
    exit 1
fi
if ! python3 "$journal/summarize.py" > "$journal/summarize.log" 2>&1; then
    printf 'FAILED summary\n' > "$journal/pipeline.status"
    exit 1
fi
git diff --cached --name-only > "$journal/no-staged-files.log"
if test -s "$journal/no-staged-files.log"; then
    printf 'FAILED staged-files\n' > "$journal/pipeline.status"
    exit 1
fi
printf 'PASS\n' > "$journal/pipeline.status"
