# 10 GB rung re-banked (the night the box had other plans)

## The run (proven base eb6aaef, full envelope, real configuration)

- corpus: real10b-ref/pile-10b-snap.txt (n = 10,000,017,258)
- config: --memory-gb 100, --finish-as-gb 400, --wit-ram-gb 8, threads 96, DEFAULT piece window (1100), 2 rolling windows
- producer (bin-v2): rc=0, wall 25:33, peak RSS 93.52 GB (== plan prediction 93.5)
- finish (fused, overlapping): rc=0, wall 31:12, peak RSS 26.6 GB
- TOTAL WALL 31 min — the fused pipeline (finish consuming pieces while the producer sorts)
  beat the morning's sequential 54 min by 42%. This is the fused mode's proof at scale.
- chi = 2,676,929,430 (exact, in-process assert)
- container sha256 e99fc0b35e7fadb030934f7c75f0b62d4d0631a626213f5173f2eca599f5f688
  == banked pf2-10g-hyg0/product.sxi2 (byte-identical)

## The evening's five "crashes" were one OOM kill, five times

All five entry failures tonight: rc=137 (SIGKILL, kernel memory-pressure killer) at 67-74 GB
RSS. Foreign memory load (impg-genome-inference, 310+ GB) shrank the box; the kernel executed
our producer when it hit ~70 GB. The "regression in the hand-merge" theory was wrong: eras were
compared without controlling for memory weather. The quarantined stack (branch
untested-20261022) was never shown guilty; it is being re-proven by scale gates on bisect-stack.

## Config verdicts corrected

- DEFAULT piece window (1100) is PROVEN at 10 GB / default target / full envelope (this run).
  The 1M-window mandate applies to deep-target configurations only (the 64K-target segfault
  differential), not to the pile's default config.
- The 100 GB envelope is correct in clear weather (93.5 GB peak, plan == measured). The
  envelope is a policy choice sized to the box's free memory at launch time.

## Discipline born tonight (binding)

1. rc=137 = ENV-KILL (memory weather), classified before any theory. Code guilt requires a
   non-137 fault with a real signature.
2. No merge without a 10 GB real-config gate. Oracles + 1 GB banks do not substitute.
3. ~~Every source-touching commit refreshes xsa/SOURCES.sha256.json~~ **REPEALED
   2026-10-30** (operator verdict): the source digest file was a hand-maintained
   duplicate of what git already records, and it refused the build on every merge
   that touched a covered file (four times in one night). It is deleted. Provenance
   is git's: every `xsa build` journals `XSA_SOURCE git_commit=<sha> git_dirty=<0|1>
   branch=<name>` as the first line of `xsa-build.log`. Editing a covered source now
   needs no regeneration step. `MANIFEST.sha256` (the INSTALLED bundle's file list)
   is a different artifact and is untouched.
4. The entry can orphan its finish-v2 child (68-year poll timeout) when a phase dies:
   reap orphans after every failed gate.

## Pile arithmetic

The fused overlap hides the finish's pool-bound work, but the wall stays producer-dominated
at 1.31 TB: ~4 days stands. The 42% fused saving applies to the finish tail only.
