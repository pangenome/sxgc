# The parse-free slim: chi derivation at scale WITHOUT the PFP parse dictionary

Status: **all gates PASS**. The finish sequence (chunk-merge outputs ->
rpfbwt_endpoints -> slim_dump -> xsa chi-rspace --stream-agg) now runs with
**no reference parse anywhere**: no `.dict`, no `.parse`, no phrase IDs, no
phrase boundaries are opened by any finish stage. Changed sources are
unstaged; retained/banked artifacts were read-only inputs and oracles.

## Route chosen and why

**Route (a), post-merge rebuild, generalized to also serve the seam repair.**

* One **structural LF walk** over the built runs (rows, per-run head-SA
  samples and the emitted bytes only — the corpus is never read, THE LAW
  holds; the walk is over the *built structure*, exactly what
  `BCR_FRONTEND_DESIGN.md`'s parse-free section prescribes: "reconstruct
  text order once from the built BWT"). Sorted run-head seeds partition the
  cyclic position axis into disjoint descending intervals, so the walk
  parallelizes with **no visited bitmap**, and every walk must close
  exactly on the next lower seed's row (O(1) exact check per walk; emitted
  byte multiset, coverage count and sidecar size are additionally checked).
  The walk emits the cyclic text to a private sidecar file (streamed,
  never resident, unlinked on success) while a single backward read of
  that sidecar builds **tau-spaced rolling suffix-hash checkpoints**
  (`tau = 8n/r`, the requested class; `n/tau = r/8` checkpoints = r bytes),
  verified at startup against direct window reads.
* Queries use the **proven SlimFingerprint discipline** (`bit6/slim_lce.hpp`):
  checked 16-symbol fast path, galloping hash probes, bisection, FULL direct
  verification of every proposed prefix and its mismatch boundary, over the
  sidecar through a bounded four-page pread cache per thread (the SlimDict
  streamed pattern). A hash/verification disagreement aborts; no
  probabilistic answer is ever emitted.
* **Discovery**: the finish had TWO parse dependencies, not one. Besides
  the slim's SlimLCE, `rpfbwt_endpoints`'s **cyclic seam repair**
  (`bit6/seam_repair.hpp`) sorted the non-certified seam classes with the
  parse-based SlimLCE (the banked fragment endpoint run printed
  `CYCLIC_SEAM_REPAIRED ... rows=273235` using the reference parse). The
  same parse-free backend now serves it: the repair walks the **padded**
  structure (all `n+w1` rotations of `P=M.0x02^w1` are distinct, so LF is
  closed by construction — no seam hole) and emits only the `n` text bytes.
  Route (b) (phi-recovery LCE) was rejected: forward text reads need
  position-to-row locate, the exact O(n log r) hazard the task prices at
  14 h/yeast. Route (c) (chunk-time fingerprints) is unnecessary at gate
  scales; it becomes the recommended pile-scale variant below.

## Journaled bounded budgets (fail-loud, no O(n) fallback)

All probes, quick compares and verified bytes are charged. The legacy slim
charged **mixed phrase/byte units and ran UNCAPPED in production**
(`total_limit=INF`); its yeast run verified ~1500 bytes/query = 84n bytes
of the same reads this backend performs. The honest byte-denominated caps:
* slim: `max(1e9, 128n)` — 2x the worst measured corpus (yeast used 85.4n;
  the first run at a 64n cap was REFUSED at 99.7% complete, which is the
  measured evidence for the recalibration; both refusals are retained).
* seam repair: `max(1e8, 16n)` bytes — comparable stringency to the old
  `max(1e6,(P+D)/8)` mixed-unit policy (~14n byte-equivalents at fragment).
Per-query verification is capped at `min(2^26, max(1000,n))` symbols;
probe symbol reads are structurally bounded by tau.

## Gates

| Gate | Result | Evidence |
| --- | --- | --- |
| (a) SMALL yeast235 | **PASS**: `chi = 85404336 (N=3336986759, R=100905045)`; sA 683,234,688 bytes byte-identical to banked; aggregate byte-identical to banked | `yeast-slim.log`, `yeast-sweep.log`; inputs: banked `fresh.ri4`+`fresh.head_sa` only, no parse |
| (b) FRAGMENT | **PASS**: `chi = 306164765 (N=1082130213, R=397723010)`; sA 2,449,318,120 bytes byte-identical; aggregate byte-identical; **fresh.ri4+fresh.head_sa byte-identical to the banked parse-produced pair** | `fragment-endpoints.log`, `fragment-slim.log`, `fragment-sweep.log` |
| (c) PROFILE | measured below + pile projection below | this file |
| (d) END-TO-END SMOKE | **PASS**: 16 banked SXCR chunks -> `cross_lcp_merge --tree` (four files byte-identical to banked) -> parse-free endpoints -> parse-free slim -> sweep: `chi = 306164765`, sA byte-identical; the smoke workdir never contained a parse | `smoke-merge.log`, `smoke-endpoints.log`, `smoke-slim.log`, `smoke-sweep.log` |

Additional gates run:

* **G0 synthetic differential** (`g0_synthetic_gate.py` + `oracle.py`, 7
  corpora: 0x1e-cyclic with 120x duplicate records, fully-periodic,
  multi-chunk random, newline multi-string k=60 and k=4, tandem-satellite,
  33-byte tiny): for every corpus (a) the parse-free endpoint adapter is
  byte-identical to the legacy parse-based adapter on the same four files;
  (b) the walk-emitted sidecar is byte-identical to the source text;
  (c) all four CRA1 columns byte-identical to an artifact-anchored
  brute-force oracle (independent per-run cyclic-BWT head contract check,
  head-sample permutation check, brute collection-LCE); (d) default tau and
  `--tau1 2` both pass; (e) `--inject-fingerprint-error` fails loudly.
  The fully-periodic corpus is refused by the adapter's bounded seam policy
  **identically by both adapters** (pre-existing, by design).
* **yeast235 endpoints regression**: parse-free repair (classes=7,
  rows=13503, max_seam_lce=2,164,880) reproduces the banked
  `fresh.ri4`/`fresh.head_sa` byte-identically (`yeast-endpoints-regression.log`).
* **legacy path regression**: the same modified binary run WITH `--parse`
  reproduces the banked yeast aggregate byte-identically
  (`yeast-legacy-path-regression.log`) — the shared-code changes do not
  disturb the parse-based path.
* **fragment repair parity**: classes=15, rows=273235, discovery_steps=16,
  max_seam_lce=97059 — identical class structure to the banked parse-based
  repair, with 49.5M byte-units of LCE work (the old run charged 47M
  mixed units).

## Measured profile (gate (c), fragment scale)

| Stage | Wall | Peak RSS | Notes |
| --- | ---: | ---: | --- |
| parse-free endpoints (seam repair + walk) | 8:28 | 33.3 GB | includes pre-existing repair structures (phi, bychar) |
| parse-free slim | 28:06 | 19.4 GB | seeds-sort 45 s, walk 62 s (48 t), fingerprints 2.8 s, resolve 105 s, LCE 1438 s, write 11.6 s |
| sweep (stream-agg) | 1:08 | ~6 MB | buffered streams (yeast sweep: 14 s) |
| yeast slim (for scale) | 17:22 | 4.94 GB | walk 352 s (32 t), LCE 642 s; work 285.1e9 = 85.4n of 128n |
| smoke re-merge (24 t) | 23:45 | 27.5 GB | four files byte-identical to banked |

Fragment LCE journal: 511,831,565 queries, 13.70e9 verified bytes (12.7n),
735M hash probes, 30.9e9 probe symbol reads, total work 18.83e9 of 138.5e9.
Yeast: 177M queries, 281.4e9 verified bytes (84.3n), work 285.1e9 of 427.1e9.
(Cosmetic: the `SLIM_SEEDS` line prints queries=0 in parse-free mode; the
authoritative journal is the `SLIM_FP parse-free-text` line.)

## Honest pile projection (R ~ 4.8e11, n ~ 1.31 TB at fragment density)

**Fingerprint-structure disk cost: r bytes = 480 GB** (tau = 8n/r ~ 22),
plus a transient n-byte = 1.31 TB text sidecar (unlinked when the dump
completes). The parse input it replaces at w=10 is **1.12n = 1.46 TB**
(fragment measured: dict 1.168 GB + parse 43 MB for 1.082 GB of text)
plus ~0.46n = 600 GB of old fingerprint/boundary RAM structures. So the
feared inversion does **not** materialize: 0.48 TB + 1.31 TB transient is
cheaper than 2.06 TB of parse artifacts — and the sidecar is transient.

**Can the slim + stream-agg run under ~400 GB at pile?** The sweep yes
(buffered, ~6 MB RSS at any scale — but it streams a 32R = 15.4 TB `.agg`
from disk, a pre-existing artifact cost). The slim's own added structures
are page-cache/reclaimable (sidecar reads) plus the r-byte checkpoint file
— both streamable well under 400 GB. However the **current artifact formats
are themselves the pile blockers, for the parse route equally**: the
resident run columns the slim holds today (`starts` 8R, `l` 4R, `a` R,
samples ~5R, `head_sa` 8R = 26R ~ 12.5 TB, plus this backend's transient
sorted seeds 16R and lfBase 8R) and the on-disk `.ssa`/`.ssa_t` columns
(2x3.85 TB) all exceed the budget at R=4.8e11. Externalizing the columns is
a shared, pre-existing requirement (see SLIM_COST.md's 466 projections);
the parse-free backend adds only the two streamable structures above.

**Wall projections (linear in the measured fragment numbers, honest):**
* walk: 1.31e12 LF steps; measured 62 s per 1.08e9 steps at 48 threads ->
  ~21 h as-is; per-step cost grows with R (log-R binary searches over huge
  arrays), so 1-3 days is the honest range. **Recommended pile variant**:
  route (a)'s merge-time stream — the final merge pass already materializes
  and fingerprints every text byte (M.M), so one extra output stream emits
  text+checkpoints with no post-merge walk at all. Seeds-sort disappears
  with the walk; if the post-merge walk is kept instead, seed subsampling
  (any stride K works — total steps are unchanged) cuts the sort to 16R/K.
* queries: 2R queries scale linearly in R: 4.8e11 runs -> ~20 days at 48
  procs at the measured per-run rate. **The parse-based reference is also
  multi-day at pile (~7 days at equal threads)**: measured per-run cost is
  ~3x for the parse-free variant, because its single checkpoint level is
  sparser (tau=8n/r vs the old dense parse level tau=1) and because the
  required work journal serializes ~18.8e9 atomics on one word (a known,
  small follow-up: per-thread sharding). Denser checkpoints trade disk for
  speed inside the same design (tau halved -> checkpoint disk 2r = 960 GB,
  probe reads halved).

## Files

* `bit6/slim_lce.hpp` — `SlimTextFile` (page-cached sidecar reads),
  `SlimFingerprint` adopt-constructor (walk-built checkpoints),
  `SlimSeamWork` explicit-limit constructor, `SlimLCEParseFree` (walk,
  checkpoints, journaled budgets, cyclic/newline collection-LCE), slim_dump
  parse-free mode (selected by `--slim` WITHOUT `--parse`).
* `bit6/chi_rspace_dump.cpp` — `--parse` optional in `--slim` mode; `LfIndex`
  gains a full/starts-only guard flag (full tables are always built: the
  `--flat` calibration and resolver fallback walk interior rows via `lf()`).
* `bit6/seam_repair.hpp` — parse-free seam repair (walks the padded
  structure; emits only the n text bytes; byte-denominated budget).
* `g0_synthetic_gate.py`, `oracle.py`, `fragment_gate.sh` — gates.
* Logs: `*.log` in this directory; work artifacts under `/tmp/slimpf/`
  (yeast, frag, smoke dirs kept for review; sidecars unlinked on success).

No commits, no staging (`git status` shows the three modified sources and
this journal only). No process left running; all banked inputs untouched
(only new files were written next to them: the smoke dir, /tmp/slimpf/*).
