# The container is built as we go, and short scratch pauses the producer

Branch `feat/container-stream`, base `pile-week-v1` `444e6cb`.
Box state during these runs: load average 47-64 (other lanes' jobs), 1007 GiB RAM,
`/mnt/nvme2n1` 1.4 TiB free, `/` 962 GiB free.

Three commits:

| commit | what |
|---|---|
| `55d1243` | SXI2 **v7** incremental layout + m1 codec **116** (stream-order members, trailing directories) |
| `ed048d5` | `--scratch-budget-gb` flow control: PAUSE the producer, never kill |
| `a077e8b` | resume at a container offset: verify the durable prefix, truncate the uncommitted tail |
| `05d67ab` | the band arena is a plan input, ALLOCATED-byte accounting, the product is not scratch |

## 1. What changed, and why the old layout could not do it

`RESUME_DESIGN.md`'s member table says it: in v6 the 64-byte header plus the 40-byte
member directory are a PREFIX whose content (length, offsets, CRCs) is only known at
close, and **m1's own 16-byte-per-block directory sits in FRONT of m1's payload**
(`payload_off = 2336 + 16*nblocks`). Both force the whole encoded run stream to be
staged in scratch before the first container byte can be written: **0.277 n of scratch
that is pure staging** — 360 GB at the Pile.

v7 (the new default) appends member bytes in **stream order** and moves both
directories to **trailing** sections:

```
[0,64)                      fixed header (length/flags/CRC patched once at close)
[64, ...)                   member bytes, appended in stream order (m1 first)
[length-40*count, length)   the member directory
```

and m1 codec 116 is `[C table 2048][lens 256][nblocks=0][block_runs][rNorm=0]
[payload_off=2336][payload ...][per-block directory 16*nblocks]`. The two close-time
fields are left **zero and derived by the reader** (`nblocks = ceil(count/block_runs)`,
`rNorm = count`), which is why the member is written ONCE and never patched - a
patched member would need its CRC recomputed over the whole member.

The payload bytes are identical to codec 114's. Measured, not asserted: at 150 MB the
new container's m1 **payload** (52,494,976 B) and **block directory** (894,992 B) are
byte-for-byte equal to the banked v6 container's, and so are m5, m11, m12 and m13; at
1 GB the same holds for all 8 members, including the 337,394,584 B m1 payload.

Readers: `xsa query` (qlite) and `sxi::Container` open v6 and v7. The v6 layout is still
reachable (`XSA_CONTAINER_STREAM=0` / `xsa_finish --container-legacy`) and is
**byte-identical to the banked containers** (measured below).

## 2. G1 — 150 MB (`/dev/shm/t150m.txt`, 150,000,001 B)

```
xsa build --tape --input /dev/shm/t150m.txt --scratch <A|B> --threads 24 --memory-gb 24
```

```
60b56ffa3502d466b80f0dc8fae2ee960ffc4ec41ec6c2c8d535cc80478689d2  g1tip/A/t150m.sxi2
60b56ffa3502d466b80f0dc8fae2ee960ffc4ec41ec6c2c8d535cc80478689d2  g1tip/B/t150m.sxi2
XSA_BUILD_DONE chi=44034230 ...   (both repeats)
```

* **chi 44,034,230 in both repeats** (the in-process assert; unchanged from every
  earlier 150 MB receipt).
* **byte-identical to itself across repeats.**
* The READER still opens the OLD banked v6 container
  (`/mnt/nvme2n1/erikg/L150/t150m.sxi2`, sha `f6abd41d...f624b3e`) and answers the
  same: `QUERY_DONE ... occurrences=1034022` from v6 and from v7, and
  `VERIFY_TOTAL patterns=1 positions=2000 bad=0 duplicates=0` with `--corpus` on both.
* Member equality against that banked container:

```
M1_PAYLOAD_IDENTICAL=1 bytes=52494976
M1_BLOCKDIR_IDENTICAL=1 bytes=894992
M5_IDENTICAL=1 bytes=20383582 count=44034230
M11_IDENTICAL=1 bytes=447512   count=55937
M12_IDENTICAL=1 bytes=454560   count=55937
M13_IDENTICAL=1 bytes=44212472 count=1839535
total bytes 118,891,088 in both layouts
```

Legacy arm on the same corpus (`XSA_CONTAINER_STREAM=0`) prints no `M1_STREAM` line,
chi 44,034,230, same total size.

## 3. G2 — 1 GB (`/mnt/nvme2n1/erikg/ar-slice-1gb.txt`, 1,000,000,005 B)

```
xsa build --tape --input /mnt/nvme2n1/erikg/ar-slice-1gb.txt --scratch <dir> --threads 24 --memory-gb 24
```

```
CONTAINER_STREAM mode=declare version=7 member_order=stream trailing_directory=1 m1_codec=116 header_bytes=64
CONTAINER_STREAM mode=incremental member=m1 off=366352 blocks=256 payload_bytes=364016 head_bits=2185228 len_bits=717784
PIECE_ABSORBED idx=0 container_off=5084104 bytes=819244        (5616 of these)
CONTAINER_SYNC why=piece-release member=m1 member_bytes=5084104 unsynced_bytes=5084104
CONTAINER_FINAL bytes=750469512 sha256=c540881c6c6d8f59410c9e4cabf035476283c025f1e91b36cdd65c110cfcaee9 members=8
XSA_BUILD_DONE chi=283296727
```

* **chi 283,296,727**, equal to the banked `restart-safe-gates-20261105/base1g`
  `v2tape` build of the same corpus, and **every member identical** to it:

```
M1_PAYLOAD_IDENTICAL=1 bytes=337394584
M1_BLOCKDIR_IDENTICAL=1 bytes=5750576
M5_IDENTICAL=1 bytes=133324207 count=283296727
M11_IDENTICAL=1 bytes=2875304   count=359411
M12_IDENTICAL=1 bytes=2920280   count=359411
M13_IDENTICAL=1 bytes=268201568 count=11160268
M4_IDENTICAL=1 bytes=12 ; M7_IDENTICAL=1 bytes=256
new v7 runs=368036332 bytes=750469512 | old v6 runs=368036332 bytes=750469512
occurrences=7021397 (v7) == occurrences=7021397 (banked v6)
```

* The **legacy arm reproduces the banked 1 GB SHA exactly**:
  `f5d1c4d323a73cdb9e45ea2997941fe4a5897fd8c299f13fccd92e797203c1c9` - the sha in
  `RESUME_DESIGN.md` for this corpus. Nothing about v6 moved.
* Same corpus, same threads/memory, only the layout differs (identical accounting:
  ALLOCATED scratch bytes, product excluded):

```
v6 legacy : SCRATCH_BACKPRESSURE_SUMMARY budget_gb=24.000 peak_gb=4.297 ...
v7 stream : SCRATCH_BACKPRESSURE_SUMMARY budget_gb=24.000 peak_gb=4.055 ...
```

  At 1 GB the band window still dominates, so the gap is only the removed staging term
  (337,394,584 B of m1 payload = 0.337 n). The term v7 deletes is exactly that payload:
  it is no longer staged anywhere, it is appended.

## 4. G3 — FLOW CONTROL: 10 GB with `--scratch-budget-gb 6`

```
xsa build --tape --input /mnt/nvme2n1/erikg/pf-10gb.txt --scratch .../g3 \
          --threads 24 --memory-gb 48 --scratch-budget-gb 6
```

```
SCRATCH_BUDGET source=operator budget_bytes=6442450944 budget_gb=6.000 free_bytes=1521986809856 \
  derived_cap_bytes=51539607552 plan_memory_gb=6 (min(0.8 x free, 2 x band window) x band arena \
  clamped to the budget; a pause never kills, never fails)
SCRATCH_BACKPRESSURE state=pause live_gb=7.185 budget_gb=6.000 peak_gb=7.185 pauses=1 resumes=0 output_gb=0.002 pause_at_gb=6.000 action=producer_stopped consumer=drains
SCRATCH_BACKPRESSURE state=resume live_gb=5.374 budget_gb=6.000 peak_gb=7.185 pauses=1 resumes=1 output_gb=0.036 why=drained paused_ms=16065 floor_gb=0.000 pause_at_gb=6.000
SCRATCH_BACKPRESSURE state=pause live_gb=6.827 budget_gb=6.000 peak_gb=7.185 pauses=2 resumes=1 output_gb=0.059 pause_at_gb=6.000 action=producer_stopped consumer=drains
SCRATCH_BACKPRESSURE state=resume live_gb=6.806 budget_gb=6.000 peak_gb=7.185 pauses=2 resumes=2 output_gb=0.060 why=drain_floor paused_ms=30163 floor_gb=6.806 pause_at_gb=7.306
PHASE emit-tape END rc=0 wall=32:07.80 peak_rss_kb=235172   disc_scratch_bytes=8281183895
PHASE order-tape END rc=0 wall=32:08.41 peak_rss_kb=12158448 disc_scratch_bytes=8278632091
PHASE finish-v2 END rc=0 wall=34:11.96 peak_rss_kb=3622484 disc_scratch_bytes=7022772001
SCRATCH_BACKPRESSURE_SUMMARY budget_gb=6.000 peak_gb=7.199 floor_gb=6.806 pauses=2 resumes=2 paused_ms=46228 last_live_bytes=45370774 output_gb=6.523 accounting=scratch_excludes_container
XSA_BUILD_DONE chi=2676925006 pipeline=v2tape container=.../g3/pf-10gb.sxi2
```

* **It COMPLETED.** chi **2,676,925,006** (the banked 10 GB value), container
  6,523,377,865 B, sha256 `573d29135c2dbe49e784c75cb6613e5199263474bd65e1612bdc6c79b953513d`.
* **peak live scratch 7.199 GB against a 6.000 GB budget**, floor 6.806 GB,
  **2 pauses / 2 resumes**, 46.2 s paused, wall 34:12 (order-tape 32:08, finish 34:12).
* Nothing was killed and nothing failed: both pauses ended in `state=resume`.
* The product's own 6.523 GB is measured separately (`output_gb`), because it is the
  output, not scratch - and the plan already prices it as its own term. Counting it
  twice would make a 6 GB budget impossible on a corpus whose container is 6.5 GB.
* Two measured corrections came out of this gate, both now in the code:
  1. **Pausing bounds the average, not the burst.** With `--memory-gb 48` the unit in
     flight is a 48 x 500 MB wave; a 6 GB budget went 5.1 -> 10.9 GB between two 2 s
     samples no matter how fast the signal landed. The band arena is now clamped to
     the budget (`plan_memory_gb=6` in the line above), so the wave fits and the
     pauses handle the finish's own growth.
  2. **Live scratch is ALLOCATED bytes.** The chi bitmap is a sparse `n/8` mapping
     (164 GB apparent at the Pile, a fraction of that allocated early); apparent size
     would charge a run for pages that do not exist.
* Peak is sampled at 2 s; the peak between samples is not observed (stated risk).

## 5. G4 — kill and restart at 1 GB, resume at a container offset

`gates/g4_resume.sh` (sorter + emitter stay alive; the FINISH is SIGKILLed after its
first durable checkpoint and restarted with the same command - the native resume shape
the finish's own checkpoint protocol defines; the driver-side producer restart is
RESUME_DESIGN.md's Lane A and is **not** in this change).

```
G4_CLEAN sha256=c540881c6c6d8f59410c9e4cabf035476283c025f1e91b36cdd65c110cfcaee9 bytes=750469512
G4_KILL after_checkpoint partial_bytes=236784896
G4_KILLED finish_pid=3324685 rc=-9
G4_RESTART rc=0
G4_RESUMED sha256=c540881c6c6d8f59410c9e4cabf035476283c025f1e91b36cdd65c110cfcaee9 bytes=750469512
G4_BYTE_IDENTICAL=1
RESUME_MODE pending container=...out.sxi2.partial retained_bytes=236784832 verify=sha256_over_durable_prefix truncate=uncommitted_tail
RESUME_MODE container_off=236498048 container_bytes=236784896 prefix_sha256=aa12de5d9bf734c66c9e9b1c220b0737fbd7b2a4b47e7110ed9975ad63c7fb77 file_bytes_before=236784896 truncated_bytes=286848
RESUME_MODE container_off=236497984 payload_bytes=236495648 closed_blocks=249855 closed_runs=255851520 pending_block_runs=1022 work_avoided_basis=finish_advance_counters
WORK_AVOIDED_SUMMARY phase=finish container_off=337396320 performed=112183790 total=368036332 avoided=255852542 avoided_frac=0.695183 basis=finish_advance_counters restored_runs=255852542 restored_bands=3904
CONTAINER_FINAL bytes=750469512 sha256=c540881c... members=8
NO_SURVIVORS checked=scatter_sort,cross_lcp_merge,xsa_finish
G4_PASS
```

* resumed container is **byte-identical** to the uninterrupted build (and that sha is
  also the driver's sha for the same corpus, so the natíve triad and `xsa build` agree).
* `avoided_frac = 0.695183`: 69.5% of the sweep was reconstructed from the checkpoint
  instead of re-run.
* 286,848 bytes appended after the last checkpoint were **truncated**; the retained
  prefix's SHA-256 was verified against the checkpoint before a byte was appended.
* The checkpoint state carries the running SHA-256 of the member prefix and the OPEN
  codec block's bits (bounded by one block): a checkpoint can fall inside a 1024-run
  block and those bits exist nowhere else in the streaming arm.

## 6. G5 — the Pile window: `--tape --input /mnt/nvme2n1/erikg/pile.txt --threads 96 --memory-gb 96 --scratch-budget-gb 200`

The run was stopped after 70 minutes of wall (the operator's instruction), of which
~34 min were the driver's pre-build corpus pass (a 1.31 TB read) and ~36 min the
stages.

```
XSA_PLAN_DISC FEASIBLE shape=tape: peak 1218.1 GiB (1307910802540 B) = 1.000n fits 1404.0 GiB free (1507567652864 B); headroom 185.9 GiB
SCRATCH_BUDGET source=operator budget_bytes=214748364800 budget_gb=200.000 free_bytes=1507567620096 derived_cap_bytes=103079215104 plan_memory_gb=96
XSA_FINISH_PFCK ... n=1307910802541 tau=22 count=59450491025 bytes=475603928240 wall=1224.227 workers=96
```

Stopped after 70 min of wall (06:36 -> 07:45 of stage time; the driver's own pre-build
corpus pass took the preceding 34 min). Final state of the window: **8 pauses / 8
resumes, peak live scratch 442.941 GB against the 200 GB budget, 0 kills, 0 failures,
no band published yet (the sorter was still inside its first 32-band wave scatter of a
1.31 TB corpus), container 2400 B (the fixed header + m1's fixed head)**. The 443 GB of
scratch was the finish's PFCK grid; the directory was deleted after the window (its
receipts are `g5.log` plus `g5_samples.txt`).

Live scratch over time (`du -s -B1`, ALLOCATED bytes, sampled every 2 min; `merged=`
is the finish's readiness scratch, the container is 2400 B because the sweep had not
started):

```
06:36:43 merged=4096          total_alloc=40960          pfck=0            container=0
06:54:44 merged=50551607296   total_alloc=50574303232    pfck=122616637776 container=0
07:00:44 merged=229155766272  total_alloc=229186850816   pfck=273719969128 container=0
07:04:44 merged=475604090880  total_alloc=475604242432   pfck=475603928240 container=0
07:14:45 merged=475603963904  total_alloc=475604250624   pfck=475603928240 container=2400
07:22:45 merged=475603963904  total_alloc=475604250624   pfck=475603928240 container=2400
07:44:45 merged=475603963904  total_alloc=475604381696   pfck=475603928240 container=2400
```

Flow control engaged and stayed engaged:

```
SCRATCH_BACKPRESSURE state=pause  live_gb=396.252 budget_gb=200.000 peak_gb=396.252 pauses=7 resumes=6 output_gb=0.000 pause_at_gb=395.244 action=producer_stopped consumer=drains
SCRATCH_BACKPRESSURE state=resume live_gb=424.179 budget_gb=200.000 peak_gb=424.179 pauses=7 resumes=7 output_gb=0.000 why=drain_floor paused_ms=30034 floor_gb=424.179
SCRATCH_BACKPRESSURE state=pause  live_gb=426.097 budget_gb=200.000 peak_gb=426.097 pauses=8 resumes=7 output_gb=0.000 pause_at_gb=424.679 action=producer_stopped consumer=drains
SCRATCH_BACKPRESSURE state=resume live_gb=442.941 budget_gb=200.000 peak_gb=442.941 pauses=8 resumes=8 output_gb=0.000 why=drain_floor paused_ms=30035 floor_gb=442.941
... 8 pauses / 8 resumes in the window, 0 kills, 0 failures, 0 bands lost
```

**What the window proves, and what it disproves.** The mechanism works at the Pile:
the producer really is stopped (`kill -STOP` on its process group), the finish keeps
draining, and nothing is dropped. But it also proves the Pile does **not** fit yet,
and it names the term: live scratch climbed to 443 GB **while the producer was
stopped**, because the binding term is not the producer's output at all - it is the
finish's own readiness grid, the PFCK, at its final
**475,603,928,240 B = 0.364 n**. No pause can shrink that.

## 6a. The next removal, with the measurement that justifies it

The PFCK is a fixed grid of `8*ceil(n/tau)` suffix hashes over the corpus, built
before the sweep (476 GB at the Pile, tau=22) and **never queried on this route**.
Measured on three product runs:

```
SLIM_FP pile-finish-corpus tau=22 bytes=3636363640 queries=0 verified_symbols=0 hash_probes=0 ...
SLIM_FP pile-finish-seam   tau=22 bytes=3636363640 queries=0 verified_symbols=0 hash_probes=0 ...
SLIM_SEEDS queries=0 lce_wall=1912.7 ...            (10 GB, --tape --lcp-bands)
```

Every LCE answer is exact from the producer's LCP band slices, so the corpus
fingerprint is dead weight: 0.364 n of scratch plus a full corpus scan. Removing it
(guard: `--lcp-bands` present, and assert `fhSeam.calls + fhSlim.calls == 0` before
publishing, so a corpus that does need it fails loud instead of shipping unverified
answers) is the single largest remaining term. An attempt to simply not build the grid
was reversed in this branch: `SlimFingerprint` needs a valid fd for `fp_self_check`
and segfaulted with `-1`, so the change needs the fingerprint's construction to become
optional, which is the next task, not this one.

With the grid gone the Pile's scratch floor becomes the chi bitmap (`n/8` = 164 GB) plus
the band window (48 GiB) plus the piece window, i.e. **~215 GB of scratch plus the
830 GB container = 1.02 TB** against the 1.4 TB now free on `/mnt/nvme2n1` (and 966 GiB
on `/`): it fits on the pile's volume, and stops fitting only because of the chi bitmap
on the smaller one.

## 7. What this does and does not make fit

Removed by v7 (structurally, not statistically): the m1 staging term, exactly the m1
payload size = **0.277 n**. Nothing stages it now; `PIECE_ABSORBED` deletes each tape
piece only after the bytes covering it are fsynced.

Terms still proportional to n at the Pile, measured on the runs above:

| term | size | state |
|---|---|---|
| band window (arena) | 48 GiB at `--memory-gb 96` | bounded, and clamped to the budget when one is given |
| chi bitmap (`frag.chi-bitmap`) | `n/8` = 164 GB at 1.31 TB | **still O(n)**, sparse, allocated as witnesses land |
| m5 EF payload | ~0.12 n | built from the bitmap at close (needs ascending positions) |
| m13 per-symbol run directory | ~0.19 n | per-symbol sample spools, symbol-grouped |
| sparse tails m11 | ~0.005 n | bounded |

So the Pile arithmetic with this change is `container 0.632 n + scratch ~0.32 n`, against
`0.632 n + ~0.60 n` before. **m5's `n/8` bitmap is the last unbounded term** and it cannot
be appended in stream order: the sweep emits witnesses in RANK order and Elias-Fano needs
POSITION order, so the set must be materialised before it can be encoded. The two ways out
are (a) build with `--no-chi` (the container becomes 0.518 n and no bitmap exists), or
(b) partition witnesses by high bits into bounded append spools at emission and merge them
in ascending order at close - a real change to the witness stream, not a layout change.

## 8. Residual risks

1. `SCRATCH_BACKPRESSURE` peak is a 2 s sample; a burst between samples is invisible.
2. The pause is SIGSTOP on the producer's process group. A stop that lands while the
   producer holds the shared checkpoint journal's `flock` (only on a resumed build) would
   block a spool-mode finish checkpoint; the controller therefore resumes after a 30 s
   drain floor and raises its pause threshold, so it settles instead of wedging. This is
   a mitigation, not a proof.
3. The driver-level PRODUCER restart (Lane A) is still absent: a `xsa build` restart
   deletes `merged/` and replays from zero. G4 exercises the finish-side container-offset
   resume, which is what the container needs; whole-driver resume is unchanged.
4. m5/m13 remain symbol- or position-ordered members; only m1 is appended in stream order.
