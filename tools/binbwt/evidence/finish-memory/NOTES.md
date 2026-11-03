# Finish memory measurements

Base: abf2415. `baseline-instrumentation.patch` adds only measurements and the
readiness probe to that base; `finish-baseline` ran the complete 10 GB pipeline
and the 100 GB readiness-only confirmation. `finish-fixed` adds buffered
`pread` bulk scans. Binary hashes and the fixed source hash are in results.json.
The source and packaged runtime copies match. Original stashes remain intact.

G1: complete, verbatim in G1.md. The top term is resident corpus mappings;
smaps identifies the backing file rather than treating all RSS as heap.
G2: measured, flatness target NOT met. Kernel peaks: 150 MB 131068 KiB,
10 GB 4180964 KiB (baseline 10 GB 12151760 KiB). Status-sampled peaks are
separate receipts, not substitutes for the kernel high-water mark.
G3: complete through chi=3000425823 at band 57601, matching archived progress.
Post-receipt VmRSS=VmHWM=18550652 KiB, RssAnon=6337984 KiB. See G3.md.
This is a marker measurement, not a completed 100 GB publication.
G4: all three chi counts, m5 bytes and whole-container comparisons pass;
150 MB query counts 100/100 and positions 574/574, 10 GB positions 20/20.
Branches, MEM, seeds, the 150 MB count regression, the three fanout unit tests,
all 64 Rust unit tests, and the packaged 8 MB build pass.

The 100 GB readiness probe used 96 workers, an inherited unlimited address-space
limit, the same corpus/remap, and XSA_FINISH_RSS_READINESS_ONLY=1. It is explicitly
not a completed 100 GB finish. The full 10/100 GB pipeline command arrays and
address-space caps come from the existing run journals; run.py records the
executed command arrays. The 100 GB run is scoped to the requested chi > 3e9
marker, then the monitor stops the three measurement processes. It does not
publish a new complete 100 GB container.

Shared-machine memory pressure reclaimed file pages during these measurements.
Do not compare their wall times as a performance benchmark. During the 100 GB
run, a separate read-only helper periodically reads the chi bitmap with a fixed
16 MiB buffer to recover cache residency. It does not modify the bitmap or the
finish binary. G3 reports this condition and the actual process RSS. This
is not a flat-memory claim: the bitmap and accumulated directory vectors still
scale. Two optional reruns of the 100 GB count/context regression were interrupted
to reduce pressure; the count regression passed on the new 150 MB container.

The bulk-read regression passes the fixed implementation and rejects the
original mapped implementation. Its byte oracle covers a nonidentity remap,
virtual END, zero-length reads and output canaries across 128 MiB.

Unabridged external evidence: /mnt/nvme2n1/erikg/finish-memory-evidence/.

For the negative regression reproduction, apply baseline-instrumentation.patch
to an abf2415 worktree, copy finish_bulk_read_test.cpp into its matching tests
path, and build it with tools/build_slim_dump.sh. The residency bound must fail.
