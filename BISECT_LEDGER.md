# BISECT LEDGER - the quarantined stack, re-merged step-by-step onto the proven base and fast-gated at 10 GB

**Base**: `eb6aaef` (manifest refresh over `aa1a95c`, the df-gate fix) - the last proven-good tree.
**Stack under test**: the quarantined linear stack `d3079bf` (prob-chi) -> `07e9769` (sampled-sa) -> `1d86e84` (phrase-radix) -> `88a7cb0` (kasai-finish) -> `d816309` (entropy-rule) -> `3362a45` (seg-triage hand-merge), re-merged one step at a time onto the base as branch `bisect-stack`.

**Gate shape** (all steps, identical): 10 GB pile snap
(`/mnt/nvme2n1/erikg/extcols-scratch/real10b-ref/pile-10b-snap.txt`, n=10,000,017,258),
single entry `xsa build ... --threads 96 --memory-gb 55 --finish-as-gb 400 --wit-ram-gb 8
--chi-expect 2676929430 --scratch-free-pct 8 --timeout 2147483647` (weather-sized envelope:
`--bin-live-cap 2533359616` derived from `--memory-gb 55`), detached, scratch on nvme.

**Verdict classification (binding, per 2026-10-22 steering)**:
- **PASS** = entry RC=0 (chi asserted in-process) **or** 45+ min healthy.
- **ENV-KILL** = rc 137 (kernel memory-pressure kill under the night's 300+ GB foreign `impg`
  load - NOT code guilt).
- **CODE-FAIL** = any non-137 fault with a real signature (assert / mismatch / segv / RC=139).

**Box weather during all gates**: 300+ GB of foreign `impg` processes; 256-core box shared by up
to 4 concurrent 10 GB gates + the step-6 oracle; RAM 1007 GB total, ~475-790 GB available at
gate starts; nvme free 1.8 TB. Walls are therefore contention-inflated, not code signals.

## Ledger

| step | tree (bisect-stack) | change under test | gate log | verdict | rc | wall | last-log-line / notes |
|------|---------------------|-------------------|----------|---------|----|------|------------------------|
| 1 prob-chi | `4aa2047` | prob-chi (`d3079bf`) | `solB-d3079bf-probchi.log` | PENDING (live, 45+ min healthy) | - | - | started 15:42:03, inherited from previous run |
| 2 sampled-sa | `e73039f` | sampled-sa (`07e9769`) | `solB-07e9769-sampled-sa.log` | PENDING (live, 45+ min healthy) | - | - | started 15:48:16, inherited from previous run |
| 3 phrase-radix | `edff29c` | phrase-radix (`1d86e84`) | `solB-step3-phrase-radix.log` | PENDING | - | - | started 16:39:19 |
| 4 kasai-finish | `8b28dec` | kasai-finish (`88a7cb0`) | `solB-step4-kasai-finish.log` | PENDING | - | - | started 16:47:16 |
| 5 entropy-rule | `9ec4c1f` | entropy-rule (`d816309`) | `solB-step5-entropy-rule.log` | NOT YET LAUNCHED | - | - | binary staged: sha256 `e8b865c3...` |
| 6 seg-triage | `04a83f3` | seg-triage hand-merge (`3362a45`) | `solB-step6-segtriage.log` | NOT YET LAUNCHED | - | - | merge landed clean (bit6 byte-identical to the proven hand-merge); binary staged: sha256 `1b764a6a...` |

## Step-6 merge notes (04a83f3)

`3362a45` merged onto `9ec4c1f` with **zero conflicts**: both sides share base `aa1a95c`
(merge-base of `eb6aaef` and `d816309` is `aa1a95c` itself), so the only semantic question -
whether the base's df-gate fix survived - is answered by ancestry, not by resolution.
`bit6/binbwt_slice.h` and `xsa/src/chain.rs` in the merged tree are **byte-identical to
`3362a45`'s proven hand-merge** (10/10 oracle families green there), and both features are
present: the DFS work-list (`BinSortFrame`, 7 references) and the entropy rule
(`bin_entropy_*` journal, `--bin-entropy-min` plumbed through `xsa build`). Steps 1-4 added no
producer-touching changes beyond `d816309`'s own content, so no re-resolution was owed.
Runtime mirror refresh per packaging rule: only `xsa/runtime/bit6/binbwt_slice.h` was stale on
the quarantined side (as at every earlier step); `tools/package_xsa_sources.py` run twice,
idempotent, mirrors verified byte-identical to `bit6/` sources (`cmp`) for binbwt_slice.h,
cross_lcp_merge.cpp, pile_finish.cpp, phrase_radix.h, slim_lce.hpp.

Tiny-scale verification at `04a83f3`: full oracle suite
(`tools/binbwt/oracle.cpp`, brute-force SA exact vs every family) - result pending, see
`solB-step6-oracle.log` / ledger update below.

## Oracle result (step 6, tiny scale)

PENDING

## Root-cause section

PENDING - filled only if a CODE-FAIL verdict lands.
