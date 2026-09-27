# Endpoint-tap source-build acceptance — G0/G1/G2 PASS; G3 incomplete

Workspace `/tmp/sxgc-laneV`, base `9af69c4`. No commits or staged files.
The minimal vendored tap and source pipeline are implemented; general
multi-string collection equivalence is **not** established. See
[SXI_BUILD.md](SXI_BUILD.md) for exact flags, build commands and limitations.

| Gate | Current result | Evidence |
|---|---|---|
| G0 source battery | PASS 7/7 | `sxi_logs/G0-final.log` |
| G1 additive tap | PASS | `sxi_logs/G1-equivalence.json` |
| G2 fresh yeast | PASS: chi 85,404,240; endpoints and witnesses equal | `sxi_logs/G2-endpoints.json`, `sxi_logs/G2-yeast.log` |
| G3 fresh k10 / RAM slice | Launched, incomplete | `sxi_logs/G3-launch.json`, `sxi_logs/G3-k10.log` |

G0 builds each text from scratch through `xsa build --text`. All aggregate
bytes match the committed baselines. Entire normalized `.ri4` files and
raw normalized heads also match. Mandatory duplicates-600k is included.
The other six are random-4-2k, random-4-20k, random-bin-20k,
random-4-200k, satellite-18k and HOR-nested.

G1 uses the duplicates-600k parse with both unpatched and patched rpfbwt.
`.ssa` and RLE bytes are identical. Raw R=14,703 and `.ssa_t` is exactly
117,632 bytes: count plus R u64 values. The tiny independent cyclic-SA
oracle tests both singleton and repetitive runs at 1, 7 and 50 chunks;
all six combinations preserve upstream head bytes and emit correct tails.

The two repetitive G0 fixtures exposed the upstream empty-chunk `.ssa`
merge failure. The pipeline uses one chunk for inputs below 1 MB; large
inputs use 50. No `.ssa` code was changed. Failed first attempts remain in
the logs; the final source battery passes all seven with explicit endpoint
gates. This is a configuration workaround, not a fix to upstream's head
merge. Large low-complexity inputs could still fail closed on that bug.

Yeast fresh endpoints have raw n=3,336,986,770 and R=100,904,883. Removing
ten padding rows gives n=3,336,986,760 and R=100,904,881. Fresh heads are
byte-equal to `/tmp/laneS/yp2.phi.head_sa` (807,239,048 bytes). The entire
fresh `.ri4` is byte-equal to `/tmp/laneQ/pass3/yp2real.ri4` (908,146,022
bytes), proving run-table and packed-tail equality, including the
`n-1-SA` mirrored convention. Neither oracle supplied construction data.

## Collection ordering limitation

The requested k10 input has 865 strings. Its oracle's first run head is
SA=242,892,443. Linear PFP over the newline-joined input should put the
final newline suffix first, at SA=30,151,407,544. This is a preflight
inference recorded in `sxi_logs/k10-convention-preflight.json`, not a fresh
G3 result. Matching newline separators alone does not prove equivalence
to BCR's independent-string ordering. The tap and padding adapter do not
reorder suffixes. The actual fresh k10 byte gates are mandatory.

Multi-string builds therefore require both explicit endpoint byte gates;
without them, publication fails closed. General oracle-free multi-contig
AGC construction remains unavailable. A tiny single-contig AGC source
build passes through the committed `agc2flat --revlines --upper` path,
including embedded names (`sxi_logs/agc-source-build.log`). No broader AGC
collection acceptance is claimed.

## Failure, format and scope checks

- `sxi_logs/source-cli-test.log`: missing executable, stage failure, wrong
  chi, wrong head oracle and unvalidated multi-string ordering cannot
  publish final SXI; existing files and dangling symlinks are preserved.
- `sxi_logs/tap-oracle-test.log`: independent tiny suffix-array oracle;
  test-only text/SA enumeration, never used in production construction.
- `sxi_logs/format-regression.log`: packed widths 2–64, malformed members,
  singleton/duplicate checks and atomic publication regression pass.
- `sxi_logs/unchanged-components.json`: legacy Phi extractor, SXI writer,
  format and slim consumer are byte-unchanged from the base.
- The tap lives only in `/tmp/rpfbwt-sxgc`; built binary is
  `/tmp/rpfbwt-sxgc/build-sxgc/rpfbwt`. Upstream tracked files are unchanged.
- No M/b_bwt/w_wt artifacts are emitted by the new downstream path; its
  endpoints are direct. rpfbwt retains its pre-existing internal L2 structures.
- No pfp466 process was signaled and no k466 files were read or written.
  No process has been killed by this lane. `contact_supervisor` is absent
  from the available tool catalog. No alternate messaging channel was used.

## Per-stage wall/RSS

Every stage is wrapped separately by `/usr/bin/time -v`; the JSONL journals
contain exact commands and stage return codes. RSS is KiB. K10 is capped
at 139,000,000,000 bytes of address space while the remaining yeast stream
runs. `sxi_logs/lane-observed-rss.jsonl` samples only this lane's process trees.
These samples are observational, not substitutes for final stage peaks.

The tables below are measured from the fresh-source runs. K10 remains
incomplete; pending stages have no final peak RSS measurement. No 466
extrapolation or completed k10 slice is claimed.

Independent reviewer approval remains required. This report records
checks and limitations; it does not self-approve acceptance.

## Fresh yeast completed

Artifact: `/mnt/nvme3n1/erikg/sxgc-laneV/yeast.sxi`. Embedded chi is **85,404,240**. Fresh aggregate
bytes and sweep witness bytes also match the committed Phi-chain outputs;
see `sxi_logs/G2-downstream-byte-gates.log`. Slim reports zero LF walks/steps.

| Stage | Wall seconds | Peak RSS KiB | Result |
|---|---:|---:|---|
| parse | 79.299 | 952,448 | 0 |
| parse-l2 | 4.093 | 263,304 | 0 |
| rpfbwt | 624.236 | 8,821,236 | 0 |
| endpoints | 24.622 | 2,048 | 0 |
| slim | 832.835 | 3,743,332 | 0 |
| sweep | 14.862 | 6,144 | 0 |
| write | 23.234 | 669,696 | 0 |
| validate | 7.777 | 2,048 | 0 |

| SXI member | Stored bytes |
|---|---:|
| RLBWT + C | 504,526,453 |
| Packed tails | 403,619,537 |
| Raw heads | 807,239,048 |
| Anchors | 12 |
| Delta chi | 88,746,976 |
| Whole container | 1,804,132,304 |

## K10 RAM profile — incomplete

Launch PID: `4170892`; journal: `sxi_logs/k10-xsa-build-vnapn3jx.jsonl`.
Address-space ceiling is 139 GB; this is not an RSS measurement.
The raw input is `/mnt/nvme3n1/erikg/sxgc-pilot/k10/h10_rl.txt`.
No k10 SXI or successful endpoint/chi gate is claimed.

| Stage | Wall seconds | Peak RSS KiB | Status |
|---|---:|---:|---|
| parse | 850.963 | 10,199,376 | exit 0 |
| parse-l2 | 57.656 | 2,714,380 | exit 0 |
| rpfbwt | pending | pending | running |
| endpoints | pending | pending | not started |
| gate-heads | pending | pending | not started |
| gate-runs-tails | pending | pending | not started |
| slim | pending | pending | not started |
| sweep | pending | pending | not started |
| write | pending | pending | not started |
| validate | pending | pending | not started |

Observed combined lane RSS peak at report time: 29,556,180 KiB. This is a sampled observation; final per-stage `/usr/bin/time -v` values
above remain authoritative for completed stages. The running k10 logs
continue to update after this report snapshot.

Snapshot UTC: 2026-09-27T11:40:14.377076+00:00.
