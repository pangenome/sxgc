# Tape Reader working agreement

Work in the `main` checkout (`~/sxgc`); commit verified changes and receipts to `main` and push to `origin/main`. Historical lanes are evidence, not the integration target. Do not remove other worktrees or rewrite their history.

The memory-bounded scaling target is **strictly less than 10×** the resident wall time per input byte, at matching phase boundaries, product profile and verification. State the baseline, RAM/swap/CPU grants, scratch, and timings from raw input vs from prepared columns separately. Do not promote a larger rung merely because the cap was respected.

A stopped experiment is evidence, not a stopping point. Carry on through process notifications and failed gates: preserve the receipt, diagnose the limiting mechanism, and start the next safe, informative study. Never treat a running but stranded worker as useful progress. Respect cooperative stops and the existing resource/scale gates; if blocked on a production arm, study independently on small inputs instead.

When the implementation path is unclear, run a theoretical study: state the invariant, cost model, assumptions and counterexamples, derive a falsifiable prediction, then test it empirically. Long-running, bounded studies are appropriate. Use well-distributed small corpus samples to probe the same access pattern; match working-set/cache and batch/input ratios, check skew and phase-specific behavior across sizes, and distinguish modeled misses from physical IO. Report what transfers and what remains unproven.
