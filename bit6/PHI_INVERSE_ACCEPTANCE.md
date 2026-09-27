# Phi-inverse lane acceptance

**PASS: G0 yeast end-to-end, G1 k10 complete byte identity, G2 466 extraction and anchor checks.**
**G3 not run:** the 466 parse remains incomplete; stopped after G2 as instructed.
Peak measured RSS across successful phases: **99.245060 GB**, below 150 GB.

## Gate evidence

- G0 extractor: all **100,904,881** heads (**807,239,048 bytes**) equal the
  streamed `saFirst` column of `/tmp/laneY/yeast_pfp2.agg`.
- G0 complete construction: a new slim dump using the extracted heads,
  `.ri4`, parse, and streamed dictionary produced a new aggregate, followed
  by the streamed sweep: **chi = 85,404,240**. NumPy sorted arrays equal the
  `chi_yeast_pfp2.sA` oracle exactly (multiset equality, stronger than set equality).
  The aggregate is also byte-identical as a **diagnostic**, not the gate.
- Yeast telemetry: **zero walks, zero LF steps**, **100,904,881 direct heads**,
  **177,082,171 direct tails**; `no_SA_ISA_LCP_RMQ=1`, `no_M_b_bwt_w_wt=1`.
  O(r) LF support is allocated by the existing dumper but never traversed.
- G1: all **1,859,825,801** heads (**14,878,606,408 bytes**) are byte-identical
  to `/mnt/nvme3n1/erikg/sxgc-pilot/k10/h10.head_sa`.
- G2: **2,739,735,806** heads (**21,917,886,448 bytes**) extracted, with
  **n = 1,403,221,068,481**. Every value is range-checked, exceeding the requested
  spot check. Anchor result: `PASS all 2739735806 heads < n=1403221068481; anchor head_checks=37760 tail_checks=37760 interior_unchecked=239 anchors=38790`.
  Interior anchors are explicitly untested where they do not coincide with a
  head or tail; no walk is introduced to extend that check.
- All three input Phi maps pass full source-domain and image-domain partition
  validation. No index is modified, regenerated, or used for an O(n) walk.
- G3 parse check: exists=False; active producer-related
  processes=3. No 466 chi/witness result
  is claimed. The pfp++ process was not touched.

## Wall time and RSS

GB means 10^9 bytes. GNU time reports KiB; the table converts with ×1024.
Zero RSS for short `cmp` processes is GNU time's reported value.

| Phase | Wall seconds | Peak RSS GB |
|---|---:|---:|
| Yeast extraction, original serial decoder | 44.46 | 3.443491 |
| Yeast head column byte gate | 3.07 | 0.068661 |
| Yeast slim dump, 64 threads, streamed dictionary | 550.33 | 3.767280 |
| Yeast aggregate byte comparison (diagnostic) | 4.63 | 0.000000 |
| Yeast streamed sweep | 13.48 | 0.004194 |
| Yeast NumPy sorted witness comparison | 21.86 | 2.085138 |
| Yeast extraction, final parallel decoder | 27.14 | 3.387032 |
| Final extractor heads vs full-run input | 0.60 | 0.000000 |
| K10 extraction, final parallel decoder | 751.48 | 64.764826 |
| K10 complete head byte gate | 11.67 | 0.000000 |
| 466 extraction, final parallel decoder | 829.65 | 99.245060 |
| 466 all-head range and boundary-anchor checks | 225.80 | 34.384826 |
| Aborted initial k10 serial attempt (excluded from gate timings) | 998.01 | 64.768954 |

The initial serial k10 decode was intentionally stopped after 998.01 seconds
to parallelize interval decoding. It produced no head output and was not a
byte mismatch. The final tool passed its synthetic tests and reproduced the
entire yeast full-run head input byte-for-byte before G1 restarted. The full
yeast dump was not repeated because its input bytes were unchanged.

### Yeast dump phases

RSS below is cumulative process peak at phase completion, not incremental
allocation. The three query phase times accumulate across streamed chunks.

| Phase | Wall seconds | Cumulative peak RSS GB |
|---|---:|---:|
| ri4-load | 2.052 | 1.717567 |
| dict-read | 0.000 | 1.717567 |
| dict-boundaries | 5.987 | 1.721762 |
| parse+boundaries | 8.653 | 1.900020 |
| parse-fingerprints | 0.202 | 1.992294 |
| dict-fingerprints | 3.073 | 2.092958 |
| lf-build | 2.729 | 3.305976 |
| resolve | 52.531935 | 3.767280 |
| LCE queries | 467.836216 | 3.767280 |
| aggregate writes | 6.806004 | 3.767280 |

### Final extractor phases

`complete` measures the inverse-query/output phase after permutation
validation; GNU time above also includes process teardown.

| Phase | Wall seconds | Cumulative peak RSS GB |
|---|---:|---:|
| G0.parallel-extract: decode-images | 3.440836 | 3.387032 |
| G0.parallel-extract: sort-images | 4.972169 | 3.387032 |
| G0.parallel-extract: validate-permutation | 0.447505 | 3.387032 |
| G0.parallel-extract: complete | 17.863110 | 3.387032 |
| G1.extract: decode-images | 66.728275 | 64.764826 |
| G1.extract: sort-images | 281.344690 | 64.764826 |
| G1.extract: validate-permutation | 37.564426 | 64.764826 |
| G1.extract: complete | 358.338108 | 64.764826 |
| G2.extract: decode-images | 125.069154 | 99.245060 |
| G2.extract: sort-images | 191.013190 | 99.245060 |
| G2.extract: validate-permutation | 78.209978 | 99.245060 |
| G2.extract: complete | 422.717776 | 99.245060 |

## Artifacts and audit

- Tool: `bit6/phi_inverse_heads.cpp`; executable `/tmp/laneS/phi_inverse_heads`.
- Gate log: `bit6/phi_inverse_logs/gate.log`; per-phase GNU time and telemetry
  logs beside it; machine-readable status in `state.json`.
- Acceptance helpers: `phi_inverse_check.py`, `test_phi_inverse_heads.py`,
  `phi_inverse_pipeline.py`; recipe and format audit in `PHI_INVERSE_README.md`.
- Heads: `/tmp/laneS/yp2.phi.head_sa`, `/tmp/laneS/yp2.parallel.phi.head_sa`,
  `/tmp/laneS/h10.phi.head_sa`, `/tmp/laneS/h466.head_sa`.
- Yeast construction: `/tmp/laneS/yeast.phi.agg`, `/tmp/laneS/yeast.phi.sA`.
- No commits, no front-end source changes, no regeneration of `.ri4`, and no
  unrelated process termination. Only the owned initial k10 extractor was stopped.

## PFP-BWT sample format: small change confirmed by source audit

The existing `h10ss_pfp.ssa` is **u64 count + count u64 head SA values**, native
little-endian on this host: count **1,859,825,862**, size **14,878,606,904 bytes**.
It has heads only and uses the producer's circular/padded coordinates.
Emitting HEAD+TAIL in the existing front-end sampling pass is a localized
change: select both endpoints, include tail-only ranges in the range gate,
handle singleton and chunk/final boundaries, and normalize coordinates in
the container writer. Changing only the final emission predicate is not enough.
The source locations and pilot-chain distinction are in `PHI_INVERSE_README.md`.
This is an audit, not an implementation or a front-end performance gate.

## Provenance scope

The named pilot `.ri4` and immutable Phi table are the explicit authorized
inputs. Baseline aggregates and head files are validation oracles only.
This lane reconstructs downstream products from those inputs; it does not
claim a newly run raw-text front end. The working checkout is `03e3773`;
the task's “CORRECTION #7 + THE LAW” was read from Git object `1cfef7d`.
The requested supervisor tool was unavailable in this session.
