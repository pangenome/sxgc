# SXI2 v6-4 (format version 6): gamma-Golomb run-permutation coding — honest negative at real corpora, format capability shipped and gated

## The measurement that killed the premise (correction to the design input)

The v6-4 brief was built on "sampled delta entropy 12.29 bits vs 26.59 raw" (yeast v6-3).
That number was a **sampling artifact**: 5,000 uniformly-spaced distinct samples over a wide
support have empirical entropy ~= log2(5000) = 12.3 regardless of the true distribution.
Exact first-pass cost accounting (the writer's tuning loop) shows the truth:

| corpus | r | best k (gamma-Golomb) | coded perm bits/run | flat rw bits/run | verdict |
|---|---:|---:|---:|---:|---|
| yeast235 | 100,905,045 | 25 | 28.16 | 28.16 | incompressible |
| pile-frag | 397,723,010 | 27 | 30.35 | 30.35 | incompressible |

The tuning pass picks k so that nearly every delta has quotient 1 (cost 1+k ~= rw), i.e. the
best Golomb family degenerates to the flat code. The 0.507 "increasing fraction" of the
permutation (coin flip) agrees: the run permutation is near-random at both regimes.
**The phi u-table is SA-sample information (established last lane); the permutation is also
real information. The v6-3 42.25/59.19/71.96 bits/run numbers already sit at the practical
floor for this component. The ~25 bits/run target is unreachable by delta+Golomb.**

## What shipped anyway (the format capability, never-regressing)

- **Format version 6**, member 8 codec **120**: block-anchored gamma-Golomb permutation —
  64-edge blocks, per-block absolute anchor + bit offset, zigzag deltas, quotient Elias-gamma
  (w-1 zeros + w-bit value) then k low bits; k chosen by exact exhaustive first pass (k=0..32).
  72-byte member header (nw, rw, ul, lowbits, highbits, exceptions, k, blocks, delta_bits).
- **Never-regress fallback**: the writer compares the exact coded total against the flat
  codec-119 layout and emits whichever is smaller. Version 6 accepts BOTH codecs for member 8;
  v5 and earlier artifacts read unchanged (119 flat, 118, 108 legacy paths intact).
- **Reader** (xsa/src/sxi2.rs): codec-120 decode with a one-block mutex cache (thread-safe;
  Phi's unused Clone derive dropped); all existing validation (duplicate runs, inverse,
  anchors, escape domains) runs on decoded values.
- **Repack path** now accepts v3/v4/v5 sources (v5's implicit-v member parsed: mask-ranked
  exception values + implicit v = u of edge(run+1)) — the gates repack the banked v6-3
  containers.
- `SXI2_FORCE_RICE=1` test knob forces codec 120 for gate coverage on corpora that would
  fall back; `test_sxi2.py --rice-forced` asserts codec 120 is emitted and byte-parity holds.

## Gates (all green)

- test_sxi2.py v6 default (fallback): PASS — MEM/HTTP byte parity, escapes, EF chi.
- test_sxi2.py v6 --rice-forced (codec 120 end-to-end): PASS.
- test_sxi2.py v5 legacy (banked v6-3 writer): PASS — backward compatibility.
- Three-scale gates (repacked from banked v6-3, /mnt/nvme3n1/erikg/sxi2-v6-4):

| corpus | v6-3 bytes | v6-4 bytes | ratio | phi bits/run | native parity | http parity | warm ms (v6-3 banked) |
|---|---:|---:|---:|---:|---|---|---|
| yeast235 | 923,770,376 | 923,770,376 | 1.000 | 59.19 | identical | identical | 393.0 (440.2) |
| pile-frag | 2,606,290,008 | 2,606,290,008 | 1.000 | 42.25 | identical | identical | 0.353 (0.325) |
| k10 | 20,224,350,184 | 20,224,350,184 | 1.000 | 71.96 | identical | identical | 3.53 (4.61) |

(Banked v6-3 results untouched; v6-4 outputs in the fresh /mnt/nvme3n1/erikg/sxi2-v6-4.)

## Honest conclusions

1. The 2x permutation-compression premise was FALSE — a sampling artifact, now corrected
   in-repo by exact cost accounting. **RESEARCH.md's "sampled delta entropy 12.29" line needs
   the same correction (supervisor note).**
2. v6-3's implicit-exception phi stands at the practical floor for the permutation component;
   remaining levers toward 25-30 bits/run are exception-value coding (114M x ~30 bits on
   pile-frag) and higher-order/adaptive models (per-block contexts) — measured, not assumed.
3. The format is strictly better than v6-3 as a *capability*: identical sizes and parity on
   real corpora, with codec 120 available if any future corpus shows a compressible
   permutation (e.g. highly collinear haplotypes).
4. Rice-unary (first attempt) was catastrophically wrong on wide tails (2^k unary blowup);
   gamma-quotient bounds the tail at 2log(r). The exact-cost first pass makes the choice
   self-verifying.

## Files (all UNSTAGED)

- bit6/sxi2_write.cpp — write_phi codec 120 + fallback, repack v3/v4/v5 inputs, min() projections
- bit6/test_sxi2.py — --rice-forced knob + codec assertion, env-capable call
- xsa/src/sxi.rs — version range 2..=6; member-8 codec 119|120 at version 6
- xsa/src/sxi2.rs — Phi rice fields, gamma decoder, mutex block cache, dispatch by member codec
- bit6/sxi_logs/sxi2-v6-4/ — run_gates.py, gate summaries/logs/time jsons, this report
- xsa/SOURCES.sha256.json — regenerated from source-of-truth (44 entries)

No long runs remain; no live PIDs to adopt.
