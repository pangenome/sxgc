# Window-pool concurrency verification — externalized merge

**Scope.** Formal-verification *draft* for the window-pool (page-cache) protocol
in `bit6/cross_lcp_merge.cpp` (`struct SharedPages`), the single correctness
property being **reader soundness**: a thread that observes a slot as VALID and
whose seqlock recheck still matches that observation must read bytes of the
slot's *current* page — never a stale-offset (ABA) impostor, never a torn page.

**Read this first.** The decisive result is split in two:

1. The **version-wrap** question is settled by **arithmetic** (§1) — it never
   needed enumeration. 8 bits was simply the wrong width; 64 bits is decisive.
2. The **live nondeterminism bug** is **not** wrap at all: it is an
   **orphaned buffer write** that is **width-independent** and is found by the
   exhaustive enumerator in an **11-step** short schedule with ≥ 2 fillers (§3,
   §5). Widening the version field cannot fix it.

Read-only on the merge lane's source (another live lane edits it). Nothing is
staged or committed. Model checking is bounded; TLA⁺ / Lean artifacts are drafts.

---

## 0. Provenance (recovered lane)

The prior lane was cut by a harness timeout and its worktree was torn down. The
checker source was re-extracted from its transcript
(`~/.pi/agent/sessions/--home-erikg-sxgc--/subagent-artifacts/4589e0fc-…_worker_transcript.jsonl`,
`write`/`edit` payloads), rebuilt, and the reproduction gate re-run once (the
full calibration program was not redone).

| file | role |
|---|---|
| `pool_checker.cpp` | exhaustive BFS enumerator mirroring the shipped bit-packing; reproduction battery; `witness` (wrap} and `orphan` (live-bug) mode, both validated against the successor relation |
| `pool_checker_fast.cpp` | same semantics, index-only open-addressing state store + `std::deque`; validated bit-for-bit against `pool_checker` on every small config |
| `run_checker.sh` | build + gate + witnesses driver |
| `pool_protocol.tla` | TLA⁺ draft of the protocol and `ReaderSoundness` |
| `PoolInvariant.lean` | Lean draft skeleton: bit-packing model + proof obligations (type-checks; proofs `sorry`) |
| `repro_gate.log`, `witness8.log`, `orphan8.log`, `orphan32.log`, `trace_twofillers_vb8.log` | recorded runs |

The enumerator is an explicit-state reachability checker over the product of the
threads' atomic steps and the shared slot words. Reader soundness is a **safety**
property, so the set of reachable states is exactly the union over all
interleavings — no path needs separate enumeration.

---

## 1. Version width — by first principles (the decisive wrap analysis)

The version field exists to keep a reader's latched stamp **unique** for at least
as long as a reader can sit between `try_page` and `try_page_recheck`. A steal
re-packs the slot (`version := version + 1 mod 2^W`); only after a full `2^W`
version cycle can the exact same packed word reappear, i.e. produce an ABA
impostor that the recheck cannot distinguish.

**Steal-rate bound.** Every steal is a fill, and every fill issues one NVMe
`pread` of `PS = 256` bytes into the slot buffer. NVMe read latency is ≈ 10–100 µs,
so one slot can be refilled at most

```
r ≈ 1 / (10–100 µs) ≈ 1e4 … 1e5  steals/s
```

(an OS page-cache hit could reach ≈ 1e6/s; the range below covers 1e4–1e6).

**Wrap time.** `T(W) = 2^W / r`, and the wrap must fit *inside a single reader's
window* — a 256-byte `memcpy` plus the recheck, i.e. tens of ns normally, but
**unbounded if the reader is preempted** between the two loads. The preemption
window is the operative bound.

| W | steals = 2^W | T(W) at r = 1e5/s | verdict |
|---|---|---|---|
| **8 (shipped)** | 256 | ≈ **2.6 ms** | **trivially reachable** — inside one preemption; the field was simply too narrow |
| 16 | 65 536 | ≈ 0.65 s | reachable on a preempted reader |
| 32 | 4.29e9 | ≈ **11.9 h** (≈ 72 min at 1e6/s) | **marginal** — within reach of a long run with unlucky preemption; *not* safely unreachable |
| **64** | 1.84e19 | ≈ **5.9 million years** | **unreachable in any physical run** |
| 128 | 3.4e38 | ≫ age of universe | unreachable |

**Conclusion.** 8 bits is wrong by construction (`2^8` steals is milliseconds);
32 bits is borderline (hours); **64 bits is the correct fix** for this hazard.
This is pure arithmetic and needs no enumeration. The `witness` mode (§2)
corroborates the *mechanism* at the shipped width (257 genuine steals reproduce a
live stamp) but is not what decides the question.

**Representational note.** At `W = 64` the version no longer fits beside the page
field in a 64-bit atomic. The widened layout needs a wider atomic (e.g. a
128-bit `cmpxchg16b` word `(page+1) << 65 | version << 1 | ready`) or the
buffer-id scheme of §5. The enumerator models whatever width the merge lane
lands by parameterising `versionBits`.

---

## 2. Wrap mechanism — corroborating witness (shipped 8-bit width)

`./pool_checker witness` builds the interleaving and **validates every step
against the exhaustive successor relation** (a legal interleaving, not a sketch):

```
== witness8 (PACKED_VERSIONED vb=8 pages=2 slots=1 ps=4 R=1 F=1 depth=258)
   path steps=1032  legal-interleaving=yes  reader-accepted=yes
   stamp S=515 (page=0 version=1 ready=1)
   reader wanted FILE[page=0][z=0]=0  but accepted 64 (page-1 byte)
   fills performed=257  version restored after 257 mod 256 = 1
   VERDICT: wrap-ABA REACHABLE at shipped 8-bit width (witness verified)
```

257 genuine cross-page steals of one slot restore the latched stamp after the
8-bit version wraps; the reader's recheck passes and it accepts a page-1 byte for
a page-0 offset. Consistent with the arithmetic in §1.

---

## 3. Short-schedule safety — exhaustive enumeration (the real contribution)

```
./pool_checker_fast 0 <W> 2 1 4 1 <F> <depth> 0     # W bits, F fillers
```

Races live in **short** interleavings, so the interesting regimes are small. The
enumerator is exact (all interleavings) and typically finds a violation, if any,
within a handful of steps.

### 3a. Single filler — CLEAN (versioned protocol)

| W | F | depth D | states | verdict |
|---|---|---|---|---|
| 8 | 1 | 40 | 2,572,833 | clean |
| 8 | 1 | 48 | 4,489,281 | clean |
| 8 | 1 | 64 | 10,772,609 | clean |
| 8 | 1 | 80 | 21,196,993 | clean |
| 8 | 1 | 96 | 36,811,009 | clean |
| 8 | 1 | 112 | 58,663,233 | clean |
| 8 | 1 | 128 | 87,802,241 | clean |

With one filler, no ABA shorter than the `2^W` wrap exists; these bounds show the
protocol sound for one writer per slot up to depth 128 (and the wrap argument of
§1 covers the rest). This is the "no short-schedule ABA" result — but it holds
only for a **single** filler.

### 3b. Multi filler (≥ 2) — **COUNTEREXAMPLE at depth 16, 11 steps, any width**

| W | F | depth | states | verdict |
|---|---|---|---|---|
| 8 | 2 | 16 / 48 / 96 | 9,567 | **COUNTEREXAMPLE** |
| **32** | 2 | 16 | 9,567 | **COUNTEREXAMPLE** |
| 8 | 2 | (R=2) | 32,331 | **COUNTEREXAMPLE** |

The counterexample is **identical at W = 8 and W = 32** (same 9,567 states): it
does **not** involve a version wrap. This falsifies the single-filler "clean"
story — with two writers per slot the versioned protocol is already unsound on a
short schedule. The mechanism is the orphaned buffer write; see §5.

*(The wrap-depth ladder beyond 128 was stopped once §1 settled the width question
by arithmetic; it is not the decisive evidence.)*

---

## 4. Reproduction-gate calibration

`./pool_checker` (default battery). Both pre-fix protocols are falsified, the fix
closes the gap at single-filler depths, and reduced widths expose the wrap
mechanism:

| config | verdict |
|---|---|
| `split_unversioned` 1 slot, R1 F1, depth 16 (pre-fix #1) | **COUNTEREXAMPLE** (19 steps) |
| `split_unversioned` 2 slots/4 pages, R1 F2, depth 16 | **COUNTEREXAMPLE** |
| `packed_unversioned` 1 slot, R1 F1, depth 16 (pre-fix #2) | **COUNTEREXAMPLE** (16 steps) |
| `packed_unversioned` 2 slots/4 pages, R1 F2, depth 16 | **COUNTEREXAMPLE** |
| `packed_versioned` W=8, R1 **F1**, depth 40 | clean (2,572,833 states) |
| `packed_versioned` W=1, R1 F1, depth 12 | **COUNTEREXAMPLE** (wrap mechanism) |
| `packed_versioned` W=2, R1 F1, depth 24 | **COUNTEREXAMPLE** (wrap mechanism) |

The W=1 / W=2 rows are the reduced-width calibration: at fill counts 2 and 4 the
wrap mechanism appears exactly as `2^W`. Note all four pre-fix rows already use
`F=2` for the "2 slots" cases and find ABA there — the multi-filler dimension was
present in the calibration but never re-applied to the *fixed* protocol.

---

## 5. Live-bug contribution — the orphaned buffer write (width-independent)

### 5a. Evidence from the live runs

The merge lane observed byte-different outputs for the same pair on repeated
runs. Its scratch logs (read-only) show:

| run | pool | `out_runs` for pair L0-2/L0-3 |
|---|---|---|
| `/tmp/nprun1.log` | `CROSS_NO_POOL` | 102,796,802 |
| `/tmp/f5crun1.log` | pooled | 102,796,803 |
| `/tmp/f5crun2.log` | pooled | 102,796,805 |
| `/tmp/v5brun2.log` | pooled | 102,796,806 |
| `/tmp/f5crun3.log` | pooled | 102,796,826 |
| `/tmp/v5brun1.log` | pooled | aborted: `CROSS_LCP_FATAL grid equality contradiction` |

Pooled runs vary run-to-run; the no-pool run is stable. The pool is supposed to be
a pure cache, so **returning different bytes is the bug**, and it is localised to
the pool. The `POOL_STATS` lines report **millions** of `torn` events (publish
CAS losses) — see 5c.

### 5b. The race (exact, machine-validated)

`./pool_checker orphan 8` and `./pool_checker orphan 32` build an 11-step legal
interleaving (validated against the successor relation) for the **versioned**
protocol:

```
F0 claims page 1:  CAS word  0 -> packFresh(1,0)   (tag+version flip, ready clear)
F1 steals page 0:  CAS word    -> packFresh(0,·)    (a genuine different-page steal)
F1 writes the buffer (page 0)
F0 writes the buffer (page 1)      <-- ORPHAN WRITE, after the thief's write
F1 publishes ready: word = (page 0, ready)
reader latches that stamp, copies the buffer (= page 1), rechecks word == stamp  -> PASSES
-> reader accepts a page-1 byte for a page-0 offset
```

`verdict = multi-filler orphan-write STALE READ (width-independent)`; identical at
W = 8 and W = 32 (and by the same construction at any W).

### 5c. Why widening the version cannot help

The seqlock guard is the packed `word`. The orphan write **never touches `word`**:
a filler whose claim was stolen still finishes its already-issued `pread` into the
shared `slot.data` buffer. The winning filler publishes `ready` for *its* page,
but the loser's bytes can land in the buffer **after** that publish, leaving
`word` naming page A while the buffer holds page B. The reader's recheck only
compares `word`, so it passes. Version width, page field, tag — all irrelevant;
the corruption is outside the word entirely. This is exactly the class of event
counted by `nTorn`: `fill()` writes the buffer *before* the publish CAS, so every
lost publish CAS is a filler that had already written the shared buffer.

### 5d. Recommended fix (for the merge lane)

Do not let a superseded claim write a buffer the live word can reference. Two
standard shapes:

* **Buffer id in the word** — pack `(page, version, bufId, ready)` so a steal
  claims a *different* buffer; the orphan write lands in a buffer no live word
  references. Requires the widened atomic from §1 (page + 32/64-bit version +
  buffer id), which dovetails with the width fix.
* **Private fill + handoff** — `pread` into a per-claim buffer and publish the
  pointer atomically with the state (single atomic word carrying the buffer slot
  index, or a hazard-pointer/epoch reclamation scheme so a buffer is not reused
  while a reader may hold it).

A "re-check I still own the slot before writing" is **not sufficient**: ownership
can be lost concurrently with the write. The published word must name the buffer.

---

## 6. TLA⁺ and Lean drafts

* `pool_protocol.tla` models `word : Slot -> Nat`, `dataPage : Slot -> Page`, one
  reader and one filler family with the exact packing (`PackFresh`, `Publish`,
  `PageOf`, `ReadyOf`, `VerOf`) and the safety property
  `ReaderSoundness == \A t \in RT : (racc[t]=1) => (rcopy[t]=PageOf(rstamp[t]))`.
  **Not machine-checked** — the host has a JRE but no `tla2tools*.jar`. The
  orphan race (§5b) can be encoded by letting a filler's `dataPage` write be
  enabled after it has lost the write to `word`; this is the TODO for a TLC pass.
* `PoolInvariant.lean` (core Lean) **type-checks** with only `sorry`/unused-var
  warnings and states: OB1 `version_advances`, OB2 `version_period` /
  `wrap_only_after_full_cycle` (the version width = anti-ABA distance fact), and
  OB3 `reader_sound_w32`. **A full proof must add the buffer-ownership invariant**
  — reader soundness is *false* today without it (§5).

---

## 7. Honest bounds

* **Not a proof of correctness.** Exhaustive reachability over a **bounded**
  model: 1–2 readers, 1–2 fillers, 1–2 slots, 2–4 pages, page size 4 bytes,
  filler depth ≤ 128 (single-filler ladder) and ≤ 96 (multi-filler sweep).
  Bytes are abstracted to a page id; tearing *within* a page is not byte-modelled
  (the seqlock recheck models it as a stamp mismatch).
* **Version wrap:** decided by arithmetic (§1), not enumeration: 8-bit reachable
  in ms, 32-bit marginal (hours), 64-bit unreachable. The `witness` corroborates
  the mechanism at 8 bits.
* **Single filler:** clean up to depth 128 (exact bounds in §3a).
* **≥ 2 fillers:** **unsound** — counterexample at depth 16, width-independent
  (§3b, §5). This is a real defect in the shipped protocol, independent of the
  version width.
* **Fidelity caveat.** The enumerator's packed filler re-packs on every fill,
  whereas shipped `fill()` early-returns when the slot is already ready for the
  *same* page. The model is therefore a **conservative over-approximation of
  version churn** (it can only flip the version more often, i.e. make wrap-ABA
  *easier*). A clean verdict under it is sound; the multi-filler counterexample
  is constructible with only genuine cross-page steals (witness validated), so it
  is a real defect, not an artefact.
* **Not checked:** the actual `WinBytes` call sites, the prefetch ring
  (`prefetch` / `worker`), `CROSS_NO_POOL` fallback correctness, and the merge
  algorithm above the pool. The pool race is sufficient to explain the live
  nondeterminism but has not been confirmed to be its *sole* cause.

## 8. Reproduce

```
cd bit6/sxi_logs/pool-verification
bash run_checker.sh                              # build + gate + both witnesses
./pool_checker_fast 0 8  2 1 4 1 1 96 0          # single-filler clean
./pool_checker_fast 0 8  2 1 4 1 2 16 0          # multi-filler COUNTEREXAMPLE
./pool_checker_fast 0 32 2 1 4 1 2 16 0          # ... identical at 32 bits
```
