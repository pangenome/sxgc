# prefetch part 2: cache hygiene, the sweep read discipline, and the balance curve

Branch `prefetch2`, base pile-week-v1 @ `e3edef0`.

The charter's four questions, in order: (A) does telling the kernel what is dead
and what is coming save the corpus's page cache; (B) does a sorted, coalesced,
monotone sweep read beat the demand path; (C) what does the finish wall look like
against pipeline depth and staging size once the corpus is COLD; and what do the
fixture banks say (byte identity) while all of it is switched on.

**The box was shared for every number below.** The operator's live 10 GB A/B
(base and pf arms) ran throughout; the load average is printed on every line that
has a wall in it. Absolute seconds are therefore comparative only, and every arm
set is INTERLEAVED so a drifting load hits all arms of a set equally.

---

## 0. What was already there, and what was not (read this first)

The base commit `e3edef0` carries parts 1-3 of the prefetch work, but two of the
pieces the charter asks about were present only in the *summary*, not in the code:

* **`PfPrefetcher` was dead code.** The struct exists in `bit6/pile_finish.cpp`,
  complete with a comment describing the band prefetch, but nothing instantiated
  it and no `PILE_FINISH_PREFETCH*` variable was read anywhere in the file
  (`grep -n PREFETCH bit6/pile_finish.cpp` at `e3edef0` returns comments only).
  The live `pre-1b-pf12` A/B that the part-1 report quotes was measured with a
  binary (`/mnt/nvme2n1/erikg/pre-build/pile_finish-pf`) built from a source that
  is not in this repository - no ref, worktree or commit contains it
  (`git log --all -S"raw_windows"` is empty). Part 2 therefore WIRES the
  prefetcher instead of tuning it.
* **The producer refused its own default.** `binbwt_main` required
  `depthCap >= 1` while PART 4 had made `--bin-depth-cap` default to 0 (AUTO), so
  every default-shaped producer run died with `CROSS_LCP_FATAL bin-bwt
  parameters`. Fixed at `bit6/binbwt_slice.h:2859` (the check now rejects only a
  bad `--bin-level-bytes`; AUTO is the documented product default).

---

## 1. PART A: cache hygiene

### 1.1 What is released, and when

| scratch | where | mechanism | why there |
|---|---|---|---|
| window file, after its bins are built (`erase`) | `bit6/binbwt_slice.h:824` | `posix_fadvise(DONTNEED)` on the open fd, then unlink | the bytes are dead; at pile scale the window files are tens of GB competing with the corpus |
| window file, at `seal` (OPT-IN `BIN_WINDOW_DONTNEED_SEAL=1`) | `bit6/binbwt_slice.h:820` | same | the charter's "after each window seals" - a KNOB, because the same file is READ BACK by the build in this process a moment later |
| witness spool chunk, after its bucket is merged | `bit6/pile_finish.cpp` `WitBuckets::releaseChunk` | `posix_fadvise(DONTNEED)` per completed bucket + the intermediate merge passes | the dropped witness stream is scratch that otherwise squats for the whole m5 phase; counted (`cache_release_calls`, `cache_released_bytes` on `PILE_FINISH_WITNESS_PATH`) |
| rlebwt, after m1 replayed it | `bit6/pile_finish.cpp` (was already here, part 1) | `posix_fadvise(DONTNEED)` | |
| corpus, before a cold arm | `bit6/pile_finish.cpp` `CorpusText::cold_all` | `madvise(MADV_DONTNEED)` on OUR mapping, THEN `posix_fadvise(DONTNEED)` | see 1.3: fadvise alone is a no-op on mapped pages |

### 1.2 WILLNEED: the kernel-side assist

`CorpusText::willneed_range` (`bit6/pile_finish.cpp:534`) issues
`POSIX_FADV_WILLNEED` over the SAME merged ranges the populate front uses, so the
read-ahead can be split: `MADV_POPULATE_READ` blocks the prefetching thread until
the pages are resident (one thread = one queuer of device reads), while WILLNEED
handed to the kernel lets it sort and overlap the reads with the workers' demand
faults. Knob: `PILE_FINISH_PREFETCH_ASSIST=populate|willneed|both` (default
`populate`, i.e. the part-1 behaviour). Counters `willneed_ranges`,
`willneed_bytes`, `willneed_failed` (advisory: a refusal is counted, never fatal)
ride on every `PILE_FINISH_CACHE_RESIDENCY` line and in `PILE_FINISH_PREFETCH`.

### 1.3 The cold-corpus drop does not work the obvious way

`posix_fadvise(DONTNEED)` **skips pages that are mapped into a page table**, and
the finish maps the corpus. Measured: the first cold arm reported
`label=cold-start resident_pages=3815 resident_pct=100.00` right after the drop -
the "cold" arm was warm. `cold_all()` therefore does `madvise(MADV_DONTNEED)` on
the adapter's own mapping first (zaps the PTEs, drops the clean MAP_SHARED pages)
and only then fadvise. After the fix, verbatim:

```
PILE_FINISH_COLD_CORPUS enabled=1 every_bands=0 drops=1 bytes=1000000665 wall=0.127 corpus_gb=1.000
PILE_FINISH_CACHE_RESIDENCY label=cold-start sampled_pages=3815 resident_pages=0 resident_pct=0.00 ...
PILE_FINISH_CACHE_RESIDENCY label=band-1     sampled_pages=3815 resident_pages=1882 resident_pct=49.37 ...
PILE_FINISH_CACHE_RESIDENCY label=band-65    sampled_pages=3815 resident_pages=3815 resident_pct=100.00 ...
```

Two consequences worth recording:

* a SECOND process that maps the same file defeats the drop completely (measured:
  an orphaned arm held the corpus mapped and every later `cold-start` line said
  100%). In the product there is exactly one finish process, so this is a
  measurement-environment hazard, not a product behaviour - but every arm must
  check the line before believing the label.
* **at 1 GB the corpus rewarms within ~64 bands** (49% after band 1, 100% by band
  65), which is the whole reason PART C needs a continuously-cold model and not
  just a cold start.

### 1.4 Residency journal (sampled, never assumed)

`PILE_FINISH_CACHE_RESIDENCY` (`bit6/pile_finish.cpp:700`) reports, per phase:
sampled/resident pages of the adapter's own mapping (mincore, coarse stride),
resident GB, the kernel's own `Cached/Dirty/MemFree/Shmem`, the populate and
willneed counters, and the cold drops. Printed ALWAYS at the end of the finish;
per phase only with `PILE_FINISH_CACHE_JOURNAL=1` (a stride-1024-pages walk is
~5 ms at 1 GB, and the phase set runs ~10 times; the per-band lines are gated).

### 1.5 O_DIRECT for the window fill: measured, and it loses badly

`tools/binbwt/scratch_io_probe.c write` - 4 GiB into a preallocated file,
256 interleaved zones, 128 KiB writes (the fill's exact shape). One run, box at
load 262-288 (a live ladder run and the operator's arms):

```
WRITE arm=buffered            bytes=4294967296 wall=0.769 gb_s=5.59  load1=287.9
WRITE_SYNC arm=buffered       wall=1.075
WRITE arm=odirect             bytes=4294967296 wall=3.601 gb_s=1.19  load1=282.8
WRITE_SYNC arm=odirect        wall=0.000
WRITE arm=buffered_dontneed   bytes=4294967296 wall=0.623 gb_s=6.89  load1=282.8
WRITE_SYNC arm=buffered_dontneed wall=1.105
```

O_DIRECT is **4.7x slower** than buffered pwrite in the fill's shape (the fill's
staging is water-filled over 256 interleaved bin streams, so its writes are
128 KiB but not 512-byte aligned per stream; O_DIRECT also pays an fsync-shaped
wait it cannot overlap). Per the charter's own escape clause: **O_DIRECT is
rejected and DONTNEED-only is kept.** The same probe shows the hygiene is not a
tax: buffered+DONTNEED is the FASTEST arm (6.89 GB/s), because releasing the dead
chunks keeps the dirty-page backlog small.

---

## 2. PART B: the sweep read discipline

### 2.1 The machinery

`PfPrefetcher` (`bit6/pile_finish.cpp:2518`) now has two arms and one discipline:

```
PILE_FINISH_PREFETCH=0        lazy (product default; no read-ahead at all)
PILE_FINISH_PREFETCH=1|sweep  sort the band's targets, COALESCE windows whose gap
                              is under PILE_PREFETCH_GAP into merged reads, split
                              each merged read into <= CHUNK pieces, and issue the
                              pieces in ONE ASCENDING MONOTONE FRONT
PILE_FINISH_PREFETCH=2|scattered  the same targets in ARRIVAL order, no sort, no
                              coalesce, one read per window (the control arm)
```

with `PILE_FINISH_PREFETCH_JOBS` (pipeline depth, default 2),
`PILE_FINISH_PREFETCH_BUDGET` (bytes of MERGED front one job may stage, default
32 MiB), `PILE_FINISH_PREFETCH_CHUNK` (default 4 MiB),
`PILE_FINISH_PREFETCH_SKIP_RESIDENT` (sweep only, default 1) and
`PILE_FINISH_PREFETCH_ASSIST` (part A).

Two design corrections that the measurements forced (both are in the history):

* **The coalescing happens on the prefetch thread, not the fold thread.** Sorting
  one band's ~262k window pairs is 15-25 ms; at 5616 bands that is 84-140 s of
  SERIAL time on the fold. Measured with the sort in `submit()`: lazy 137.5 s vs
  sweep 196.6 s - a 43% regression that was pure critical-path cost. The fold
  thread now pays two pushes per raw run and nothing else.
* **The sweep skips the pages the kernel already has** (`page_residency` ->
  `mincore`, `bit6/pile_finish.cpp:560`). Without it a band's front re-reads the
  whole corpus once per band (touched mass 188 passes at 1 GB) even though the
  workers' demand faults had already made most of it resident. With it, the same
  run fetched **8.2 MB of 187.5 GB requested** (`fetched_mass_pct=0.8160`): the
  read-ahead becomes idempotent and its device cost is the corpus's COLD mass.
  The scattered arm does NOT consult residency - at 256 B per window that is one
  syscall per window, measured at 132 s of `mincore` time.

### 2.2 Counters (verbatim, 1 GB sweep arm, cold start)

```
PILE_FINISH_PREFETCH_POLICY mode=1 (sweep) assist=0 (populate) skip_resident=1 window_bytes=256 budget_bytes=33554432 gap_bytes=65536 chunk_bytes=4194304 jobs=2 source=next_band_targets
PILE_FINISH_PREFETCH jobs=5605 raw_windows=734631424 merged_reads=814276 coalesce_ratio=0.001 fronts=813615 reads=814276 avg_read_bytes=230266.5 bytes=187500520163 touched_bytes=187500520163 fetched_bytes=8159933 fetched_mass_pct=0.8160 skip_resident_reads=814021 skip_resident_bytes=187403268902 residency_unknown=0 corpus_bytes=1000000665 touched_mass_pct=18750.0395 pages_populated=1992 skipped=11 dropped_bytes=0 busy_s=79.440 willneed_s=0.000 mincore_s=2.658
```

`raw_windows -> merged_reads` = 734,631,424 -> 814,276 (one read per 902 windows,
average 230 KB); the touched mass per band is bounded by the 32 MiB staging
budget; the fronts (813,615) say the band's mass is ~145 disjoint ascending
clusters at 1 GB.

### 2.3 The thing that matters: read shape on this device

`tools/binbwt/scratch_io_probe.c read`, 2 GiB file, cold before every arm:

```
READ arm=sequential_1m            bytes=2147483648 wall=1.145  gb_s=1.88 load1=287.5
READ arm=scattered_4k_random      bytes=2147483648 wall=41.694 gb_s=0.05 load1=273.4
READ arm=scattered_4k_ascending   bytes=2147483648 wall=1.155  gb_s=1.86 load1=276.3
```

The operator's 5.24 GB/s sequential figure is not reproducible on this box while
it carries 200-300 load (we measure 1.9 GB/s sequential); what IS reproducible is
the RATIO the discipline rests on: **scattered 4 KiB random reads are 38x slower
than the same bytes in ascending order** (0.05 vs 1.86 GB/s). A sorted monotone
front is what turns a scattered SA walk into that ascending arm.

### 2.4 The producer's leaf staging gets the same discipline

`bin_stage_leaf` (`bit6/binbwt_slice.h:246`) used to read ONE densest span per
leaf (part 1); it now opens up to `BIN_LEAF_STAGE_SEGS` (default 4) coalesced
spans that share ONE staging buffer, chosen by the densest-cluster two-pointer
over the not-yet-covered starts and read in ascending order - the staging buffer
doubles as the reorder buffer (`BinLeafStage::Seg{lo,hi,off}`, `covers()/at()`
with a last-segment hint). `BIN_LEAF_STAGE_SEGS=1` is byte-for-byte the part-1
behaviour. New counters on `BIN_WORK`: `stage_raw_windows`, `stage_merged_reads`,
`stage_avg_read_bytes`, `stage_segs_max`. The counters the ORDER depends on
(comparisons, symbols, cert_decided, ties, leaf_sorts, leaf_elems, level_passes,
level_elems, bin_passes, emitted_rows, depth_max, periodic_rows) are untouched by
construction and the oracle's STAGE pass proves it (section 5).

---

## 3. PART C: the cold-corpus three-way at 1 GB

Fixture: `extcols-scratch/mergperf-1b-snap.txt` (1,000,000,665 B), 96 threads,
ONE pristine producer output (5 windows, 8.0 GB of window files,`BIN_WORK`
counters identical to the banked run: comparisons=21,184,557,983,
symbols=223,383,555,397), hard-linked per arm so every arm consumes the identical
piece wire. Every arm: corpus AND its own scratch dropped from the page cache
before it starts; `chi=283296933` and `sha256=38f03eb4...144f` (== the 1 GB bank)
on EVERY arm (12/12). The wall is `PILE_FINISH_PHASE sweep` (the band loop), and
each line carries the box load it ran under - the box carried the operator's live
10 GB A/B the whole time.

### 3.1 Cold START only (`PILE_FINISH_COLD_CORPUS=1`, one drop before the sweep)

| rep | lazy (mode 0) | batch-scattered (mode 2) | batch-sweep (mode 1) | load |
|---|---|---|---|---|
| 1 | **139.0 s** | 121.1 s | 144.2 s | 238-266 |
| 2 | **131.2 s** | 138.4 s | 138.0 s | 235-295 |

### 3.2 Continuously cold (`PILE_FINISH_COLD_EVERY=64`, 88 drops = 88 GB of forced
corpus eviction during the run - the pile regime at fixture scale)

| rep | lazy (mode 0) | batch-scattered (mode 2) | batch-sweep (mode 1) | load |
|---|---|---|---|---|
| 1 | **167.6 s** | 173.0 s | 197.9 s | 241-295 |

### 3.3 Why it is flat, in numbers

| arm (e64) | device reads (readband + advance) | prefetch fetched | prefetch CPU | sweep wall |
|---|---|---|---|---|
| lazy | 10.6 + 7.7 = 18.3 GB | - | - | 167.6 s |
| scattered | 6.0 + 5.2 = 11.2 GB | 32.1 GB requested | 171 s busy | 173.0 s |
| sweep | 10.7 + 8.0 = 18.7 GB | 0.74 GB (74% of one corpus) | 106 s busy | 197.9 s |

The prefetcher does not REDUCE the device traffic, because **the demand path is
already a monotone sweep**: the producer publishes runs in SA order, so a band's
corpus reads arrive with ascending head coordinates and the LCE walks long spans
from them. `max_seam_lce` is 97060 at 1 GB and 699594 at 10 GB - the reads are
large and ordered, not the scattered 4 KiB windows PART B's premise assumes.
What the prefetcher adds is CPU (the coalescing of ~262k windows per band) and
populate contention, and at 1 GB nothing is cold long enough for that to pay:
the corpus re-warms inside ~64 bands (section 1.3). **The 1 GB fixture cannot
show the hierarchy tax, and the knob sweep cannot either - the wall tracks the
box load, not the knobs.** Reported as a negative result; the pile regime is
measured at 10 GB (section 4).

## 4. PART C (operator's steer): the 10 GB hygiene A/B

Arms: `tools/binbwt/pf2_hygiene10g.sh hyg0|hyg1` - the live 10 GB shape (producer
and finish CONCURRENT, streaming wire, 96 threads, auto bin cap under a 60 GiB
phase ulimit), prefetch sweep ON, cold-corpus drop before the sweep, residency
journal on; the ONLY difference is `PILE_FINISH_HYGIENE` +
`BIN_WINDOW_DONTNEED` (0 = every dead scratch page stays in the page cache,
1 = released at erase/merge). Logs: `/mnt/nvme2n1/erikg/pf2-10g-hyg{0,1}/`.

**Both arms are producer-paced on this box, so their wall is a
contention-number, not a finish number.** The supervisor killed the operator's
`pre-10g-base` / `pre-10g-pf` arms mid-run (superseded binaries + four concurrent
10 GB finishes thrashing the same device); while my arm ran, the box carried the
ladder's 12h-deep finish, 17 other `pile_finish` processes at one point, and a
load average of 240-360. The finish's own `lce_wait` (time waiting for the
producer's bands) is 95-99% of its LCE span.

### 4.1 The residency readout (the point of the A/B)

`hyg0` (hygiene OFF), band samples from `PILE_FINISH_CACHE_RESIDENCY` (stride
1024 pages = 4 MiB, 2385 samples per line over the 10 GB corpus):

| band | 1 | 65 | 129 | 193 | 257 | 321 | 385 | 449 | 513 | 577 | 641 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| corpus resident % | 97.95 | 99.79 | 99.96 | 99.96 | 99.83 | 99.83 | **85.20** | 99.54 | 100 | 100 | 100 |

and `meminfo_cached_gb` never dropped below ~62 GB while the arm's own window
files were 42 GB of live scratch.

**Honest reading: at 10 GB on a 1 TB box the corpus is NEVER squeezed**, hygiene
or not. 10 GB of read-only text against 60-90 GB of page cache is not the pile's
regime; the one dip (85% at band 385) is the window-file writeback of the
concurrent producer, and it recovers by itself. The hygiene machinery is correct
and byte-neutral (section 5), but this fixture cannot price it either - the
condition it exists for is `corpus_bytes >> pool_bytes`, i.e. the 1.31 TB pile
(corpus cache there is the 17.2 GB pool budget, not the corpus).

### 4.2 The cold-corpus precondition FAILS at 10 GB, and why

At 10 GB the corpus adapter is WINDOWED (`mmap=0`, 4 x 8 GB regions), and three
other processes (the ladder's finish, the two pre-10g arms before the supervisor
killed them) have the same file mapped. Measured: `label=cold-start
resident_pct=99.87` right after the drop. `MADV_DONTNEED` can only drop clean
MAP_SHARED pages that no other address space maps; with a second process holding
the file mapped, the drop is silently a no-op. The 1 GB arms (no other process
mapped that fixture) DID go cold - `resident_pct=0.00` - which is why their table
is the clean one.

### 4.3 Recommendation for the supervisor

Schedule ONE clean 10 GB arm (`hyg0` alone, nothing else on the device, after the
ladder lands) and then `hyg1`; with no competing mapper the cold-corpus drop will
take, and with the box quiet the finish's wall will be the finish's wall. The
harness is ready and needs no code change:

```
nohup setsid bash tools/binbwt/pf2_hygiene_ab.sh 96 > /mnt/nvme2n1/erikg/pf2-logs/hygiene-ab-clean.log 2>&1 &
```

Better still at the pile's own scale: the 1.31 TB corpus with `--memory-gb 100`,
where the corpus cache is 17.2 GB against a 1.31 TB text - THAT is where the
hygiene term is a first-order number.

## 5. GATES

*(verbatim, section filled at the end)*
