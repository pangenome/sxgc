# Ordered publisher: where did emit wall change?

Synchronous batched sink and independent audit, one global END plus the same owned-row wire width (12 B). Different texts, row layouts and sorting kernels across rungs. **Not a sinkless `cat` benchmark.**

## packed-10g-l2-b1-t4-r1 vs packed-radix-4-batch

Interval | 1 GB observed s | 10 GB observed s | 10 GB minus byte-linear 1 GB (s) | 10 GB ns/owned row
---|---:|---:|---:|---:
plan | 2.415 | 62.404 | +38.256 | 6.24
packet_receive | 3.338 | 695.127 | +661.748 | 69.51
sink_write | 91.915 | 230.559 | -688.594 | 23.06
audit_write | 78.704 | 1427.831 | +640.788 | 142.78
other_publisher_elapsed | 0.362 | 3.197 | -0.420 | 0.32
**emit** | 176.734 | 2419.117 | +651.778 | 241.91

619,387 whole-key ranges, 620,434 ready data packets (mean 193,413 B, largest range 1,021,836 rows), 3,145,728 B queue. At 1 GB: 66,589 ranges, 66,593 packets (mean 180,199 B, largest 103,397 rows). No per-row comparisons or patching at join. Plan is before the data writer, packet receive includes fast successful calls, and other elapsed includes untimed setup/accounting. The sink and verifier may work while lanes compute. The table locates wall on the publisher, not causes of stalls, worker CPU or removable wall.
