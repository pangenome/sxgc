# Run-index u64 qualification

Base `83cb20f`; evidence: `/mnt/nvme2n1/erikg/wc-u64`.
See [the field-by-field audit](RUN_INDEX_U64_AUDIT.md) for changed and retained widths.
Native finish and packaged finish sources are identical. New m1/m13 codecs
are 114/115; m12 stays 112. Chi defaults on; `--no-chi` remains available.

| Fixture | Chi | Bitmap bytes | Published chi bytes | Bits/entry | Finish peak RSS KiB | m1 bytes | m12 bytes | m13 bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| g8 | 6544643 | 1000001 | 1818105 | 2.222404 | 79272 | 9074776 | 63280 | 37514840 |
| g150 | 44034230 | 18750001 | 20383582 | 3.703225 | 276484 | 53392304 | 454560 | 44212472 |
| g10 | 2676929445 | 1250002158 | 1294233465 | 3.867815 | 13486528 | 3254873328 | 27571832 | 2400697096 |

G3: 10 GB finish peak is 13486528 KiB versus the recorded
no-chi baseline 11781604 KiB, ratio **1.144711**.
This is below 1.5x (17672406 KiB), so proceed with
the requested 100 GB launch at `--threads 96 --memory-gb 96 --timeout 68000`.
The qualification uses the previously validated 32-thread chi configuration;
the baseline is the existing no-chi measurement, not a new controlled A/B.

Validation:

- Chi is exactly 6,544,643 / 44,034,230 / 2,676,929,445. Every m5 is byte-identical
  to the bitmap qualification baseline, including dimensions, CRC and SHA-256.
- 150 MB: 100/100 counts and 574/574 positions exact versus
  `tools/binbwt/oracle.tsv`; both new and legacy containers pass the new reader.
- The deterministic 8 MB chi fixture is random data, not the oracle corpus.
  Its 100 oracle patterns are independently rescanned: all counts/position sets
  match (zero occurrences). An additional 8 MB text-prefix build matches all
  100 clipped oracle count/position sets, including all 24 occurrences;
  the clipped oracle was independently checked against the source bytes.
- 10 GB: 20/20 low-count patterns have exact counts and positions versus
  `/tmp/g10/oracle.tsv`; see `query10.log`.
- Packaged default-chi build passes the 8 MB expected chi without `--with-chi`.
  Explicit `--no-chi` omits m5 with correct flags; every other member is identical.
  The final package logs `XSA_FINISH_PEAK_RSS` into `xsa-build.log`. The entire
  native and packaged 8 MB containers are byte-identical.
- Forty production Rust tests pass, including >2^32 run/rank indices, offsets
  >=32 GiB and legacy decoding. The already documented baseline remap test is
  excluded. Bitmap, packed-bit and sweep-state native self-tests pass.

Residual risks: the 100 GB build is a launch, not a completed-product gate.
The required 96-thread launch differs from the 32-thread 10 GB qualification;
the existing sorter has previously refused an oversized completed band at
96 threads. This change does not weaken that assertion. Old readers reject
new codec 114. Query columns require 13 bytes/run plus the row directory;
construction RSS is not a query-memory guarantee. Run lengths retain their
existing checked u32 encoding; all run/position/rank indices are u64.

The raw logs, commands, `/usr/bin/time -v` reports, checksums and query outputs
are retained under the evidence root. Launch metadata and the first 120 seconds
of journals are written there after commit/push/merge, without waiting for the
100 GB build to complete.

Verbatim finish journal measurements:

```text
XSA_FINISH_CHI_PUBLISHED entries=6544643 bytes=1818105 bits_per_entry=2.222404
XSA_FINISH_M1_BLOCKS codec=114 runs=7966303 block_runs=1024 blocks=7780 dir_bytes=124480 payload_bytes=8947960 m1_bytes=9074776
XSA_FINISH_M12_RUNSTARTS stride=1024 samples=7780 l2_stride=65536 l2_entries=122 bytes=63280
XSA_FINISH_M13_RUNDIR occ_stride=256 gap=1024 entries=1560797 l2_stride=512 l2_entries=3221 bytes=37514840
XSA_FINISH_PEAK_RSS kib=79272
XSA_FINISH_CHI_PUBLISHED entries=44034230 bytes=20383582 bits_per_entry=3.703225
XSA_FINISH_M1_BLOCKS codec=114 runs=57279194 block_runs=1024 blocks=55937 dir_bytes=894992 payload_bytes=52494976 m1_bytes=53392304
XSA_FINISH_M12_RUNSTARTS stride=1024 samples=55937 l2_stride=65536 l2_entries=875 bytes=454560
XSA_FINISH_M13_RUNDIR occ_stride=256 gap=1024 entries=1839535 l2_stride=512 l2_entries=3716 bytes=44212472
XSA_FINISH_PEAK_RSS kib=276484
XSA_FINISH_CHI_PUBLISHED entries=2676929445 bytes=1294233465 bits_per_entry=3.867815
XSA_FINISH_M1_BLOCKS codec=114 runs=3474889847 block_runs=1024 blocks=3393448 dir_bytes=54295168 payload_bytes=3200575824 m1_bytes=3254873328
XSA_FINISH_M12_RUNSTARTS stride=1024 samples=3393448 l2_stride=65536 l2_entries=53023 bytes=27571832
XSA_FINISH_M13_RUNDIR occ_stride=256 gap=1024 entries=99898713 l2_stride=512 l2_entries=195238 bytes=2400697096
XSA_FINISH_PEAK_RSS kib=13486528
```
