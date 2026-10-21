# KASAI + the column: the finish's text-consultation organ dissolves

**Status**: Phase 0 (design). Written before any code, on branch `kasai-finish`,
base `pile-week-v1 @ 1d86e84`.

This is the implementation charter for the constant that
`tools/binbwt/SAMPLED_SA_DESIGN.md` §4.2/§5.2 *priced* and did not build: the
run-boundary LCP column, produced in `O(n)` by Kasai's amortisation, consumed by
the finish as a table read instead of as an LCE walk. The audit that led here is
that document; the emission hook it says to keep (`--sa-sample-k`, ALREADY
MERGED) is the raw material of this one.

Everything numeric below that is not re-measured here is quoted with its source.
Numbers marked **(pending)** are filled by RESULTS_KASAI_FINISH.md, not asserted
here.

---

## 1. What the finish spends today, and why both terms are removable

The finish's only LCE call sites (measured, `bit6/pile_finish.cpp:3505-3506` in
the pre-charter tree; `lceJob` today):

```cpp
s.topLcp[i]   = gx[i] ? slimLce(prev, first) : 0;         // F1: adjacent ranks
s.interior[i] = v.len == 1 ? INF : slimLce(first, tail);  // F2: intra-run min
```

with `first = v.h` (the run's head SA value), `tail = v.t`, `prev = nb[i-1].t`.
Two properties of that pair of lines carry the whole thesis:

1. **Both arguments are positions the pieces already hand us** (`v.h`, `v.t`,
   the previous run's `v.t`), and their *ranks* are known without any locate
   step: the runs are contiguous BWT intervals emitted in ascending rank order,
   so run *i*'s head is rank `r_i` where `r_i` is the **cumulative sum of the
   lengths of runs 0..i-1**. There is no `SELECT` in this query, and never was.
2. **Both answers are entries of one array.** With `col[r] := LCE(SA[r],
   SA[r+1])` (cyclic LCE, cap `n`), `sweep.advance()` consumes exactly

   ```
   topLcp[i]   = col[r_i - 1]
   interior[i] = min(col[r_i .. r_i + len_i - 2])      (len_i >= 2)
   ```

   i.e. one column entry and one range-minimum over a *contiguous, ascending*
   rank range. `LCP(SA[a],SA[c]) = min_{a<t<=c} col[t-1]` holds for the cyclic
   rotation order exactly as it does for a sentinel-ed suffix array: if two
   rotations share a prefix of length h, every rotation sorting between them
   shares that prefix too, so the min cannot dip below the answer, and it cannot
   exceed it either. (`--selftest` of the column builder checks this against a
   brute force on all-equal, periodic, separator and random corpora.)

Today both terms are paid the hard way. Measured at the 1 GB fixture
(`RESULTS_SAMPLED_SA.md` §4, banked):

| quantity | measured at 1 GB | per corpus byte |
|---|---:|---:|
| queries | 473,695,414 (1.287 per BWT run) | 0.474 |
| verified symbols (`SlimFingerprint::lce` direct-verify) | 12,820,424,563 | **12.82** |
| hash probes (the locate ladder inside the fingerprint) | 669,260,274 | 0.669 |
| suffix symbol reads | 28,110,010,031 | 28.1 |
| LCE wall | 165.0 s of a 229 s finish (load ~266); 154.8 s banked at load ~120 | — |
| naive adjacent-LCP from scratch (`sa_anchor_verify --full`) | 53,288,895,864 comparisons | **53.3** |

So: the finish pays **12.82 symbol reads per corpus byte** for values that a
single `O(n)` pass can produce at **~2 comparisons per corpus byte**, and it pays
`O(answer)` per query because `SlimFingerprint::lce` has no memory of the answer
its neighbours already produced. That is the whole of the LCE term, and the
"locate" term is the 669M hash probes inside it — probes that exist only because
the fingerprint has to *find* the mismatch depth. With the column in hand there
is nothing left to locate.

## 2. The pipeline as landed

Four stages. Stage 1 already exists and is gated (`sa_anchor_oracle`,
`sa_anchor_verify`); stages 2 and 3 are `tools/binbwt/kasai_column.cpp`; stage 4
is `bit6/pile_finish.cpp`.

### Stage 1 — the producer emits the whole SA (k=1)

`--sa-sample-k 1 --sa-sample-out FILE`. The emit pass holds the global SA in
RAM, in order (`bit6/binbwt_slice.h:2124` `emitBin`, `:2799` `publishBin`), so
this is a sequential stride-1 read of the slice it is already streaming
(`SaAnchorWriter::emitSlice`). Measured cost of the side-stream:
`SA_ANCHOR_DONE k=1 entries=100000503 bytes=800004072 write_s=0.984` at 100 MB
= 813 MB/s of sequential pwrite, and producer wall flat within load noise across
k ∈ {64, 1024}, k=1 at 1 GB 104-179 s vs 179 s OFF (`RESULTS_SAMPLED_SA.md` §2,
§3). The file is a plain `{u64 position}` array under a 48-byte header
(`n, k, count, rows, pad=10`), and it is **not** on the critical path: the
producer's bands are the wire, this is a side file.

### Stage 2 — invert to the ISA (the machine's own scatter primitive)

`kasai_column` reads the k=1 stream once and emits the inverse permutation.
Bounded RAM, no sort of the world:

* **bucket to disc by position range.** `B` bucket files; entry = `{u32
  p_local, u64 rank}` (12 B) appended buffered. `B` is chosen so a bucket's
  entries fit the RAM sort budget (`span = ramBudget / 16` entries per bucket,
  `B = ceil(n / span)`, clamped `[8, 65536]`); the buffers total `B × 1 MiB`.
* **sort each bucket in RAM**, in parallel across buckets: `std::sort` by
  `p_local` (a bucket holds a few million entries at fixture scale).
* **the coverage check is free and exact**: a bucket covering positions
  `[b0,b1)` must sort to *exactly* `b0, b0+1, …, b1-1`. Strict increase within
  the bucket plus first==b0 plus last==b1-1 plus contiguous buckets is a
  permutation proof with O(1) extra memory. A missing or duplicated position
  fails the gate loudly; nothing downstream ever sees a bad ISA.
* **sequential emit**: bucket `b`'s sorted ranks are written at ISA byte offset
  `b0 × 4` — the concatenation of the buckets *is* the ISA indexed by position,
  in one ascending write. Width `u32` when `n < 2^32`, else `u64` (the pile plan
  packs to `w` bits; see §5).

Counters: `invert_s`, `isa_bytes`, `isa_dump_bytes`, `buckets`, `bucket_max_entries`.

### Stage 3 — one Kasai walk (the ratchet)

Text order, cyclic, `O(n)` total comparisons:

```
h = 0
for p in 0..n-1:                       # text order
    r = ISA[p]
    if r == n-1: h = 0; continue       # no successor in rank space
    q = SA[r+1]                        # one random read of the SA stream
    h = extend(p, q, from h)           # >= h-1 symbols are KNOWN
    col[r] = h
    h = h ? h - 1 : 0
col[n-1] = LCE(SA[n-1], SA[0])         # the wrap entry, one bounded walk
```

The ratchet `col[rank(p+1)] >= col[rank(p)] - 1` holds for cyclic rotations by
the same argument as for sentinel-ed suffixes (shifting both sides of a length-h
match down by one leaves a length-(h-1) match, and the successor of
`rotation(p+1)` sorts between `rotation(p+1)` and `rotation(q+1)`), with the
equal-rotation case (`h == n`) trivially satisfying it. Comparisons are therefore
`<= 2n + resets`; the tool **asserts** `kasai_comparisons <= 3n` and journals the
exact count and the ratio, because a broken ratchet shows up as `~53n` (measured,
§1) and must never pass silently.

Text reads go through a read-only `mmap` of the corpus and compare **raw bytes**:
the remap table is a bijection (`load_remap_table` proves it, `pile_finish.cpp:372`),
so byte equality is preserved and the walk is exact for `T[i] = REMAP[corpus[i]]`
without a per-byte table lookup.

Counters: `kasai_comparisons`, `walk_s`, `max_lcp`, `mean_lcp`.

### Stage 4 — the column, and the finish reading it

Column file: 48-byte header `{magic, version, n, rows, escapes, max_lcp}`, then
`n` bytes: `u8` value, `255` = "escaped", then a sorted escape list of `{u64
rank, u32 value}`. This is the §4.2 pricing made real (`1 B` fast path + an
`o(n)` deep tail — the 1 GB fixture's `max_lcp` is 97,059 and the escape census
is journalled).

The finish gains one option and one knob:

* `--lcp-column FILE` — the column of the same corpus;
* `PILE_FINISH_KASAI=0` — force the OLD `slimLce` path on the same inputs (the
  A/B arm; the column is then never read);
* `PILE_FINISH_KASAI_AUDIT=1` — compute BOTH and assert entry-by-entry equality
  (the exactness gate: the audit compares every `topLcp` and `interior` value
  the sweep is about to consume against the old path's answer).

`lceJob` becomes, per band: one `pread` of the column slice
`[rankStart-1, rankStart + live)`, then for each run a `col[]` read and a range
minimum. `rankStart` is the running `sum(len)` over the runs the ordered fold has
emitted so far — assigned in `feed()` (the only serial, ordered stage), so a band
slot carries its own rank base and the pool touches nothing shared. The total of
all bands' `len` is asserted `== n` at the end of the sweep: that assert is the
mapping's own closure condition (runs are contiguous and exhaustive in rank
space) and it costs one compare.

In the Kasai arm the sweep's inputs are *identical by construction* to the old
arm's; the T3 claims checker (`--check-claims`, `PROB_CHI_ANALYSIS.md`) is
untouched and still runs over the finished product as the certainty gate — it
reads text at claimed positions, which stays cheap and exact. The claims
*producer* tier (`PILE_FINISH_LCE_TRUST=1`) is refused in the Kasai arm: there
are no LCE queries left to claim about.

## 3. Disc plan (transient), fixture and pile

`n` = corpus bytes, `w = ceil(log2 n)`, `R` = BWT runs. At the 1 GB fixture
`n = 1,000,000,665`, `w = 30`, `R = 368,036,610`; at the pile
`n = 1.31e12`, `w = 41`, `R = 4.82e11` (r/n = 0.368 projected from the fixture).

| artifact | 1 GB fixture | pile (n = 1.31e12) | lifetime |
|---|---:|---:|---|
| SA stream (k=1, `8n`) | 8.0 GB | **10.5 TB** | producer → consumed by stage 2 |
| ISA dump (`12n`, transient) | 12 GB | 15.7 TB | stage 2 only, deleted per bucket |
| ISA (`4n`, or `w/8·n` packed) | 4.0 GB | **6.7 TB** packed (10.5 unpacked) | stage 3 only |
| column (`1n` + escapes) | 1.0 GB | **1.31 TB** | finish's lifetime (its locate table) |
| thinned column (`2 B/run`, the §4.2 form) | 0.74 GB | **0.97 TB** | optional, see below |

Order of deletion: the ISA dump is unlinked bucket by bucket as it is consumed;
the SA stream and the ISA are unlinked at column publish; the column is unlinked
when the finish publishes (it is the same lifetime class as `frag.rlebwt` today).
Peak live bytes at the pile are `SA stream + ISA dump` during stage 2 and
`ISA + column` during stage 3 — i.e. **~26 TB peak**, against the pile's
10.5 TB SA stream that has to exist anyway. Every one of those bytes is scratch:
no text is materialised, the corpus stays the only text (THE LAW).

Two honest costs this plan *adds* and must be labelled in the launch table:

* the producer's side-stream write: `8n` sequential = 10.5 TB at 813 MB/s
  (measured) ≈ **3.6 h** at the pile;
* stage 2's I/O: one sequential read of `8n` + `12n` write + `12n` read + `4n`
  write = 36n ≈ 47 TB at the pile, ≈ 6.5 h at 2 GB/s.

Against that: the finish's LCE term is 165 s of 229 s at 1 GB (21% of the finish,
and the `RESULTS_SAMPLED_SA.md` §5.2 projection is **~9 h of a 43.3 h pile
finish**), plus the locate ladder. The column does not have to *win* by a large
multiple to be worth building — it has to win at all, and it retires a term whose
growth is `O(answer)` per query rather than `O(1)`.

The **thinned** column (0.97 TB at the pile: per-run `topLcp` + per-run
`interior` min, i.e. `2 B/run`) is strictly better on disc and on the finish's
read bandwidth, and it is a pure post-process of the full column *given the run
boundaries* — i.e. a piece-aware thinning pass. It is priced, not implemented
here: the full `n`-byte column already makes the finish's access sequential and
one-pass, and the finish deletes it as it goes. `d(bytes) = 0.34 TB` at the pile
for one extra pass over the run stream; the design note exists so a pile
implementation does not have to re-derive it.

## 4. Fail-loud gates (all of them die, none of them warn)

1. **Header/scale identity**: `k == 1`, `count == n == rows == corpus size`,
   `count == (rows-1)/k+1`, exact file size — stage 2 refuses anything else.
2. **Inversion is an exact permutation**: per-bucket strict increase + exact
   contiguous coverage (§2 stage 2). A dropped or duplicated position is fatal.
3. **Ratchet bound**: `kasai_comparisons <= 3n`, asserted.
4. **The tiny oracle** (`--selftest`): brute-force SA + brute-force cyclic LCE on
   adversarial corpora (all-equal, periodic, separators + repeats, random over a
   small alphabet, `n = 1, 2`) checks *every* `col[r]` AND every derived
   `topLcp`/`interior` for a synthetic run structure — i.e. the stage-4 mapping
   (`topLcp = col[r-1]`, `interior = min col[r..r+len-2]`) is verified as
   arithmetic, not as a container byte-identity.
5. **The finish's own closure**: `sum(len) over all runs == n`, asserted at the
   end of the sweep in the Kasai arm.
6. **The audit arm**: `PILE_FINISH_KASAI_AUDIT=1` asserts old==new for every
   value the sweep consumes, at fixture scale.
7. **The bank gates**: 100 MB and 1 GB byte-identical to the banked containers
   with the Kasai arm ON and OFF; ON == OFF products identical.

## 5. Residual risks, stated before the measurement

* **The ratchet's random reads.** Kasai's inner loop needs `SA[r+1]` and
  `T[q+h]` at arbitrary positions: one random `SA` read and a random corpus read
  per text position. At fixture scale both are in page cache; at the pile the
  `SA` stream is 10.5 TB and the corpus 1.31 TB, so the walk is a random-read
  engine over cold text unless it is rank-blocked. This is the honest risk of the
  whole structure and the reason stage 3 is measured apart (`walk_s`) from stage
  2 (`invert_s`): the pile number for `walk_s` is not extrapolable from a cached
  fixture run, and must be measured on a pile-shaped arm.
* **The inversion's width.** `u32` ISA needs `n < 2^32`; the pile needs `w = 41`
  bits packed. Packed reads are still sequential on the `p`-ascending side, but
  the write is bit-unaligned; priced at 6.7 TB in §3, not implemented.
* **The column's RAM at the pile.** The implemented builder holds `col` in RAM
  (`n` bytes: 1.0 GB at 1 GB, 1.31 TB at the pile — over any sane budget). The
  pile form buckets the emitter by rank the same way stage 2 buckets the
  scatter; the format is unchanged. The tool asserts its RAM budget rather than
  swapping.
* **The T3 checker's cost is unchanged** — deliberately. It is the certainty
  gate and it must not be able to "benefit" from the structure it certifies.
* **Equal rotations.** `col[n-1]` (the wrap) is computed but never consumed; the
  equal-rotation case (`h == n`) is exercised by the all-equal and periodic
  oracle arms, where the finish's own cyclic LCE returns `n` too.
