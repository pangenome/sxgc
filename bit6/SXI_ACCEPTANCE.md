# SXI final UX revision — INCOMPLETE / source construction BLOCKED

Workspace `/tmp/sxgc-laneU`, base `6330f6d`; no commits. The requested
one-command source build is **not delivered**. This revision keeps the
working container/consumer changes from the stopped lane, adds a fail-closed
Rust source CLI boundary, and corrects gate labels. The earlier fixture
conversion result is not G0 under the final task definition.

## Required gates

| Gate | Result | Evidence |
|---|---|---|
| G0: seven `xsa build --text` runs | BLOCKED, 0/7 built | `sxi_logs/ux-source-gates.jsonl`; all seven attempts exit without output |
| G1: yeast born from source, chi 85,404,240 and witnesses | BLOCKED | Same log; input `/mnt/nvme3n1/erikg/sxgc-yeast/grl/yeast_pfp2.txt` |
| G2: k10 source build and RAM profile | BLOCKED | Same log; input `/mnt/nvme3n1/erikg/sxgc-pilot/k10/h10_rl.txt` |
| G3: fresh k10 heads vs `h10.head_sa` | NOT RUN | No fresh heads; oracle not substituted as construction input |

The blocker is a producer/algorithm dependency, **not a wall-clock timeout**.
No front-end process was launched and no new k10 RAM profile exists.

The unchanged installed r-pfbwt writes heads only; the proposed post-step
assumes tails. The local Phi predecessor construction requires heads to
define its interval starts. Lean `headFromTail` proves inversion given a
sound Phi table; it does not construct that table from tails. The producer's
single-string padding convention also differs from the requested k10
collection oracle. Exact source locations and the required contract repair
are in [SXI_CONSTRUCTION_BLOCKER.md](SXI_CONSTRUCTION_BLOCKER.md). This is
not an impossibility claim about every possible r-space algorithm.

The requested `contact_supervisor` tool is absent from the tool catalog.
The frontend-untouched instruction was preserved rather than overriding it
with the older ledger's endpoint-emission proposal.

## Implemented and checked

- `xsa/src/build.rs` accepts exactly one `--agc`, `--fasta`, or `--text`
  source, `-o`/`--output`, and `--verbose`. It checks paths and fails before
  any stage launch or output creation. **This is not a functioning build
  orchestrator.** There is no successful internal chi gate to claim.
- Usage lists SXI1 and continued pilot-era `.ri4` support. `stats --sxi`
  now reports embedded chi without requiring an external `.sA`, member
  sizes, compression ratio and actual container size.
- Preserved C++ writer (`sxi_write.cpp`), SXI1 v1 checked member format,
  Rust loader, native slim loader, embedded endpoints/anchors/names and
  delta chi. The writer remains an endpoint-input container writer.
- Preserved pilot Phi inverse extractor and its explicit rejection of
  `--from-front-end`; the requested new constructor is still absent.
- Added `tools/build_sxi_tools.sh` to compile writer, pilot extractor, slim
  consumer and Rust executable using the existing slim build script.
  No native-library integration was introduced.

| Check | Result | Log |
|---|---|---|
| Release C++/Rust tool builds | PASS | `ux-build-tools.log` |
| Fail-closed CLI, all modes, invalid args, existing output and dangling symlink | PASS | `ux-cli-contract.log` |
| Packed widths 2–64, checksums, malformed members, atomic/no-clobber failure | PASS | `ux-format-adversarial.log` |
| Tiny pilot Phi inversions | PASS, four fixtures | `ux-phi-contract.log` |
| Format differential | PASS, 7/7; **not G0** | `ux-format-differential.log` |
| Revised stats on existing yeast conversion | PASS; **not G1** | `ux-yeast-conversion-stats.log` |

All logs above are in `bit6/sxi_logs`. Format differential fixtures are
duplicates-600k, random-4-2k, random-4-20k, random-bin-20k, random-4-200k,
satellite-18k and HOR-nested. Aggregate bytes, streamed/resident sweep bytes,
exact queries and matching statistics match the `.ri4` route; sorted chi
round-trips preserve witnesses. Those fixtures consume inherited endpoints.

## Per-stage timing/RSS and k10 profile

| Required fresh-build stage | Yeast | k10 |
|---|---|---|
| Input preparation / parse | Not launched | Not launched |
| Front-end / RLBWT | Not launched | Not launched |
| Phi interval construction / inverse heads | Unimplemented | Unimplemented |
| Slim aggregates | Not launched | Not launched |
| Sweep / internal chi gate | Not launched | Not launched |
| Completed SXI write | Not launched | Not launched |

There are **no measured fresh-build stage wall/RSS values or k10 peak RAM**.
`ux-source-gates.jsonl` records only preflight attempt durations. Reporting
those as front-end measurements would be misleading. Inherited pilot or
conversion measurements are not a substitute for the requested 466 planning
slice. No 466 memory extrapolation is justified by this revision.

## Existing yeast conversion (explicitly NOT G1)

Artifact: `/tmp/laneU/yeast.legacy-converted.sxi`, inherited from the prior
lane, built from existing `.ri4`/head/chi files. Header: n=3,336,986,760,
k=1, R=100,904,881. Revised `xsa stats --sxi` validates it and reports
**chi=85,404,240**; that is not evidence of a fresh source build.

| Member | Stored bytes |
|---|---:|
| RLBWT run table and C | 504,526,453 |
| Packed tails | 403,619,537 |
| Raw heads | 807,239,048 |
| Anchors (empty) | 12 |
| Delta chi | 88,746,976 |
| Names | absent |
| Container, including header/padding | 1,804,132,304 |

Chi raw size is 683,233,920 bytes; delta/raw ratio is **0.129892521**
(12.9893%, reduction 87.0107%). The new stats measurement is **7.73 s,
2,048 KiB peak RSS** (`/usr/bin/time`). No fresh witness-equality claim is
made. Earlier conversion writer/reader and slim logs remain historical
diagnostics, not acceptance evidence for G1.

## Reproduction

```sh
bash tools/build_sxi_tools.sh /tmp/laneU/sxi-tools
python3 bit6/test_sxi_build.py
python3 bit6/test_sxi_format.py --writer /tmp/laneU/sxi-tools/sxi_write
python3 bit6/test_phi_inverse_contract.py /tmp/laneU/sxi-tools/phi_inverse_heads
# Use a fresh work directory:
python3 bit6/sxi_gate.py --work /tmp/laneU/new-format-check \
  --writer /tmp/laneU/sxi-tools/sxi_write --dump /tmp/laneU/sxi-tools/slim_dump
xsa/target/release/xsa stats --sxi /tmp/laneU/yeast.legacy-converted.sxi
# Expected to fail preflight in this revision:
xsa/target/release/xsa build --text /tmp/laneY/bat/duplicates-600k.txt \
  -o /tmp/laneU/new-source.sxi --verbose
```

No frontend changes, no O(n) post-step, no new M/b_bwt/w_wt consumed artifact,
no pfp466 interaction, no signals to unrelated PIDs, and no git commits.
The running h466rl_pfp output remains reserved for the later 466 build.
