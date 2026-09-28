# K10 endpoint fix and from-zero acceptance

Published: `/mnt/nvme3n1/erikg/sxgc-k10sep/from-zero-p_fwd37s/k10.sxi`.
Canonical measured chi: **1,627,067,257**.
Boundary-delta identity: **1,627,067,257 = 1,627,063,183 + (+4,074)**.
This is a measured cardinality difference across conventions, not an independently
proved BCR witness-set identity or an attribution of every difference to separators.

Run A and run B each performed all parse, producer, endpoint, slim, sweep,
write, and validation stages from empty, separate scratch directories with
16 threads. Run B used only A's fresh endpoints as byte gates. Both complete
SXI publications are byte-identical. Stage commands, timings, and RSS are in
the journals and `from-zero-stages.json`; member sizes are in `member-sizes.json`.
Sampled fresh-build process-tree peak RSS: 133.931 GB.

The retained k10 completion was exclusively a debug gate: 7 seam classes,
1,161 rows, 8 discovery steps; n=30,151,407,545, r=1,859,825,862. Its ri4 has
17,435,869,551 bytes and heads have 14,878,606,896 bytes. No retained k10
slim, sweep, or write stage was run. Originals retain their sizes and mtimes.
The debug process peaked at 130,763,968 KiB; sampled combined endpoint
regression jobs peaked at 141.194 GB, below 150 GB.

Root cause: the failed journal omitted TERMINAL_HEX and used a stale binary.
Live instrumentation localized the check to row 0, 0x1e versus default 0x0a.
The adapter now validates and infers the padding-row terminal for legacy
callers and shares clean/straddled padding checks. No tap, producer, pipeline,
front-end, seam-repair, slim, sweep, or writer change was made by this task.

Validation: 7 new padding checks, 2,214 cyclic endpoint cases, 74 dense
aggregate/witness checks, 47 separator checks, and exact yeast/yeast235
endpoint regressions pass. The first separator attempt used a stale agc2flat
without --sep; rerunning with the existing separator-capable binary passed.
No staging or commits; git diff --check passes. Prior uncommitted lane work
is preserved. See fix.diff, diagnosis.md, REVIEW.md, and final-hygiene.json.
Both runs pin XSA_TOOLS=/tmp/k10-endpoints-debug/tools and
XSA_PIPELINE=/tmp/sxgc-laneV/bit6/sxi_pipeline.py. The legacy
/tmp/laneV/tools installation was not overwritten; see build-environment.json.

The required independent reviewer gate remains pending. No supervisor tool
was available, and no external approval is claimed.
