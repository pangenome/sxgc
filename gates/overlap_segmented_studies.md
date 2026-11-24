# Intermediate qualification only — 100 GB REFUSED

The ordinary `overlap_lexparse100.cpp` build is still fixture-only. The shell gate builds a separate supervised entry, checks the *actual* published 6118ab4 10 GB bitmap product (build/row oracle exits, final rank, nine-member reference and SHA), and accepts only the two exact lengths below. No prepared-product speedup, suffix verification, write saving, 100 GB launch or automatic promotion follows from success.

**Parent CLI**, from the reviewed checkout, on the designated host with the timing lease available. Choose an **absolute future Unix deadline** per run. Do not invoke the internal worker directly.

```
gates/overlap_segmented_gate.sh 1g "$DEADLINE"
# Stop. Independently review the printed private run directory: preflight,
# binaries, complete front and decode logs/JSON, parity, ratios and guards.
# ONLY after approval, record the digest of that exact pair in this order:
cat "$ONE_RUN/segmented.json" "$ONE_RUN/decode.json" | sha256sum
# Separate parent decision, no implicit chaining:
gates/overlap_segmented_gate.sh 10g "$NEW_DEADLINE" "$ONE_RUN" "$REVIEWED_HEX_DIGEST"
```

The [completed 1 GB rehearsal](receipts/segmented-front-1g/) is independently reviewed, digest `66b27d6282569376a117df24d05403edf7746a7e8aff7d13f65723fc21f6339c`. The launcher pins its source; for 10 GB it checks actual live published `main`, replays the historical serial control and 10 GB scan calibration, checks the reviewed original pair and binary manifest against the published bank, and checks both new native executables against those used at 1 GB. These checks do not require a new 1 GB run. Require ≥1,800 s at admission and ≥1,400 s after copying. The digest binds the parent's review to the two files; it is not a signature. The gate needs ordinary immutable bank directories, not a writable cache or synthetic same-sized inputs. Each run reads the fixed raw source once into a **new copy** in mode-0700 scratch, hashes during copying, chmods the copy read-only, fsyncs, evicts **only** that private copy, and requires zero resident pages by `mincore`. Record the private path, pages, raw SHA, bank inode/size/time identities, free space and grant. Require free `3N+1 GiB` before copy (N private input, 2N hard logical scratch incl. both columns, plus overhead). Refuse before raw IO on missing receipts, unavailable dedicated cgroup or bank extents. Never unlink, advise, or write bank files. Treat interruption, stop.requested, missing receipts, guard trips, failed comparisons or changed bank metadata as refusal. Preserve partial directories. Read comparisons may warm the bank; no cache reset on the bank.

| Standalone raw→packed baseline, 1 CPU, 192 MiB, swap=0 | N (bytes) | wall | attributed reads / writes | bank in TAPEREADER/sandbox |
|---|---:|---:|---:|---|
| `receipts/packed-parse-1g/pack.{log,time}` | 1,000,000,665 | 30.38 s | 3,861,856,256 / **unknown** | `packed-parse-ORX2dw/columns` |
| `receipts/packed-front-10g/front-end.json` | 10,000,000,000 | 357.7573 s | 37,612,232,704 / 33,635,045,376 B | `packed-front-10g-YebiCY/columns` |

Time **only** the front, from cold private raw input to closed word+ID columns. Use the same-grant standalone fronts above, **not** the 20/200 GiB resident product-service front times. Report absolute new process counters (including cancelled writes), per-counter ratios where known, wall ratio, shared `io.stat` device lines and deltas; the 1 GB matched write ratio is null. No inference of saved writes or seeks from missing counters. `io.stat` can count stacked devices and delayed writeback; not additive with `/proc/PID/io`. Worker IO accounting is required; undelegated per-device IO stays null, distinct from measured zero. Goal <2×; qualification requires <3× **and** exact comparison/decode. Verification and cold preparation are outside front wall and IO. No prepared-to-product measurement here.

The per-run front unit contains only supervisor + pinned single-thread worker, CPUQuota=100%, MemoryMax=192M including file cache, no swap. Fixture mlock is replaced by the no-swap cgroup for intermediate modes (worker address-space cap remains). Observe memory.peak including setup, worker HWM, CPU, OOM events, and exact pre-write ledger (see [invariants](overlap_lexparse100.md)). Sample column logical sizes separately from whole-directory logical/allocated (files, including tools/logs/private input); sampled allocation isn't a hard logical peak. Front samples worker-attributed traffic and cgroup counters per device where delegated (otherwise null, never zero or a sum over stacked devices). It cooperatively stops on **either** worker or observed per-device reads/writes >16N, sampled logical over 2N, free disk <256 MiB, stop file or absolute deadline. These IO thresholds are runaway guards, **not** IO-saving targets; sampling may overshoot. Deadline caps compilation; copy/comparison, front and independent native verifier check it cooperatively. Require admission headroom for both stages. Touch `$RUN/stop.requested` for a cooperative stop; neither timed child is killed on a normal guard. The online ledger enforces logical space before writes; directory sampling is throttled so it doesn't dominate the sort.

After the timed stage, stream **both** new columns byte-for-byte against the fixed bank, report SHA256 for each equality; no huge Python objects. For 10 GB require the private copy SHA to match the exact [10 GB raw scan calibration](receipts/scanplan-10g/), then require both column equality and independent decode to that hash. New `copy_cold` and `column_compare` receipts meter separate wall/CPU/IO; the latter's device write guard allows delayed writeback up to the prior 192 MiB service grant plus margin. Then a fresh unit (one CPU, swap=0, **3 GiB / 16 GiB** for 1 / 10 GB) cold-checks only the two *new* columns and runs the independent decoder. Its deadline and stop/IO guard (8N reads or 16 MiB writes) are separate; its time/traffic is **not** charged to the front. No decoded text file.

## Verifier invariant and costs

`overlap_segmented_verify.cpp COLUMNS EXPECTED_N RAW_SHA256 METADATA_LIMIT_BYTES` (compile CLI below) treats columns as immutable. Walk a read-only mapping of the word tape, checking a u64 remaining extent before every tag/payload, max 1 MiB word, strict `(raw,END)` unsigned lexical order, exactly one END, no skipped trigger and canonical last trigger for non-END. Count k ≤ m ≤ UINT32_MAX and compute unique owned descriptors/D; only then allocate exactly `8k+ceil(k/8)` payload bytes for checked u64 offsets and a reachability bitmap. A second walk builds offsets. Stream little-endian IDs (64 KiB blocks), reject holes/out-of-range/unreferenced keys, require END iff final ID. Adjacent words share the exact eight bytes. Each owns all but those eight bytes except the last, which owns its entire payload. Feed precisely those bytes to SHA-256 in parse order, bound the running sum by N, check final N and the independent raw digest. A short all-END singleton and an 8-byte terminal after a final cut are legal. No `m`-sized heap, decoded output, suffix ranks, or random parse writes.

**Not a 192 MiB verifier.** Offsets + bitmap alone are ~206 MB / 1.91 GB on the real 1 / 10 GB k. Each ID indexes a potentially random dictionary page. Cold scan cost is linear in tape+IDs; page reuse vs storage rereads depends on the text-order ID working set, not on offset width. Budget the mapped tape resident **in addition** to metadata, page tables, streaming and runtime margin; report the actual cgroup peak and device IO. Mapping alone proves neither residence nor cheap decoding. For future larger tapes, census before granting that memory or design/test an external locality-bounded verifier; the current CLI deliberately refuses N=100 GB. A passing intermediate gate cannot resolve actual 100 GB k/m, u32 occurrence limit, dictionary/rank widths, IO/time for multi-pass merges or random decode, v10 ordering/tap/product verification, or a matched resource plan. Real 100 GB remains unscoreable.

## Offline fixtures only

```
T=$(mktemp -d)
g++ -std=c++17 -O2 -Wall -Wextra -Werror -DSEGMENTED_INTERMEDIATE gates/overlap_lexparse100.cpp -o "$T/front"
g++ -std=c++17 -O2 gates/overlap_lexparse10.cpp -o "$T/old"
g++ -std=c++17 -O2 -Wall -Wextra -Werror gates/overlap_segmented_verify.cpp -lcrypto -o "$T/verify"
python3 gates/overlap_lexparse100_test.py "$T/front" "$T/old"
python3 gates/overlap_segmented_test.py "$T/front" "$T/verify"
python3 gates/overlap_segmented_gate_test.py "$T/front" "$T/verify"
```
