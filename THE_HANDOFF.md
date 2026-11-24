# THE HANDOFF — one writer to the next

Written by the orchestrator at the operator's order, per the incoming
owner's protocol. The prior writer (the GLM lane) is checkpointing and
pausing; its final report (last passing / next failing commands, any
in-flight run's result) appends below in RESULTS_TAPE_READER.md. Do
not take code from the paused writer — resume here as the ONE writer.

## The mission (one paragraph)

Suffixient arrays (chi) over massive text. HPRC v2 done and banked.
The goal is the pile: /mnt/nvme2n1/erikg/pile.txt, 1.31 TB. The
machine that builds its container is pfp_sort: a one-mode, PFP-based,
scale-invariant sorter — one streaming parse, one byte-sort (whole
phrases), the census deciding everything BY NUMBER (bins, levels),
SA construction at the deepest parse level, LCP carried not walked,
and an emit that is a pure concat. Never write text to disk; only
positions/IDs. No signals; cooperative stop only.

## Where the code is

- THE MACHINE: xsa/pfp/ (a Rust crate in the xsa workspace) — the
  full rewrite, Amendment R, complete and G0 9/9 (commit 9b41d0b) plus
  the instrumentation pass (8f332e8). Build: cargo in xsa/. Binary:
  xsa/target/release/pfp_sort_r. The finish/container side is xsa's
  existing Rust (xsa_finish binaries under TAPEREADER/bins-p1/bin).
- THE REFERENCE ORACLE (frozen, do not modify): xsa/runtime/tools/
  binbwt/pfp_sort.cpp. It holds the W+B+L amendments and PASSED the
  1 GB / 3 GB identity ladders before its level code showed an
  unreproducible ghost. Its receipts define byte semantics.
- SPECS OF RECORD: TAPE_VISION.md, AMENDMENT_WIDE_BLADES.md,
  AMENDMENT_B_BINS.md (incl. the B4 operator's clause: scratch
  budget <= 2xN at EVERY scale), AMENDMENT_L_LEVELS.md (incl. L4:
  the level comes first, libsais64 only if no level condenses),
  AMENDMENT_R_REWRITE.md.

## Certified anchors (the identity gates)

- Sandbox 100 MB: product SHA 6325c843c0d9aa9be00d...f341
  (n=100,000,504, chi=29,716,347), corpus
  /mnt/nvme2n1/erikg/TAPEREADER/sandbox/pile-slice100.bin
- G0 band-invariance record: 31a764b6d6787d5e...5644eb3 — ALREADY
  reproduced byte-identical by the Rust machine at two dials.
- 1 GB: b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3
- 3 GB: the run14e nine-member container (member SHAs in
  RESULTS_TAPE_READER.md; corpus /mnt/nvme2n1/erikg/KEYRADIX/c3g.txt)
- Banked 100 GB rehearsal: /mnt/nvme2n1/erikg/PILE100-REHEARSAL/
  pile-head100.sxi2 (65.53 GB) — the Rungs-4+5 identity target.
- Fixtures: /tmp/tr_p3/fixtures/ (adv, docsep, eofrun, internsep,
  mixed, mixed64, period2, r0, single, refuse) — G0 runner scripts
  in /tmp/tr_p3/ (run_gates.sh). The oracle fixtures stay C++.

## Known issue (the only open defect)

refuse.txt — the degenerate-census corpus (9 phrases, 4 distinct).
Three loop incarnations fixed (record format, pairs path, and the
latest: the word-chunk law + cost-naming receipts, commit 8f332e8);
the latest fix's verdict was pending at pause. All loops now carry
die-with-numbers budgets (SS_L1_PROGRESS rule=stalls_die) — a stall
is a named die() in seconds, never a silent grind.

## THE DEFINITION OF DONE (the operator-adopted milestone — no
fresh design, no rewrite, no scope changes)

1. pfp_sort_r runs the RECURSIVE path (XSA_SA_CEILING=2000000 forces
   >= 2 levels) on the 100 MB sandbox corpus; the finish produces a
   container; that container is byte-identical to the certified
   reference (SHA 6325c843...f341). One arm, receipts journaled.
2. Repeat at a second dial arm (e.g. the 16 MB dial) with actual
   memory (DIAL_LAW: anon peak <= dial), disk (SCRATCH_LAW: <= 2xN,
   per-component SS_SCRATCH_PEAK), and wall time recorded.
3. Only then the ladder: 1 GB -> 3 GB identity -> the 100 GB launch
   package (walls + scratch curve + census level decision) to the
   operator. The pile run is the operator's launch order, not ours.

## Operating rules (all standing, all learned the hard way)

One arm per bash call; detach anything > 10 min (setsid nohup) and
poll with short calls — no wrapper loops (three zombie-wrapper
incidents on the prior writer). No wall-clock timeouts on
measurement runs (truncated = VOID, SS_DONE is the completion
receipt). Commit per gate, push verified, origin == HEAD. Scratch
under /mnt/nvme2n1/erikg/TAPEREADER only. The G0 oracle is the C++
suite — an independent checker in another language is a feature.
rows_sorted_total == phrase count is a die(). The dial and the
scratch law are asserted, never assumed.

## The receipts

Everything is in RESULTS_TAPE_READER.md (STATE OF RECORD on top) and
the git log (feat/tape-reader; origin == HEAD at the handoff commit).
The prior writer's full journals live in its session; the numbers
that matter are in the committed docs.
