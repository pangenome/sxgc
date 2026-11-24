# First bounded producer component: exact 100 MB only

`gates/overlap_bounded_component.sh ABSOLUTE_UNIX_DEADLINE` creates a new private receipt and **refuses** unless the pin below exists in a clean, published main; the portable `radix-full-1g` product comparison and batch comparison both replay and pass; the series flock is obtained without waiting; and known timed workers are idle. No 1 GB input (or larger), automatic retry, product build, default selection or promotion. `--plan-component` remains arithmetic only. Do not call the native binaries as a substitute for qualification. `--component-100m COLUMNS STOP` has no adjustable caps, expected-rank file, or generic rung parameter; both fixture entry caps are unchanged.

## Missing source pin is a stop, not a trust-on-first-use opportunity

Publish exactly these four fields in `gates/receipts/bounded-component-100m/source-pin.json`:

|field|value|
|---|---|
|`source`|`/mnt/nvme2n1/erikg/TAPEREADER/sandbox/pile-slice100.bin`|
|`n`|100000503 (integer)|
|`raw_sha256`|64 lower-case hex digits, obtained independently as below|
|`narrow_row_sha256`|`82747f11a9925bb4d89cc812410d98e411ce80b1767ee1ff4dc22e94e2cd874e`|

Generate separately under the nonblocking `${XDG_RUNTIME_DIR}/overlap-ten-timing.lock`, with no active timings, outside all scored clocks. Inspect the published `radix-emit-100m/{result.json,verify/{metrics.json,oracle.log,digest.log}}` against source arm `sandbox/radix-emit-100m-YQ7txA/verify`. Require successful producer/digest and full oracle, same narrow digest and all 100000504 rows. Verify original and **both** historical inputs `verify/in/corpus` and `verify/oracle-corpus` are distinct regular, non-symlink files of the fixed size. SHA-256 all three, requiring equality before recording `raw_sha256`; retain provenance. Do not evict or change banked files. Neither the old product digest nor a digest of today's original alone authenticates the raw pin. Missing historical files, failed publication, nonmatching pin or unestablished provenance: refuse, not an alternate path.

## What is measured

Build the old standalone assertion-enabled `overlap_lexparse.cpp --packed`, unchanged resident serial binary, bounded compact binary and component-specific C++ verifier from that checkout in isolation. Make private raw and oracle inputs (exclusive create, ordinary copied bytes, hash pinned, fsync/fadvise/mincore zero). For **each** arm run a new independent cold standalone preparation under 192 MiB, one CPU, zero swap. Check census/column sizes: `m=3170531 k=2689467 T=127708112 U=95434516`; hash both output files and require identical column hashes across arms. No retained control columns during candidate preparation. Re-cold both private columns and oracle input before the prepared start. Do not evict shared source files.

Control uses the existing serial wide resident row43 fixture entry: its explicit `n<=101000000` guard includes this exact bank. Only that entry's environment is set (packed, wide dictionary, checked recurrence). It gets **1536 MiB arrays, 2048 MiB cgroup**. Candidate uses **320 MiB arrays including 64 MiB order arena, 8192 heap cursors, 512 MiB cgroup**. Both have a finite 100% CPU quota, no swap, and one thread. Each sends directly through a bounded OS pipe to its own **140 MiB / 1 CPU / no swap** verifier; no Python row loop or row file. Verifier maps the separate immutable raw, checks tag and both high fields, every predecessor, adjacent prefix/order, END, count, and independent final rank, and requires the exact banked SHA of the checked little-endian u32 triples including END. Strict order plus full count proves permutation. Working-set or verifier failure under these finite grants is a receipt, not permission to raise the cap.

`prep_s` covers standalone cold prep service launch through exit. `prepared_to_verified_EOF_s` covers launches through both exits, including full verification. `raw_to_verified_EOF_s` is continuous elapsed between those boundaries, **including** intervening column certification/eviction. Also report the two-stage wall sum separately. Private copy setup and compilation excluded. Observe `*.json` and `producer.log`: per-stage actual cgroup settings/ancestors/affinity/events/peak, CPU, process-attributed reads/writes (final counters before reap), sampled process high water, and cgroup `io.stat` where available. Physical-device traffic is not inferable from logical classifier passes, wall ratios, or process counters alone. Compact pass, digit-read, tie and window counts versus resident phase logs help distinguish CPU/order/merge and verifier costs. This is not an equal-memory speed claim or a physical seek measurement.

## Admission, scratch, and stop

Require 900 seconds before admission, 420 before each prep, 360 before each producer/consumer pair, >=2 GiB scratch free and >=3 GiB host MemAvailable. All prepare/produce/consume roles verify effective grants before launching native work. Guard per-process attributed reads at 6N (prep) / 8N (others), writes at 8N (prep) / 4 MiB (others). Recheck during and after execution; request cooperative stop on deadline, contention, excessive IO or logging, or peer error. Polling is not hard preemption. A sort/blocked syscall may retire after the deadline. Touch the receipt's `stop.requested`; allow both services to exit, close unowned pipe endpoints, retain counters/exit/status and never treat partial rows as verified. Do not terminate a running service to obtain a score. Tool build timeouts are not producer timeouts.

Count simultaneous construction scratch, not just sampled directory sizes: prep starts with at most `N+8(m-1)+12m=163411115` bucket bytes, then deletes each bucket before replacing its rows by `8` assignment bytes plus at most `4+len` packed bytes per distinct word; finally replaces 8-byte assignments with 4-byte parse entries. Require the old worker's actual logical peak no higher. Prepared stage retains only `T+4m=140390236` bytes. Add the 4 MiB queue/log reserve to **each** boundary: at most **167605419** and **144584540**, respectively, versus **200001006**. One row pipe and at most two text pipes (admit <=1 MiB each), capped 64 KiB native logs per role plus small receipts. Executables/build tree are tooling, not output; two private immutable N-byte input copies during prep, one during streaming, are additional storage, **not** construction scratch. Stop before the next arm unless its predecessor succeeded and cleaned up. Keep failed receipts for inspection. No completion or performance claim without the new real receipts.

### Tiny validation (no bank or timed workers needed)

```
cargo build --locked --offline -j1 --manifest-path xsa/pfp/Cargo.toml --bin overlap_bounded_packed
python3 -E gates/overlap_bounded_component_test.py xsa/target/debug/overlap_bounded_packed
```
