# STANDALONE-LCE LANE — HANDOFF (worktree /tmp/sxgc-laneN, HEAD 22759b1)

## Deliverable

`bit6/chi_rspace_dump.cpp` gains `--resolve-ri4`: the dump builds ONLY the
dependency set `pfp_lce_support` actually reads, and resolves positions from
the `.ri4`'s own SA sample array (LF walk). **No `M`, no `b_bwt`, no `w_wt`,
no `pfp_sa_support`, no `.dict`-derived `daD`/colex arrays.**

### The code-read finding this exploits (RESEARCH.md correction #3)
`pfpds::pfp_lce_support::operator()` (150 lines) touches only:
* parse: `rank_b_p`, `select_b_p`, `pars.p`, `isaP`, `lcpP`, `rmq_lcp_P`
* dict:  `select_b_d`, `isaD`, `lcpD`, `rmq_lcp_D`, `length_of_phrase`
`M`, `b_bwt`, `w_wt` are **never queried** — they belong to r-pfbwt's
text-BWT construction (`pf_parsing::build_b_bwt_and_M`), which the standard
constructor forces. The `--pfp-index` fold lane measured the RAM of THAT
object (a >210 GB build at k10, still climbing), not of the LCE dependency set.

### Implementation
* `Ri4::load` now reads the trailing per-run SA sample array (sdsl
  `int_vector<saW>`, mirrored convention `sample(i) = (n-1) - SA[run_end(i)]`).
* `LfIndex` — `Cless` + per-char run lists/prefix sums (hoisted from the flat
  spot-check) → `run_of`, `lf` in O(log R)/O(log char-runs).
* `SampleResolver::sa_at(row)` — mirrors `xsa/src/main.rs` `s_at` (already
  gated byte-exact at 466 in the Rust sweep): walk LF until a run END, return
  `(n-1) - (sample(run') - steps)`; an interior `0x0A` row needs a string-start
  anchor (`--anchors XANC`) or the run FAILS LOUDLY.
* `Anchors` loader (XANC: u32 magic 0x434E4158, u64 k, k*(u64 row, u64 S)).
* Dictionary ctor flags `(saD=1, isaD=1, daD=0, lcpD=1, rmq_lcp_D=1,
  colex_id=0, colex_daD=0)`; `saD` is FREED after build (build intermediate,
  never queried).
* `pf_parsing` via `defer_build_t` + `compute_n()` + `compute_b_p()` +
  `W_flag=true` — so `M.size()==0` in every run (printed).
* All position call sites unified through a `SAfun` shim, so calibration /
  flat spot-check / `resolve_row` work in both modes; standalone starts at
  `.ri4` row 0 (`ROW_OFF=0`, offsets scanned −2..+2 → the only zero-bad
  offset, an independent decode-vs-flat validation).
* `COST-TABLE` printout: every retained structure's bytes + P/n + |D|/n.

## GATES

G0 (battery, 7 texts incl. duplicates-600k): **BYTE-IDENTICAL .agg vs the
committed dumper's baselines** (all 7), flat spot-check 512/512 ok, no
`0x0A`-interior walk failures, `resolveFail=0`.

G1 (k10, R=1.86e9): see below.

## COST (measured)

duplicates-600k: retained 2.15 MB vs the fold's full XPF1 index 3.73 MB
(42% smaller; M alone was 50.4% of that index). `|M|=0`.

Walk cost (sum LF steps over run heads): duplicates-600k 1.94M steps
(max 601/run), dup+unique-120k 1.27M (max 300), random texts ≈0.3–0.5
steps/run (max 6–11).

## PROJECTION CAVEAT (recorded)

`|D|` (the dictionary) = 0.154·n at yeast, 0.182·n at k10, 0.48·n at
duplicates-600k. Since LCE needs `isaD`/`lcpD`/RMQ over the dictionary,
`Theta(P + |D|)` IS `Theta(n)` with a ~1.3–1.6 byte/symbol constant. The
standalone path is a **constant-factor** win (~2–3× vs the full in-process
object), not an asymptotic one; the 466 conclusion needs the k10 numbers.

## K10 MEASUREMENT (R = 1,859,825,801; n = 30,151,407,545; text h10_rl.txt)

Chain built by this lane: `pfp++ -t h10_rl.txt -o h10rl_pfp -w 10 -p 100`
(**27.4 min**, 333,531,723 phrases, dict 5,446,049,184 B — the `\n`-joined
text; the lane before me correctly found the pre-existing `h10ss_pfp` parse
is the `!`-joined text and therefore unpaired with `h10new2.ri4`.)

Phases (measured, wall):
| phase | wall | what |
|---|---:|---|
| dict-BUILD | **6265 s (104 min)** | gsacak SA of D 39 min; isaD 52 s; lcpD (Kasai) 18.5 min; **RMQ over lcpD 39 min**; b_d |
| parse-read/build | 244 s | sacak SA of P + isaP/lcpP + RMQ |
| pf-minimal-BUILD | 539 s | `compute_n` + `compute_b_p` (3.8 GB b_p fill) |
| lf-index | 48 s | .ri4-derived LF tables |
| calibration + flat spot-check | 50 s | **ok=512 bad=0** (decode validated vs the 30 GB flat) |
| queries | ~2 h (in flight) | 2 LCE + 1 walk per run |
peak RSS **164 GB** (of which 59.5 GB = the four output arrays; structures
≈ 105 GB) vs the production walk's **178.6 GB** at k10.

`saD` freed after the dict build: **27,418,772,030 B** reclaimed.
`|M| = 0` printed by every standalone run (structural proof M is absent).

Baseline for the gate: `h10.walk.agg` (59,514,425,644 B, the production
`teralcp_chi --agg-out` walk: **61.9 min / 178.6 GB**), which produced
χ = 1,627,063,183 (set-equal, the k10 GATE in /tmp/laneK/k10_gate.log).

## PROJECTION TO 466 (k10 per-symbol ratios; n = 1.403e12, R = 2.74e9)

| component | k10 bytes | k10/n | 466 projection |
|---|---:|---:|---:|
| d (dictionary) | 5.48e9 | 0.182 | 255 GB |
| isaD | 2.74e10 | 0.91 | **1.53 TB** (6 B/entry at |D|~2.6e11) |
| lcpD | 1.65e10 | 0.55 | 766 GB |
| b_d | 6.9e8 | 0.023 | 32 GB |
| p | 1.33e9 | 0.044 | 62 GB |
| saP+isaP+lcpP | 4.0e9 | 0.133 | 186 GB |
| b_p (n bits) | 3.77e9 | 0.125 | 175 GB |
| .ri4 (runs+samples) | 1.74e10 | — | 38 GB |
| LfIndex (starts+samples) | 2.3e10 | — | 47 GB |
| **total** | **≈1.0e11 (3.3 B/sym)** | | **≈3.1 TB** |

**Honest verdict:** the standalone dependency set is **Θ(|D|)-dominated**
(|D| = 0.154n yeast, 0.182n k10) — `isaD`+`lcpD`+`d` are 83% of the total.
So "LCE in parse space" is **Θ(n) space with a ~3.3 byte/symbol constant**,
not the ~0.3 B/sym of Θ(P+|D|) as I first hoped (RESEARCH.md correction #3
is itself corrected by this measurement). It is a **constant-factor** win
(≈1.7× RAM, ≈40% smaller than the fold's index at 466-scale ratios), and at
466 it needs ≈3 TB — **infeasible on this 1 TB machine**.
Caveat with real error bars: |D|/n at 466 is UNKNOWN and probably smaller
(the 466 corpus is far more repetitive, n/r=512 vs k10's 16); if |D|/n
halved to 0.09 the total falls to ≈1.5 TB, and to ≈0.9 TB at 0.05.

Named reductions (not implemented): free `saD` (DONE), replace `b_p` with a
Θ(P·log(n/P)) position↔phrase map (−0.1n), Elias-Fano/compress `isaD`/`lcpD`,
a cheaper RMQ than sdsl's (39 min of the 104), and recursive PFP over the
dictionary (the only asymptotic door, unmeasured).

## THE DECISIVE COST FINDING: the .ri4-sample LF walk is Theta(sample-gap);
## at human scale it needs ~4,249 LF steps/run -> the full k10 dump is INFEASIBLE

MEASURED (200,003-run sample): avg 2,156 steps per run-head walk, max
933,995; 4,249 steps per run. At this implementation's throughput
(~2.7e5 steps/s single-threaded — each LF step is a random access into the
char-run rank tables) the full dump needs ~333 core-days. Earlier estimate
before the measurement:
1.86e9 runs / 110 per s = **1.7e7 s ≈ 165 core-days** (≈ 18 days on 9
cores). The full dump was run for **3+ h of query phase (9.4 cores) without
finishing** and had to be stopped. Cause: the v4 `.ri4` stores ONE sample
per run **at the run END**; the run-HEAD SA is only reachable by walking LF
to the nearest sampled row, and at human scale those gaps are huge.
(Compare: random battery texts 0.3-0.5 steps/run; duplicates-600k 132
steps/run; k10 human ≈ 1e5 steps/run.)

**Consequence (this is the lane's most useful result):** the standalone
route's position decode is CORRECT (7/7 battery byte-identity; k10 flat
spot-check 512/512; sampled-run cross-check vs the walk baseline) but
PRACTICALLY USELESS at human scale **unless run-head samples are stored**.
This is a measured justification for the already-recorded **v5 decision
(head-SA samples alongside tail samples)**: with head samples both endpoints
are O(1), the walks vanish, and the query phase becomes purely the LCE calls
(yeast-scaled ≈ 83 min at k10 for R=1.86e9 — matching the fold lane's
r-scaled query phase). Without them, the route is dominated by Θ(n)-ish
walk scans.

## G1-EQUIVALENT AT K10 (the full byte-identity is infeasible; this replaces it)

`SAMPLE_RUNS=200000 SAMPLE_BASELINE=h10.walk.agg` (step 9299 → runs spread
over the WHOLE row space; compares decoded `saFirst`/`saLast` against the
production walk's `h10.walk.agg`, seeking in the 59.5 GB baseline):

    SAMPLE_RUNS: checked=200003 step=9299 badFirst=0 badLast=0
                 walk0aFailures=0 walks=394068 sumSteps=849727721 maxSteps=933995

* **0/200,003 position mismatches at k10 scale** (R=1.86e9) — the `.ri4`
  sample decode reproduces the production walk's SA positions exactly.
* 0 runs needed the `0x0A` anchor path (`\n`-chain walks never landed
  interior to a 0x0A run) — the anchor machinery is implemented but unexercised.
* Walk cost: avg **2,156 LF steps** per run-head walk, max **933,995**;
  4,249 steps per run (≈1.9 step per input symbol is NOT the right way to
  read it — the walk crosses *sample gaps*, so it scales with the gap
  distribution, not with n/R). At the measured walk throughput of this
  implementation (~2.7e5 steps/s; each LF step is a random access into the
  char-run rank tables) a full k10 dump needs ≈**333 core-days** → stopped
  after 3+ h of query phase and reported as INFEASIBLE, not failed.

Plus at k10: flat spot-check **ok=512 bad=0** (decode validated against the
30 GB flat text) and `|M|=0` printed by the run.

## WHAT IS PROVEN / WHAT IS NOT

PROVEN (gated): LCE needs NO `M`/`b_bwt`/`w_wt` (they are never queried;
`|M|=0` at every run); the dump produces byte-identical `.agg` on 7/7
battery texts (incl. duplicates-600k) with only `.ri4 + .parse + .dict`; the
`.ri4`-sample position decode is exact at battery scale (7/7 full) and at
k10 scale (200,003 sampled runs), and the k10 flat spot-check passes 512/512.

NOT PROVEN / NEGATIVE: the standalone route is NOT practical for a full
human-scale dump with the v4 `.ri4` (no head samples) — the run-head LF walk
dominates. And its SPACE is `Theta(|D|)`-dominated, i.e. `Theta(n)`:
measured 3.3 B/symbol at k10 (|D|/n = 0.182), projecting ≈3.1 TB at 466
(isaD 1.53 TB + lcpD 0.77 TB + d 0.26 TB = 83%). The earlier hope that "LCE
in parse space is Theta(P+dict)" is corrected: since |D| ≈ 0.15–0.18·n, that
IS Theta(n) with a smaller constant than the full pf_parsing object (~2× at
k10/yeast scale: 105 GB of structures vs the walk's 178.6 GB RAM, and 42%
smaller than the fold lane's serialized index at the same corpus).

## RECOMMENDATION (measured)

1. **v5 must store run-HEAD SA samples** (already the recorded v5 decision):
the measurement above is the justification — with head samples both run
endpoints are O(1) and the whole walk cost disappears; the dump then reduces
to the LCE query phase (yeast-scaled ≈ 83 min for R=1.86e9).
2. Do not port the standalone route to 466 on this machine (3.1 TB); the
TeraLCP walk remains the 466 aggregate route.
3. Cheapest further wins if the route is pursued: blocked LF (as in
xsa's `BLK=64` run index) instead of `lower_bound` per step; a non-sdsl RMQ
(39 min of the k10 construction); free `saD` (DONE); replace `b_p` with a
Theta(P·log(n/P)) position↔phrase map; recursive PFP over the dictionary
(the only asymptotic door).
