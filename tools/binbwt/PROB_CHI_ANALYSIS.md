# PROBABILISTIC CHI — the error structure of a hash-trusted construction, and the
# deterministic checker that makes the product certain anyway

Branch: `prob-chi`. Base: `pile-week-v1 @ aa1a95c`.
Subject: `bit6/pile_finish.cpp` (the pile-builder v2 fused banded finish) and its
LCE engine `bit6/slim_lce.hpp` (`SlimFingerprint`, the two-level sampled
fingerprint ladder).

The operator's question, verbatim: *"How probabilistic could we go? chi can be a
bit wrong - if the errors are really rare it might still be a powerful object. We
can keep the text around and check. We could emit errors in some structured
way."*

This document answers with one structural claim and one measurement plan:

> **The probability can be moved entirely into the constructor's speed, out of the
> product.** Construct chi with hash trust (no per-claim byte confirmation), emit
> a CLAIMS list (position, span, claimed fact), and run a checker that reads the
> text **at the claimed positions** — no searching, no probing, embarrassingly
> parallel. If the checker is exact, the product is certain. The product is then
> only as uncertain as the checker's own code, which is the one thing a
> verifier-style proof can cover.

Everything numeric below is either (a) an exact counter printed by the existing
binary, (b) arithmetic on those counters, or (c) marked ANALYTIC with the model
stated. Nothing is extrapolated without the word EXTRAPOLATED.

---

## 0. Where the probability would live: today's asymmetric split

The finish's expensive act is **finding** the LCE answer; its confirming act is
cheap and local. Measured on the banked 1 GB fixture
(`binbwt-gate-1b-t96`, n = 1,000,000,665):

```
SLIM_FP pile-finish-corpus tau=22 bytes=363636608 queries=473695414
  verified_symbols=12820424563 hash_probes=669260274 mean_verified=27.064700616
  max_verified=97060 max_requested_lce=97059 verification_work=14816771223
  hash_probe_charges=2677041604 suffix_symbol_reads=28110010031
  total_work=17493812827 total_limit=128000085120
SLIM_FP pile-finish-seam   queries=4038381 verified_symbols=29271872 hash_probes=1363613
SWEEP_PHASE_WALLS repair=106.9 lce=68.5 advance_witness=53.1 lce_threads=96
xsa chi-rspace: chi = 283296933 (N=1000000665, R=368036609)
```

Read that with the code in hand (`bit6/slim_lce.hpp:313-357`): `lce()` walks a
16-symbol byte-exact quick path, then **galloping hash probes** + bisection on
`equal(i,j,L)` (a length-L polynomial-hash equality, `slim_lce.hpp:299-302`), and
finally **directly verifies** the proposed answer over its whole length, byte by
byte, including the boundary symbol. The comment at `slim_lce.hpp:1-4` is the
doctrine: *"Hash guesses are ALWAYS checked over the entire proposed prefix and
at its boundary. Exactness therefore costs O(answer), NOT O(tau)."*

So today:

| act | counter at 1 GB | who pays |
|---|---|---|
| finding (hash probes) | 669,260,274 probes, 2.68e9 charged probe units, 2.81e10 suffix symbol reads | constructor, inside the probe ladder |
| confirming (byte verify) | 12,820,424,563 verified symbols (mean 27.06/query, max 97,060) | constructor, inline, random-access |

`mean_verified = 27.06` is the whole point: the confirmation volume is ~12.8n, not
O(1) and not O(tau) — it is the *answers*, and answers are short. Finding is
1.4 hash probes and ~59 suffix symbol reads per query. The asymmetry the operator
is asking about is real but inverted from the naive guess: **finding is the
small per-query cost, confirming is the large per-query cost**, and confirming is
also the *exact* part.

Probabilistic construction = stop paying the confirming term inline, and pay it
later in a form that is cheaper per byte because it never probes and can be
sorted.

---

## 1. Error taxonomy (what a wrong fact actually does)

A hash-trusted constructor emits, per query, one claim. The claim language is
deliberately minimal — three arm types, all local, all decidable by reading
bytes at two positions:

```
EQUAL(i, j, g)      T[i..i+g) == T[j..j+g)          # lower bound: the answer is real
SEPARATE(i, j, g)   T[i+g] != T[j+g]                # maximality: no longer answer exists
TRUNC(i, j, g)      g == cap                         # answer was capped by the query, not the text
```

An LCE answer `g = min(lce(i,j), cap)` is certified exactly by
`EQUAL(i,j,g) ∧ (TRUNC(i,j,g) ∨ SEPARATE(i,j,g))`. There is no other way to be
wrong: `EQUAL` false means over-claim; `SEPARATE` false (when not `TRUNC`) means
under-claim.

The *error classes* are the ways the hash ladder can emit a claim that is false,
and they map onto the finish's three consuming sites:

**EC1 — mis-placed break (over-claim, `EQUAL` false).** A collision at some
bisection length makes the ladder believe a longer prefix matches than it does.
The claimed `topLcp` for a run is then too large. The sweep
(`pile_finish.cpp:2036-2070`) takes `m3 = min(m, m2, lcp_b)` and resets every
char-class whose `rr_len > m3`; an inflated `lcp_b` **suppresses witness
emissions** (`threshold_reset` does not fire), so `chi` comes out *too small*.
Damage: the chi count is a **global** quantity — it sizes the m5 EF (`chi*l`
low bits + high part, `pile_finish.cpp:3837-3840`) and indexes every witness — so
one wrong fact anywhere shifts the trailing structure of the container. This is
the dominant, catastrophic class.

**EC2 — missed seam merge / spurious seam merge.** The seam-class discovery
(`SeamClasses`, `pile_finish.cpp:1468+`, LCE through `fhSeam`) merges occurrence
classes on LCE evidence. A false equality merges two classes that are not the
same row family; a missed one leaves a class split. Both change the *repair
transducer's* emitted run set, i.e. they change `r` and every run index.

**EC3 — wrong LCE fact (under-claim, `SEPARATE` false).** The answer is shorter
than the truth. `interiorMin` comes out too small, `m3` too small, and the sweep
**emits spurious witnesses** → `chi` too large. Symmetric damage to EC1 but the
opposite direction, and it is the direction a "conservative" hash-trust design
would naively prefer — the analysis below shows that preference is a trap: chi is
exact-counted in both directions, so both are equally fatal.

**EC4 — silent (the real hazard).** EC1–EC3 are only *detected* today by the
fixture byte-identity gate (`cmp` against a banked container) and by
`--chi-expect`. At the pile there is **no bank** and no reference chi: the check
that catches all of EC1–EC3 today does not exist at the scale where the
probability actually accumulates. That absence is the argument for the checker —
it is not a speed optimisation, it is the only gate that scales.

Damage locality (worth stating because it is *not* what saves us): a wrong `m`
value propagates only to the next char change, so the *witness geometry* damage
is local — but chi is global, and the container's member layout is derived from
chi. Localistic damage, globalistic detection. One wrong fact out of 4.7e8
breaks byte identity.

---

## 2. The probability model

### 2.1 The hash, exactly

`SlimFingerprint` uses an additive polynomial hash in Z/2^64 (`slim_lce.hpp:192-300`):

```
H(i) = Σ_{k<n-i} (T[i+k]+1) · B^(n-i-1-k)   mod 2^64,      B = 0x9e3779b185ebca87
```

and tests a length-`L` equality between positions i and j as

```
H(i) - B^L·H(i+L)  ==  H(j) - B^L·H(j+L)      # = hash of the length-L window
```

Equivalently, for the two length-`L` windows x and y, the test fails to
distinguish iff

```
D(B) ≡ 0 (mod 2^64),   D(z) = Σ_k (x_k - y_k) z^(L-1-k),   x_k,y_k ∈ [0,255]
```

so `D` is a **nonzero** polynomial of degree < L with coefficients in
[-255, 255]. `B` is odd (verified: `B & 1 = 1`).

### 2.2 The rigorous non-adversarial bound: one free symbol

Take the **highest-degree nonzero term** of `D`, say at index m, and solve for it:

```
c_m ≡ -B^(-m) · Σ_{k>m} (x_k - y_k) B^(m-k)   (mod 2^64)
```

`B` odd ⇒ `B^(-m)` exists; the right-hand side is one specific residue. `c_m`
is a *difference of two byte values*, hence lies in {-255,…,255}, a set of 256
values whose residues mod 2^64 are all distinct. So **whatever the rest of the two
windows is** (adversarial, repetitive, symmetric — it does not matter), the
probability that the two windows are a collision is at most

```
p_test ≤ 256 / 2^64 = 1.39e-17
```

for random-marginal content at those two positions. Note two things this bound
buys, which the "w = 64" hand-wave does not:

1. **It is degree-free.** A 97,060-symbol deep-repeat answer gets the *same*
   per-test bound as a 27-symbol one. Deep repeats are not a worse class.
2. **It rules out the boilerplate variants the operator asked about, in the
   strong form.** If two variants differ by a *single* symbol at offset k, then
   `D = c·z^k` with `c ≠ 0`, so `D(B) = c·B^k ≡ 0` would need `c ≡ 0 mod 2^64`
   (B^k invertible) — impossible for |c| ≤ 255. **Single-substitution boilerplate
   variants cannot collide, ever, at any L.** More generally `v2(B-1) = 1` (the
   2-adic valency of `B-1`, computed: `B-1 = 0x9e3779b185ebca86 = 2 × odd`), so
   the structured family that normally wrecks mod-2^k polynomial hashing — the
   finite-difference family `(1-z)^m ≡ 0 mod 2^(m·v2(B-1))` — needs `m ≥ 64`
   differencing steps to reach 64 bits, and the coefficients of `(1-z)^64` reach
   `C(64,32) ≈ 1.8e18`, far outside the ±255 byte range. The *cheap* structured
   collisions do not exist for this hash. Collisions need a genuine 64-bit
   relation among the coefficients.

### 2.3 Per-claim and per-query-class bound

A claim consumes `probes_per_query` hash tests (measured: 669,260,274 probes /
473,695,414 queries = **1.41**). Per claim:

```
p_claim ≤ 1.41 × 1.39e-17 = 1.96e-17
```

Two query classes, and they differ only in *where* this lands, not in the bound:

- **diverse class** (mean answer 27, typical of the run-sweep queries): the
  equality tests are between short windows of unrelated text. Bounded by 2.2.
- **deep-repeat class** (`max_verified = 97,060`: a run inside a real repeat):
  more bisection steps (log2(97060) ≈ 17 probes for that query) but the *same*
  per-test bound. The class risk factor is `log2(span)`, i.e. ≤ 17/1.41 ≈ 12×
  on the rare deep queries — worth naming, worth 1.2e-16 per such claim, and
  worth nothing against the union bound below.
- **boilerplate-variant class** (the operator's explicit worry): see 2.2 —
  *single*-symbol variants are structurally impossible to collide; *multi*symbol
  variants fall back to the general bound. The only way to make this class
  dangerous is to *design* it (§2.5).

### 2.4 Union bound over the build (measured counters, not guesses)

Per-test bound × measured test counts:

| scale | hash tests (measured) | union bound, p ≤ 256/2^64 per test | Σ tested lengths (measured) | cruder deg/2^64 bound |
|---|---|---|---|---|
| 100 MB (n=1.000e8) | 41,570,537 | **5.8e-10** | 1.00e10 (`probe_degrees`) | 5.5e-10 |
| 1 GB (n=1.000e9) | 669,260,274 | **9.3e-9** | 1.170e11 (`probe_degrees`) | 6.3e-9 |
| pile (n=1.3079e12) | 8.75e11 (EXTRAPOLATED: linear in n) | **1.2e-5** | 1.53e14 | 8.3e-6 |

The two bounds are independent models that happen to agree within ~30%: the
first is the degree-free one-free-symbol bound of 2.2 applied per test, the
second is the classical `deg/2^64` form applied to the exact sum of tested
lengths (`PILE_FINISH_PROBE_DEGREES=1` prints it; mean tested length 174.8 at
1 GB — galloping overshoots the answer, which is why the mean is 6.5x the mean
answer). Agreement means the collision risk is not hiding in the length
distribution.

The extrapolation is linear in n because the counters are per-byte constants in
the measured range (`queries/n = 0.474` at BOTH 100 MB and 1 GB;
`hash_probes/n = 0.669` at both), which is the same scaling the plan model
already uses for r and chi.

**Birthday effects.** The naive worry — "n²/2 pairs over a 64-bit space, so
collisions must exist" — is true and irrelevant: with n = 1.31e12 there are
~8e23 positional pairs and *some* pair certainly shares a hash somewhere. What
matters is the probability that a pair the ladder **actually tests** collides,
which is the union bound above (the ladder tests O(log span) lengths for the
~6.2e11 pairs it is asked about, not 8e23). The birthday effect only bites a
design that *searches* the hash table for matches instead of querying it — and
the collision it would produce there is exactly the EC1/EC3 class the checker
catches.

### 2.5 The honest negative: adversarial text

Every number in 2.4 assumes the *marginals* at the two compared positions are not
chosen by someone who knows `B`. `B` is a compile-time constant
(`slim_lce.hpp:194`) and the corpus is web text. Under a hostile-corpus threat
model the bound collapses to **1**:

- We need Σ c_k B^(L-1-k) ≡ 0 mod 2^64 with c_k ∈ [-255,255]. The reachable
  residue set for m nonzeros has ≤ 511^m elements, so **m ≥ 8** differing byte
  positions suffice in principle (511^8 ≈ 2^72), and a meet-in-the-middle over
  ~2^32 is a laptop-day. The required difference pattern is a fixed 8–16 symbol
  template — *exactly* the "engineered near-collision between boilerplate
  variants" the operator asked about: plant both variants, spread them over the
  corpus, and every suffix pair that straddles them collides deterministically.
- Consequence: a **hash-trust tier applied to uncontrolled text must use a
  per-build random base** (which restores 2.4 and costs nothing), and even then
  the *product* claim rests on "nobody chose this corpus". For HPRC assemblies
  (curated, and the pile's actual input) that assumption is defensible; for
  arbitrary web text it is not.
- **This asymmetry is the decisive argument for the checker**: the checker's
  guarantee is content-independent. It does not care whether the collision was
  random or engineered — it reads the bytes.
- One more honest limit: `suffix()` reads `tau-1 ≈ 21` symbols to reconstruct a
  checkpoint value, and the *checkpoint grid* itself (`frag.pfck2`) is computed
  by an exact backward stream from the text (not a hash of a hash), so the grid
  is not an error source; only the *comparison* is probabilistic.

### 2.6 The comparison anchor: hardware error rates

A probabilistic step is only a liability if it is the *weakest* link. The pile
run is ~5 days on ~100 GB of RAM and ~2 TB of NVMe; the corpus is 1.31 TB.

- DRAM soft-error rate, industry order of magnitude: `1e-13` errors/bit-hour
  (typical measured) to `1e-12` errors/bit-hour (`1000 FIT/Mbit`, the
  worst-case JEDEC-class figure for DDR). Both are quoted figures, not measured
  by us; they are used only as an order-of-magnitude anchor.
- Over the 5-day build: 100 GB = 8e11 bits × 120 h × 1e-13 = **9.6 raw bit
  errors** (96 at 1e-12). SECDED ECC turns most into corrected events; the
  *silent* fraction (double-bit) is far smaller.
- Per product position, that is `9.6 / 1.31e12 = 7.3e-12` for the whole build.

So the anchor is: **a construction whose per-position error probability over the
build is below ~1e-11 is not the weakest link in its own build.** Measured
against it:

| tier | expected wrong facts, 1 GB | expected wrong facts, pile | vs hardware (9.6 raw bit errors/build) |
|---|---|---|---|
| FULL (today) | 0 by construction | 0 | below |
| HASH-TRUST | 9.3e-9 | **1.2e-5** | pile: 8e5× *below* the hardware rate — i.e. **already not the weakest link** |
| HASH-TRUST + CHECKER | 0 | 0 | below |

This is the quantitative form of "if the errors are really rare it might still be
a powerful object": **at the pile, a hash-trusted chi has an expected wrong-fact
count ~8×10^5 times smaller than the number of bit flips the build's own RAM will
suffer.** Two caveats keep this from being a green light:

1. It is a *bound under random marginals*, voided by §2.5.
2. Expected-value is the wrong statistic for a **product**. 1.2e-5 expected
   errors is a 1-in-80000 chance that the published container is *silently*
   wrong in a way that no gate at pile scale can see, and the failure is not
   graceful (chi is global, EC1). A 5-day 612 GB artifact that is 1-in-80000
   corrupted is not a product; it is a lottery ticket with good odds. The
   checker converts it to a certificate at a measured cost (§3) and *then* the
   comparison to hardware stops mattering.

---

## 3. The claims-list checker

### 3.1 The contract

Constructor, hash-trust tier: emit `(i, j, g, flag)` per hash-derived LCE answer,
no byte reads. Checker, one pass:

```
for each claim, in any order:
    EQUAL:    T[i..i+g) == T[j..j+g)         (g bytes compared, early exit on mismatch)
    flag==0:  T[i+g] != T[j+g]               (1 byte each side)
    flag==1:  (truncated claim: equality only)
    any violation ⇒ hard failure, loud, with the claim and both substrings' first mismatch
```

Accepted claims ⇒ every answer the sweep consumed equals `min(true_lce, cap)` ⇒
the sweep (a deterministic function of its inputs, `pile_finish.cpp:1967-2070`) is
bit-identical to the FULL tier's sweep ⇒ **the container is byte-identical to the
FULL tier's container.** No probability remains in the product; what remains is
the *checker's* correctness, which is a verifier of ~10 lines of logic — the
formal-methods sweet spot (Deliverable 3, `lean/ProbChi.lean`).

### 3.2 Cost model

Three terms, all measured or exactly countable:

1. **Claim volume**: NOT the query count. Only HASH-DERIVED answers become
   claims; an answer that resolves inside the 16-symbol byte-exact quick path is
   already exact and needs no claim. Measured: `0.0748` claims/text byte at
   100 MB, `0.1250` at 1 GB (7,483,125 and 125,011,605 claims), against
   `0.474` *queries*/byte at both scales. The claim ratio therefore RISES with
   scale, because answers get longer (mean 23.6 -> 27.1 -> deeper at the pile),
   so the honest pile bracket is `0.125 .. 0.474` claims/byte, i.e. 1.6e11 to
   6.2e11 claims, i.e. a 1.9 TB to 7.1 TB spool at the measured 11.5 B/claim.
2. **Claim span bytes**: measured `9.019` symbols per text byte at 1 GB
   (9,018,639,088 over n = 1.000e9), i.e. ~9 passes over the text, not 25.6:
   the quick-path symbols are not re-read by the checker. The checker reads both
   sides, so it touches `2 × 9.019 = 18` byte-reads per text byte. (The FULL
   tier's inline confirmation reads 12.82 symbols per byte; the difference is
   exactly the quick path it already ran.)
3. **Access order**: the inline confirmation happens inside the sweep's LCE
   workers, interleaved with hash probing and pointer-chasing a checkpoint grid.
   The checker touches each span once, in an order we choose:
   - **sorted by position** (bucket claims by `i`, sweep buckets in order): the
     `i` side becomes a monotone front, the `j` side stays scattered. Passes over
     the text go from 4.7e8 independent random starts to `#buckets` sorted
     streams.
   - **fused** (verify in the worker that produced the claim): no spool, no
     sorting, same access pattern as today's confirmation but with no probing and
     no budget accounting.

   **Space is the discriminator between them.** A claim is 11.5 B varint-encoded
   (`i`, `j`, `g`, flag; measured: 1,434,778,749 B / 125,011,605 claims): at the
   pile that is `1.6e11 .. 6.2e11 × 11.5 B ≈ 1.9 .. 7.1 TB` of spool, against a
   pile scratch ceiling of ~2 TB. So at pile scale the *only* viable shape is:
   **bucketed spool, consumed and deleted band-by-band (or fused)**, so live
   claim bytes are bounded by one band's worth. The fixture measurements below
   are run in both shapes precisely to price the sorting. **Concretely at the
   pile**: buckets are capped at 8192 (`pf_claim_buckets`), so a bucket would hold
   2e7..7.6e7 claims = 0.6-2.4 GB of `PfClaimView` records, times 96 workers —
   i.e. tens of GB. The checker therefore FAILS LOUD above
   `PILE_FINISH_CLAIM_BUCKET_MAX` (default 1 GiB per bucket) rather than
   degrading, and the message names the two shapes that do work at that size
   (fused, or a band-consumed spool). Measured at 1 GB: 1024 buckets, 73 KB ..
   1.5 MB per bucket, max 156,516 claims — two orders below the default cap.

### 3.3 What this buys beyond speed: the gate that scales

The fixture gates work by `cmp` against a banked container. At the pile there is
no bank. The claims checker is a **self-contained acceptance test**:

- it is *per-position local* (no global invariant to compare against);
- it is *cheap enough* to run on the real artifact (measured below);
- it is *complete* in the sense that matters: it covers every text-derived fact
  the container depends on, and completeness is enforceable as a counter
  equality (`claims_checked == hash_derived_queries`, both printed).

That is what turns "probabilistic construction" from a risk into a speed lever:
the probability is allowed to exist only inside the constructor, where a miss
costs one wasted pass, never a wrong byte.

---

## 4. The self-correcting query layer (why errors are mispriced queries, not corruption)

The text is retained and randomly accessible: the r-index toehold lineage
(`r-index-toehold/`, and the m11 sparse 1/1024 tail samples in the container)
allows any query to be *resolved against the text* rather than against the
structure. Therefore even a chi error that survives the build is repairable
downstream:

- a query that lands on a mis-placed break finds a mismatch instead of an
  occurrence at the claimed position — a *detectable* failure per query, not
  silent wrong output;
- the toehold lineage can re-derive the local structure for that interval
  (bounded work: the interval, not the corpus);
- this makes the *marginal* value of the checker's certainty high even in the
  tier where it is not run: errors degrade to a small rate of expensive-but-
  correct queries.

The asymmetry to keep honest: query-time repair is a *safety net*, not a
certificate. It costs per-query work at query time and cannot prove the absence
of errors. That is why the recommendation (§6) is checker-before-publish, with
the query layer as the second line of defence.

---

## 5. The tiers

| tier | constructor | claims emitted | checker | residual undetected wrong facts | measured wall (1 GB, ceiling) |
|---|---|---|---|---|---|
| **T0 FULL** (today) | verifies every answer byte-exactly | none | none | **0** | lce 94.2-101.8 s, finish 1:59.8-2:11.8 |
| **T1 SAMPLED(f)** | verifies a fraction f | none | none | `(1-f) × 9.3e-9` (1 GB) | linear in f — but the residual is *undetected*, so T1 is only a knob for a *bound*, never a certificate |
| **T2 HASH-TRUST** | no byte reads | none needed | none | `9.3e-9` (1 GB) / `1.2e-5` (pile) | lce 92.5 s, finish 1:57.9 (measured as the `floor` arm) — i.e. **~0 saving**; adversarial-void |
| **T3 HASH-TRUST + CHECKER** | no byte reads | all | full pass | **0** (checker exact) | lce 91.5-92.9, finish 2:01.2-2:02.4 **+ 8.6 s checker** — wall-neutral, and *certain*. The recommended tier |
| **T4 T3 + per-build random base** | as T3 | all | full pass | 0, and T2's adversarial hole closed as a side effect | free (one constant becomes a runtime value) |

The ordering that matters: T1 is *worse than T3* for a product. It is cheaper
than T3 only if f is small, it never certifies anything, and it grows the
undetected-error rate in exchange for a wall you then have to hedge. **Sampling
is not a tier, it is an admission of not having a checker.** T3 dominates T1 for
every f < 1 on both axes that matter (certainty and total passes over the text).

---

## 6. MEASURED GATES

Every wall below was taken with the operator's live 100 GB rung on the box
(`uptime` load average 118-211 in every arm; recorded per arm in
`tools/binbwt/prob_chi_gate.sh`'s log). **They are ceilings, not quiet-box
numbers.** Fixture: corpus `extcols-scratch/mergperf-1b-snap.txt`
(n = 1,000,000,665), producer = the binned `binbwt --binbwt` at cap 250 M slots
(the same producer the banked fixture came from), bank = the banked container
`binbwt-gate-1b-t96/product.sxi2` (467,814,822 B,
sha256 `38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f`,
chi = 283,296,933).

### 6.1 The 1 GB gate — all four arms byte-identical to the bank

```
t0       SWEEP_PHASE_WALLS repair=52.5 lce=99.8 advance_witness=41.7  finish 2:05.45  chi=283296933 cmp_bank=0
t3spool  SWEEP_PHASE_WALLS repair=45.5 lce=92.9 advance_witness=42.0  finish 2:02.41  chi=283296933 cmp_bank=0
t3fused  SWEEP_PHASE_WALLS repair=48.8 lce=91.5 advance_witness=39.2  finish 2:01.17  chi=283296933 cmp_bank=0
floor    SWEEP_PHASE_WALLS repair=52.0 lce=92.5 advance_witness=37.8  finish 1:57.85  chi=283296933 cmp_bank=0
t0-r1    SWEEP_PHASE_WALLS repair=60.8 lce=101.8                              finish 2:11.83  cmp_bank=0
t0-r2    SWEEP_PHASE_WALLS repair=51.6 lce=94.2                               finish 1:59.77  cmp_bank=0
t3spool2 SWEEP_PHASE_WALLS repair=43.9 lce=84.1   (same binary as the floor arm) finish 1:50.51  sha=bank
```

`t3spool2` re-ran the spool tier with the FINAL committed-tree binary
(`sha256 e1f6417faea12fe099cbd2dae2fcbd11d8b626a338c6e4ec533cfd3c0e3a65a9`,
verified reproducible by rebuilding from the tree): same chi, same sha, same
claim count (125,011,605), checker 6.983 s / 0 violations.

All seven runs (six arms + the A/B pairs below): sha256 == the bank's. **A hash-trusted construction that never
byte-confirmed a single answer produced the banked container byte for byte.**

The claims ledger at 1 GB (`t3spool` / `t3fused`):

```
SLIM_FP pile-finish-corpus queries=473695414 verified_symbols=3812481808
  hash_probes=669260274 max_requested_lce=97059 hash_trust=1
  claimed_symbols=9007942755 exact_fast=348923741 probe_degrees=116988436628
PILE_FINISH_CLAIMS_SPOOL claims=125011605 span_symbols=9018639088
  encoded_bytes=1434778749 buckets=1024 n=1000000665
PILE_FINISH_CLAIMS_LEDGER hash_answers=125011605 claims=125011605
  span_symbols=9018639088 complete=1
```

and the standalone checker (`--check-claims`, run as its own process AFTER the
finish had already published, i.e. it never shared a page-cache warm-up with the
constructor):

```
PILE_FINISH_CHECK_CLAIMS claims=125011605 claims_declared=125011605
  symbols_compared=9018639088 buckets=1024 swept=1024 max_bucket_claims=156516
  threads=96 wall=8.592 violations=0 first_bad_i=none
  source=claim_list independent_of_structure=1
CHECKER_RC=0   (Elapsed 0:08.70)
```

### 6.2 The headline number the operator asked for: what the tier BUYS

**Paired A/B, the decisive measurement.** Three pairs, alternating FULL (T0) and
hash-trust-with-NO-check (the floor arm), same pristine producer output, same
binary (`sha256 e1f6417f...`, built from this branch's tree), same box, load
recorded per run. Every one of the six runs is sha256-identical to the bank.

| pair | T0 lce | floor lce | Δ lce | T0 finish | floor finish | Δ finish |
|---|---|---|---|---|---|---|
| 1 | 95.7 | 92.3 | **3.4** | 2:01.50 | 1:57.83 | 3.7 |
| 2 | 92.8 | 91.8 | **1.0** | 1:58.90 | 1:57.70 | 1.2 |
| 3 | 94.2 | 85.5 | **8.7** | 2:01.08 | 1:51.21 | 9.9 |
| mean | 94.2 | 89.9 | **4.4** (1.0 … 8.7) | 2:00.5 | 1:55.6 | **4.9** |

So the FULL tier's inline confirmation of **12.82e9 symbols** costs **1 to 10 s**
of the finish (mean 4.9 s, 4%), and the *checker's* separate sorted pass costs
**7.0-8.6 s** (measured twice: 8.592 s at load ~209, 6.983 s at load ~143).

| | T0 FULL | T3 hash-trust+checker | floor (measure only) |
|---|---|---|---|
| inline byte confirmation | 12,820,424,563 symbols | 0 | 0 |
| LCE wall (1 GB, load 95-211) | 92.8 / 94.2 / 95.7 / 94.2 / 99.8 / 101.8 | 84.1 / 91.5 / 92.3 / 92.5 / 92.9 | 85.5 / 91.8 / 92.3 / 92.5 |
| finish wall | 1:58.90 … 2:11.83 | 1:50.51 (spool) / 2:01.17 (fused) | 1:51.21 … 1:57.85 |
| claims emitted | 0 | 125,011,605 (11.5 B each) | 125,011,605 |
| checker pass | — | **6.98-8.59 s** (9.02e9 symbols, 1.05e9-1.29e9 sym/s, 96 t) | — |
| product certain? | yes (by construction) | **yes (by certificate)** | no |

**The honest verdict: at fixture scale the probabilistic tier is wall-NEUTRAL.**
Taking the confirmation out of the constructor and paying it back as a
position-sorted pass costs 7-9 s of checker against 1-10 s of saved confirmation.
There is no wall win to bank, and the paired variance is as large as the effect.

**Why the operator's asymmetry does not pay in wall time — and it is a
measurement, not a guess.** The confirmation is a *linear scan*: each answer's
`g` bytes are read sequentially from a corpus that is fully resident (1 GB), so
12.8e9 symbols cost single-digit seconds across 96 threads. The hash ladder that
FINDs those answers is a *dependent random-access chain* — 669 M probes, each a
checkpoint-grid load plus up to `tau-1 = 21` symbol reads, mean tested length
174.8 — so the LCE wall is probe latency, exactly as the pile-week README says.
The expensive part of placement is finding, not confirming; deferring the
confirmation therefore buys almost nothing **at a scale where the text is
resident**.

**Where the tier does pay — and these are the reasons to take it anyway:**

1. **The certificate, which is the only gate that scales.** At the pile there is
   no banked container and no reference chi: `cmp` does not exist, `--chi-expect`
   does not exist. The claims list + checker is a per-position acceptance test
   that runs on the real artifact. Measured price: 7.0-8.6 s at 1 GB = 9.02
   symbols/text byte. EXTRAPOLATED linearly: 1.18e13 symbol comparisons at the
   pile, ~2.6-3.1 h at the measured rate — 2-3% of a 5-day build, and it is the
   difference between a product and a lottery ticket (T2's 1.2e-5 expected wrong
   facts).
2. **The verification BUDGET disappears.** The FULL tier refuses loudly when
   `verification_limit` (2^26 units/query) or the per-fingerprint work cap is
   exhausted (`refuse_verify`, `refuse_budget` in `slim_lce.hpp`). Hash-trust
   charges no verification units, so a deep-repeat corpus that today would
   REFUSE (a hard failure) becomes a construction that succeeds and is certified
   afterwards. `max_requested_lce=97059` vs `verification_limit=67108864` shows
   the current headroom; at the pile the deep-repeat tail is the risk.
3. **At the pile the text is NOT resident**, so both sides become device-latency
   bound — but the checker's `i` side is a monotone front inside each position
   bucket (measured: 1024 buckets, max 156,516 claims/bucket at 1 GB) and can be
   prefetched as a stream, while the constructor's confirmation is interleaved
   with the probe ladder and cannot be reordered. This is the one place where
   the *sorted* checker is structurally cheaper than the inline confirmation, and
   it is EXTRAPOLATED, not measured (10 GB+ is the rung's to measure).

### 6.3 The 100 MB gate (dev loop, and the negative tests)

```
T0 full    chi=29716349 cmp=0   SWEEP_PHASE_WALLS lce=9.4
T3 spool   chi=29716349 cmp=0   claims=7,483,125 (74,826,483 B) complete=1  lce=9.0
           checker: claims=7483125 symbols_compared=754040614 wall=1.000 violations=0
T3 fused   chi=29716349 cmp=0   claims=7,483,125 complete=1 violations=0  lce=9.3
```

Same story at 100 MB: lce 9.4 (full) vs 9.0-9.3 (trust), checker 1.0 s. Neutral.

### 6.4 The negative tests — the checker, not the byte gate, is the detector

`PILE_FINISH_LCE_FAULT=n` perturbs every n-th hash-path answer by one symbol
(the scale-relevant fault: one wrong fact in 4.7e8). With n = 1,000,000 at
100 MB:

* **fused tier**: aborts with the exact violating claim and **publishes no
  container**:
  ```
  PILE_FINISH_CLAIM_VIOLATION mode=fused i=84331142 j=84331143 g=21 cap=15669360
    truncated=0 at_offset=20 Ti=9 Tj=104 Ti_g=104 Tj_g=114
  FATAL pile_finish: hash-trusted LCE answer is FALSE (fused checker)
  ```
  (`i` and `j` one apart — a hard LCE — over-claimed by exactly one symbol, and
  the boundary symbols that the claim said must differ are equal. The fault is
  caught at the exact offset it was injected.)
* **spool tier**: the construction completes and publishes, then the standalone
  checker refuses it: `violations=9`, first at `i=15881088`, and the ledger is
  complete (`claims=7483065 == hash_answers`, so the 9 are real wrong facts, not
  unverified stragglers).
* **and the finding that matters most**: in **4 of 5** fault placements
  (`n = 2e6, 3e6, 5e6, 11e6`) the corrupted construction produced a container
  that is **byte-identical to the bank** (chi unchanged, `cmp` clean) while the
  checker found 2-5 wrong claims in each. The fixture's byte-identity gate is
  BLIND to a wrong fact that does not happen to move the sweep; the checker sees
  every one. (In the 5th placement, n=7e6, chi moved and `cmp` caught it.) So a
  pile-scale run without a claims checker would have no detector at all: not
  "a weak detector" — none.

## 7. RECOMMENDATION

**Run the pile as T3, and do not sell it as a speed lever.** Three findings, in
the order that decides it:

1. **T2 (hash-trust alone) buys nothing measurable and risks everything.** The
   measured constructor floor is 92.5 s against the full tier's 94.2-101.8 s: the
   inline confirmation is inside the box's noise band. So there is no wall
   argument for dropping the confirmation *without* a checker — and dropping it
   buys a 1.2e-5 expected-wrong-fact rate plus a content-independent adversarial
   hole (§2.5). T2 is strictly dominated.
2. **T3 costs the same wall as T0 and returns a certificate.** 8.6 s at 1 GB
   (0.7% of the finish) buys a per-position acceptance test that is the only gate
   that exists at pile scale (§3.3, §6.4). At the pile, EXTRAPOLATED: 1.18e13
   symbol comparisons, ~3.1 h at the measured rate, 2.6% of a 5-day build — and
   it can be run as a separate pass AFTER the container publishes, so a failure
   is a re-run of the checker, not of the build.
3. **Take T4's random base as well** (one compile-time constant becomes one
   runtime value). It costs nothing measurable and it closes §2.5, so the tier
   does not depend on the corpus being honest.

What the tier does NOT do, stated plainly so nobody budgets for it: it does not
shorten the pile build. The finding act (669 M-probes-per-GB, dependent random
access) is the LCE wall; the confirmation is a small linear-scan term on top of
it. If the pile build must get shorter, the lever is in the ladder (probe
count, checkpoint density, `tau`), not in the exactness split. What the split
buys is the *right to publish* at a scale where nobody can cmp against a bank.
