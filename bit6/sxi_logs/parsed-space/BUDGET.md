# Full-pile projection: exceeds measured capacity

Measured input `/mnt/nvme2n1/erikg/pile.txt`: **1,307,910,802,540 bytes**.
All TB/GB in this report are decimal. Free-space bytes were measured before
the experiments; the input pile is already included in used space.

| Volume | Measured free TB |
|---|---:|
| nvme2n1 | 5.814 |
| nvme3n1 | 1.710 |
| home | 3.455 |
| Combined | 10.980 |

Calibration: cleaned 100,000,000-byte prefix, w10/p100, 926,333 distinct
phrases, 993,476 parse entries, D=108,415,194 bytes. Byte cleaning is documented
in `fixtures/pile-cleaning.json`. R calibration is the **separate retained**
100 MB web gate (R/n=0.38647349); this lane has not measured R for its cleaned
prefix. Constant ratios are capacity scenarios, not a proven model for unseen
data. Exact arithmetic is retained in `full-pile-budget.json`.

| Item | Projected capacity |
|---|---:|
| Dictionary | 1.418 TB |
| Parse, current four-byte format | 51.98 GB |
| Distinct phrase IDs | 12.116 billion; uint32 ABI **fails** |
| Parser bucket heads + sorted keys/IDs | <=339.24 GB, before small buffers/current phrase |
| Parser transient disk (phrases, 56-byte records, dictionary, hash/rank parses) | 3.658 TB |
| SA/LCP runs (4+2 bytes/suffix) | 8.508 TB |
| Dictionary + SA/LCP runs | 9.926 TB |
| Final BWT + both endpoint columns, using retained R/n | 10.109 TB |
| Dictionary + SA + final front-end outputs | **20.035 TB** plus parse/L2 |
| Same, including RLE and endpoint-copy overlap | **30.145 TB** plus parse/L2 |
| Existing slim resident run arrays, minimum | **15.227 TB RAM** |
| Mapped heads, possible additional resident pages | 4.044 TB RAM |

The parser metadata estimate is an allocation formula for the new store, not
a full-pile measured RSS bound. It excludes the unavoidable current phrase and
allocator overhead. The ABI rejects the projected phrase count before that
scale. The complete front-end peak also depends on L2 dictionaries, E/V tables,
and run metadata; no under-900-GB full-pile peak is established. Under the measured-ratio extrapolation, the existing
slim arrays make the whole-chain target fail by a large margin. Larger-scale
measurements could change these ratios; this is not a proven full-corpus R or
unique-phrase count.

## A feasible SA-only placement, and why it is not a full build plan

With a 1 GiB dictionary block, there are approximately 1,321 local SA runs
(phrase boundaries can increase the count). Each run is approximately 6 GiB;
the final partial run is smaller. Set `SXI_SCRATCH_DIRS` to the weighted roots
`nvme2:nvme2:nvme2:home:home:nvme3`, using **fresh owned directories**, not the mount roots.
This places approximately 4.254 TB of runs on nvme2, 2.836 TB on home, and 1.418 TB on nvme3.
Dictionary + parse on nvme2 leave only about 90 GB of its measured free space
before L2 and other scratch: placement must be rechecked, not assumed safe.
Run buffers total roughly 1.39 GB at 1 MiB/run; block text+SA+LCP workspace needs
about 9 GiB; configure an additional fixed dictionary read cache.

This deliberately stores local four-byte offsets with two-byte LCP hints and replays the
final merge. Exact LCP hints prevent repeated long-prefix scans in the
tournament; values above 65,535 are extended exactly against paged bytes.
A globally materialized six-byte SA would require another 8.508 TB. This lane
does not create that file. Even without it, the unchanged endpoint products
and copies exceed aggregate free space. Individual endpoint columns also
exceed the smaller volumes, and the existing writer does not stripe a column.
There is therefore **no placement plan that fits this full-pile projection on
the measured disks** for the complete unchanged chain. A feasible claim needs
a materially different measured R/U scenario or representation changes.

## Wall estimate from actual web parsing

The final external parser built the cleaned 100 MB prefix on nvme2n1 in
**22.91 s GNU time wall** (23.05 s driver/polling wall), peak **27,262,976 bytes
RSS**, under a **500,000,000-byte RLIMIT_AS**. Polling measured 304,918,528 bytes
of simultaneously allocated construction files. This includes disk metadata,
temporary hash parse, dictionary/rank outputs, and journal allocation, not the
input text. The filesystem may allocate ahead of logical file length.

That is 4.36 MB/s, or **83.2 hours for parsing alone** if rate and ratios remain
constant. The initial input-read/phrase-insertion portion took 10.37 s/100 MB;
the remaining time includes sorting, rank substitution, and dictionary output.
This is not a whole-build wall estimate: the current full-pile build is blocked
by ABI, RAM and disk, and random dictionary-page accesses during the external
merge have not been calibrated at TB scale. Cache-resident yeast timing must
not be extrapolated as a TB-scale external-I/O guarantee.

Process RSS excludes the kernel filesystem cache. Small calibration files fit
in host cache; physical I/O and sustained TB-scale spill can make the linear
wall extrapolation optimistic. No cgroup-wide full-pile RAM measurement exists.
