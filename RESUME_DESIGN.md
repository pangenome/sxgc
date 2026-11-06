# Resume design: retain incremental finish state, assemble SXI2 once

Status: native finish spool checkpoints implemented from ee2561c. See the implemented protocol and native gate evidence first. The original f251b14 analysis/specification is retained below as historical context; its proposed PXCP protocol and all-phase cheapness gate are not assertions about this implementation.

## Implemented finish protocol (2026-11-06)

The finish implementation is in `xsa/runtime/bit6/xsa_finish.cpp` and `finish_spool.h`, with identical `bit6/` mirrors. It is explicitly enabled by `XSA_FINISH_SPOOL=1`; use an absolute `PREFIX`. The ordinary route-B container is unchanged. No Rust driver source was changed. The partial attempt 77cdf6a was read, not cherry-picked; neither its producer changes nor its recovery claims were adopted.

This is **native finish recovery**, not a replacement for the driver's replay policy. Lane A must preserve the finish spool directory, PFCK, producer start/progress/terminal records, raw pieces and LCP slices, and relaunch the finish with the same arguments and `XSA_FINISH_SPOOL=1`. It must not clear these as replay-v1 intermediates. The native gate keeps the sorter/emitter alive across the injected finish exit; it does not attest restart of those stages or whole-driver recovery.

The filesystem is authoritative. `PREFIX.finish-spool/state` is an atomically published, checksummed snapshot. When `XSA_CHECKPOINT_JOURNAL` is supplied, a shared-flock, fsynced text receipt is also appended:

```
FINISH_CHECKPOINT next_band=<n> runs=<n> generation=<n> eof=<0|1> state=<path>
```

A receipt alone does not replace verification of the state and its dependencies. The existing `PIECE_DURABLE` producer receipt is not reinterpreted as a finish checkpoint.

### What differs from the original specification below

| Predicted | Implemented |
|---|---|
| PXCP0001 hash-chained binary journal and PXST0001 tagged state | Atomic `state` replacement, a versioned FSPOOL01 envelope, explicit little-endian integer serialization, SHA-256, and optional text journal receipt. No native struct/pointer dumps. |
| Checkpoint at every producer-piece boundary | Quiescent barrier every 64 raw-run bands after raw runs fed reach n/4, plus genuine EOF. Feed-ahead is disabled in this mode; verification has completed before publication. An arbitrary interruption can redo the uncommitted tail after the last barrier. |
| Unencoded carry of at most 1023 normalized runs | The state records the partial codec block's source `(c,len)` pairs, exact physical stream prefixes, logical bit counts, pending accumulator and pending-bit count. Complete 1024-run blocks alone become entropy spools; EOF alone permits a short final block. The partial encoded carry is private snapshot state, never advertised as a complete entropy spool. |
| Directory manifest streams and open m13 samples | Explicit snapshot of m12/m13 vectors, last/open sample state, counters, sparse-tail bytes and both entropy stream prefixes. This is larger and less incremental than the proposed manifest. |
| Immutable chi base plus ordered deltas, occasional compaction | Ordered first-set position deltas are committed and compacted into a **new** immutable base at every checkpoint. Pending delta positions are retained in RAM until that checkpoint. Base and referencing state are durable before deletion of the delta or previous base. |
| Early retirement of semantic inputs behind the committed frontier | Raw pieces, progress records, PFCK and LCP slices are retained. Producer credits can advance because the published source files remain available. The active bitmap/bit/sparse scratch is disposable: the committed base and state contain its recoverable bytes. |
| Reconstruct missing output by replaying retained source | Every entropy block has a separate durable `.runs` source of normalized `(c,len)` pairs. A missing `.entropy` file is re-encoded independently from that source; no repair, LCE or sweep is performed for it. |

Entropy files are named `piece-<ending-piece>-block-<1024-run-index>-keys-<lo>-<hi>.entropy`, with a companion `.runs` file. Here `lo/hi` are half-open **normalized BWT row-rank keys**, not suffix strings or sorter's byte-prefix keys. The 160-byte FSPOOL01 envelope carries piece, codec block index, rank interval, rows, runs, payload length, and a SHA-256 over metadata and payload. Its SHA is stored as 64 ASCII hex characters. Publication is `.part` write, `fdatasync`, close, rename, directory fsync. Torn `.part` files are ignored; invalid complete files fail closed.

Assembly reads the verified entropy block files in codec-block order and uses the restored directories/sparse tails/chi base for the other members. The existing SXI2 v6 codec IDs, bit padding, member order, CRCs, directory placement and header remain unchanged. Reassembly after EOF performs zero sweep advances. When the requested output already exists, the rebuilt container must compare byte-for-byte equal before success; a different output is refused.

The snapshot saves both repair coalescers, seam classes and ordered source cursor/maps, sweep registers/statistics with deterministic heap reconstruction, inter-band tail, fingerprints/closure guards, full codec/directory state, sparse bytes, and the live LCP overlap and its read cursor. Corpus, remap and executable identity, plus chi/Kasai policy and PFCK spacing, are checked. Experimental replay/chunk/debug/claim modes and sampled ISA inputs are refused in spool mode. PFCK is published atomically and reused. Reopening still performs corpus/remap verification, identity hashing and the corpus histogram for the codebook; these are I/O/setup, not skipped sweep work.

`XSA_KILL_AT=after_fsync_before_unlink` exits with status 86 after the new base and referring state have both been published and directory-synced, and before deleting the compacted delta or previous base. An interrupted compaction can leave an orphan base/delta; only the generation referenced by the verified state is authoritative. The fault hook is not a timer. `--poll-timeout 0` disables native finish polling deadlines; spool mode also disables them. The gate uses `XSA_TAPE_WAIT_S=inf` for the unchanged sorter's legacy slice-credit wait and installs no subprocess timeout.

`WORK_AVOIDED phase=finish done=... total=... ratio=...` reports **sweep work executed in this invocation divided by total normalized runs**. Ratio 1 is full replay; ratio 0 after EOF means no sweep. The restored prefix is not fed through repair, LCE or sweep. The original document's stricter all-phase/end-to-end elapsed-time gate is not claimed by this native-only gate.

### Validation and remaining limits

The standalone `finish_spool_test.cpp` checks writer exclusion, integer state serialization, metadata and payload corruption refusal, torn-part isolation, and the crash window with both durable consuming output and retained input. It also runs under AddressSanitizer/UndefinedBehaviorSanitizer. Python hashlib independently checks the snapshot, base, block source and entropy envelope SHA-256, corpus/remap/executable identity, and the 150 MB committed base popcount (44,034,230).

The reproducible native gate is:

```sh
python3 xsa/runtime/bit6/finish_spool_gate.py /tmp/lane-b-native-final /dev/shm/t150m.txt /mnt/nvme2n1/erikg/lane-b-spool-gate/gate150
python3 xsa/runtime/bit6/finish_spool_gate.py /tmp/lane-b-native-final /mnt/nvme2n1/erikg/ar-slice-1gb.txt /mnt/nvme2n1/erikg/lane-b-spool-gate/gate1g
```

Each gate directory must be new. Each command builds a clean reference, injects the requested exit during sweep, resumes the native finish, compares the whole-container SHA, deletes one entropy block, and requires exactly one block re-encode and identical reassembly. It retains command logs and dependencies for inspection. The stage directory contains native stages compiled from this worktree; no timeout wrapper is used.

The 150 MB gate passed: chi 44,034,230 and clean/resumed/repaired SHA-256 `f6abd41da6e3c3797abae2e288596a6042337a894869f573fecf51c53f624b3e`, equal to the existing uninterrupted L150 container. Its resume performed 19,530,460 of 57,279,194 sweep advances (ratio 0.340969532), with 36,863 entropy blocks already present. Removing block 31,488 re-encoded exactly that block and performed zero sweep advances. The 1 GB gate also passed: chi 283,296,727 and clean/resumed/repaired SHA-256 `f5d1c4d323a73cdb9e45ea2997941fe4a5897fd8c299f13fccd92e797203c1c9`. It executed 116,378,094 of 368,036,332 sweep advances after restart (ratio 0.316213601), with 245,759 entropy blocks already present. Removing block 225,593 re-encoded exactly that block and performed zero sweep advances.

A 4 MB no-chi EOF/short-final-block case also passed clean/kill/resume/single-block-repair, with SHA-256 `9380704848d3e05269dcd804cc67f46e7c1cbd501190fb19e6e11bee82bae3d9`. `SPOOL_GATE_CHI=0` selects this extra gate mode. An initial 65 KB fixture was rejected by the unchanged sorter's minimum band-size configuration; the harness now propagates peer failure instead of leaving the finish waiting. The meaningful no-chi validation uses the supported 4 MB input.

### Verbatim required gate receipts

150 MB (150,000,001 corpus bytes plus virtual END)

```text
(b) clean_sha256=f6abd41da6e3c3797abae2e288596a6042337a894869f573fecf51c53f624b3e resumed_sha256=f6abd41da6e3c3797abae2e288596a6042337a894869f573fecf51c53f624b3e byte_identical=1
(c) SPOOL_STATE blocks_present=36863 blocks_missing=0 resumed_from=36863
WORK_AVOIDED phase=finish done=19530460 total=57279194 ratio=0.340969532
(d) deleted=piece-492-block-31488-keys-66499813-66500984.entropy
SPOOL_RECOMPUTE block=31488 piece=492 runs=1024
SPOOL_STATE blocks_present=55936 blocks_missing=1 resumed_from=55936
WORK_AVOIDED phase=finish done=0 total=57279194 ratio=0.000000000
(d) repaired_sha256=f6abd41da6e3c3797abae2e288596a6042337a894869f573fecf51c53f624b3e byte_identical=1 recomputed_blocks=1
```

1 GB (1,000,000,005 corpus bytes plus virtual END)

```text
(b) clean_sha256=f5d1c4d323a73cdb9e45ea2997941fe4a5897fd8c299f13fccd92e797203c1c9 resumed_sha256=f5d1c4d323a73cdb9e45ea2997941fe4a5897fd8c299f13fccd92e797203c1c9 byte_identical=1
(c) SPOOL_STATE blocks_present=245759 blocks_missing=0 resumed_from=245759
WORK_AVOIDED phase=finish done=116378094 total=368036332 ratio=0.316213601
(d) deleted=piece-3524-block-225593-keys-500327452-500329324.entropy
SPOOL_RECOMPUTE block=225593 piece=3524 runs=1024
SPOOL_STATE blocks_present=359410 blocks_missing=1 resumed_from=359410
WORK_AVOIDED phase=finish done=0 total=368036332 ratio=0.000000000
(d) repaired_sha256=f5d1c4d323a73cdb9e45ea2997941fe4a5897fd8c299f13fccd92e797203c1c9 byte_identical=1 recomputed_blocks=1
```

Remaining limits: this does not implement Lane A's driver/producer restart. The 64-band checkpoint spacing permits redo of the uncommitted tail. Retaining raw/LCP inputs, 16-byte source pairs per normalized run, embedded stream prefixes, full directory snapshots, full-base compaction, and pending in-memory chi deltas cost substantially more scratch, memory and checkpoint I/O than the original streaming finish. The 150 MB/1 GB gates do not certify 100 GB resource bounds. Garbage/retained dependencies require explicit cleanup after a durable build completion; this implementation prefers retention over unsafe early deletion. The completed P100c scratch was not modified or signalled.

---

## Original specification (historical; not all items implemented)


## Decision and verified facts

**The current SXI2 container cannot be appended per producer piece without rewriting its prefix.** The premise that xsa_finish writes the container while consuming the stream is false on this revision: it creates ContainerOut only after the sweep, at xsa/runtime/bit6/xsa_finish.cpp:6026. During the sweep it writes entropy spools, sparse tails and a random-access chi bitmap, and accumulates directories in memory. Container members are then assembled sequentially, followed by a header/directory back-patch. Sequential final serialization does not imply incremental appendability.

All finish references below mean xsa/runtime/bit6/xsa_finish.cpp. The requested sxi2_write.cpp exists at bit6/sxi2_write.cpp, not in xsa/runtime/bit6. Its Out also reserves the directory and seeks back at finish (lines 25–47). bit6/SXI_FORMAT.md describes several historical layouts; this decision uses the actual v6 lite writer. In particular m7 is remap, not LCP; m4 is empty anchors; m10 is empty derived LF; m11 contains sparse tail anchors. No LCP container member is emitted. LCP inputs remain restart dependencies until consumed durably.

The actual codec block is **1024 normalized runs**, not 256 (finish:2933). 256 is the symbol occurrence sampling interval (2940). Nearby comments saying 64 are stale. Use the recorded block size and reject a mismatch.

## Member decision table

“Appendable” means append to the existing final-layout member across input pieces, with no prefix rewrite. “Spool yes” is a proposed restart path, not an existing reopen API.

| member | appendable? | checkpoint state | safe boundary | file:line |
|---|---|---|---|---|
| m1, codec 114 | No final member; entropy spools yes. Directory precedes payload and grows with final run count; payload offset is 2336+16*nblocks. Codebook is fixed before sweep; block byte rates depend on current symbols/lengths, not future runs. | Separate physical head/length byte ends; logical bit totals; completed block count; each block's two bit lengths; fixed code lengths and corpus weights; run count, row count, symbol totals; unencoded carry runs if piece ends inside codec block. | Complete 1024-run codec blocks; final short block only at EOF. Never checkpoint a partially encoded block. | finish:2933, 5080–5125, 5154–5183, 6280–6323 |
| m4, codec 4 | Constant 12-byte XANC empty anchors; write once at assembly. No stream append needed. | Presence and fixed bytes/checksum; count=0. | Whole member. | finish:6076–6080 |
| m5, codec 105 | No. Population chi determines low width and both bitstream dimensions; sweep witnesses are not position ordered. Bitmap updates are random, not append. | Committed immutable bitmap base plus ordered set-bit delta ends/hashes; totalValues, duplicates, sweep chi and full sweep continuation. EF cursor only needed if separately checkpointing final assembly. | Quiescent completed sweep band plus transactional bitmap delta commit; EF rebuilt after EOF. | finish:2633–2689, 6028–6065; bit6/SXI_FORMAT.md, “Mmap chi construction” |
| m7, codec 7 | Constant 256-byte remap; no LCP payload and no stream append needed. | Remap SHA256 and bytes in plan. | Whole member. | finish:6081–6082 |
| m10, codec 110 | Zero payload; final directory count requires rNorm. No append needed. | Final normalized run count; symbol totals for derived LF verification. | Whole directory entry at final assembly. | finish:6083; xsa/src/sxi2.rs:486–502 |
| m11, codec 111 | Raw sparse-tail spool yes; final member no without changing its count header. | Sparse spool byte end and hash; sample count; normalized run cursor (sample indices 0,1024,...). | Complete u64 tail record at quiescent piece checkpoint. | finish:5198–5202, 6085–6101; xsa/src/sxi2.rs:537–550 |
| m12, codec 112 | No final member: final counts and level-2 array precede level-1 entries. Sample spool yes. | Run-start spool end/count/hash, row cursor; level-2 stride=65536, level-1 stride=1024. | Complete u64 sample; assemble both levels at EOF. | finish:6383–6414 |
| m13, codec 115 | No final member: symbol-grouped entries, final counts/offsets and level-2 array precede entries. Previous sample stride is back-updated when next sample arrives. | Per-symbol sample spool ends/counts/hashes; occ, cum, lastSample, lastSampleOcc; one unresolved last sample per symbol, including pending stride. | Complete closed sample triples; keep last open sample in checkpoint, never patch committed spool. | finish:5095–5106, 5170–5178, 6417–6478 |
| SXI2 header and member directory | No: final r, lengths, counts, offsets, CRCs and header CRC are written at EOF over reserved zeros. | Reserve size, member identities/codecs; assembly cursor and completed member offset/bytes/count/running CRC if assembly recovery is added. | Final publish after all members; not a producer piece. | finish:3153–3206; bit6/sxi2_write.cpp:25–47 |

**Cannot be appended finding:** m1, m5, m11, m12, m13 and the SXI2 header/directory cannot be appended across producer pieces in their current final layouts without prefix rewriting. m4/m7 are constants and m10 has no bytes. There is no emitted LCP member to append.

## Chosen fallback and durable boundary

Keep the on-disk SXI2 format unchanged. Resume the finish **spools and machine state**, then assemble the final container once. This avoids recomputing suffix sorting, seam repair, LCE and witness selection for the committed prefix. It does not avoid one final serialization pass. Reassembling m1 costs O(encoded bytes + blocks); m12/m13 O(samples); m11 O(R/1024); m5 costs O(n/8 + chi) bitmap scan/EF emission. Reconstructing directories by decoding the durable m1 prefix costs O(prefix runs) and is an emergency recovery mode, not the cheap-resume path.

A piece checkpoint is a barrier: stop accepting source records, join all band workers, complete all LCE and sweep work attributable to those records, flush witness bridge buffers, and capture delayed repair records. Disable feed-ahead at a checkpoint target: no next-band feed may execute before the current band has completed finishAdvance; both slots must have empty normalized-run/answer vectors. Prefetch may be drained and discarded because it has no semantic output. Save the inter-band `prevTail` scalar (finish:5079,5697–5718). A physical piece ending inside a band is not yet consumable durably: retain it until the containing band completes, and checkpoint the contiguous group of fully consumed pieces together. Do not call finishStream or sweep.finish at a checkpoint: those are EOF transitions. Source pieces and normalized codec blocks need not align.

To support every piece boundary without a mid-block codec checkpoint, stage at most 1023 already-selected normalized (c,len) pairs in an **unencoded** carry. Encode only complete 1024-run blocks, pad both streams, drain and fsync. Keep separate selected_run_count and encoded_run_count; directory accounting follows selected runs. On restart encode carry with new selected runs; do not feed carry back through repair or sweep or duplicate sparse samples. Alternative: checkpoint only intersections of piece and complete codec block boundaries, but that can delay durability indefinitely and is not the recommended design. Existing code encodes immediately, so this staging is a required future change.

Chi requires a transaction, not merely msync: later uncommitted pieces can set bits anywhere, including pages containing durable bits. Chosen design: immutable committed bitmap base, append-only per-piece first-set position deltas, and a volatile overlay. Never mutate committed base in place. Delta IDs are unique positions first added by that piece; committed population is base population plus committed delta entry counts. Compact to a new immutable bitmap generation atomically, retaining the old base and deltas until a checkpoint references the new generation. Recovery rebuilds the overlay from committed deltas; no uncommitted set may survive. Budget this scan separately. A full bitmap snapshot every piece is correct but not cheap.

## PIECE_CHECKPOINT journal record

New protocol, not PIECE_DURABLE. Existing PIECE_DURABLE only certifies producer files (xsa/runtime/bit6/durable_piece.h:75–91); it does not certify finish consumption. Use a separate binary journal with the name `PIECE_CHECKPOINT`; do not mix binary with today's text journal. All integers little endian, no native struct padding. Fixed size **256 bytes**:

| offset | bytes | field and meaning |
|---:|---:|---|
| 0 | 8 | magic ASCII `PXCP0001` |
| 8 | 4 | version=1 |
| 12 | 4 | record_bytes=256 |
| 16 | 8 | sequence, strictly increasing from 1 |
| 24 | 32 | SHA256 of previous complete 256-byte record, zeros for first |
| 56 | 32 | plan SHA256: corpus/remap, all binaries, knobs, machine-state schema and codec constants |
| 88 | 8 | last fully consumed piece ID |
| 96 | 8 | next global band ID |
| 104 | 8 | next key frontier; interpretation/exclusivity fixed in plan |
| 112 | 8 | certified normalized rows selected |
| 120 | 8 | selected normalized run count |
| 128 | 8 | container_prefix_end; **zero during streaming in this design** |
| 136 | 32 | SHA256(container[0:end]); SHA256(empty) during streaming |
| 168 | 8 | checkpoint-state file byte length |
| 176 | 32 | checkpoint-state file SHA256 |
| 208 | 8 | flags: bit0 chi enabled, bit1 true EOF; all others zero |
| 216 | 8 | reserved zero |
| 224 | 32 | SHA256 of bytes 0..223 |

State filename is `resume/state.<sequence>`; paths within it are scratch-relative and must not escape the scratch root. This record and its referenced state make the piece self-describing; no process pointers, native C++ layouts or recomputed-from-zero counters are allowed.

State file envelope: 8-byte magic `PXST0001`, u64 section_count, then sections `(u32 tag,u32 version,u64 payload_bytes,payload)`. No padding. Every collection uses u64 count followed by records; every string uses u64 byte length then UTF-8 bytes. u64 integers unless specified; booleans u8 0/1; signed values i64. Unknown mandatory tags/versions refuse recovery. The journal records the exact variable total size; no fixed-size claim for vectors.

Required version-1 sections (tag order 1..8):

1. File manifest: entries `(role string,path string,generation,end,count,sha256[32])`; role binds each spool, directory stream, immutable bitmap base, chi delta, producer manifest, seam description, PFCK and LCP dependency. end is exact durable byte extent, not stat size. Append files hash only committed prefix; immutable files hash whole content. Zero or absent container is explicit. Include each symbol's sample stream as a distinct role.
2. Producer/source: next piece, band, raw-record index and raw-row cursor, key interval bounds/exclusivity, wave ledger reference/hash and wave base; PieceSource progress/consumed/window/depth/finalBand/capturedRows/delivered/S/R/n/B/w, terminal flag, shards and bandRefs/bandCounts/classExtents/completed collections (finish:1137–1151). Durable consumer credit must equal checkpoint frontier. Reopen views lazily; never serialize fd/mutex. Include any unread records already fetched, or rewind input cursor to before them and retain their source files.
3. Repair: N,n,sampleChecks,rows,srcHave,srcPending,sentinelRows,row,next,have,pending,rNorm,nNorm,totals[256],runFreq[256] (finish:2185–2210). RawRun is `(c:u8,len,head,tail)`; NRun is `(c:u8,len,h,t,rk,ht,tt)` (1135,2100). Save seam classes/identity and the next class cursor. Rebind seam and sink pointers. Preserve both pending coalescers; piece boundaries do not close runs.
4. Sweep: cyclic:u8,n,bigN,chi; rr_len[256]:i64, rr_pos[256], rr_act[256]:u8; previous_interior,m:i64,p:i64,p_sa_last,first:u8; statistics statAdvance/statCharChange/statScanWords/statFired/statActSeen/statEmit/statMaxAct. Rebuild heap/heapPos deterministically from rr_len, not process memory (finish:2343–2410). Require work queues and both band slots empty at the barrier defined above; no in-flight answer records are permitted. Save prevTail in section 8. Chi-disabled section is empty.
5. Codec: encoded_run_count, completed_block_count, physical head/length ends, logical head/length bit totals, code lengths[256]:u8, weights[256], carry_count and carry `(c:u8,len)` records; per-block `(head_bits,length_bits)` pairs. nbits=acc=0 is an invariant, not recoverable mid-block state. Physical ends are sums of ceil(block_bits/8), **not** ceil(total logical bits/8), as finish:2950–2959 explains.
6. Directories: row,occ[256],cum[256],lastSample[256],lastSampleOcc[256]; run-start count; per-symbol closed entry count plus optional open `(cum,run,occ_at_sample)` record. Closed entries are `(cum,run,stride)` u64 triples in manifest streams. Track sparse count/end in manifest, with count=ceil(selected_runs/1024). Last open sample is closed at next sample or EOF only.
7. Chi: n,bitmap_bytes,base_generation,base_population,delta_count,totalValues,duplicates; ordered manifest-role indices for base and deltas. Require totalValues=sweep.chi, duplicates=0 and totalValues=base_population+sum(delta counts). Full popcount verifies compaction; do not derive population from mutable mmap or current file length. Include bridge first-occurrence spool end if enabled.
8. Guards: prevTail,expectedRlebwtBytes,certifiedRows,streamFnv,streamH,streamT,streamLen,streamRuns,rawRunsFed,bandReads,advRuns; optional bridge output ends and pending records. Initial protocol refuses experimental repair-chunk scheduling, Kasai/column audit modes with additional mutable validation state, legacy m1 replay, debug stream and bridge modes unless their extra state is explicitly versioned. Reconstruct immutable LCE/PFCK/seam indexes from retained files; caches and timing counters do not determine output.

Publication order: materialize new immutable state dependencies; drain/fsync append prefixes; fsync new delta and state files; rename immutable files into their final generation names and fsync parent directories; append one complete checkpoint record and fsync journal; only then advance consumer credit or unlink dependencies. Kill before journal fsync leaves the preceding checkpoint authoritative. Torn/invalid trailing record is ignored. This replaces current eager deletion, including finish:1298–1301 and 1452–1469. File checksums bind counts and cursors to bytes, not just filenames.

## Restart algorithm

1. Lock the build; verify plan and executable identities before mutation. Read complete valid hash-chained records; select latest with all referenced dependencies present and verified. Invalid referenced durable data is corruption: fail closed, never silently replay from zero or discard evidence.
2. **Verify container prefix SHA -> truncate to last durable piece end -> reopen members for append -> resume is not implementable literally for existing SXI2.** In chosen streaming design the prefix is empty/end=0; verify SHA256(empty), discard/truncate only the owned uncommitted `.partial` assembly output. There are no live final members to reopen. Verify every spool prefix SHA and immutable dependency SHA before truncating any append spool to its manifest end; reopen those spools at those exact physical ends. Do not use O_EXCL constructors or cleanup destructors that unlink retained state.
3. Load sections, restore both repair carries, sweep state and codec unencoded carry, reconstruct heap and caches, restore directory open samples. Rebuild committed chi membership from immutable base/deltas; population comes from section 7 plus validated delta counts. Offsets come exclusively from section 1/5; never infer logical offsets from incomplete files.
4. Reuse verified retained producer pieces above the consumer frontier; start producer at first absent piece using the ledger/key frontier. Do not equate a producer publication frontier with finish consumption. adf2e82 supplies `--resume-band` and wave skipping, but may redo current-wave scatter; its commit message explicitly says unverified and finish append absent. Readiness/histogram/PFCK must be reused or their corpus scans counted. Mainline recovery.rs:174–202 currently removes intermediates and journals REPLAY from_key=0; replace that path only for this new protocol.
5. Resume from next raw record; never feed selected codec carry through sweep again. Continue barrier checkpoints. At genuine EOF close repair and sweep once, encode final partial codec block, close open samples, materialize committed bitmap and assemble all SXI2 members. Back-patch header and directory once, fsync output and publication directory, then publish BUILD_DONE with complete container SHA. Assembly interruption restarts assembly from retained spools; it must not restart producer or sweep.

If literal final-container append is a product requirement, a new chunked container with per-piece members/footer indexing and new reader support is required. Reserving guessed directory capacity or padding each piece into a fake final codec block is not this format and is not an acceptable shortcut.

## RESUME_RETENTION

For an owned file f, deletion is allowed iff:

`BUILD_DONE_verified_and_durable OR (checkpoint_durable(C) AND f not in transitive_dependencies(C) AND every byte/semantic input from f needed after C is either in another fsynced, hashed dependency of C or strictly below C's consumed frontier AND no live reader holds a future dependency on f)`.

Additionally retain all dependencies of the previous accepted checkpoint until the new checkpoint and its directory entries are durable. Uncommitted producer pieces may be discarded only if regeneration starts at or before their first key and no checkpoint references them. Never delete producer inputs merely because read() completed or a volatile consumed marker moved. Retain PFCK/seam/LCP inputs unless their future required state has been checkpointed; normal bitmap/spool destructors must not delete restart dependencies. Final assembly success alone is insufficient; durable BUILD_DONE plus verified publication authorizes final cleanup. This predicate applies to run columns, LCP slices, scatter/wave ledgers, chi deltas, all spool generations and checkpoint states.

## Cheapness gate: a full replay must fail

Proposed instrumentation/gate interface below is a requirement for implementation, **not existing commands asserted to work**. Use a new isolated scratch outside P100c. No timing-only gate: cached replay can look fast. Count actual completed operations, including preflight, validation and skipped-output work; hash I/O is separate, not sorting work.

Required counters per phase: corpus bytes scanned for scatter/histogram/PFCK; suffix rows sorted; raw runs decoded/repaired; LCE answers computed; sweep advances; normalized runs encoded. Also log prefix-specific versions (keys/rows strictly below selected checkpoint), checkpoint restore bytes, SHA verification bytes, directory restore entries and final assembly bytes. A skip counter alone is not evidence.

Let T_j be clean full-build actual work for each expensive phase, P_j its work completed at checkpoint, and R_j actual work performed after restart. Define `WORK_AVOIDED_j=(T_j-R_j)/P_j` for P_j>0. A full replay has R_j>=T_j and scores <=0. Require **each** expensive phase with P_j>=0.5*T_j to score >=0.90; require checkpoint at >=75% sorted rows and >=75% sweep advances; require prefix reprocessed rows/runs/LCE/advances=0, except <=1023 carry run encodes and at most one explicitly measured producer wave. If current-wave scatter exceeds 10% of P_scatter, gate fails: improve producer checkpoint granularity. Report all components, never a weighted score that hides a replayed finish. Hash/restore/assembly scans are exempt only from semantic work counters, remain measured, and must have bytes and wall time reported. For end-to-end cheapness require restart elapsed <=0.50*clean elapsed on an otherwise idle machine; this extra gate can fail even with semantic reuse.

Recommended exact invocation contract after implementing the named test hooks:

```sh
export XSA_BIN="$PWD/xsa/target/release/xsa"
export RESUME_GATE_ROOT="/mnt/nvme2n1/erikg/resume-gate-isolated"
export RESUME_GATE_INPUT="/path/to/dedicated/representative-corpus.bin"
mkdir -p "$RESUME_GATE_ROOT"
"$XSA_BIN" build --tape --input "$RESUME_GATE_INPUT" --scratch "$RESUME_GATE_ROOT/clean" --threads 16 --memory-gb 32
XSA_TEST_STOP_AFTER_DURABLE_FRACTION=0.75 "$XSA_BIN" build --tape --input "$RESUME_GATE_INPUT" --scratch "$RESUME_GATE_ROOT/interrupted" --threads 16 --memory-gb 32
"$XSA_BIN" build --tape --input "$RESUME_GATE_INPUT" --scratch "$RESUME_GATE_ROOT/interrupted" --threads 16 --memory-gb 32
python3 tools/binbwt/tests/resume_work_gate.py --baseline "$RESUME_GATE_ROOT/clean" --resumed "$RESUME_GATE_ROOT/interrupted" --min-checkpoint-fraction 0.75 --min-work-avoided 0.90 --max-elapsed-ratio 0.50
```

The stop hook must request a graceful durable barrier, not use the running production build. Future gate script must compare final full-container SHA and query receipts, inspect operation counters, and fail if a REPLAY from_key=0 marker occurs. Repeat on chi/no-chi, nonaligned piece/block boundaries, same-symbol runs across pieces, and crashes before/after each fsync/rename/journal step. Include bitmap writes from an uncommitted piece and m13 open-stride continuation. Final file SHA equality is necessary but cannot establish work avoidance.

## Evidence and residual risks

Verified by source reading: member layouts/order, post-sweep ContainerOut construction, block size, spool padding, bitmap random mutation, final-population EF dimensions, directory dependency on final counts, m13 stride mutation, delayed repair state, eager source deletion, and replay-v1 cleanup. Read xsa/src/sxi2.rs for EF/legacy run semantics and derived LF/sparse anchors; blocked m1/m12/m13 are actually read in xsa/src/qlite.rs:724–876. No runtime behavior was attested by this analysis.

Proposed/inferred: transactional delta bitmap, stable binary schema, unencoded block carry, producer/finish barrier, and operation-counter gate. They require implementation and fault injection. A checkpoint must include every inter-band LCP carry and producer wave dependency; this spec requires them to be drained or explicitly serialized, not silently dropped. Corpus identity verification and rebuilding chi overlay can remain O(n) I/O; report that cost candidly. Large directory state restore remains O(samples) I/O. Neither f251b14 nor adf2e82 proves incremental resumption, and neither can migrate an existing running replay-v1 build into this protocol safely.
