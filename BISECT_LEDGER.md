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

**Box weather**: round 1 ran under the night's 300+ GB foreign `impg` storm and was
ENV-KILLED with the whole box at ~17:33 (all four gates SIGKILL'd silently - logs end at
their PHASE START lines, no END/RC markers, no surviving processes; the step-6 oracle run
died in the same window despite an operator belief it survived - its log froze at 17:07).
Per the 2026-10-22 18:32 steering those runs are NOT verdicts. Round 2 (-r2 suffix) relaunch
in clear weather: 961 GB available, the operator's rung100 pair holding ~200 GB, gates at
~65 GB each.

## Ledger

| step | tree (bisect-stack) | change under test | gate log | verdict | rc | wall | last-log-line / notes |
|------|---------------------|-------------------|----------|---------|----|------|------------------------|
| 1 prob-chi | `4aa2047` | prob-chi (`d3079bf`) | `solB-step1-probchi-r2.log` | **PASS (healthy-arm, 45+ min rule)** | - | 4h47m+ healthy at 23:20 | r2 started 18:37:53; producer window 3/4, finish band 41,281/152,589 chi_so_far on-pace; run continues for the rc/product upgrade |
| 2 sampled-sa | `e73039f` | sampled-sa (`07e9769`) | `solB-step2-sampled-sa-r2.log` | **PASS (healthy-arm, 45+ min rule)** | - | 4h47m+ healthy at 23:20 | r2 started 18:37:54; lockstep with step 1 (band 41,345) |
| 3 phrase-radix | `edff29c` | phrase-radix (`1d86e84`) | `solB-step3-phrase-radix-r2.log` | **PASS (healthy-arm, 45+ min rule)** | - | 4h47m+ healthy at 23:20 | r2 started 18:37:56; lockstep (band 41,409) |
| 4 kasai-finish | `8b28dec` | kasai-finish (`88a7cb0`) | `solB-step4-kasai-finish-r2.log` | **PASS (healthy-arm, 45+ min rule)** | - | 4h47m+ healthy at 23:20 | r2 started 18:37:57; lockstep (band 41,473) |
| 5 entropy-rule | `9ec4c1f` | entropy-rule (`d816309`) | `solB-step5-entropy-rule-r2.log` | PENDING | - | - | launched 23:25 (binary staged: sha256 `e8b865c3...`) |
| 6 seg-triage | `04a83f3` | seg-triage hand-merge (`3362a45`) | `solB-step6-segtriage-r2.log` | PENDING | - | - | launched 23:25 (binary staged: sha256 `1b764a6a...`); oracle r2 already GREEN (all 10 families) |

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

## Oracle result (step 6, tiny scale) - GREEN

`solB-step6-oracle-r2.log` (relaunched 18:52 after the r1 run died in the 17:33 storm mid
family-1 with no family verdicts): completed 20:12, **ORACLE_RC=0, zero FAIL lines, all
10 families PASS exact vs brute SA**, matching the `3362a45` hand-merge's own proof:

- `BINBWT_ORACLE_PASS cases=36180 ... exact_brute_SA=1` (multi_window_runs=22500, window_instances=343530)
- `BINBWT_ORACLE_WINDOW_PASS cases=12864 ... exact_brute_SA=1`
- `BINBWT_ORACLE_REFUSAL_PASS refusals=5 modes=unlink,truncate,stale,nomagic,nodone`
- `BINBWT_ORACLE_STAGE_PASS cases=4824 ... counters_identical=1 both_paths=1` (three STAGE passes)
- `BINBWT_ORACLE_PERIODIC_PASS cases=4824 ... over_target_leaves=0 exact_brute_SA=1`
- `BINBWT_ORACLE_ADAPTIVE_PASS cases=4016 ... leaf_rows_max_le_target=1 over_target_leaves=0`
- `BINBWT_ORACLE_ENTROPY_PASS cases=7728 ... order_identical_to_off_arm=1 exact_brute_SA=1 theta0_counter_identical=762`
- `BINBWT_ORACLE_FREQ_PASS cases=6432 ... exact_brute_SA=1`
- `BINBWT_ORACLE_PHRASE_PASS cases=672 ... exact_brute_SA=1` (dictionaries=7, probes=385312, blocks_unaddressable=0)

This is the brute-force-oracle tier of the pyramid at the integrated tip `04a83f3`: the
work-list + entropy-rule coexistence is exact at tiny scale, including the adversarial
fixtures (nested, periodic, degenerate, mixed).

## Root-cause section

PENDING - filled only if a CODE-FAIL verdict lands.
