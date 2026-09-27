# Research: CHI-NECESSITY LOWER BOUND RECON (theory frontier session)

**Deliverable:** the `LOWER_BOUND_PLAN` content requested by the task (formal statement design, literature map, ranked attack strategies, Lean-first scaffold), produced as a research brief with verification labels.
Workspace state read: `lean/Sxgc.lean` (through `chi_eq_maxClasses`, `chi_le_of_suffixient`, `chi_le_of_suffixient_mem`, Slice-4 covering machinery), `RESEARCH.md` (Bit-2 program, open problems, measured facts). Fresh at 5e24cc5; nothing committed.

## Summary

No space lower bound proportional to χ (nor even one proportional to r) is proven anywhere in the literature for **any** pattern-matching operation class — every published r- and χ-index (r-index, MONI, suffixient array) is an upper bound whose optimality is time-only. The recon found that the χ-necessity question is **not free-standing**: a MEM/locate-one floor at Ω(χ·f) is equivalent, up to logarithmic factors, to the open placement of χ relative to δ·log n, because Navarro's CPM 2023 structure already computes MEMs in O(δ log n) words. What the repo already has (`chi_eq_maxClasses`, `chi_le_of_suffixient`) proves χ is minimal among *suffixient position sets*; the missing piece for a floor claim over arbitrary *indexes* is a two-step bridge — (1) "the positions an index must emit form a suffixient set" (provable now, near-mechanically, from existing Lean pieces) and (2) a witness-perturbation counting argument (a genuinely new combinatorial construction).

---

## Part 0 — What is already in the repo (grounding; all labels are kernel-verified facts from `lean/Sxgc.lean`)

**PROVEN (exact forms and hypotheses):**

```lean
theorem chi_le_of_suffixient (T : Text) (S : List Nat)
    (hsub : S.Sublist (positionsT T)) (hs : suffixient S T = true) :
    chi T ≤ S.length
-- and the duplicates-allowed strengthening:
theorem chi_le_of_suffixient_mem (T : Text) (S : List Nat)
    (hmem : ∀ x ∈ S, x ∈ positionsT T) (hs : suffixient S T = true) :
    chi T ≤ S.length

theorem chi_eq_maxClasses (T : Text) (hT : positive T = true) :
    chi T = maxClassCount T          -- needs the alphabet-boundedness side-condition
```

Hypotheses, spelled out: `S` must be a list of valid 1-based positions of `T` (sublist of `positionsT T`, or membership in the `_mem` variant) and must satisfy Def. 9 (`suffixient S T`). `chi_eq_maxClasses` additionally requires `positive T` (characters in `1..SIGMA-1`, sentinel `0` reserved) — the fixed `SIGMA = 128` candidate table is a model artifact, so any "for all texts" floor statement must be alphabet-parametric (the file itself documents this).

**What these give:** χ is the exact minimum cardinality over *suffixient position sets*, and χ equals the number of distinct inclusion-maximal coverage classes, each with a private requirement (the privacy lemma is the load-bearing minimality mechanism, already kernel-proven via `count_le_of_disjoint_witnesses`).

**What is missing for a floor claim:** everything beyond position sets. No bit-space model, no query algorithm, no time measure, and no theorem connecting "an arbitrary index supports an operation class" to "the index determines ≥ χ positions". This is the entire gap between "χ = min cover size" (proven) and "χ is the information-theoretic floor" (open, nowhere proven in the literature — see Part 2, item 2).

**Measured facts (project-internal, as given):** χ/r = 0.847 (yeast: 85,404,240/100,904,881), 0.875 (k10), 0.821 (466); as-built sxi ≈ 18 B/run (yeast.sxi 1.80 GB / 100.9M runs) vs ~12.7 B/run engineering floor; delta-chi 12.99%. Calibration arithmetic (researcher inference, honest): a hypothetical proven floor Ω(χ·log₂(n/χ)) bits at yeast is ≈ 85.4M × 5.29 bits ≈ 56 MB ≈ **0.56 B/run** — the proven-floor candidate underwrites only ~4% of even the 12.7 B/run engineering floor; even the strongest plausible pure-χ floor (χ positions × log₂n bits ≈ 337 MB ≈ 3.3 B/run) underwrites ~26%. If "delta-chi 12.99%" reads as the delta-compressed χ-set's share of the 1.80 GB container, witnesses cost ~2.7 B ≈ 22 bits each — already *below* log n and within ~4× of the χ·log(n/χ) information floor; if it reads as a compression ratio vs raw χ·u64, witnesses cost ~8.3 bits each, ~1.6× the floor. Either way: **the engineering floor is a representational artifact of the format family, not (yet) a theorem; the honest theory floor, if provable, is χ·log(n/χ) bits ≈ 0.56 B/run at yeast.**

---

## Part 1 — FORMAL STATEMENT DESIGN

### 1.1 The operation class (the "suffixient operation class")

All operations are on a **static text T[1..n]**; patterns arrive **online** (left-to-right, à la the suffixient paper's Thms 18/19). In increasing power:

- **O_req** (the minimal core): for every requirement (w,c) of T — i.e., every one-character right-extension of a right-maximal string that occurs — the index must report **one text position x with T[i..j] = wc ending at x** (`coversAt (w++[c]) x T`). This is exactly the set of queries the covering predicate forces.
- **O_loc1** (the paper's headline): for every pattern prefix P[1..i] (streamed), report one occurrence position (or correctly report "none").
- **O_MS**: matching statistics with positions — for each i, the length ℓ_i of the longest prefix of P[i..] occurring in T, plus an occurrence position.
- **O_MEM**: enumerate all MEMs of P (with positions).

Implications (KNOWN, immediate): O_MEM → O_MS → O_loc1 → O_req (an answer to query pattern wc is an occurrence position of wc, hence covers the requirement). Crucially, **count does NOT sit in this chain**: count returns an integer, not a position, and the χ-minimality mechanism (privacy: distinct maximal classes need *distinct emitted positions*) is about positions. Any χ-floor must be stated for a position-reporting class. MS answers ARE occurrence positions of requirements: for P = wc, MS(P,1) returns (|wc|, x) since wc occurs, so `coversAt` holds at x. (Interpretation from definitions; the bridge lemma below makes it a theorem.)

### 1.2 The index model (two variants; space measured in **bits**)

- **M0 (standalone index):** the index D is an arbitrary bit string; the answer function f is a pure function of D (deterministic decode; randomized variants deferred). Cleanest for information-theoretic floors — answers are functions of D alone, so counting arguments apply directly.
- **M1 (the sA model, what the product actually is):** D bits **plus a free random-access oracle to T**; answers depend on D *and* T. This is the paper's model ("provided with a random access mechanism", O(χ) words index + oracle). Floors here must be **time-space tradeoffs** (cell-probe/communication), since positions are brute-force computable from the oracle: the statement becomes "either space(D) ≥ Ω(χ·f) or per-query time is ω(1)".

Floor statement shape: `space(D) ≥ c · χ(T) · f` bits, for which **f**:

| tier | f | matches | status |
|---|---|---|---|
| weak | Θ(1) | — | CONJECTURED, unproven anywhere |
| natural | Θ(log(n/χ)) | attractor-density text storage O(χ log(n/χ)) words/positions (KNOWN upper: every suffixient set is a string attractor, Cor. 12 of the paper); the sA's own text bound | CONJECTURED — this is the defensible target |
| strong | Θ(log n) | naive witness words (χ positions × log n bits ≈ 3.3 B/run at yeast) | CONJECTURED, and **the strongest tier already contradicted in practice** by the delta-coded witness cost (~8–22 bits/witness measured, < 32) — so f = log n is refuted as a *forced* floor by the project's own artifact, unless container overhead is counted in the op |

### 1.3 Candidate formal statements (graded)

- **(S0) PROVEN (repo):** χ T = min{|S| : S suffixient for T}; χ = #maximal coverage classes (for `positive T`).
- **(S1) Bridge (provable now, model M0/M1-independent):** if f is a correct O_req oracle (e.g., any O_loc1/MS/MEM index's answer function), then the positions f emits on requirement words form a suffixient set, hence χ T ≤ |emitted f T| (corollary of `chi_le_of_suffixient_mem` + `coversAt` semantics). This transfers the *proven* minimality from position sets to **answer functions** — the first rung below.
- **(S2) The floor (OPEN, the target):** any M0 index D of s bits answering all requirement queries correctly satisfies s ≥ c·χ·log(n/χ) (natural tier) — at minimum s ≥ Ω(χ) bits (weak tier; also open).
- **(S3) M1 tradeoff (OPEN, harder):** any M0∪oracle index answering O_req with per-query time t and space s satisfies s·t^Θ(1) ≥ Ω(χ log(n/χ)) or similar; cell-probe machinery required.
- **(S4) Operation separation (OPEN):** count is *not* in the chain — is there an O(δ polylog)-space count index with no χ dependence? If the SODA 2026 SA-interval equivalence yields a δ-space count, then any χ-floor is strictly a statement about position-reporting operations. This sharpens rather than weakens the program.

**Honest placement:** S2 is the conjecture "χ is the information-theoretic floor". Per Part 2 item 9, S2 for O_MEM is *equivalent up to logs* to the open problem "χ = O(δ log n)?" from the SPIRE-2025 measure paper — the frontier is exactly there.

---

## Part 2 — LITERATURE MAP (each entry web-verified this session unless flagged)

1. **Suffixient Arrays (the substrate paper).** Cenzato, Depuydt, Gagie, Kim, Manzini, Olivares, Prezza, "Suffixient Arrays: a New Efficient Suffix Array Compression Technique," arXiv:2407.18753, journal version in *Theory of Computing Systems* (2026). **Sources:** [arXiv abs](https://arxiv.org/abs/2407.18753), [arXiv HTML v2](https://arxiv.org/html/2407.18753v2). **Support: direct evidence** (abstract + HTML fetched and queried).
   - Defs 1/8/9/13 as formalized in `Sxgc.lean`; model = O(χ) words + free random-access oracle; supports online locate-one-per-prefix (Thms 18/19) and all MEMs (Thms 22/27/29/31); O(χ+g) words with an SLP of size g. **Count and full locate are not the sA's op class.**
   - **Lemma 10: χ ≤ 2r̄** (r̄ = equal-letter runs of the BWT of the *reversed* text; the ≤ 2r̄ run-boundary positions are suffixient). **γ ≤ χ** (every suffixient set is a string attractor), and text storage in O(χ log(n/χ)) words follows from attractor machinery (Lemma 11/Cor. 12).
   - **No lower bound anywhere in the paper**; explicitly open: whether O(χ) words suffice to *store the text* ("is χ reachable"). **Technique applicability to a χ-LB:** the Lemma-10 run-boundary construction is upper-bound machinery; the useful LB gadget is the **supermaximal-extension characterization of smallest suffixient sets (Lemma 34 / Def. 32)** — the paper's own minimality mechanism, the external analogue of our privacy lemma. Confidence: high.

2. **The measure paper.** Fujimaru, Navarro, Romana, Urbina, "Smallest Suffixient Sets: Effectiveness, Resilience, and Calculation," SPIRE 2025 + journal version arXiv:2506.05638. **Sources:** [arXiv HTML v4](https://arxiv.org/html/2506.05638v4), [SPIRE'25 PDF abstract page](https://users.dcc.uchile.cl/~gnavarro/abstracts/spire25.2.html). **Support: direct evidence** (HTML fetched and queried).
   - **KNOWN, verified:** δ ≤ χ ≤ 2r̄ (attributed to Depuydt–Gagie–Langmead–Manzini–Prezza, CoRR 2312.01359); **Lemma 11: χ ≤ 2r** (r = runs of the text's own BWT — this is the project's "chi ≤ 2r PROVEN"); χ = o(r) on Fibonacci words ("settles χ as a strictly smaller measure than r"); χ = Θ(n) on de Bruijn sequences (Lemma 5, via supermaximal-repeat count = 2^k); χ = Ω(g log n) hence χ incomparable with grammar/LZ measures (Cor. 5/6, Lemma 14); χ ≤ 2·χ(reversed text) (Lemma 10); sensitivity ±2 under append/prepend, non-monotone (Lemma 4; matches the project's baab+a measurement).
   - **Explicitly open:** is r = O(χ log χ)? is χ = O(δ log n)? χ vs bidirectional macro schemes b?
   - **Directly decisive for us (verified quote from the fetched text): no theorem shows χ is necessary space for any index.** The χ-floor question is untouched. Confidence: high.

3. **The r-index.** Gagie, Navarro, Prezza, "Fully-Functional Suffix Trees and Optimal Text Searching in BWT-runs Bounded Space," *J. ACM* 67(1), 2020 (with SODA 2018 predecessor "Optimal-Time Text Indexing in BWT-runs Bounded Space", arXiv:1705.10382). **Sources:** [arXiv 1705.10382](https://ar5iv.labs.arxiv.org/html/1705.10382), [JACM PDF](https://users.dcc.uchile.cl/~gnavarro/ps/jacm19.pdf), [U. Chile record](https://repositorio.uchile.cl/handle/2250/178684). **Support: direct evidence.**
   - O(r) words; count O(m log log_w(σ+n/r)), locate O(occ log^ε n). **"Optimal" = time-optimal within O(r) space; O(r) space-optimality is NOT proven** — no Ω(r) lower bound for count or locate exists in anything found. **Attribution correction (see Contradictions): the task's "Belazzougui-Canovas-Navarro r-index" is a misattribution**; the r-index is GNP. Adjacent real papers: Belazzougui–Cunial–Gagie–Prezza–Raffinot, "Flexible indexing of repetitive collections" (CiE 2017), and Belazzougui–Cunial measure results ("e = Ω(max(r,z,g))" as quoted in the JACM paper's intro). Confidence: high.

4. **String attractors.** Kempa, Prezza, "At the Roots of Dictionary Compression: String Attractors," STOC 2018, arXiv:1710.10964. **Sources:** [arXiv abs](https://arxiv.org/abs/1710.10964). **Support: direct evidence** (abstract).
   - γ lower-bounds every dictionary-compression measure (z, g, RLSLP, collage systems, macro schemes); **random-access time lower bound: O(α polylog n) space ⇒ Ω(log n / log log n) access time** (extends Verbin–Yu), with matching attractor upper O(γτ log_τ(n/γ)). **Technique applicability:** two-fold. (a) Since γ ≤ χ, attractor floors transfer *up to the gap between χ and γ* — insufficient alone (χ = Ω(g log n) on de Bruijn while γ ≤ g). (b) The **time-space tradeoff template** (an index of α-space cannot give fast access) is the machinery for Attack 4. Confidence: high.

5. **The z/r collapse (BWT conjecture).** Kempa, Kociumaka, "Resolution of the Burrows-Wheeler Transform Conjecture," FOCS 2020, arXiv:1910.10631. **Sources:** [arXiv abs](https://arxiv.org/abs/1910.10631). **Support: direct evidence.**
   - **r = O(z log² n)** for every text; first non-trivial relation between r and the runs of the reversed BWT (exact form not on abstract — full-paper read needed); O(z polylog n) conversion LZ77 → RLE BWT. **Applicability to χ-LB:** transfer directions are upper-bound directions (they bound χ ≤ 2r ≤ O(z log² n) etc.); no lower-bound use, but the **string synchronizing sets** machinery is the candidate tool if an Attack-1 family needs run-structure control. Confidence: high (statement), medium (mechanism details unread).

6. **THE COLLAPSE THEOREM.** Kempa, Kociumaka, "Collapsing the Hierarchy of Compressed Data Structures: Suffix Arrays in Optimal Compressed Space," FOCS 2023, arXiv:2308.03635. **Sources:** [arXiv abs](https://arxiv.org/abs/2308.03635). **Support: direct evidence** (abstract).
   - δ-SA: **full suffix-array functionality in O(δ log((n log σ)/(δ log n))) space, O(log^{4+ε} n) query time** — the same asymptotic space as random access (attributed by them to Kociumaka–Navarro–Prezza, *IEEE Trans. Inf. Theory* 2023), described as "collapsing the hierarchy … into a single point"; previously SA functionality needed O(r log(n/r)) (Gagie–Navarro–Prezza, J.ACM 2020).
   - **Applicability — the make-or-break question for our program:** "SA functionality" as stated = SA[i]-type queries. **Whether pattern-matching (count/locate/SA-interval/MS/MEM) collapses into δ-space is not settled by this abstract** and must be pinned by a full-paper read. If locate/count collapse to δ·log-space, then no Ω(χ)-floor can hold for count (χ ≥ δ, sometimes χ = ω(δ)); the χ-floor would then be a statement exclusively about position-reporting/maximal-context operations. Confidence: high (what's proven), with the boundary explicitly flagged as unverified.

7. **SA-functionality tradeoffs (lower-bound-adjacent).** Kempa, Kociumaka, "Explaining the Inherent Tradeoffs for Suffix Array Functionality: Equivalences between String Problems and Prefix Range Queries," SODA 2026, arXiv:2510.19815. **Sources:** [arXiv abs](https://arxiv.org/abs/2510.19815). **Support: direct evidence** (abstract).
   - First **bidirectional reduction**: SA queries ≡ prefix-select queries up to an additive O(log log n) query-time term; six problem pairs including **SA-interval queries, inverse SA, pattern ranking, lexicographic range**. **No space lower bounds in the abstract.** Applicability: the reduction/equivalence toolkit is exactly the "reduction from known bounds" machinery the task hypothesizes — if a prefix-query problem with a known floor is equivalent to one of our op-class primitives, the floor transfers. Currently the equivalences are tool-transfer, not yet floor-transfer. Confidence: medium-high (abstract only).

8. **MEM indexes (upper bounds bounding our conjecture).** Rossi et al., MONI (pangenomic MEM index, O(r) space family); Boucher et al., "MONI Can Find k-MEMs," CPM 2023 ([LIPIcs](https://drops.dagstuhl.de/entities/document/10.4230/LIPIcs.CPM.2023.26)); Navarro, "Computing MEMs on Repetitive Text Collections," CPM 2023 ([LIPIcs PDF](https://drops.dagstuhl.de/storage/00lipics/lipics-vol259-cpm2023/LIPIcs.CPM.2023.24/LIPIcs.CPM.2023.24.pdf)). **Support: direct evidence** (abstracts).
   - **The decisive one:** Navarro CPM 2023 — MEMs of P computable in O(m² log^ε n) time on a run-length grammar of size g_rl, and in O(m log m(log m+log^ε n)) time on **a locally consistent grammar of size O(δ log n)**. **Consequence (researcher inference, flagged as such): MEM-support is achievable in O(δ log n) words. Therefore an Ω(χ)-word floor for MEMs is possible ONLY IF χ = O(δ log n) — which is precisely the open conjecture of the SPIRE-2025 measure paper. The χ-floor program and the χ-vs-δ placement are the same problem up to log factors.** Confidence: high on the quoted result; the linkage is my inference.

9. **Sequence/succinct representation lower bounds (machinery).** The rank/select lower bounds for sequence representations ([TALG paper via gnavarro's site](https://users.dcc.uchile.cl/~gnavarro/ps/talg14.pdf) — authors not verified this session, flagged) and the grammar random-access cell-probe lower bound ([arXiv 1203.1080](https://ar5iv.labs.arxiv.org/html/1203.1080), "Data Structure Lower Bounds on Random Access to Grammar-Compressed Strings"). Applicability: components for tradeoff statements, not for a bare χ-floor. Confidence: medium (not author-verified; tangential).

10. **Project-internal KNOWN (from RESEARCH.md, not re-verified this session):** χ ≤ 2r̄ matches the literature's Lemma 10; the constructions (Cenzato et al. linear; Olivares–Navarro SPIRE'26 one-pass; Urbina sublinear O(n log σ/√log n + min(r,r̄)·log^ε n)) are all upper bounds with n-terms; the repo's open-problems section (χ_tag, non-monotonicity) is orthogonal to the floor question but reuses the same scaffold (Part 4).

---

## Part 3 — RANKED ATTACK STRATEGIES (χ-necessity)

### Attack 1 (rank 1): witness-perturbation family + answer-function counting (model M0)
- **Would prove:** (S2) — family-specific, then general: any M0 index answering requirement queries correctly uses ≥ c·χ·log(n/χ) bits. Family-specific form first (standard for measure-based bounds): a family F of texts with χ(T) = k where the k maximal-coverage witnesses are independently movable among ~n/k positions each, so |F| ≈ (n/k)^k and correct answer functions are unique per text ⇒ distinct D per text ⇒ s ≥ log|F| = Ω(k log(n/k)) = Ω(χ log(n/χ)).
- **Mechanism:** S1 bridge gives χ ≤ |emitted positions|; uniqueness (each supermaximal requirement has exactly ONE covering position) makes the correct answer vector unique per text; pairwise-incompatible answer vectors across F force injectivity of D into answer functions.
- **Main technical obstacle:** constructing texts with (a) Ω(k) supermaximal extensions whose witness positions are independently perturbable over ~n/k choices, (b) all within one text family of uniform χ = Θ(k), (c) pairwise answer-incompatibility (no single f correct for two family members — this is where uniqueness does the work; must ensure a wrong-position answer is detectable by *some* requirement query, i.e., the perturbation must change the requirements/coverage, not just positions).
- **First lemma to attempt (in Lean, executable via the existing brute-χ machinery):** a block family T(p₁..p_k) = B₁(p₁)·#·B₂(p₂)·#··· where block B_i(p) = a^p b a^{L-p} d-style gadgets each carrying a unique supermaximal extension; `#eval`-verify χ(T_p) = k + const, uniqueness of covers, and χ-invariance across the parameter grid; then statement-lock `family_counting` (Part 4).
- **Why ranked first:** it reuses the proven privacy lemma verbatim, needs no cell-probe machinery, and its combinatorial core is exactly what the repo's `#eval` batteries are good at falsifying cheaply.

### Attack 2 (rank 2): MS-position reconstruction (the operation class is MS/locate-one)
- **Would prove:** the same bit floor for the *actual* sA op class (O_MS / O_loc1), not just O_req; and, in model M1 with a free oracle, the target shifts to a tradeoff: extracting the χ witness positions requires Ω(χ) queries unless D stores them.
- **Mechanism:** MS with positions on requirement patterns returns coversAt-valid positions (see 1.1); a bounded transcript of MS queries determines a suffixient set (S1); add the observation that on the Attack-1 family, the MS answers *are* the unique witness vector.
- **Main obstacle:** in M1 the oracle is free — a time-unbounded adversary computes positions from T; the floor must be time-aware (cell-probe / one-round communication: Alice holds D, Bob holds T-oracle and P…), which is Attack-4 machinery. In M0 the attack reduces to Attack 1.
- **First lemma:** "MS-emitted positions are suffixient" — the MS analogue of S1, a two-line variant once S1 exists. Then: "one MS query per requirement suffices to recover a smallest suffixient set on family F."

### Attack 3 (rank 3): transference/conditional analysis — resolve χ vs δ·log n
- **Would prove:** either direction is a result. (a) If χ = O(δ log n) always, then the natural χ-floor is within a constant of the known representation floor, and the χ-floor statement becomes "MEM/locate-one needs Θ(δ log n) on families where χ = Θ(δ log n)" — provable via Attack 1 instantiated on de Bruijn-like families (χ = Θ(n) = Θ(δ log n) there). (b) If some family has χ = ω(δ log n), then **the Ω(χ)-floor for MEMs is REFUTED** by Navarro's CPM 2023 O(δ log n)-word MEM structure — a negative theorem, also publishable, and it would cleanly delimit the op classes for which a χ-floor can ever hold (e.g., maybe only online/I/O-efficient locate-one retains it).
- **Main obstacle:** both directions are hard, open problems posed by the measure paper's own authors; but the recon should force the choice: the floor program cannot proceed past family-specific results without settling this relationship or explicitly conditioning on it.
- **First lemma:** lower-bound the χ of a candidate family by its supermaximal-repeat count (de Bruijn: sre = 2^k, Lemma 5, already in the literature) and upper-bound its δ·log n — find the largest family where χ/(δ log n) is maximized; measure on project corpora first (χ/r ≈ 0.85 says nothing here — need δ and γ of the same corpora, currently unmeasured).

### Attack 4 (rank 4): cell-probe time-space tradeoff in the sA model (M1)
- **Would prove:** (S3): either D uses Ω(χ·f) bits or requirement queries cost ω(1) probes; the Kempa–Prezza random-access template (extends Verbin–Yu) plus the SODA-2026 prefix-query equivalences as reduction glue.
- **Main obstacle:** the known tradeoffs are for *representation* access (grammar/attractor), not for index query answering; adapting needs "MS-support ⇒ representation-power" — a sub-question with unknown status (does the op class force random-access-equivalent power? count/membership of all strings does NOT pin down text positions; the position-reporting ops plausibly do, via the witness vector, but this needs proof).
- **First lemma:** "on family F, the answer vector of a correct index determines the witness positions without oracle access" — then the M1 problem collapses to M0 for the family, and Attack-4 machinery is only needed for general T.

---

## Part 4 — WHAT TO FORMALIZE FIRST (minimal Lean scaffold; sketches, nothing fake)

The LB scaffold does **not** wait on the sorry ledger: S1 needs only PROVEN theorems (`chi_le_of_suffixient_mem`, `suffixient_of_witnesses`, `mem_rightExts`, `mem_requirements`). Order of work:

**Step 1 — the index model + operation class (definitions only, alphabet-parametric):**

```lean
/-- An answer function: pattern ↦ optional 1-based text position. -/
abbrev Answer := List Nat → Option Nat

/-- O_req correctness: every occurring word gets an occurrence-covering position. -/
def CorrectLocateOne (f : Answer) (T : Text) : Prop :=
  ∀ w, occurs w T = true → ∃ x, f w = some x ∧ coversAt w x T = true

/-- MS answer function (op class O_MS): per suffix, (length, position). -/
abbrev MSAnswer := List Nat → List (Nat × Nat)
def CorrectMS (g : MSAnswer) (T : Text) : Prop :=
  ∀ P i, ...  -- the length-ℓ prefix of P[i..] occurs at the returned position,
               -- ℓ is maximal, and the prefix occurs in T  (lcp-style spec, mirrors lcpOf)
```

**Step 2 — the bridge (S1; provable immediately from existing pieces):**

```lean
def reqWords (T : Text) : List (List Nat) :=
  (requirements T).map (fun p => p.1 ++ [p.2])

def emitted (f : Answer) (T : Text) : List Nat :=
  (reqWords T).flatMap (fun w => match f w with | some x => [x] | none => [])

-- needs a tiny lemma: coversAt w x T = true → w ≠ [] → 1 ≤ x ∧ x ≤ T.length
--   (follows from pref_length + coversAt_eq_true)
theorem emitted_suffixient (f : Answer) (T : Text) (h : CorrectLocateOne f T) :
    suffixient (emitted f T) T = true := by
  -- per requirement p: wc occurs (mem_rightExts), h gives an emitted x, coversAt holds,
  -- x lands in emitted via mem_flatMap; conclude by suffixient_of_witnesses
  sorry  -- routine from mem_requirements + mem_rightExts + coversAt plumbing

theorem chi_le_of_oracle (f : Answer) (T : Text) (h : CorrectLocateOne f T) :
    chi T ≤ (emitted f T).length :=   -- dedup variant; via chi_le_of_suffixient_mem
  sorry  -- requires the coversAt-position-range lemma
```

**Step 3 — bit-space model + the statement-lock candidate (no proof yet, honest):**

```lean
def Index := List Bool
def space (D : Index) : Nat := D.length
def Realizes (D : Index) (f : Answer) : Prop := ∃ alg, alg D = f  -- decode; agnostic to alg model

/-- Pigeonhole core: distinct texts with pairwise-incompatible correct answers
    force distinct small indexes. THIS is the shape of the eventual floor proof. -/
theorem family_counting (F : List Text) (s : Nat)
    (hex : ∀ T ∈ F, ∃ D, space D ≤ s ∧ ∃ f, Realizes D f ∧ CorrectLocateOne f T)
    (hincompat : ∀ T ∈ F, ∀ T' ∈ F, T ≠ T' →
        ∀ f, CorrectLocateOne f T → ¬ CorrectLocateOne f T') :
    F.length ≤ 2 ^ s

/-- TARGET (S2, statement-lock candidate; NOT a theorem yet):
    on the witness-perturbation family parametrized by k free position choices... -/
-- theorem chi_floor_bits (k : Nat) (n : Nat) (F : family k n) :
--   (∀ T ∈ F, ∃ D, space D ≤ c * k * log2 (n/k) ∧ correct D T) → ... → contradiction
```

`family_counting` is elementary (pigeonhole over decode functions) and provable now; `hincompat` is where Attack 1's combinatorics lives. The `#eval` route: brute-χ oracle + the 729-family battery pattern for a small parametric family (k ≤ 4, n ≤ 12) to check χ-invariance and answer-uniqueness before any proof grinding — the house methodology (four false models died to batteries this month; statement-lock only after the differential is green).

**Step 4 — only after 1–3:** MS variants, the M1 (oracle) model, and the family construction.

---

## Contradictions

1. **Task attribution vs. literature:** the task says "Belazzougui-Canovas-Navarro r-index". The r-index is **Gagie–Navarro–Prezza (J.ACM 2020)**; no BCN r-index surfaced in any search (related-but-distinct: Belazzougui–Cunial–Gagie–Prezza–Raffinot, CiE 2017 "Flexible indexing of repetitive collections"). Recorded, not silently resolved.
2. **χ-floor vs. the collapse program:** the FOCS 2023 collapse theorem puts *SA functionality* at δ-space, which superficially threatens any χ ≥ δ floor. Resolution (provisional, researcher interpretation): "SA functionality" = SA[i]-type point queries; pattern locate/count/SA-interval status in δ-space is not settled by the abstracts read. **Unresolved; full-paper read required before the floor conjecture is stated in final form.**
3. **Strong-tier floor vs. the project's own artifact:** f = log n ("each witness costs a full position word") is already contradicted by the delta-coded witnesses (~8–22 bits vs 32) — the strong tier cannot be a *forced* floor; the defensible target is the natural tier.
4. No contradictions found *within* the verified literature itself (the χ ≤ 2r vs χ = o(r) pair is consistent: universal upper bound + strict-separation family).

## Missing evidence

- Whether count/locate/SA-interval collapse to O(δ polylog) space (full read of arXiv:2308.03635, 2510.19815 needed) — the single most decision-relevant unknown.
- Exact provenance/statement of the "asymptotically smallest space to represent a string" lower bound (Kociumaka–Navarro–Prezza, IEEE-IT 2023 — cited by the collapse abstract; not independently fetched).
- No ν (suffix-tree node / right-maximal-string count) ↔ χ bound found in either χ paper; the task's hypothesized "chi-vs-node relationship" reduction has no published anchor — any ν-based attack must first establish the relation.
- Whether the 12.7 B/run floor derivation in the project includes the run-structure (r) components or only χ — affects how much of the floor a χ-theorem would underwrite.
- δ and γ of the project corpora (yeast/k10/466) are unmeasured; without them the practical gap χ vs δ log n — the crux of Attack 3 — has no project-side datapoint.

## Sources

- **Kept:**
  - Suffixient Arrays, arXiv:2407.18753 (HTML v2) — the substrate paper; model, op class, χ ≤ 2r̄, γ ≤ χ, open problems. Direct evidence.
  - Smallest Suffixient Sets, arXiv:2506.05638 (HTML v4) — the measure paper; δ ≤ χ, χ ≤ 2r, separations, and the explicit absence of any lower bound. Direct evidence.
  - Gagie–Navarro–Prezza JACM 2020 / arXiv:1705.10382 — r-index space/time and the true optimality claim (time-only). Direct evidence.
  - Kempa–Kociumaka arXiv:1910.10631 (FOCS 2020) — r = O(z log² n), the z/r collapse. Direct evidence.
  - Kempa–Kociumaka arXiv:2308.03635 (FOCS 2023) — the collapse theorem; the frontier's boundary. Direct evidence (abstract-level).
  - Kempa–Kociumaka arXiv:2510.19815 (SODA 2026) — prefix-query equivalences; reduction toolkit. Direct evidence (abstract-level).
  - Kempa–Prezza arXiv:1710.10964 (STOC 2018) — attractors; random-access tradeoff template. Direct evidence (abstract-level).
  - Navarro CPM 2023 MEMs (LIPIcs) — the O(δ log n)-word MEM upper bound that links the χ-floor to the χ-vs-δ open problem. Direct evidence (abstract-level).
  - MONI / MONI-k-MEMs (CPM 2023) — MEM upper bounds in r-space, calibration. Abstract-level.
- **Rejected/deprioritized:** the TALG sequence-representation bounds paper (authors unverified, tangential to a χ floor); Urbina arXiv:2607.00204 and Olivares–Navarro SPIRE'26 (constructions, upper bounds only — kept as project-internal notes, not re-verified); R-enum / OptBWTR (engineering-facing, no LB content); the ar5iv "e = Ω(max(r,z,g))" quote-context (secondhand, not needed for the plan).

## Next steps

1. Full-paper read of arXiv:2308.03635 + 2510.19815 to pin the collapse boundary (does pattern count/locate have a δ-space index?) — this decides the final form of every floor statement.
2. Lean Step 1+2 (index model + bridge `emitted_suffixient`/`chi_le_of_oracle`) — mechanical, stands on proven theorems only.
3. #eval battery for a small witness-perturbation family (χ-invariance + unique answers + pairwise incompatibility) — Attack 1's go/no-go.
4. Measure δ (and γ if cheap) on yeast/k10 slices to place χ vs δ log n empirically before choosing Attack 1 (family-specific) vs Attack 3 (transference).

## Supervisor coordination
None needed — recon complete, no blocking decisions; the one adjudication-relevant discovery (the χ-floor ⟺ χ-vs-δ-log n linkage via Navarro CPM 2023, and the r-index attribution correction) is recorded above in the deliverable itself.
