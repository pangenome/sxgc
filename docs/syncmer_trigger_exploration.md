# Closed syncmer triggers in PFP++: slice result

## Implementation

The optional `--syncmer-s S` switch makes the text/AGC parser select a
trigger when the minimum hashed S-mer in the current K-mer occurs at its
first or last position; `-w K` is both K and the PFP phrase overlap.
`--syncmer-canonical` uses the smaller forward/reverse-complement S-mer
hash. A/C/G/T are complemented; N, 0x1e, and other bytes are
self-complementary. The scanner uses a rolling hash and monotone queue,
consumes the source in one pass, keeps O(K) state, and does not reset at
phrase or input-block boundaries. Equal minima qualify. With the switch
absent, mod-p behavior is unchanged. The AGC reader and byte remap are
preserved by the base AGC patch, then the syncmer patch applies on top.

Rebuild with `PFP_SYNCMER=1 bash tools/build_pfp_agc.sh`. The incremental
patch is `bit6/patches/pfp_syncmer.patch`; its replay on the pinned PFP
revision plus `bit6/patches/pfp_agc.patch` was checked.

## Measurement

Web input was copied from `/home/erikg/sxgc-piletest/pile-frag.txt`
(1,082,130,213 bytes); 229 bytes of value 0..5 were removed for this
measurement only, leaving 1,082,129,984 bytes. The DNA slice was
`/home/erikg/yeast/shards/chrI.txt` (43,657,847 bytes, including N and
`$`). Each PFP++ run read its input once. All output prefixes and logs are
under `bit6/sxi_logs/syncmer/`; JSON files from
`tools/syncmer_stats.py` hold exact counts and distributions.

`D/n` is the sum of distinct dictionary phrase byte lengths divided by
input bytes, including the one final marker phrase. Trigger density is
`(parse phrases - 1)/n`; parse bytes are four times parse phrases. Length
statistics below weight phrase *occurrences* and omit the final phrase,
which appends K markers. The observed nonterminal syncmer maxima obey
the K-S gap bound: phrase length is at most K+(K-S).

| Input | Trigger | Density | D/n | Distinct phrases | Parse phrases / bytes | Length mean / p50 / p90 / max |
|---|---|---:|---:|---:|---:|---:|
| Web 1.082 GB | mod w10/p100 | 0.984% | 1.0699 | 9,835,282 | 10,648,986 / 42,595,944 | 111.6 / 78 / 244 / 27,896 |
| Web 1.082 GB | closed K260/S8 | 0.998% | 3.2830 | 9,539,653 | 10,796,215 / 43,184,860 | 360.2 / 328 / 512 / 512 |
| Web 1.082 GB | mod w3/p5 | 19.014% | 0.4717 | 35,497,470 | 205,756,890 / 823,027,560 | 8.26 / 7 / 14 / 7,957 |
| Web 1.082 GB | closed K13/S1 | 18.631% | 2.4106 | 134,174,849 | 201,607,992 / 806,431,968 | 18.37 / 17 / 25 / 25 |
| Yeast chrI | mod w10/p100 | 1.040% | 0.2581 | 75,376 | 453,942 / 1,815,768 | 106.2 / 77 / 232 / 10,507 |
| Yeast chrI | closed K220/S8, canonical | 1.051% | 1.1076 | 150,069 | 459,011 / 1,836,044 | 315.1 / 291 / 432 / 432 |
| Yeast chrI | mod w3/p5 | 19.413% | 0.0557 | 147,166 | 8,475,230 / 33,900,920 | 8.15 / 7 / 14 / 1,011 |
| Yeast chrI | closed K15/S5, canonical | 19.187% | 0.3394 | 722,518 | 8,376,470 / 33,505,880 | 20.2 / 19 / 25 / 25 |

Extra full-input web points: K200/S8 reached density 1.299%, D/n 3.2757;
K10/S1 reached density 23.188%, D/n 1.8021. DNA K200/S8 canonical
reached density 1.151%, D/n 1.0487. The full-input density sweep is in
`web-density-full.csv`, `dna-density-full.csv`, and `web-density-p5.csv`.
The first 4 MiB density pilot was insufficient to match the full web
distribution, so full-input counts chose K260/S8 and K13/S1.

At matched density, the web dictionary **tripled** relative to mod-p at
1%, from 1.070n to 3.283n. It grew about fivefold relative to mod-p at
19%. The yeast dictionary grew 4.29-fold at 1% and 6.09-fold at 19%.
Closed syncmers removed long phrase tails in both regimes, but matching
the sparse density forces a much longer K/overlap; dictionary bytes still
rise sharply. Short K makes triggers and parse IDs far denser.

Parser wall time and peak RSS on these runs also rose: web w10/p100 took
43 s / 2.4 GB versus K260/S8 58 s / 4.7 GB; web w3/p5 took
3 min 21 s / 5.3 GB versus K13/S1 9 min 24 s / 20.5 GB. The runs shared
a machine, so these timings are directional; dictionary size is the
deciding result.

## End-to-end smoke and integration limit

A 43,657,848-byte chrI collection (chrI plus one 0x1e terminator) passed
first-level syncmer parse (K10/S3 canonical), L2 mod-p parse (w5/p11),
`rpfbwt --chunks 50`, endpoint extraction, slim, chi sweep, writer, and
SXI validation. The independent mod-p control and syncmer run both had
chi=2,093,235; their `fresh.sA` files were **byte-identical** with SHA-256
`027155db2c21c807776856713c877a1c83745b0e71934a944dc48c849`.
The syncmer pipeline used `--expect-chi 2093235`, so a count difference
would fail loudly. This is a chrI control value measured here; the
published full yeast235 chi=85,350,673 was not rerun in this slice smoke.
Both measured DNA syncmer dictionaries were also checked in sorted order:
no phrase was a prefix of its next neighbor.

The current downstream code hardcodes first-level w=10:
`bit6/rpfbwt_endpoints.cpp` removes 10 padding rows, and
`bit6/slim_lce.hpp` constructs a dictionary with w=10. An exploratory
K200/S8 chrI build completed the 50-chunk front end but the endpoint tap
correctly refused its n-200 padding frame. The pipeline now rejects
`--w1` other than 10 before construction. Full integration for the
density-matched K220/K260 choices requires passing w1 through endpoint
normalization and slim's dictionary/position mapping, then repeating
the chi and text-audit gates. L2 can remain w5/p11 for a first gate, but
its parse-space size and dictionary should be remeasured and tuned under
the changed first-level phrase IDs. The 50-chunk merge itself completed
at K200 and at K10; downstream normalization is the observed blocker.

The [syng selector](https://github.com/richarddurbin/syng/blob/master/seqhash.c)
and [syng README](https://github.com/richarddurbin/syng) use closed
syncmers with endpoint minima. [Edgar (2021)](https://peerj.com/articles/10805/)
introduced open and closed syncmers; the K-S window guarantee and
approximately 2/(K-S+1) density are summarized in this
[later analysis](https://link.springer.com/article/10.1186/s13015-025-00270-0).
The [WABI 2024 mod-minimizer](https://drops.dagstuhl.de/entities/document/10.4230/LIPIcs.WABI.2024.11)
is adjacent sampling work; it was not implemented or measured here.
