# BCR lane journal, 2026-09-30 UTC

No retained artifact was modified. Inputs and PFP reference outputs in other
trees were read only. No yeast235 or full fragment BCR gate was launched:
the flat-run prototype has O(nr) time and is capped at one million runs.

## Local gates

```
g++ -std=c++17 -O2 -Wall -Wextra -Werror -o /tmp/bcr_frontend_lane bit6/bcr_frontend.cpp
python3 bit6/test_bcr_frontend.py --binary /tmp/bcr_frontend_lane --pfp /home/erikg/.cache/xsa/81c4a8c0271c4096ec4c983e33b22ba23ccdfaac12bdc5db9a5bf1b5ef867f28/pfp++ --rpfbwt /home/erikg/.cache/xsa/81c4a8c0271c4096ec4c983e33b22ba23ccdfaac12bdc5db9a5bf1b5ef867f28/rpfbwt
```

Result: compile PASS; 33 independent tiny cyclic-SA oracles PASS across all
four serialized outputs; PFP byte identity PASS for 9,705-byte near-duplicate
and distinct collection; PFP byte identity PASS for 10,005-byte collection
with 12 remapped bytes. Each PFP gate compares `.rlebwt`, `.rlebwt.meta`,
`.ssa`, and `.ssa_t` byte for byte and raises at the first difference. A
separate no-clobber retry refused an existing prefix and preserved its BWT
SHA-256 digest.

Retained local synthetic build: `synthetic.txt`, `pfp.*`, `bcr.*`, and
`synthetic-bcr.time`. `/usr/bin/time -v`: 0.30 s wall, 2,048 KiB peak RSS.
Retained local nonidentity-remap build: `remap-synthetic.txt`, `remap-pfp.*`,
`remap-bcr.*`, and `remap-bcr.time`: 0.24 s wall, 2,048 KiB peak RSS.

## Read-only large reference inventory

Yeast235 source:
`/tmp/sxi-repair2/yeast235/xsa-build-26al38ee/collection.txt`

Yeast235 PFP prefix:
`/tmp/sxi-repair2/yeast235/xsa-build-26al38ee/parse`

Remapped fragment source:
`/home/erikg/sxgc-piletest/pile-frag.txt`

Remapped fragment PFP prefix:
`/home/erikg/worktrees/sxgc/pi-worktree-e7286239-b261-424c-8585-f2604e5d83e0-s0-0/vendor/byte-remap-gates/xsa-build-_69d9g2j/parse`

Metadata verified read only: yeast padded n=3,336,986,769, r=100,905,044;
fragment padded n=1,082,130,223, r=397,723,016. The fragment retained
PFP parse + level 2 + rpfbwt stage times total 2,066.3 seconds, max stage
RSS 18,479,248 KiB. The endpoint adapter separately took 390.6 seconds and
28,257,076 KiB. These are PFP observations, not BCR measurements.

## Pending acceptance gates

1. Implement block-based dynamic rank and insertion with bounded sample
   recovery and compact samples as described in `BCR_FRONTEND_DESIGN.md`.
2. Run BCR on yeast235 and remapped fragment once, in private worktree scratch,
   with `/usr/bin/time -v`; compare each of four outputs with `/usr/bin/cmp`.
3. Verify sample output downstream via `rpfbwt_endpoints` and exact `.ri4`
   plus `head_sa` checks. No full gate is claimed by this journal.
