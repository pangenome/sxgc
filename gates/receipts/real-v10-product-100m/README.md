# Real Pile prefix: identical complete v10 products within 2N

`proc_1a74`, pinned `c2f5025cae4bc4d0c196bd51cfd99612224c5b95`, `$S/real-v10-block100-y2Zifw` where `$S=/mnt/nvme2n1/erikg/TAPEREADER/sandbox`. Reference closed at Nov 24 19:31:00 Memphis / Nov 25 **01:31:00 UTC**; rolling at 19:33:31 / **01:33:31 UTC**. 252 s for both services, score and launcher. All four outer exits 0; **20/20** required native jobs per builder plus the independent raw verifier zero, both offline filesystem checks per arm clean, no STOP or OOM. [Input evidence](../real-v10-map100/), [fixed contract](../../../docs/overlap_real_v10.md); the [native log/physical-space STOP](../real-v10-first-stop/) and [extent-image unlink STOP](../real-v10-extent-stop/) stand unchanged.

**Matched target:** 100,000,503 authenticated real bytes, 16,556 complete documents. Independent older resident lexical/dictionary sorter and original band tap vs `front64 pos-manifest` + certified `parse64 rank` + 4 rolling posting epochs + Rust direct trie pipe. No reference rows or member payloads supplied to candidate. Both use the same fixed narrow sink, two-thread frozen finish, spacing-64 PFCK, in-place trie and XSEP joins. Each checker verified 100,000,504 raw-position/LCP/predecessor rows + final rank 100,000,504, narrow SHA `82747f11a9925bb4d89cc812410d98e411ce80b1767ee1ff4dc22e94e2cd874e`. m14 geometry computed by the checker is **not** an output member. **All 87,192,432 bytes** compared equal, SHA `eac582ca1e51ead51239b60b1506c44cacf9b6713b675b16003f57fc4f3f98bd`. 38,647,521 runs, chi=29,716,347. Exact ten-member table, codecs/offsets/lengths/counts/CRCs/SHA in each `verified.json`. Seven stable v8 payloads, new complete m7 remap; boundary-aware m20 6,803,128 B; m15 37,192 B. 1,024 document samples, 3 contexts against raw and unique 33-byte cross-cut query rejected by default, kept at 13,408 only with `--cross-doc`.

| Fixed measured endpoint | Resident + band tap | Rolling + pipe tap |
|---|---:|---:|
| Raw → prepared incl private FS setup/check/cold (s) | 6.321 | 1.699 |
| Prepared → joined index (s) | 88.425 | 145.172 |
| **Raw → joined index (s)** | **94.747** | **146.871** |
| Additional CRC/doc/context/query/certified publication (s) | 3.140 | 3.152 |
| Raw → certified publication (s) | 97.887 | 150.023 |
| Whole service body incl input setup and final FS check (s) | 98.378 | 150.595 |
| Build cgroup memory.peak (B) | 1,751,048,192 | 665,382,912 |
| Verifier memory.peak (B) | 112,709,632 | 112,693,248 |
| Sampled logical scratch (B) | 165,390,356 | 164,265,623 |
| Reserved max logical incl transport/queue or stage margin (B) | 167,605,419 | 199,001,906 |
| Sampled hardware scratch allocation incl outer controls (B) | 177,696,768 | 191,717,376 |
| Above allocation + reserved transport/queue or stage margin (B) | 185,036,800 | 196,960,256 |
| **Outer capacity ceiling** incl reservations vs **200,001,006 B** | **187,695,104** | **199,229,440** |
| Worker-attributed reads / writes (B, incl FUSE) | 353,636,352 / 1,579,208,704 | 225,558,528 / 1,419,337,728 |

**Decision:** passes <2× aim and <3× promotion limit on this one matched product (raw ratio **1.550143**, prepared 1.641745; not faster than resident). Measured shared peak 38.0% of resident on identical four-core / 2 GiB / no-swap builder grants. Modeled array caps distinct: resident 1.4 GiB, rolling 320 MiB. Each concurrent checker had a separate 140 MiB/fifth core/zero swap and 100,003,840 B attributed reads. Native worker totals exclude the supervisor's input-copy and postcheck attribution (see status), and are **not device traffic**; no cgroup `io.stat`. Excluding declared inputs and output, all large data scratch on a preallocated 168/181 MiB private ext2 image; inside extents and outer backing not double-counted. Reference remnants still counted during candidate. Inputs distinct copies + map/remap = 200,069,606 B/arm. Output reservation separately 128 MiB/arm; largest sampled whole allocation across both 812,421,120 B. This is a quota-filesystem comparison, not native-XFS speed.

Rolling removed **800,004,032 B each way of logical position-band traffic** (3,052 bands → 0); 1,200,006,048 B tagged row pipe is not disk traffic. Largest posting extent 49,896,400 B vs 165,974,592 B total including all epochs. No full byte SA/LCP spool. The direct tap still charged 62.21 CPU-s vs 62.16 CPU-s for band tap; resident sorter 35.08 CPU-s vs 211.18 CPU-s rolling emitter, not interchangeable wall times. Max exporter queue blocking is not eliminated by dropping the band files. Both sides include the raw witness in their wall clocks. Do not extrapolate this ratio, memory or layout to 100 GB.

`score.json` is the original `score.log`, SHA `f973be2d6e160263c9334f92a9b3d128834de0eb3989bccc1e390e23d5269547`. For host-side **read-only** review (no row or build replay): `python3 -BE gates/overlap_real_v10_score.py score "$S/real-v10-block100-y2Zifw" | cmp - gates/receipts/real-v10-product-100m/score.json`. `originals.sha` identifies original full logs, status/verification and per-worker commands, grants, IO and exit records remain accessible. Extract logs are labeled. Corpus, products, 1,200 MB row stream and scratch images are not checked into Git. This does not change shipping tools or admit a larger run.
