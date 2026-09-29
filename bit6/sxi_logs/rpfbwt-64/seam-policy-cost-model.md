# Seam-LCE policy: cost model note (final; for RESEARCH.md; authored by seam-fix lane)

The 466 endpoints refusal was a miscalibrated work-unit bound, not an algorithmic failure.
SlimFingerprint already gallops (exponential search + binary search over sampled suffix
hashes) and verifies every answer symbol-by-symbol; the old cap
max(1000, bit_width(n)^3) = 68,921 compared symbols was calibrated before pangenome scale.
Measured since: yeast235's own max seam LCE is 2,164,880 text symbols (~24k phrase
comparisons - under the cap, which is why yeast never refused); at 466 haplotypes,
multi-megabyte collinear haplotype identity pushes the same arithmetic past it.

New policy (bit6/slim_lce.hpp, SlimSeamWork; wired in bit6/seam_repair.hpp):
- One work unit = one phrase/byte comparison, one hash probe, or one directly verified
  symbol. A hash probe reconstructs one sampled value from at most probeLimit
  (= bit_width(n)^3, unchanged) symbols; those tau-bounded reads (tau <= 8*max(P,D)/r,
  ~45-54 at 466) are REPORTED (suffix_symbol_reads) but charge ONE unit. Per-symbol
  charging multiplies probe cost by tau and exhausts honest budgets on small structures
  (caught by the dense integration gate: 984,245 charged units on a 267-byte fixture;
  fixed before any 466 claim).
- Per-direct-verification cap: min(2^26, structure size).
- Total seam budget: min(2^32, max(10^6, (P+D)/8)) units, shared atomically across the
  parse and dictionary fingerprints, journaled per run as CYCLIC_SEAM_WORK.
  At yeast: 11.8M used / 68.4M limit. At 466: limit 4.27e9 units = 0.3% of n.
- Exactness discipline unchanged: hashes only propose; the complete prefix and the
  boundary byte are verified directly; collision-injection mode refuses; every budget
  exhaustion refuses loudly (no O(n) fallback, ever).

Gates: slim_seam_policy_test (pass + 5 refusal modes), slim_lce_test (216k exact LCE
checks), test_seam_repair.py full suite (7 padding refusals, 2,214 endpoint cases,
adversarial 2 GB class refusal, 74 dense witness/aggregate checks) - all green on the
corrected model. Yeast: endpoints ri4/head_sa byte-identical to accepted distribution
outputs; slim agg A/B old-vs-fixed byte-identical AND byte-identical to accepted fresh.agg.
k10: front-end regeneration reproduces pilot rlebwt/ssa byte-identically and emits the
era-missing .ssa_t (count=R); endpoints A/B old-vs-fixed on those outputs: ri4+head_sa
byte-identical. NOTE: pilot h10ss_pfp is the PRE-0x1e-era text (its 0x0a padding terminal
fails the endpoint gate with terminal 1e; its head samples diverge from k10.sxi member 3),
so canonical-k10 anchoring is impossible from it; publication anchoring is carried by
the yeast gates.

466 MEASURED (the datum the recalibration needed):
- Old policy (68,921 cap) refused at the first dict-level LCE of 110,001 symbols
  (measurement run, exit 2 after 2h34m, peak 192.6 GB).
- Corrected policy absorbed the full seam: max_seam_lce = 18,180,977 text symbols
  (18.2 MB of collinear haplotype identity at the cyclic seam); largest single direct
  verification 193,423 phrase comparisons; total seam work 426,807,960 of the
  4,266,152,237-unit budget (10.0%); seam classes 5,836, rows repaired 274,952;
  SLIM_FP journaled per fingerprint (parse: 928,679 queries, tau=46; dict: 1,947,558
  queries, tau=55); exact=1.

466 VALIDATION-ONLY RESULT (from retained parse; NOT the from-zero milestone):
chi = 2,250,211,129 (n = 1,403,221,068,481; R = 2,739,737,285; chi/n = 0.1604%;
chi/R = 0.8214, inside the proven [0.82, 0.88] band; k = 1 cyclic string, 38,790 named
records). Stage walls/RSS: endpoints 2h48m/183.7 GB, slim 4h25m/178.1 GB, sweep 6.6m,
audit 12.4m (1000/1000 samples verified against the AGC), write 12m/16.8 GB, validate 4m.
hprc.validation.sxi = 52,175,300,288 bytes at /tmp/rpfbwt-64-466/.

INCIDENT (parent-session follow-up): the accidental agc2flat prepare verb that was logged
as the 69 GB materialization near-miss ALSO truncated the work-dir
collection.txt.names.tsv to zero bytes (sidecar next to its -o output, 00:00:07Z Sep 29;
deleting collection.txt did not restore it). It silently broke the audit stage
("missing names row" via the agc2flat range service, 2.8s fail) and was restored from
the original work dir. Ledger-worthy: prepare-verb sidecars clobber work-dir metadata.
