# Parse speedup lane report

## Result

The dictionary now has one hash-owned partition per `-j` thread, with a mutex,
pool resource, node storage, and phrase-byte storage per partition. A phrase is
stored once. The sorted phrase list contains references and still assigns the
same global lexicographic ranks. The existing AGC reader and byte remap files
were copied intact into this worktree. The supplied closed-syncmer patch was
integrated because the inherited fork did not yet contain that second trigger
scheme. The incremental diff against the pre-existing AGC/remap fork is
`partitioned-dict.patch`; `patch --dry-run -p1` succeeded against that fork.

The crucial source finding is that `ParserText::operator()` performs the
phrase loop on **one thread**. For plain `-t` input, `-j` previously affected
the AGC reader but did not create parser workers. Consequently the measured
dictionary lock contention on text is zero. Partition count can still affect
map growth and memory locality. No reader data was materialized by the parser;
it continues to consume source blocks once.

## Profile

`PFP_PROFILE=1` was used on 100,000,000-byte yeast and web slices with
`-j 48`. The legacy profile build has the original one-map dictionary plus
timers. Profiled output passed `cmp` against the uninstrumented baseline for
`.parse`, `.dict`, and `.remap`. Timers explicitly perform and time map growth
before the insert, so profile wall times differ from production wall times.
The table gives medians of three CPU-pinned runs, in milliseconds. Host load
caused appreciable variation, especially in the legacy yeast lookup counter.

| Slice / dictionary | Hash | Contended lock wait | Lookup | Insert | Dictionary rehash | Rank-map rehash | Output write calls | Sort and rank-map build | Rank probes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Yeast / legacy | 66.8 | 0 | 699.1 | 150.6 | 106.0 | 15.1 | 570.7 | 1166.3 | 669.3 |
| Yeast / partitioned | 59.7 | 0 | 302.8 | 180.9 | 44.8 | 18.6 | 447.1 | 663.9 | 131.5 |
| Web / legacy | 57.9 | 0 | 354.6 | 218.7 | 263.7 | 67.1 | 696.5 | 1446.0 | 310.3 |
| Web / partitioned | 58.4 | 0 | 332.5 | 258.6 | 41.2 | 61.7 | 665.0 | 1189.3 | 292.7 |

There were zero measured lock contentions in every profiled run. The rehash
columns are separate: dictionary growth is outside insertion timing; rank-map
growth is a subset of the reported sort/build time, so the table is not
additive. The 100 MB yeast profile had 580,331 new phrases in 1,016,320
probes (57.1% misses); web had 923,755 in 985,993 (93.7% misses). At the
full 3.337 GB yeast size, 3,159,456 of 34,134,006 phrases were new (9.3%
misses), making that larger run lookup-heavy. The full 8 GB web run had
74,300,789 new phrases in 82,486,748 probes (90.1% misses). Profile logs
are under `profile-pinned/`; the exact counter values and wall ranges are
retained there.

## Production throughput and memory

Inputs were 3,336,986,760 bytes for yeast and 8,388,474,069 bytes for web.
Each run used `-w 10 -p 100`. MB/s uses decimal 1,000,000-byte MB. Baseline
is the copied pre-change AGC/remap binary; all other rows are the final
partitioned implementation. Peak RSS is from `/usr/bin/time -v`.

| Corpus | Binary / `-j` | Wall | MB/s | Peak RSS |
|---|---|---:|---:|---:|
| Yeast | baseline / 48 | 49.81 s | 66.99 | 0.91 GiB |
| Yeast | partitioned / 1 | 54.69 s | 61.02 | 0.93 GiB |
| Yeast | partitioned / 16 | 89.17 s | 37.42 | 0.93 GiB |
| Yeast | partitioned / 48 | 52.53 s | 63.53 | 0.93 GiB |
| Web 8 GB | baseline / 48 | 452.27 s | 18.55 | 17.50 GiB |
| Web 8 GB | partitioned / 1 | 691.81 s | 12.13 | 17.63 GiB |
| Web 8 GB | partitioned / 16 | 668.43 s | 12.55 | 17.56 GiB |
| Web 8 GB | partitioned / 48 | 324.21 s | 25.87 | 17.97 GiB |

The matched web `-j 48` speedup is 1.395x. The unpinned yeast `-j 48`
comparison is 0.948x, a 5.5% slowdown. The anomalous yeast `-j 16` run was
repeated (102.09 s), and a controlled CPU-70 sequence gave baseline 62.53 s,
then partitioned 57.49 s (`-j 1`), 59.08 s (`-j 16`), and 60.80 s (`-j 48`).
This confirms substantial host/run variation and no reliable DNA speedup.
The CPU-pinned sequence and its byte comparisons are retained under
`yeast-pinned-*`. The full web phase logs show that rank assignment and
dictionary output also occupy substantial wall time; the parser is not simply
waiting on the dictionary lock.

## Exactness and tests

- Final binary, mod-`p` trigger: `.parse`, `.dict`, and `.remap` were byte
  identical to the baseline on both 100 MB slices at `-j 1`, `16`, and `48`
  (`ship-final/`). They also matched at full yeast scale for all three `-j`
  values and on the full 8 GB web slice for all three values.
- Closed-syncmer trigger: 10 MB yeast and web inputs produced byte-identical
  `.parse`, `.dict`, and `.remap` files at `-j 1`, `16`, and `48`
  (`syncmer-ship/`). The old binary had no syncmer option, so this is a
  cross-partition identity gate rather than an old-binary comparison.
- `ctest -R '^pfp_partition_test$'` passed. It inserts duplicate phrases
  from eight threads, compares sorted phrases against one partition, and
  checks a second sort after an insertion. The standalone closed-syncmer
  selector test passed.
- `tests/test_agc.py` passed on four reader counts (1, 16, 48, 96), including
  parse/dictionary/names identity and seek/separator checks.
- `git diff --check` passed. Neither the root worktree nor the nested fork
  has staged files. No process outside this lane was killed or changed.

## Projections and next decision

Scaling the supplied 7.25-hour 466 parse by the unpinned yeast matched
ratio gives **7.65 hours** at `-j 48`; scaling by the CPU-pinned ratio gives
**7.05 hours**. The supported conclusion is approximately **7.1–7.7 hours**,
with no dependable DNA gain. At the measured 25.87 MB/s web `-j 48` rate,
1.31 decimal TB would take **14.06 hours**, assuming rate stays constant.

That pile run is not feasible under a 900 GB RAM ceiling with this in-memory
dictionary. The 8 GB run held 8.885 GB of distinct phrase bytes; at the same
novelty rate, 1.31 TB would need about **1.39 TB of phrase bytes alone**.
Linear scaling of observed RSS gives about **3.0 TB**. Disk-backed partitions
are therefore warranted before a full pile parse. A shared read-mostly
prefilter is not justified by these profiles: the text parser has no
contended dictionary lock, and the larger DNA run did not speed up. Rank-map
construction and hash-to-rank conversion also deserve separate optimization
work, outside this dictionary change.

## Reproduction commands

The `run.log`, `time.log`, and output files in each named run directory retain
the exact CLI configuration and byte products. Representative commands:

```sh
cmake -S vendor/pfp-agc-fork -B vendor/pfp-agc-fork/build -DPFP_ENABLE_AGC=ON -DPFP_BUILD_PARTITION_TEST=ON
cmake --build vendor/pfp-agc-fork/build --target pfp++ pfp_partition_test pfp64 -j 8
ctest --test-dir vendor/pfp-agc-fork/build -R '^pfp_partition_test$' --output-on-failure
python3 vendor/pfp-agc-fork/tests/test_agc.py --pfp vendor/pfp-agc-fork/build/pfp++ --probe vendor/pfp-agc-fork/build/pfp_source_probe --agc /home/erikg/agc/bin/agc --agc2flat /home/erikg/sxgc/agc2flat/target/release/agc2flat
/usr/bin/time -v vendor/pfp-agc-fork/build/pfp++ -t /mnt/nvme2n1/erikg/sxgc-trend/slice8.txt -o bit6/sxi_logs/parse-speedup/final-web8-j48/parse -w 10 -p 100 -j 48 --tmp-dir bit6/sxi_logs/parse-speedup/final-web8-j48/tmp
patch --dry-run -p1 -d /home/erikg/sxgc/vendor/pfp-agc-fork < bit6/sxi_logs/parse-speedup/partitioned-dict.patch
```

The temporary profiling fork lives at `vendor/pfp-agc-fork-profile-legacy`.
It is evidence for phase timing only; the deliverable source diff is the
incremental patch and `vendor/pfp-agc-fork`.
