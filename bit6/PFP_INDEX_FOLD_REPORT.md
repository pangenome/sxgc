# PFP-INDEX FOLD — GATE LOG + HONEST COST/SIZE TABLE

Lane: PFP-index fold. Worktree `/tmp/sxgc-laneJ` at `1947628` (fork of the
port lane's tip). No git commits (protocol).

**Goal.** `bit6/chi_rspace_dump.cpp` already needed only `--ri4 + --parse`
(TeraLCP and the lcp_index are gone). But at dump time it still *built*
in-process: the pfpds dictionary (gsacak SA of D, isaD/daD/lcpD, RMQ,
colex), the parse index (sacak SA of P, isaP/lcpP, RMQ), and `pf_parsing`
(O(n)-bit `b_p`, O(n)-bit `b_bwt`, `|M| ~ 0.1n`, W-wavelet). This lane
persists those structures once at build time and makes the dump LOAD them.

**Deliverable.** `bit6/pfp_index_build.cpp` (new; writes the index) +
`--pfp-index` load mode in `bit6/chi_rspace_dump.cpp` +
`bit6/pfp_ds_vendor/pfp/` (the vendored headers, with exactly two added
constructors — see "VENDORING" below).

---

## WHAT IS PERSISTED (and what is deliberately not)

The dumper's only parse-side queries are `pfp_sa_support` (= `resolve_row`)
and `pfp_lce_support`. Everything the two read is restored:

| owner | members persisted | members rebuilt on load |
|---|---|---|
| `pf_parsing` | `n`, `w`, `W_flag`, `b_bwt`, `b_p`, `M`, `w_wt` | rank/select on `b_bwt`, `b_p` |
| `parse` | `saP`, `isaP`, `lcpP` | `rmq_lcp_P`; `p` re-read from `.parse` |
| `dictionary` | `b_d`, `isaD`, `lcpD` | rank/select on `b_d`, `rmq_lcp_D` |

- **Not serialized (deliberately):** `rank_support_v` / `select_support_mcl`
  / `rmq_succinct_sct` supports. They are deterministic functions of the
  loaded vectors, and sdsl's own `select::load` is O(n) like its build, so
  serializing them buys nothing. Rebuilding them is a linear pass in
  *bits* with tiny constants (a few seconds class at yeast for the whole
  set — it is inside the 33 s "index-LOAD" phase below, which is dominated
  by the 8.4 GB read).
- **Not needed on the load path:** the dictionary symbol vector `d`. The
  queries never touch it (`length_of_phrase` uses `select_b_d`;
  `n_phrases()` uses `d.size()`, and is BUILD-path-only). So the load path
  neither reads `.dict` (0.154·n bytes at yeast; ~221 GB projected at
  466) nor materialises `d`. This is why `D.b_d` is persisted: it is the
  only `d`-derived structure the queries need.
- `p` is read from the retained `.parse` artifact, which is the same
  136 MB at yeast as serializing it — no win, so the artifact is reused
  (and is cross-checked against the index's recorded `|P|`, which catches
  a mispaired index).

## VENDORING (the only reason `bit6/pfp_ds_vendor/` exists)

`bit6/pfp_ds_vendor/pfp/` holds verbatim copies of the seven vendored
headers; **six are byte-identical to upstream** (`utils.hpp`, `parse.hpp`,
`wt.hpp`, `sa_support.hpp`, `lce_support.hpp`), and exactly **two** gain a
defer-build constructor:

- `pfp.hpp` — `pf_parsing(D, PP, defer_build_t)`. The standard constructor
  cannot be told to skip `build_b_bwt_and_M()` (the O(n)-bit `b_bwt` and
  `|M| ~ 0.1n`), which is the whole cost being avoided.
- `dictionary.hpp` — `dictionary(w, comp, defer_build_t)`. The standard
  constructor always reads `.dict` and builds `b_d`.

Both constructors are 3 lines of members-init plus a documented comment;
the query code (`sa_support.hpp`, `lce_support.hpp`) is untouched, so the
build path and the load path run *the same* query implementation. The
vendor dir must be first on the include path (see `/tmp/build_idx.sh`), so
`#include "pfp/*.hpp"` resolves there and the relative includes inside
those headers resolve among themselves.

Serialization is sdsl `serialize`/`load` for the bit/int-vectors and the
WT (self-delimiting), plus a compact raw block for `M`
(`u32 len, u32 left, u32 right` per entry; the writer fails loudly if a
value does not fit `u32`). `M` is 12 B/entry rather than the in-memory
`M_entry_t`'s 24 B.

Index file: magic `"XPF1"`, `u32 version = 2`, then
`W, pf_n, m_count, d_size, p_size, wt_size` (u64 each), then the blocks in
order `b_bwt, b_p, w_wt, saP, isaP, lcpP, b_d, isaD, lcpD, M`.

---

## GATE LADDER

Gate set = the standing 7 battery texts (including **duplicates-600k**,
which has caught two false greens before) + the real yeast chain. Build:
`/tmp/build_idx.sh <src> <out>`. Baselines are the committed `.agg`
files in `/tmp/laneY/bat/` and `/tmp/laneY/yeast_pfp2.agg`.

### Regression — the BUILD path is unchanged
The rebuilt dumper (vendored headers + `--pfp-index` option) with no index
must still reproduce the previous lane's output exactly:

| check | result |
|---|---|
| battery `.agg` vs baseline (build mode) | **7/7 byte-identical** |
| yeast `.agg` vs committed baseline (build mode) | **byte-identical** (3,228,956,204 B) |

### G1 — load-mode `.agg` byte-identical, 7/7 texts

| text | `.agg` byte-identical | index bytes |
|---|---|---|
| random-4-20k | YES | 386,968 |
| random-bin-20k | YES | 373,288 |
| random-4-200k | YES | 4,026,444 |
| satellite-18k | YES | 304,152 |
| HOR-nested | YES | 22,496 |
| duplicates-600k | YES | 3,910,660 |
| dup+unique-120k | YES | 1,905,172 |

**7/7 PASS.**

### G1b — structural digest, build vs load, 7/7 texts

The `.agg` only exercises the rows a text happens to visit. `PFP_DIGEST=1`
FNV-hashes the *logical values* of all ten structures
(`b_bwt b_p M saP isaP lcpP isaD lcpD b_d w_wt`), so build-run and
load-run are compared on **every value**, not just queried ones.

| text | digests equal |
|---|---|
| all 7 battery texts | **YES (7/7)** |

### G2 — yeast, full scale (n = 3,336,986,770; R = 100,904,881)

Index: `pfp_index_build --parse $Y/yeast2_pfp -o yeast2.idx`
→ **568.54 s / 25.8 GB RSS, 9,083,877,227 B (8.460 GB)**,
`n=3336986770 |M|=317,476,865 |D|=513,395,745 |P|=34,134,007 |w_wt|=34,134,006`.

| check | result |
|---|---|
| load-mode `.agg` vs committed baseline | **BYTE-IDENTICAL** (3,228,956,204 B) |
| build-mode `.agg` vs committed baseline | **BYTE-IDENTICAL** |
| all 10 structure digests, build vs load | **EQUAL** (incl. `b_d`) |
| flat spot-check / LCE cross-checks | ok=512 bad=0; 1,190,377 checks, **0 mismatches** |
| `xsa chi-rspace` on the load `.agg` | **chi = 85,404,240**, witness sorted-set equality vs `chi_yeast_pfp2.sA` = **True** |

**G2 GREEN.** The standing yeast gate now holds with the PFP index LOADED
from a sidecar — no in-process dictionary/parse/pf_parsing construction.

### G2d — mispaired-index guard (fail loudly, not silently)

The loader cross-checks the index against the artifacts it is paired with
*before* streaming the big blocks. Tested both directions:

```
# yeast index fed to a battery text:
FATAL: index parse p size 34134007 != .parse 5247 (mispaired index?)   [no .agg written]
# battery index fed to yeast:
FATAL: index parse p size 5247 != .parse 34134007 (mispaired index?)   [no .agg written]
```

This is deliberate: the cross-chain mispairing class cost the yeast lane a
day, so a wrong index must die, not produce a plausible-looking `.agg`.

### G3 — cost (yeast, `-t 32`)

Phase costs in seconds, as deltas between consecutive `PHASE` lines.
`BUILD` = in-process construction (default); `LOAD` = `--pfp-index`.
Two independent runs of each are shown because the machine is shared and
the construction phases vary run-to-run by ~10–25% (the query and load
phases are stable to ~2%):

| phase | BUILD run 1 | BUILD run 2 | LOAD run 1 | LOAD run 2 |
|---|---:|---:|---:|---:|
| dictionary | 202.45 | 255.81 | 4.07 | **0.95** |
| parse | 17.90 | 15.51 | 0.18 | 0.21 |
| `pf_parsing` build / **index load** | 315.91 | 354.87 | 32.29 | 35.56 |
| calibration + spot-check | 1.34 | 2.38 | 1.42 | 1.65 |
| **queries (r-scaled)** | 271.24 | 272.66 | 293.20 | 297.47 |
| write `.agg` | 3.94 | 4.36 | 4.07 | 4.39 |
| **wall** | **819.70** | **913.27** | **341.02** | **346.59** |
| maxRSS | 25.3 GB | 25.3 GB | 20.0 GB | **19.5 GB** |

(Committed baseline from the previous lane: 841.60 s / 25.3 GB. LOAD runs
2 is the final v2 code; its `.agg` is byte-identical to the baseline, as
is BUILD run 2's. `PFP_DIGEST=1` adds ~21 s when enabled, not shown here.)

- **Construction phase** (dictionary + parse + `pf_parsing`/index-load):
  **536.26–626.19 s → 36.54–36.72 s** (~15–17×). The residual ~36 s is the
  8.4 GB sequential read *plus* rebuilding rank/select/RMQ supports over
  the loaded vectors — no suffix sorting, no O(n) fill, no O(n)-space
  allocation, no `.dict` read. (≈ 260 MB/s throughput; the phase is
  I/O/memcpy-bound.)
- **Total dump: 819.70–913.27 s → 341.02–346.59 s (2.40–2.64×), RSS
  −5.8 GB.**
- **Queries are 85–86% of the load-mode wall** — the compute is r-dominated.

#### Honest reading of "the dump is now r-dominated"

True of the *compute*: no step in the load path scales with n except
sequential I/O of the persisted index and linear support rebuilds. At yeast
the O(n)/O(|D|) construction is 0 s out of ~345 s. It is **not** true that
the dump's wall time is independent of n: the index is an O(n)-bit sidecar
and reading it costs O(n) time. The right statement is:

> dump-time work is separated into a sequential O(n)-bit load plus an
> O(r polylog) query phase; the O(n) *construction* (suffix sorts, fills,
> allocations) is amortised into a once-per-parse index build.

Also note this lane did not make the load path the default: `--pfp-index`
is explicit, so the build path remains available in the same binary as a
self-check and as the fallback when no sidecar is retained.

## 466-SCALE PROJECTION (for the retention decision)

Measured yeast ratios: `|D|/n = 0.154`, `|M|/n = 0.0951`, `|P|/n = 0.01023`.
Linear extrapolation with `n_466 = 1.44e12` (factor **431.6×**):

| component | yeast | ×431.6 → 466 |
|---|---:|---:|
| `M` (0.0951 n × 12 B) | 3.63 GB | **1.57 TB** |
| `isaD` (0.154 n × 4 B) | 1.96 GB | 845 GB |
| `lcpD` | 0.98 GB | 423 GB |
| `w_wt` | 0.84 GB | 365 GB |
| `b_bwt` (O(n) bits) | 0.40 GB | 172 GB |
| `b_p` (O(n) bits) | 0.40 GB | 172 GB |
| `saP` / `isaP` / `lcpP` | 0.39 GB | 169 GB |
| `b_d` | 0.06 GB | 28 GB |
| **total** | **8.46 GB** | **≈ 3.74 TB** |

- **2.60 bytes per input symbol.** For comparison at 466: the revlines text
  is 1.40 TB, `h466.ri4` is 27.7 GB, and the now-dead
  `h466rt.lcp_index.lcp_index` is 82.5 GB.
- Free space today: `/mnt/nvme3n1` 1.9 TB (too small), `/` 3.8 TB,
  `/mnt/nvme2n1` 6.5 TB.
- **Time at 466** (construction ∝ n; queries ∝ R, `R_466/R_yeast = 27.15`):

| | BUILD-mode dump | LOAD-mode dump | index build (one-off) |
|---|---:|---:|---:|
| projected | ≈ 66.4 h | ≈ 6.1 h | ≈ 69.4 h |

  (66.4 h = 536.3 s×431.6 construction + 271.2 s×27.15 queries;
  6.1 h = 33 s×431.6 index load + 293.2 s×27.15 queries + write.)

**So the trade at 466 is: 66 h per dump → 6 h per dump, for a one-off
~69 h index build and ~3.7 TB of retained artifact.** The index is a
function of the *parse* alone, so it is rebuilt only when the parse
changes. If the `.agg` is produced exactly once per corpus, persistence
merely moves work (and adds 3.7 TB); it pays where the dump is re-run
(algorithm iterations, refreshes, additional aggregate variants), which is
the "build and refresh become the same claim" endgame.

The 466 numbers are **projections from yeast ratios, not measurements** —
no PFP parse exists for the 466 chain (its build is iteration-2 work).

---

## TRAPS / NEGATIVE RESULTS

- **Two writer bugs found and fixed before any gate:** the `M` block was
  written with `8*buf.size()` bytes instead of `4*buf.size()` (2× oversize,
  reading past the buffer — the loader happened to read only the correct
  first half, so the *gates* would have passed on undefined behaviour), and
  the total-size report was taken after `ofstream::close()` (`tellp()` = −1
  → 2^64). Both were visible as nonsense in the size table (M reported as
  1.7e19 MB) and are fixed; the byte accounting is now asserted by the
  loader's size cross-checks.
- `satellite-18k` has a 16-byte parse (one phrase) — it is in the set for
  completeness; it cannot discriminate much. The discriminating texts are
  `duplicates-600k`, `dup+unique-120k`, `random-4-200k`.
- The loader's cross-checks (`b_bwt`/`b_p` size == header `n`,
  `.parse` size == header `|P|`, `b_d` size == header `|D|`,
  `w_wt` size == header) exist specifically to turn a mispaired
  index into a loud failure rather than a silently different `.agg` —
  the cross-chain mispairing class that cost the yeast lane a day.
- Residual, not a bug: the load path still reads `.parse` (136 MB at yeast)
  and rebuilds the supports. Both are cheap and were left in place
  deliberately (see above).

## FILES

- `bit6/pfp_index_build.cpp` — new: builds + writes the index.
- `bit6/chi_rspace_dump.cpp` — `--pfp-index` load mode, `PFP_DIGEST`
  structural digest, `PHASE` timers; build path unchanged.
- `bit6/pfp_ds_vendor/pfp/` — the seven vendored headers; two modified
  (defer ctors), five verbatim.
- Gate logs: `/tmp/laneJ/yeast_gate.log`, `/tmp/laneJ/yeast_gate2.log`,
  `/tmp/laneJ/g3.log`, `/tmp/laneJ/yeast/{idxbuild,idxbuild2,dumbuild,dumpload,dumpload2,timed_load,timed_build}.log`,
  `/tmp/laneJ/yeast/dig.build`, `/tmp/laneJ/yeast/dig.load2`.
- The final battery re-verification (after the last edit) passed 7/7 G1
  and 7/7 G1b; the final v2 yeast runs are `/tmp/laneJ/yeast/timed_load2.log`
  and `timed_build2.log`.
- Reproduce: `/tmp/build_idx.sh bit6/pfp_index_build.cpp OUT1`,
  `/tmp/build_idx.sh bit6/chi_rspace_dump.cpp OUT2`,
  `/tmp/laneJ/g1_battery.sh`, `/tmp/laneJ/g1b_digest.sh`,
  `/tmp/laneJ/yeast_gate2.sh`.
