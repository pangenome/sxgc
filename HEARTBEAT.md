# HEARTBEAT.md

Checklist for a periodic health check of this project. Run every 15 minutes from an isolated
session. **Print `HEARTBEAT_OK` and nothing else when everything below passes.** Otherwise print
one short block per failing check: what is wrong, the evidence line, and the recommended action.
Never kill a process, never commit on someone else's behalf, never rewrite history: report and
recommend, the operator decides.

The point of this file is that a *generic* "no output for N minutes" watchdog would murder a
healthy build here: the corpus census is legitimately silent for ~35 minutes at 1.3 TB, a monster
band sort is silent for minutes, and a phase waiting on its sibling has zero CPU and zero I/O by
design. So every check below measures a signal that is only quiet when something is actually wrong.

## 1. Repository is published

```sh
cd /home/erikg/sxgc
git fetch -q origin
git rev-parse origin/main; git rev-parse main
git log --oneline origin/main..main | head        # must be empty
git status --porcelain | head                     # must be empty
```

FAIL if `main` is ahead of `origin/main`, or the tree is dirty **and** the newest modification in
it is older than 30 minutes (a lane died holding work — this has happened repeatedly).

Action: report the dirty paths; recommend `wip/auto-<timestamp>` commit + push, or finishing the gate.

## 2. Builds are moving

For each scratch directory under `/mnt/nvme2n1/erikg/*/` that contains `xsa-build.log`:

```sh
# newest journal line and its age
tail -1 <scratch>/xsa-build.log
# phase processes and their CPU time
pgrep -x scatter_sort -x cross_lcp_merge -x xsa_finish
ps -o pid,etime,times,args -p <pids>
du -sh <scratch>
```

PASS if either (a) the newest journal line is younger than **20 minutes**, or (b) some phase has
consumed CPU time since the previous heartbeat (compare `times`), or (c) scratch grew since the
previous heartbeat.

FAIL as `WEDGED` only when all three are false for **two consecutive heartbeats** (~30 min): no new
journal line, no CPU delta, no scratch delta, with phases still alive. Name the phase and, for the
tape protocol, the file it is waiting on:

```sh
ls <scratch>/bands-tape/*.consumed | tail -3
grep -hoE "SS_DIAGNOSTIC .*" <scratch>/order-tape.log | tail -1
```

## 3. No orphaned phases

```sh
pgrep -x scatter_sort -x cross_lcp_merge -x xsa_finish
```

FAIL if any phase is running while no `xsa build` driver is alive (a phase outliving its driver
keeps a scratch locked and blocks the next run). Action: report pids; recommend `kill -9` on the
exact names, then `pgrep -x` to confirm none remain.

## 4. Disk headroom

```sh
df -h /mnt/nvme2n1 /mnt/nvme3n1
```

FAIL if free space on the device holding a live scratch is below **1.2 x** that build's peak
scratch recorded in its journal (`PEAK_INTERMEDIATE` / `SCRATCH_BACKPRESSURE_SUMMARY`), or below
50 GiB on any device we are writing to. Note that a foreign training job also writes here, so the
margin can vanish without us doing anything.

## 5. Scratch stays inside its budget

```sh
grep -hoE "SCRATCH_BACKPRESSURE_SUMMARY budget_gb=[0-9.]+ peak_gb=[0-9.]+ .*" <scratch>/*.log | tail -1
```

FAIL if `peak_gb` exceeds `budget_gb` by more than 25 % (backpressure should bound the average and
the arena should bound a single wave; a bigger overrun means the accounting is wrong).

## 6. Agents are not silently dead

For each active subagent run, the harness reports `no activity for N minutes`. FAIL if any run has
had no activity for **> 20 minutes** while its worktree shows no new commit and no build process of
its own. That combination means it is gone, not thinking.

Action: report the run id and branch; recommend interrupt, then a fresh lane starting from the last
pushed commit. Do not attempt to resume a dead external-cli lane.

## 7. Containers answer

For the most recent container in each scratch that has completed (`XSA_BUILD_DONE` in the journal):

```sh
xsa query <container> --count "the "   # must return a number, not an error
```

FAIL on error, or on a count of 0 for a corpus that certainly contains "the ". This catches a
container that built but cannot be read — the failure mode that has no other alarm.

## Escalation

- One failing check: report it, recommend the action, carry on.
- Two consecutive heartbeats with the same failure: prefix the block with `ESCALATE` and say what
  will be lost if it is not fixed in the next hour (hours of build time, uncommitted work, disk).
- Never act on the build itself. A wedge is diagnosed from the journal lines above, and stopped by
  a human with the evidence in front of them.
