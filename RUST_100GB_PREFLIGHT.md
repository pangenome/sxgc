# Recursive Rust builder: 100 GB launch decision after the 3 GB gate

**Hold the 100 GB identity run.** The recursive core has passed at 100 MB, 1 GB and 3 GB. The rung-2 driver is not a driver for the banked 100 GB target. Resolve the finish contract and resource envelope before spending days running it. No pile launch.

## Measured current path (decimal units)

| input | finished wall | counted peak scratch | strict budget |
| --- | ---: | ---: | ---: |
| 100 MB (512 MB dial) | 184.04 s | 0.120 GB | 0.200 GB |
| 1 GB | 2275.20 s | 1.255 GB | 2.000 GB |
| 3 GB | 6637.53 s | 3.489 GB | 6.000 GB |

Linear extrapolation from the 1 and 3 GB walls gives roughly 61–63 hours at 100 GB. This is a planning calculation, **not a measured bound**: cache misses, bin skew, number of windows, finish version and trie/map construction can change it. Counted scratch extrapolates to ~116–126 GB; keep the 200 GB law, but do **not** size the filesystem to 200 GB. The scratch gate excludes columns (6.87 GB at the end of 3 GB), and inputs, final output and optional tap/finish artifacts need separate headroom. Only 100 MB has sampled whole-arm disk and anonymous RSS receipts. `dictsort.rs` retains deduplicated bucket results and other phases use parse-sized vectors. A workspace setting alone does not establish a hard RSS cap at 100 GB.

## Census and widths

Use the natural ceiling (`i32::MAX`), not the forced ladder ceiling. If the 100 GB parse has ~3.05 billion rows, as projected, it needs an upper parse. At the 3 GB observed ratio (91,745,024 → 5,659,214, ~16.2x), that would be ~188 million upper rows, inside the signed 32-bit kernel; size this from the actual Rust census, not the older 29.9x census projection. Verify the lower parse fits every u32 rank/occurrence column and the signed kernel is used only at the contracted level. Stop if widths or peak allocations don't fit; the two-level pass is not a general arbitrary-depth or pile-width proof.

## Identity preflight and next gate

Read the trailing SXI2 directories (not just the filename) of both banked anchors:

- 3 GB `TAPEREADER/bins-p1/run14e/reader/product.sxi2`: v8, members 1,4,5,7,10,11,12,13,14.
- 100 GB `PILE100-REHEARSAL/pile-head100.sxi2` (65533578480 B): v10, members 1,4,5,7,10,11,12,13,15,20. The v10 target includes the corpus-matched document starts and boundary-aware trie. See `RESULTS_REHEARSAL_100GB.md` for the reference command and query certificates.

Before scale-up, reproduce **this v10 finish/embedding profile on a small matching corpus+map** from the new producer, compare the member set, payloads and counts against the reference profile, and check the boundary-sensitive queries. Do not silently accept a common-members-only comparison. Confirm member-20 tap storage is bounded concurrently; a materialized 8-byte-per-row SA tap is not within the scratch law. Measure producer and finish anonymous/RSS and total disk, including columns and tap, and guard free space. Once that profile is demonstrated and authorized, use fresh scratch with `/mnt/nvme2n1/erikg/pile-head100.txt`, `PILE100-REHEARSAL/remap-256.bin`, and `/mnt/nvme2n1/erikg/pilemap100`. Never overwrite the banked rehearsal. Compare against its ten-member container and verify the documented counts (`the ` = 704942163; `Harper Reed` = 55). No 100 GB or pile run has been issued by this preflight.
