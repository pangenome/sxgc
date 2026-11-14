# INVESTIGATION: the scratch governor's pause froze every process of the user

**Date:** 2026-11-14 · **Severity:** critical (user-wide process stop) ·
**Status:** fixed in `main` (`2001b3b`), a stronger fix and gates land with
`fix/scratch-ceiling`.

## Symptom

`xsa build` at the pile froze. Not crashed, not slow: **stopped**. The driver and
all four phases sat in state `T` for 3 h 57 m. Nothing advanced, no error was
written, and `ps` showed a healthy process tree that simply was not running.

## Mechanism

`chain.rs` paused the producer with a shell-out:

```rust
Command::new("kill").arg("-STOP").arg(format!("-{pgid}"))     // arg is "-STOP", "-1257815"
```

`Command::new("kill")` resolves to `/usr/bin/kill` (procps-ng 4.0.4), **not** the
bash builtin. procps parses a negative pid spec by keeping only its **first
digit**:

```
/usr/bin/kill -CONT -1999999          →  kill(-1, SIGCONT)            ← every process of the user
/usr/bin/kill -s CONT -- -1999999     →  kill(-1999999, ...) = ESRCH  ← correct
```

Both forms verified by strace on this box. So the signal that reached the kernel
depended on the *digits* of a process-group id:

| phase pgid | `kill -STOP -<pgid>` becomes | effect |
|---|---|---|
| 1,000,000–1,999,999 | `kill(-1, SIGSTOP)` | **stops every process of the user** |
| 2,000,000–2,999,999 | `kill(-2, SIGSTOP)` | ESRCH → reported "No such process" → treated as success → **no-op** |
| 3,000,000+ | `kill(-3, …)` | ESRCH → **no-op** |

The KILL path had the same shape: `kill -KILL -1257815` would have been
`kill(-1, SIGKILL)` — every process of the user killed.

## Two failure modes, one bug

1. **The freeze.** When a phase's pgid happened to fall in 1.0–1.99 M, the pause
   stopped the whole session — the build, every subagent, every editor, every
   unrelated job owned by this user. That is what happened at 2026-11-14
   09:38:46 (frozen 3 h 57 m), and at 2026-11-09 ~23:00.
2. **The silent no-op.** When the pgid fell anywhere else — which is almost
   always — the kill hit an unrelated nonexistent group, returned ESRCH, and the
   code treated "No such process" as success. **The backpressure never paused
   anything.** The journals looked correct: `signal=STOP`, `state=pause`,
   `paused=1`.

## Evidence that the pause was never real

The recorded peak exceeding the budget is the signature of a no-op pause:

```
SCRATCH_BACKPRESSURE signal=STOP phase=order-tape pgid=3222853   ← first digit 3 → kill(-3) → ESRCH
SCRATCH_BACKPRESSURE state=pause live_gb=7.185 budget_gb=6.000 peak_gb=7.185 pauses=1 resumes=0
    -> the gate reported "8 pauses / 8 resumes, 0 kills" as a SUCCESS. It paused nothing;
       the live set grew past the budget, exactly as it would have with no governor at all.
SCRATCH_SAMPLE live_gb=8189.275 budget_gb=250.000 paused=0      ← the file-filled incident:
    the governor took no action at 32x its budget
SCRATCH_SAMPLE live_gb=251.115 budget_gb=250.000 paused=0 ceiling_gb=1045.058
    -> and then, one sample later, with pgids at 1,257,815, the same pause froze the box
```

Every "backpressure works" claim in this project's results files was measuring a
mechanism that could not act. The 100 GB rehearsal's "zero backpressure pauses"
and the G3/G5 "8 pauses / 8 resumes" are both consistent with a no-op.

## Why it was invisible

A pause that silently does nothing is indistinguishable from a pause that works
**in the journal**: both print `signal=STOP`, `state=pause`, `paused=1`. Only two
independent facts can distinguish them:

1. the syscall that reaches the kernel (`kill(-N, SIGSTOP)` with the right N), and
2. the producer's progress actually stopping (bytes/rows frozen while paused) and
   the live set actually falling back under the budget on resume.

Neither was ever asserted.

## Fix

* `main` (`2001b3b`): explicit form `kill -s <SIG> -- -<pgid>`, verified by strace
  (`kill(-2540866, SIGSTOP)`, zero `execve` of `/usr/bin/kill`).
* `fix/scratch-ceiling` (stronger): call `kill(2)` directly with
  `-(pgid as i64) as i32` — no shell, no parser, no PATH dependency.
* **Guard**: refuse to signal a pgid ≤ 1 (pgid 1 is legitimately "everything";
  for a build phase it can only mean a bug).
* The two remaining shell-outs to `kill -9 <pid>` (`corpusv2.rs`, `pilemap.rs`)
  pass a *positive* pid and are not affected.

## Gates

* **G6a (syscall argument):** for pgids covering the dangerous ranges
  (1, 999,999, 1,000,000, 1,257,815, 1,999,999, 2,000,000, 9,876,543), assert the
  program issues exactly one `kill(2)`/`killpg(2)` with the sign-correct pid_t and
  never execs a shell or `/usr/bin/kill`.
* **G6b (the pause pauses):** during a paused interval, assert the producer's
  output bytes do not advance (sample twice, require delta == 0), and after resume
  they do. A pause with zero measured effect fails the gate.
* **G6c (the budget holds):** peak live ≤ budget + one documented class, on a run
  that actually pauses (the previous "8 pauses" run had peak 7.185 GB against a
  6.000 GB budget and passed — that is the calibration error this gate removes).

## Lesson

A safety mechanism that cannot act is worse than no mechanism, because it reports
success. The journal recorded `paused=1` and `signal=STOP` for months of runs in
which nothing was ever paused, and one in which everything was. **Verify a
mechanism by its effect, not by its log line.**
