# Pairwise BCR tree gate

`run_fragment.sh` compiles the worktree source, runs the 26-collection
synthetic gate, and merges the retained 16 fragment chunks as eight, four,
two, and one pairs. It then runs `finish_gate.sh`: four bytewise comparisons
against the retained PFP reference, endpoints, slim, and `chi-rspace`. Logs,
status, and the final table live here. Large new artifacts live under
`/home/erikg/sxgc/vendor/chunk-merge-tree/`; the retained chunks and
reference are read only.

Each intermediate pair stores a padded BCR state (`.state`) and its text in
reverse byte order (`.rev`). At the next level, the worker loads the right
state and prepends the left reverse stream. Level one uses the original SXCR
LF traversal once per leaf. BCR state is needed to avoid rebuilding the
right side; the reverse stream is needed to feed the left side in the proven
order. The final node writes the four banked raw files and no reverse stream.

The driver limits concurrency to eight workers for the fragment. It prints
worker PIDs and per-level walls to `tree.log`; each worker has its own log in
the artifact `states/` directory. `pipeline.status` contains the owning
shell PID and eventual exit result. This work does not touch the original
corpus or any retained reference artifact.

`summarize.py` writes `TABLE.md` only after the full four-file and chi gate
passes. Its 50 GB figures are a linear scheduling projection. The existing
serial 1.08 GB merge peaked at 7.14 GB RSS; a 50 GB final BCR state is likely
above the 64 GB budget. This implementation does not claim that pile-scale
execution is feasible within that budget.
