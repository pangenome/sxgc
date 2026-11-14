# NOTE TO SXGC: the 15:47 box freeze was YOUR s4 gate — fired from the stale `mainbin` (base-src), not your worktree binary

**From:** sysadmin incident response (erikg account, lambda01)
**When:** 2026-11-14 ~17:30 UTC
**Also read:** `/home/erikg/sysadmin/2026-11-14-mass-sigstop-procps-kill-incident.md` (full timeline + evidence)

## TL;DR

The box froze AGAIN at **15:47:40 UTC**, ~2h after your 13:00 relaunch. You probably
did not notice: every one of erikg's processes (including all your sessions) was
SIGSTOPped mid-flight until 17:27, when I unfroze them. The shooter this time was
**your own s4 gate's `xsa-main` driver (PID 1679364)** — running the **stale
`$WORK/stage/mainbin/xsa-main`** (sha256 `85924aeb…`, derived from
`base-src` = origin/main@4e275f6, which still has the vulnerable `signal_group`).
Its producer pgid was **1681117** — leading digit `1` → on this box's procps-ng
4.0.4, `kill -STOP -1681117` issues **`kill(-1, SIGSTOP)`** → SIGSTOP to every
process of the user. The `[kill] <defunct>` zombie under PID 1679364 is the receipt.

Your worktree binary (rebuilt 15:29:58, `process_control=none`) did NOT fire — it
is safe. Only the main-bin comparison arm was the landmine.

## What I did at ~17:27 (so you don't re-derive it)

1. **Killed both s4 driver trees** — they were frozen mid-governor-pause running
   OLD binaries; waking them would have re-fired `kill(-1, SIGSTOP)` (and the
   ceiling path is `kill(-1, SIGKILL)` — an account-wide massacre):
   `kill -9 1679363 1679364 1681117-1681119 1681124-1681126` (s4_main)
   `kill -9 1679352 1679353 1681489-1681491 1681496-1681498` (s4_ours)
   → the s4 gate you launched at 15:30 will have reported those builds as
   killed/failed. **Rerun s4** — your runs' scratch is retained (resumable).
2. **SIGCONTed the other ~58 frozen processes** (your sessions, mosh, tmux,
   systemd --user, the two `gates all` bash suites from 15:34/15:45, the emender
   GPU-lease bench, etc.).
3. **Patched `base-src`** (`$WORK/base-src/xsa/src/chain.rs` `signal_group` →
   `kill -s <sig> -- -<pgid>`) and rebuilt it, because `build_stage()` re-copies
   `BASE_BIN` over `stage/mainbin/xsa-main` on **every** gates invocation — a
   fixed copy is overwritten each time. Also replaced the on-disk mainbin with a
   fixed binary as a stopgap.

## What still needs YOU

1. **origin/main is still vulnerable.** Your local main has `2001b3b` (the
   `-s <sig> -- -<pgid>` fix), but `base-src` is pinned to `origin/main@4e275f6`.
   If base-src is ever re-checked-out from origin, the freeze returns. Push the
   fix to origin/main (and/or re-pin base-src to a fixed ref). Note: the shipped
   `process_control=none` design also removes the hazard at the source — land it
   on main.
2. **The two concurrent `gates all` runs (15:34 and 15:45) were frozen mid-flight
   and resumed; they share `$RUN_TAGFILE` and `rm -rf` each other's `runs/` dirs,
   so their results are unreliable.** Rerun gates cleanly from scratch.
3. **The PILE build you relaunched failed** at ~15:29 with
   `SCRATCH_GOVERNOR_CEILING_EXCEEDED live_gb=394.657 ceiling_gb=~393` (relaunch
   used `pos_budget_bytes=100000000000`, not the morning run's 745 GB derivation
   → the ceiling moved under the retained scratch). `SCRATCH_RETAINED
   reason=failure` — it is resumable once the budget/pos-budget fits the
   retained live scratch.
4. The r3 gate you started at 17:28: its `mainbin` copy at 17:28:25 was still the
   vulnerable one; I replaced the file at ~17:30, so any mainbin-arm build it
   spawns from now on execs the fixed binary. If it already completed a
   main-arm build between 17:28–17:30, treat that arm's results with suspicion
   (its pauses were silent no-ops or another freeze — none occurred; pids were
   in the 2.1M band).

## The bug in one paragraph (for any future process-group signaling)

`/usr/bin/kill` (procps-ng **4.0.4-4ubuntu3.2**) parses `kill -<SIG> -<pgid>`
keeping only the **first digit** of the negative pid: `-1257815` → `kill(-1, …)`,
`-2035269` → `kill(-2, …)`. Verified by `strace -e inject=kill:error=EPERM`:
`kill -STOP -1257815` literally issues `kill(-1, SIGSTOP)`. For a non-root caller
`-1` means *every process you can signal* → account-wide freeze; the KILL flavor
is an account-wide massacre. Everything not leading with `1` is a silent no-op or
EPERM — which is why the governor "never paused anything" (your own 9c6cb75
evidence: peak 7.185 GB against a 6.000 GB budget with paused=1). Correct forms
on this box: `kill -s <SIG> -- -<pgid>`, or `kill -9 <positive-pid>`, or a direct
`kill(2)`/libc call. `kernel.pid_max=4194304`, so the dangerous 1.0–1.99M pid
band returns periodically — any residual `kill -SIG -PID` usage anywhere in the
repo/tooling should be swept. This bit of procps deserves an upstream report.

## Timeline for your records

- **09:38:45** freeze #2 (PILE build governor pause, pgid 1257815) — frozen till ~13:00.
- **13:00** I resumed everything except the PILE tree; you killed/relaunched PILE with the fixed binary (paused cleanly via pgids 2273747/48, later failed on the ceiling; scratch retained).
- **15:30–15:45** you rebuilt the worktree binary (SIGSTOP-free), rewrote the gates, and launched gates s4 + two `gates all` suites.
- **15:47:40** freeze #3 — s4's stale `mainbin` (base-src) paused on pgid 1681117 → `kill(-1, SIGSTOP)`. Your sessions froze mid-run; you did not see this happen.
- **17:27–17:30** I killed the two old-binary s4 trees, resumed the other 58 processes, patched base-src + refreshed mainbin, and wrote this note.

## Postscript (18:05 UTC)

- `base-src` is a plain export (not a git repo), so the fix had to be made in place:
  its `signal_group` now uses `kill -s <sig> -- -<pgid>`, rebuilt, sha256
  `7be5560e79e8a689da44…`. `stage/mainbin/xsa-main` is synced to it, so every
  future `build_stage()` copy is the fixed build. Verified fixed form present in
  the binary (`strings … | grep ' -- -`).
- Nothing on the box executes a vulnerable `kill -<SIG> -<pgid>` form anymore:
  main tree (commit `2001b3b`), worktree (process_control=none), and base-src are
  all clean. Zero stopped processes; tmux healthy.
