# Ordered publisher: where did emit wall change?

Synchronous batched sink and independent audit, one global END plus the same owned-row wire width (12 B). Different texts, row layouts and sorting kernels across rungs. **Not a sinkless `cat` benchmark.**

## packed-10g-l4-b1-t4-r1 vs packed-radix-4-batch

Interval | 1 GB observed s | 10 GB observed s | 10 GB minus byte-linear 1 GB (s) | 10 GB ns/owned row
---|---:|---:|---:|---:
plan | 2.415 | 46.711 | +22.563 | 4.67
packet_receive | 3.338 | 620.318 | +586.939 | 62.03
sink_write | 91.915 | 523.725 | -395.427 | 52.37
audit_write | 78.704 | 1145.997 | +358.954 | 114.60
other_publisher_elapsed | 0.362 | 4.237 | +0.621 | 0.42
**emit** | 176.734 | 2340.988 | +573.649 | 234.10

619,387 whole-key ranges, 620,434 ready data packets (mean 193,413 B, largest range 1,021,836 rows), 6,291,456 B queue. At 1 GB: 66,589 ranges, 66,593 packets (mean 180,199 B, largest 103,397 rows). No per-row comparisons or patching at join. Plan is before the data writer, packet receive includes fast successful calls, and other elapsed includes untimed setup/accounting. The sink and verifier may work while lanes compute. The table locates wall on the publisher, not causes of stalls, worker CPU or removable wall.
