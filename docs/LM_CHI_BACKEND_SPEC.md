# Research: Chi-index as ML backend (Emender ⇄ sxgc `.sxi`) — research + spec lane

Deliverable: **`/tmp/sxgc-laneX/docs/LM_CHI_BACKEND_SPEC.md`** (full integration spec, reproduced verbatim below).

## Summary

The sxgc suffixient index (`.sxi`: RLBWT + run table + SA samples + anchors + delta-compressed χ set, served by `xsa` with MEMs, MS vectors, name+offset annotation, and an HTTP server) can serve as the rulebook backend for ANY sequential model, with Emender (/home/erikg/emender) as the concrete instance: the index externalizes memorization (the bridge theorem says any continuation-answering system must consult ≥ χ positions; χ/n measured at 2.6% yeast, 5.4% k10), the network spends its capacity on fork policy via a three-mode FORCED/FORK/OFF-CORPUS loop with hard-masked candidate selection, witness pins as episodic pointers, and a MEM-op reasoning space. The integration is unusually clean because Emender's byte-level data path uses 0x1E as its document delimiter — the same byte the `.sxi` container reserves as record separator — and because `LadderLM.forward` already supports `loss_mask`/`reset_before`/`valid_mask`, so masked fork-conditioned pretraining needs no loss-machinery changes. This machine (AMD EPYC 7713, 1 TB RAM, ≥7× RTX 6000 Ada 48 GB ≈ 336 GB VRAM) can run the full staged plan at slice scale — Emender's own paper proves 1.273B trains at 7,492 tok/s on exactly one such GPU — but the full 1.31 TB byte-level pile run is honestly out of single-box reach (Frontier-class, as the E97 513B-token run was).

## Findings

1. **Claim (bridge theorem, proven form).** Any correct locate-one oracle emits ≥ χ positions: `emitted_suffixient` + `chi_le_of_oracle`, kernel-checked. **Sources:** lean/LowerBound.lean (Sxgc.lean Def. 9 `coversAt`/`requirements`/`suffixient`/`chi`). **Support:** direct evidence. **Confidence:** high.
2. **Claim (continuation-answering form is interpretation, not proven).** The "must consult ≥ χ positions" LM framing follows by the same requirement-covering pigeonhole but is not formalized; recorded as open question with a `CorrectContinuation` statement-lock candidate. **Support:** researcher inference, labeled as such in the spec. **Confidence:** medium (as a claim about provability).
3. **Claim (what a model may discard).** Given the index, the model may discard the n − χ bulk; measured χ/n: yeast 2.6% (85,404,240), k10 5.4% (1,627,067,257), 466 ~0.16%; χ/r ∈ [0.82, 0.88]; χ-floor χ·log₂(n/χ)/3 bits proven family-specific (`fam_floor_chi`). **Sources:** RESEARCH.md ledger. **Support:** direct evidence (gated artifacts). **Confidence:** high.
4. **Claim (Emender architecture, code-cited).** Per-head nonlinear delta-memory recurrence S ← tanh(dS + k(silu(v) − Sᵀk)ᵀ); heads are 32×32 matrix states; production 1.3B paper instance dim=1664/depth=12/H=370/N=32 (not the code-file defaults dim 2176/H 98); E97 = same core + mandatory split-edit gates. **Sources:** ndm/models/e88_fused.py, ndm/models/e88_fla_hybrid.py, ndm/models/e97.py, paper/main.typ §5. **Support:** direct evidence. **Confidence:** high.
5. **Claim (training-loop hooks already exist).** `LadderLM.forward` accepts loss_mask (→ −100 targets), reset_before/doc_boundaries, valid_mask, actual_length; byte-level vocab 256 default; TBPTT; schedule-free AdamW; DiLoCo for the no-NVLink PCIe fleet. **Sources:** ndm/models/ladder_lm.py forward body; train.py args. **Support:** direct evidence. **Confidence:** high.
6. **Claim (the alignment finding).** Emender's `DocumentStreamDataset` uses 0x1E as document delimiter (ndm/data/dataset.py); `.sxi` reserves 0x1E as record separator and rejects it in queries (bit6/SXI_QUERY.md). Model byte stream and index multi-string convention are the same contract; one policy decision remains (open question §5.8). **Support:** direct evidence + interpretation. **Confidence:** high.
7. **Claim (pins ride beside the NDM delta memory, not on it).** The 32×32 delta memory is a bounded learned associative store, not a pointer store; forcing corpus pointers through it would re-spend freed capacity; E97 split-edit gates are the (stage-3) candidate mechanism for pin maintenance. **Support:** interpretation grounded in cited code; explicitly labeled. **Confidence:** medium.
8. **Claim (compute reality).** Live probe: AMD EPYC 7713 64C/128T, 1,006 GB RAM, ≥7× RTX 6000 Ada 48 GB (buses 01/41/61/81/a1/c1/e1:00.0; minors 0,1,2,4,5,6,7 — 8th slot unconfirmed); NVIDIA driver 570.172.08. Emender's own anchor: 1.273B → 0.973 bpb on The Pile in ~23 days on ONE RTX 6000 Ada at 7,492 tok/s (median 100% util, 15.7% MFU). Slice-scale stages feasible here; full-pile byte-level is a cluster job (E97 513B tokens ran on Frontier 256n). **Sources:** /proc probe; paper/main.typ §4; docs/validation/e97-huggingface-dense-base-and-cli-agent-release.md. **Support:** direct evidence for hardware/anchors; inference for feasibility estimates (labeled in spec §4.3). **Confidence:** high for evidence, medium for estimates.
9. **Claim (stage-0 sweep cost).** Anchors: k10 (30 Gbp) from-scratch build ≈ 2 h-class / ≤134 GB peak on this box; pfp++ parse 30 GB ≈ 27 min; queries microsecond-class (gated). Inference: 100 GB pile slice index build ≈ 5–10 h (pile n/r 10–50× worse than DNA), per-position fork annotation ≈ 1–4 h, fork tables ≈ 0.3–2 GB → **stage 0 ≈ a day-scale CPU job per 100 GB slice**; full-pile sweep ≈ 5–13 days, gated on the slice verdict. **Support:** inference anchored on direct evidence, labeled. **Confidence:** medium.
10. **Claim (stage 2 needs no new build).** `k10.sxi` (33.96 GB, χ=1,627,067,257) and `yeast235.sxi` (χ=85,404,336) are published; the web→DNA index-swap transfer test consumes them directly. **Sources:** RESEARCH.md. **Support:** direct evidence. **Confidence:** high.
11. **Claim (falsifiable prediction).** Witness-density stratification: net+index advantage must concentrate in fork-dense strata and vanish in the FORCED bulk; uniform or absent advantage refutes the memorization-externalization thesis at that scale. Baselines: fork-histogram (n-gram), net-only, k-NN retrieval, net+index. **Support:** direct restatement of the RESEARCH.md design. **Confidence:** n/a (it is the test).

## Contradictions

- E88 geometry: code-file default (dim 2176, depth 14, H 98, ndm/models/e88_fused.py) vs paper's measured 1.3B run (dim 1664, depth 12, H 370, paper/main.typ §5). Resolution recorded in the spec: cite the run config, not class defaults.
- No substantive contradictions found between the sxgc ledger and the emender codebase. One scope caution recorded honestly: the published Emender/E97 checkpoints are p50k-token models; the byte-level v1 required for index alignment cannot transfer from them.

## Missing evidence

- Pile constants (n/r, parse, χ) unmeasured — all pile cost numbers carry that uncertainty; stage 0 exists to measure them.
- The continuation-answering bridge form is unformalized (locate-only is proven).
- Byte-level throughput (tok/s) for a 100–300M byte-level Emender is inferred, not measured — the 7,492 tok/s anchor is the 1.273B p50k instance.
- Possible 8th GPU unprobed (minor 3 gap); fleet treated as 7.
- Off-corpus evaluation methodology has no ground truth by definition — unresolved design question.

## Sources

- Kept: emender README, ndm/models/{e88_fused,e88_fla_hybrid,e97,ladder_lm}.py, train.py, ndm/data/dataset.py, paper/main.typ §4–5, docs/validation/e97-huggingface-dense-base-and-cli-agent-release.md; sxgc RESEARCH.md (LM-OVER-CHI-INDEX, pile recon, k10/yeast/466 ledger), lean/Sxgc.lean, lean/LowerBound.lean, bit6/SXI_QUERY.md; live /proc hardware probe.
- Rejected/deprioritized: E88 code-default geometry (superseded by the paper's run config, cited both).

## Next steps

1. Lean statement-lock: `CorrectContinuation` bridge analog (mechanical from `chi_le_of_oracle`).
2. Stage 0 pilot: 100 GB pile slice → `slice.sxi` + fork tables, measuring the pile's n/r, parse, χ (the missing constants).
3. Byte-level throughput measurement (one RTX 6000 Ada, 100–300M E97, hours) before stage-1 size commitment.

---

# Full deliverable (verbatim): LM_CHI_BACKEND_SPEC.md

# LM-over-CHI-INDEX — Integration Spec: a chi-index as the ML backend

**Emender ⇄ sxgc `.sxi`** — generic interface, concrete instance.

Status: **SPEC, not implementation.** Nothing here is built; the build comes after
the paper and the human index. No overclaims: every architectural claim about Emender
cites a code path; every index claim cites the sxgc ledger or a `.sxi`-gated artifact;
every estimate is labeled *direct evidence*, *interpretation*, or *researcher inference*.
Agents never commit.

Sources for this spec: `/home/erikg/emender` (read-only), `/home/erikg/sxgc/RESEARCH.md`
(the LM-OVER-CHI-INDEX section), `/home/erikg/sxgc/bit6/SXI_QUERY.md`,
`/home/erikg/sxgc/lean/Sxgc.lean`, `/home/erikg/sxgc/lean/LowerBound.lean`,
paper `/home/erikg/emender/paper/main.typ`, and a live hardware probe of this machine.

---

## 1. The generic design

### 1.1 The index as rulebook — the bridge theorem

The core thesis (RESEARCH.md, LM-OVER-CHI-INDEX): **memorization is externalized into
the index; network capacity is spent on policy** — the choice among forks — not on
rulebook storage.

The formal warrant is the χ-bridge, in two parts:

**(a) The definitions (lean/Sxgc.lean).** A set S of text positions is *suffixient*
for T (Def. 9) iff every *requirement* — a pair (w, c) where w is a right-maximal
string of T and `wc` occurs — is *covered* by some position x ∈ S:

```
def coversAt (wc) (x) (T)      -- `wc` is a suffix of T's prefix of length x
def rightMaximal (w) (T)       -- w occurs and has ≥2 right extensions (or is a suffix)
def requirements (T)           -- all (w,c): w right-maximal, wc occurs   (Def. 8/9)
def suffixient (S) (T)         -- ∀ requirement ∃ x ∈ S covering it        (Def. 9)
def chi (T)                    -- minimum |S| over suffixient sets          (brute def)
```

A requirement is exactly a *fork*: a context w that the corpus continues in more
than one way, plus one specific continuation c. The suffixient set is the minimal
set of corpus positions that witnesses every fork.

**(b) The bridge (lean/LowerBound.lean, kernel-checked).**

```
theorem emitted_suffixient (f) (T) (h : CorrectLocateOne f T) :
    suffixient (emitted f T) T = true

theorem chi_le_of_oracle (f) (T) (h : CorrectLocateOne f T) :
    chi T ≤ (emitted f T).length
```

Any correct locate-one oracle — *any* system that, queried with a word, returns a
genuine corpus position where that word occurs — must emit at least `chi T`
positions. This is the proven form. The design's continuation-answering form —
*any system that answers "what follows w" correctly for every fork of the corpus
must be able to consult at least χ positions* — follows by the same
requirement-covering pigeonhole (each answer for (w, c) points at a covering
position; one position covers many requirements; the minimum is χ), but this form
is **not yet formalized**. It is a mechanical statement-lock candidate
(`CorrectContinuation` analogous to `CorrectLocateOne`); recorded as an open
question (§5.1), not a claim. Interpretation label: the locate-one bridge is
direct evidence; the continuation framing is interpretation.

**What a model MUST retain vs may discard.** Given the index, the model may
discard everything except the policy-relevant state: the bulk of the corpus is
n − χ bytes. Measured witness densities (RESEARCH.md ledger, all gated):

| Corpus | n | χ | χ/n | witness artifact |
| --- | --- | --- | --- | --- |
| yeast (single-string pfp2 chain) | 3.34 Gbp | 85,404,240 | 2.6% | `yeast.sxi` (1.80 GB) |
| yeast235 multi-string (0x1E contract) | ~3.34 Gbp, k=9,901 | 85,404,336 | 2.6% | `yeast235.sxi` |
| k10 human pangenome | 30,151,407,545 | 1,627,067,257 | 5.4% | `k10.sxi` (33.96 GB) |
| 466 human (oracle on disk) | ~1.4 Tbp | 2,249,968,075 (historical convention) | ~0.16% | `chi_h466.sA` |
| the pile (web) | 1.31 TB | unmeasured; index projected 30–300 GB (n/r ~ 5–50 expected) | ~3–20% (inference) | pile recon only |

χ/r is measured in [0.82, 0.88] across corpora. The index is *novelty-priced*:
its size tracks forks, not bulk. The complementary result — a space *floor* of
χ·log₂(n/χ)/3 bits for a fixed-decoder index answering locate-one on the
witness-perturbation family (`Sxgc.LowerBound.Fam.fam_floor_chi`, kernel-checked) —
says the rulebook cannot be compressed below χ-scale either. The rulebook is
irreducible; everything else is discardable.

### 1.2 The generic interface: ANY sequential model ⇄ ANY chi-index

The interface is deliberately model-agnostic and index-agnostic. One party is any
system with a serialized state that consumes bytes and emits bytes/decisions; the
other is any structure that can answer, per context:

```
IndexServer (abstract ops — all served by xsa today, see §1.7):
  MS(ctx)          → matching-statistics length of the current context (0 = off-corpus)
  FORKS(ctx)       → the fork set C: distinct next-bytes of the context's interval,
                     each with occurrence count and ≥1 witness (doc, offset)
  WITNESS(pin)     → bytes at/after a pinned corpus position (doc, offset)
  STATS()          → n, k (records), r (runs), χ
```

The model maintains: its recurrent state; a bounded pin buffer of episodic pointers
(doc, offset); an op-policy. The index maintains: the rulebook (RLBWT + run table +
SA samples + χ set, one `.sxi` file). Neither knows the other's internals. The
contract is the three-mode loop (§1.3) and the op vocabulary (§1.6).

### 1.3 The three-mode generation loop (FORCED / FORK / OFF-CORPUS)

Per generated position, backward search against the index delimits the reaction
point exactly (RESEARCH.md, LM-OVER-CHI-INDEX):

- **FORCED** — the overwhelmingly common case (χ ~ 0.85·r ≪ n means most contexts
  admit exactly ONE next byte in-corpus). The mask dictates; the model passes the
  byte through. No choice, no compute spent on the choice.
- **FORK** — at witness-neighborhood decision points (contexts whose right-extension
  set has |C| ≥ 2): the fork set C, with counts and witness (doc, offset)
  annotations, is featurized into the model, which CHOOSES. Its expressive freedom
  is χ-shaped: the total number of fork decisions over the corpus is O(χ), not O(n).
- **OFF-CORPUS** — MS collapses to 0: the walk has left every known continuation.
  Free emission until re-entry; the index marks novelty exactly (the MS boundary
  is ground truth, not a guess).

Training = learning the FORK policy + when to leave and re-enter.

### 1.4 Hard-masked candidate selection

At a FORK position the model's output distribution is **hard-masked to C ∪ {exit
ops}**: logits for bytes not in the fork set are −∞. The mask is not a hint; it is
a correctness constraint from the rulebook — the corpus itself never continues
this context with a byte outside C, and off-corpus emission must be declared
(explicit `emit-new` op), never confused with a masked choice. Counts enter as
features (log-counts), not as a forced prior: the pure fork-histogram baseline
(§3) is exactly count-max, so any net+index win over it isolates the learned
policy's value.

### 1.5 Witness pins as episodic pointers; attention-as-query

Each committed walk step carries a witness (doc, offset) — one corpus position
proving the chosen continuation. A bounded rotating/circular **pin buffer** holds
the last P committed witnesses. This is the episodic memory: content-addressed by
the index, not stored in the network.

Hybrid extension (ablation-gated, not v1): **attention = query emission**. A head
emits offsets into the corpus; the index returns the bytes there ("what happened
next at this witness"); those bytes enter the state. Keys/values are not stored in
the context window — they live in the corpus, retrieved by position. This unifies
attention and index lookup: attention heads become pointer heads, KV memory becomes
the corpus itself.

### 1.6 MEM-op reasoning mode

The matched MEM content needs no re-reading (it equals the probe). What is fed
back is the witness **continuations**: the next m bytes after each candidate
MEM's target position, per-MEM encoded, then chosen among. The op vocabulary:

```
byte-fork        choose c ∈ C (hard-masked fork selection)
mem-copy         copy ≥1 bytes from a pinned witness's continuation
mem-with-edits   copy with bounded substitutions (a fork per edited byte)
jump-to-doc      move a pin: (re)bind to a witness of a new MEM interval
emit-new         off-corpus free emission (declared novelty)
```

Free supervision for the op policy (RESEARCH.md): haplotype pairs (and pile
near-duplicate families) ARE supervised MEM-op traces — the op sequence
reconstructing haplotype B from A is an alignment; mine it with the MEM
machinery. DNA mode: op-traces = variant recombination, annotated by nature.

### 1.7 What xsa serves today (the concrete index side)

Direct evidence, `bit6/SXI_QUERY.md` + RESEARCH.md query-product entries:

- `xsa mems --sxi ... --reads fq|fa|gz -j N`: bounded-memory rayon streaming;
  MEM(len, qstart, name, offset, strand) JSONL; name+offset annotation via
  boundary array.
- `xsa query --ms`: per-position MS vectors; `--mode auto` (DNA ↔ generic text);
  0x1E always rejected in queries (it is the record separator).
- `xsa serve`: POST /query, /ms, /batch (≤256 reads/2 MiB), GET /stats
  (n, records k, runs, χ, mode). **The server is the dataloader** (stage-1
  training pulls fork tables from it).
- One-file index: `.sxi` = RLBWT + run table + head/tail SA samples + anchors +
  delta-compressed χ set (magic SXI1, internally versioned). `k10.sxi` (33.96 GB,
  χ = 1,627,067,257 embedded) and `yeast235.sxi` are published; both are ready
  inputs for stage-2 (§3) with no new build.
- Query cost: "microsecond-class (gated)" per RESEARCH.md's production-path
  motivation entry; MEM brute-verified (46K+ MEMs, 23 fixtures + yeast 1,720).

---

## 2. The Emender concrete mapping

### 2.1 The substrate, with code citations

Emender (README, `/home/erikg/emender/README.md`) is a family of pure recurrent
language models built from many small nonlinear matrix memories — the nonlinear
delta-memory (NDM) mechanism. Per-head recurrence (README "The Memory Update";

implemented in `ndm/models/e88_fused.py` `E88FusedLayer.forward` PyTorch fallback
and the fused CUDA path, and documented in `ndm/models/e88_fla_hybrid.py`'s
module docstring):

```
k_t     = l2_norm(silu(k_t));  q_t = l2_norm(silu(q_t))
r_t     = S_{t-1}^T k_t            (retrieve)
delta_t = silu(v_t) - r_t           (delta correction)
S_t     = tanh(d_t·S_{t-1} + k_t delta_t^T)
y_t     = silu(g_t) * (S_t^T q_t)
```

Key architectural facts, each with its citation:

- **Heads/layers**: each `E88FusedLayer` owns `n_heads` independent matrix
  states S of size `n_state × head_v_dim` (default 32×32; `ndm/models/e88_fused.py`,
  `E88FusedLayer.__init__`). The LM stacks `depth` prenorm residual layers
  (`E88FusedLM`: embed → [RMSNorm → E88 layer] × depth → final norm → tied head;
  `ndm/models/e88_fused.py`). The production 1.3B paper instance is
  **dim=1664, depth=12, H=370, N=32** (`paper/main.typ` §5 table; the code-file
  defaults dim=2176/depth=14/H=98 are a different geometry — always cite the
  run config, not the class default).
- **The 1.3B-class proof of trainability**: E88 1.273 B trained on **a single RTX
  6000 Ada (48 GB)**, reaching **0.973 bits/byte on The Pile in ~23 wall-clock
  days**, sustained **7,492 tokens/s at median 100% GPU utilization, 15.7% MFU**
  (`paper/main.typ` §4 "Measured throughput and utilization"). Multi-programming:
  22,200 small recurrent programs per token (370 heads × batch 5 × depth 12),
  each a 32×32 register-resident state tile (§1 contributions).
- **E97 split-edit**: `E97SplitEditLayer` = the E88 shared core with mandatory
  `use_split_edit=True`, adding GDN-2-inspired independent key-axis erase/read
  and value-axis write gates (`ndm/models/e97.py`; `docs/E97_E88_KERNEL_NAMING_
  CLARIFICATION_20260802.md`). The E97 1.3B dense base was trained to a
  **513,013,841,920-token authority on Frontier (256 nodes)**, p50k tokenizer,
  146 tensors (`docs/validation/e97-huggingface-dense-base-and-cli-agent-release.md`).
  A 4B variant exists as training checkpoints (README).
- **Training loop**: `train.py` — schedule-free AdamW or AdamW+warmup+cosine,
  bf16, TBPTT with hidden-state carry (`--tbptt`, default `--chunk_size 512`),
  gradient checkpointing, chunked CE (`--loss_chunk_size`), DiLoCo periodic
  weight averaging for multi-GPU (`--diloco*`; on a no-NVLink PCIe box, DiLoCo
  recovers a ~62k tok/s independent ceiling vs ~31k for per-step DDP — train.py
  `--diloco` help text).
- **Model-protocol hooks that already exist** (this is the load-bearing
  integration luck): `LadderLM.forward` natively accepts `reset_before` /
  `doc_boundaries`, `valid_mask`, **`loss_mask`** (masked targets → −100), and
  `actual_length`, with validation that `loss_mask` may not select
  cross-document predictions (`ndm/models/ladder_lm.py`, forward body). Masked
  pretraining needs *zero changes* to the loss machinery.
- **Data path**: `DocumentStreamDataset` — memory-mapped raw bytes, byte-level
  vocab 256 (`--tokenizer` default None = byte-level, train.py), **0x1E as
  document delimiter**, boundaries respected for state resets, all bytes
  (including 0x1E) served as tokens (`ndm/data/dataset.py`).
- **Parameter-count machinery**: `create_ladder_model` sizes models by dynamic
  per-layer counting with dim configs 100m→(768, 1.5×) … 1.3b→(1792, 2×)
  (`ndm/models/ladder_lm.py`).

### 2.2 THE ALIGNMENT FINDING: one byte, two systems, same contract

Emender's data path uses **0x1E (ASCII record separator) as its document
delimiter** (`ndm/data/dataset.py` docstring). The sxgc `.sxi` container uses
**0x1E as its reserved record separator** ("The reserved separator 0x1E is
always rejected in queries", `bit6/SXI_QUERY.md`; the yeast235 seam-repair
publication is the 0x1E cyclic contract, RESEARCH.md). The model's byte stream
and the index's multi-string convention are therefore the **same byte-level
contract natively**: document boundaries in the training stream coincide with
record boundaries in the index, FORCED-mode dictates the 0x1E at document ends,
and `reset_before`/`doc_boundaries` in `LadderLM` align with `--boundary`-aware
MS queries. No re-tokenization layer, no offset bookkeeping between systems.
(Open question §5.8: the 0x1E dual-use still needs one policy decision — see
there.)

### 2.3 Input featurization: fork-set features into the recurrence

Per generated/training position t the index sweep supplies a fork record:

```
fork_t = { mode ∈ {FORCED, FORK, OFF},
           C_t ⊆ bytes (candidate set, |C_t| ≥ 1; empty+OFF = novelty),
           { log count_c : c ∈ C_t },
           ms_len_t, interval_len_t (log),
           witness_t = (doc_id, offset) of the committed continuation,
           doc features of witness_t (record length, position-in-record) }
```

Featurization (v1 "dumb-first" per RESEARCH.md's ablation discipline):

1. **Candidate multi-hot**: a 256-dim 0/1 vector over the byte alphabet (pile has
   207 distinct byte values, RECON entry — the vector is naturally sparse).
2. **Count/length scalars**: log-counts of top-k candidates, log MS length, log
   interval size, mode one-hot (3 dims).
3. **Witness features**: a small learned embedding of (doc_id mod-hash, offset
   bucket); NOT a raw pointer into the net's input in v1.
4. **Projection**: `fork_proj: R^{256+k+3+e} → R^dim` (a single new `nn.Linear`,
   mirroring the existing separate-GEMM style of `E88FusedLayer`'s `qkv_proj` /
   `a_proj` / `g_proj` in `ndm/models/e88_fused.py`), **summed with (or
   concatenated-then-projected onto) the byte embedding** before the first
   layer. This is the minimal, one-GEMM change to `LadderLM.forward`'s
   `x = self.embedding(inp)` line (`ndm/models/ladder_lm.py`).

Per-head conditioning happens *through* the standard projections: the fork
features modulate k/q/v/decay/gate indirectly because they are part of x that
feeds `qkv_proj`, `a_proj`, `g_proj` (`ndm/models/e88_fused.py`). No per-head
fork wiring in v1.

### 2.4 Where the hidden state carries the fork policy

The answer is architectural, not a bolt-on: **the recurrent state S (per head,
32×32, `ndm/models/e88_fused.py`) IS the policy carrier.** The delta-correction
write (`delta_t = silu(v_t) − S_{t-1}^T k_t`) is literally a *prediction-error
write*: it stores what the current read got wrong. In the integrated setting the
policy-relevant signal is exactly "which fork did I take, and what did the
rulebook say the alternatives were" — an error signal against the fork histogram.
The heads that would otherwise spend capacity memorizing co-occurrence rules
(the rulebook's job) are free to specialize on decision state: how far into a
boilerplate block we are, which duplicate family the current fork selected, how
long since the last off-corpus excursion. The memorization-tax thesis predicts
(RESEARCH.md scaling-law entry) that at fixed net size, loss-vs-index-presence
curves flatten: the capacity freed is reallocated to policy.

This is design interpretation, grounded in: the delta write's form (direct
evidence, cited code), the χ-freed capacity accounting (§1.1), and the paper's own
finding that loss is a poor guide to what a recurrent architecture computes
(`paper/main.typ` abstract) — which cuts both ways: **the fork-policy claim must
be measured (§3's stratified metrics), not asserted.**

### 2.5 Pointer/pin memory vs the NDM delta memory: rides BESIDE it

Honest architectural analysis. The NDM delta memory is a *bounded, learned,
content-addressed* matrix store — 32×32 scalars per head, tanh-latched
(`paper/main.typ` §1: "Tanh-with-latching… Memory persists… and remains
overwritable when a sufficient counter-delta arrives"). A **pin is an index into
the corpus**, an unbounded-address external object. These are different type
classes:

- **Do NOT map pins onto delta memories.** Forcing corpus pointers through a
  32×32 associative store would re-spend the capacity the index just freed, and
  the tanh latch is not an addressing mechanism.
- **The pin buffer rides beside the recurrence**: a P-slot circular buffer
  (model-side tensor of (doc_id, offset) pairs, P ≈ 16–64), written by the walk
  (deterministic, from the index's witness annotations), read by featurization
  (witness-continuation snippets fetched via `WITNESS` and encoded in v1) and —
  only in the ablation-gated hybrid extension (§1.5) — by pointer-attention.
- **The one genuine NDM-side opportunity**: E97's split-edit gates
  (`use_split_edit=True`, `ndm/models/e97.py` — independent erase/read and
  value-write gates) are a natural fit for *pin-buffer maintenance as a gated
  memory op* — erase/bind/release of pointer slots as a learned, gated write.
  This is a stage-3 candidate at most; v1 keeps pins purely deterministic.

Rule (RESEARCH.md): v1 = dumbest version (RNN + mask + features); pointer
attention only if the ablation demands it.

### 2.6 Training-loop changes

1. **Masked targets — already supported.** Fork-conditioned training scores
   next-byte CE with `loss_mask` semantics already in `LadderLM.forward`
   (`ndm/models/ladder_lm.py`: `target[~loss_mask] = -100`, plus the
   cross-document guard). What we add is the *construction* of the mask:
   (a) FORCED positions scored (cheap correctness: the model must agree with
   the rulebook — this is the pass-through contract); (b) FORK positions scored
   (the policy's supervision is the corpus's own choice — teacher forcing on the
   real text is automatically fork-supervised: the corpus took one branch of
   every fork); (c) OFF-corpus positions handled by the declared-novelty op
   (v1: score them as ordinary byte prediction; the novelty flag is a feature).
2. **Candidate conditioning**: the `fork_t` vector enters per position (§2.3).
   Batch shapes are unchanged: `[B, T, ·]` throughout (`E88FusedLM.forward` is
   already `[B, T]`-tokenized, `ndm/models/e88_fused.py`).
3. **Stratified logging**: per-position mode flags give, for free, loss
   decomposed by FORCED/FORK/OFF and by fork-set size |C| — the witness-density
   stratification of §3.4 falls out of the dataloader, not a separate eval pass.
4. **Op-policy traces (stage ≥ 2)**: MEM-op supervision from haplotype
   near-duplicate alignments (§1.6) enters as *auxiliary* targets (predict the
   op sequence between paired near-duplicate documents); the primary LM loss is
   untouched. Not in v1 scope.

### 2.7 Tokenizer/data pipeline

- **Byte-level, mandatory for v1.** The index is byte-exact; any multi-byte
  tokenizer breaks the byte ↔ fork-set alignment and re-introduces a
  memorization channel (BPE merges are corpus statistics the index already
  prices). Emender's default path is byte-level (train.py `--tokenizer` None →
  vocab 256; `DocumentStreamDataset`). Cost: byte-level runs ~3.8× more
  tokens/byte than p50k (the 3.783 bytes/token conversion constant in train.py
  `--heldout_bytes_per_token`); benefit: bpb is the native unit and the paper's
  0.973 bpb anchor is directly comparable.
- **Corpus**: the pile, `/mnt/nvme2n1/erikg/pile.txt` — 1.31 TB, 207 distinct
  byte values, English-dominant, unique-dominant (dup 0.002% systematic, 32-gram
  local dup 1–7%) — the **adversarial regime**: n/r ~ 5–50 expected, index
  30–300 GB (RESEARCH.md recon). Standing directive honored: **slice-first**
  (50–100 GB, one day) before any full-scale commitment.
- **The precomputed fork tables = the sweep products** (stage 0, §3.1): per
  position, a compact record `mode | C_t | counts | witness`. FORCED positions
  compress to run-lengths (the fork positions are witness-neighborhood-scale,
  ~χ of them; χ/n measured 2.6% yeast, 5.4% k10, projected 3–20% pile-slice).
  Two serving modes, both already buildable from `xsa serve`'s contract: (a)
  offline tables on disk next to the slice; (b) on-demand `POST /batch` from a
  long-lived server sharing one mmap'd index.

**Precompute-pass cost estimate (from the logs; label: inference anchored on
direct evidence).** Direct evidence anchors: k10 (30.15 Gbp) full from-scratch
build ≈ 2 h-class on this machine (slim pilot 1h36m49s / 73.98 GB peak; parse
adds ~27 min at 30 GB; from-zero peak RSS 133.9 GB); pfp++ parse of 30 GB human
in 27.4 min / 10 GB RAM; xsa yeast streamed sweep 15.36 s; queries
"microsecond-class (gated)". For a 100 GB pile slice (researcher inference):
index build ≈ 5–10 h CPU (pile n/r is 10–50× worse than DNA; the r-term grows,
the pipeline is r+parse-priced — RESEARCH.md v3 reduction); per-position MS/fork
annotation ≈ 1e11 positions × 1–5 µs / 128 threads ≈ 0.9–4.4 h; fork-table
storage ≈ 0.3–2 GB (χ-scale, run-length-compressed). **Net stage-0: a
day-scale CPU job per 100 GB slice on this box.** At full 1.31 TB scale the same
arithmetic gives ~5–13 days of sweep — doable but unjustified before the slice
verdict (the syng lesson, generalized).

---

## 3. Training plan

### 3.1 Stage 0 — fork-table sweep of the pile at slice scale (data prep)

1. Slice the pile: 100 GB contiguous slice (document-aligned at 0x1E), held-out
   5 GB validation slice from a distant offset.
2. `xsa build --text slice → slice.sxi` (one pass). Record n, r, χ, parse — the
   pile constants the RECON says are missing.
3. Sweep: per-position mode flag, fork sets, counts, witnesses → fork tables
   (run-length-compressed) + the server as fallback dataloader.
4. Gate: spot-verify fork sets against brute-force n-gram tables on sampled
   positions (the sxgc house style — every derived artifact differential-gated).

### 3.2 Stage 1 — masked pretraining

- Model: byte-level Emender `--level E97 --tokenizer` unset (byte vocab 256),
  100M–300M params (`create_ladder_model` 100m/200m configs,
  `ndm/models/ladder_lm.py`), TBPTT chunk 512–2048, bf16, schedule-free AdamW
  (train.py defaults).
- Input: byte + fork features (§2.3); loss: next-byte CE on all positions
  (FORCED included), stratified logging by mode and |C| (§2.6.3).
- Ablation arms (identical data, identical tokens): **net+index** vs **net-only**
  (fork features zeroed) — the memorization-tax measurement at fixed size; plus
  the net-size × index-presence grid (RESEARCH.md scaling-law entry) at 2–3
  sizes if the first pair shows signal.

### 3.3 Stage 2 — index-swap transfer (the headline)

Swap the index under the SAME weights: pile slice → second web slice →
**DNA pangenome** (`yeast235.sxi` and `k10.sxi` are already published — zero
build cost for this rung). Measure re-adaptation: frozen-weights bpb on the new
domain immediately after swap, and tokens-to-recover-X% of a from-scratch
net+index run on that domain. The claim under test: the net learned
**domain-agnostic fork navigation**, not web-specific rules. DNA mode brings
strand/revcomp (xsa handles it; the byte alphabet shrinks to IUPAC) — a
deliberately brutal transfer. Honest note: byte embeddings are shared; alphabet
distribution shift is part of the test, and a from-scratch-on-DNA control is
required to interpret the gap.

### 3.4 Baselines and the falsifiable prediction

Baselines (RESEARCH.md):
1. **Pure n-gram / fork histogram**: count-max over C_t — the index alone with
   the dumbest policy. This is the floor the learned policy must beat.
2. **Pure Emender (net-only)**: no index at any stage — the memorization-tax
   control at matched params/tokens.
3. **k-NN retrieval** (Infini-gram-style): longest-match continuation without
   the χ-theory framing.
4. **Net+index** (this work).

**Falsifiable prediction — witness-density stratification**: index-augmented
agreement (vs the corpus's own continuation, and vs net-only) **concentrates at
witness-dense regions**; stratified by local fork density (windowed χ-count per
kb), the net+index advantage should appear in the top strata and vanish in the
FORCED-dominated bulk. If the advantage is uniform (or absent in fork strata),
the memorization-externalization thesis is **refuted at this scale** — the index
is then merely a data-serving convenience. Secondary readouts: pile_set_name
per-component analysis (free labels); scaling-law flattening (net size × index
presence).

---

## 4. Compute reality

### 4.1 This machine (live probe, 2026-10 read of /proc; direct evidence)

- CPU: AMD EPYC 7713 64-core / 128 threads (`/proc/cpuinfo`).
- RAM: 1,006 GB (`/proc/meminfo`: MemTotal 1,056,599,340 kB).
- GPUs: NVIDIA driver 570.172.08 (`/proc/driver/nvidia/version`); **≥7 × NVIDIA
  RTX 6000 Ada (48 GB each)** at PCI buses 01:00.0, 41:00.0, 61:00.0, 81:00.0,
  a1:00.0, c1:00.0, e1:00.0 (device minors 0,1,2,4,5,6,7). Confirmed aggregate
  VRAM: 336 GB. Minor 3 is absent from the probed set — an 8th slot is plausible
  but **unconfirmed**; treat the fleet as 7. No NVLink: PCIe only (train.py's
  DiLoCo notes apply).
- Disks: the pile lives at `/mnt/nvme2n1/erikg/pile.txt` (1.31 TB), k10.sxi on
  nvme3n1 (RESEARCH.md).

### 4.2 Emender's own scale anchors (direct evidence)

| Fact | Value | Source |
| --- | --- | --- |
| E88 1.273B (dim 1664, depth 12, H 370, N 32), The Pile, p50k, ctx 2048 | **0.973 bpb in ~23 days on ONE RTX 6000 Ada** | `paper/main.typ` §1/§5 |
| Sustained throughput at 1.273B | **7,492 tok/s**, median 100% util, 15.7% MFU, 97% of 300 W | §4 "Measured throughput" |
| GDN chunked-scan control on same GPU | 8,248 tok/s, 18.4% MFU (Emender ≈ 91% of it) | same |
| E97 1.3B dense base | **513,013,841,920 tokens**, Frontier 256 nodes, step 2,322,520 | `docs/validation/e97-huggingface-dense-base...md` |
| Sparse-checkpoint backward | K=16 → ~16× activation shrink | §4 "Sparse-checkpoint" |

### 4.3 Feasibility verdict (researcher inference, anchored above)

**Feasible on this box — at slice scale, byte-level, ≤300M params, with the
stage plan as written:**

- Stage 0 (per 100 GB slice): ~1 day CPU (§2.7). Fits trivially in 1 TB RAM.
- Stage 1 (100M–300M byte-level, 10–50 GB slice): at 1.273B the fleet sustains
  7,492 tok/s/GPU; a 4–10× smaller byte-level model should sustain roughly
  15–60k tok/s/GPU (inference from the same kernels at smaller dim/depth —
  not measured). One 10 GB-slice epoch = 1e10 bytes ≈ **2–8 days single GPU**,
  or **hours-to-days across 7 GPUs with DiLoCo** (62k tok/s-class independent
  ceiling, train.py). A 50 GB epoch: ~1–4 weeks single-GPU, ~2–5 days on 7.
- Stage 2: **zero index-build cost** (published `.sxi` files); transfer/adapt
  runs are stage-1-class.
- **NOT feasible on this box**: full 1.31 TB byte-level pretraining. At
  7,492 tok/s a 1.3B byte-level model needs ~13 years single-GPU; even at
  optimistic small-model throughput across 7 GPUs it is a multi-month job.
  The honest statement: the full-pile rung is a cluster job (Frontier-class,
  per the E97 precedent), and per the standing directive it is not even the
  right next step — the slice verdict gates it.
- The machine does NOT lack GPUs; it lacks NVLink and the scale for full-pile.
  Options, ranked: (1) slice-scale on this box (recommended, matches the
  falsification goal — the witness-density stratification does not need
  terabyte scale to be decisive); (2) 7-GPU DiLoCo slice runs for
  epoch-throughput; (3) external cluster only if the stratified result
  demands scale replication.

---

## 5. Open questions (surfaced by this research)

1. **The bridge-theorem form.** `chi_le_of_oracle` is proven for locate-one
   oracles (lean/LowerBound.lean). The continuation-answering form ("must emit
   ≥ χ positions") is interpretation; a `CorrectContinuation` analog is a
   mechanical Lean statement-lock candidate. Until then the spec says "consult",
   not "proved", for the LM setting.
2. **Credit assignment for op-policies.** Teacher forcing on the corpus text
   supervises byte-forks for free, but composite ops (mem-copy,
   mem-with-edits, jump-to-doc) have many valid decompositions; the
   haplotype/pile-duplicate traces give ONE alignment, not the only one. Op
   supervision needs an ambiguity-tolerant loss or beam — unresolved.
3. **The off-corpus exit policy.** Who decides re-entry (index says MS=0; the
   model must choose when its free emission is done), and how is off-corpus
   quality evaluated when ground truth is definitionally absent? Open.
4. **Featurization depth.** Fork features pre-embedding (v1) vs per-layer
   conditioning vs gated cross-talk with E97 split-edit gates (§2.5) — the
   ablation ladder, cheapest first.
5. **Pins: injection vs attention-as-query.** The hybrid extension is
   ablation-gated; if fork features alone saturate the win, pointer attention
   may be unnecessary machinery.
6. **Byte-level vs the flagship checkpoints.** All published Emender/E97
   checkpoints are p50k; v1 must train byte-level from scratch for index
   alignment. No transfer from the 513B-token base is possible without a
   byte-level re-tokenization story. (A p50k-index variant is conceivable —
   fork sets over token strings — but breaks the byte-exact contract and is
   out of scope.)
7. **Throughput accounting.** 15.7% MFU at 1.273B is the paper's honest
   number; the byte-level 256-vocab head is cheaper but the 3.8× token
   multiplier means bpb-normalized wallclock needs its own measurement before
   any stage-1 size commitment.
8. **0x1E dual-use.** Emender serves 0x1E as a data byte AND uses it as the
   document delimiter; the index rejects it in queries and treats it as the
   record separator. The integrated loop must fix one policy: v1 proposal —
   0x1E is FORCED at record boundaries in generation, and never a fork
   candidate; flag for the spec's test battery.
9. **Dynamic corpora / χ non-monotonicity.** χ can DECREASE under appends
   (baab+`a`: 3→2, RESEARCH.md). Any training pipeline that swaps or grows
   index versions needs an explicit re-sweep and versioning policy; fork
   tables are not incrementally maintainable today.
10. **Cross-domain witness namespaces.** Stage-2's doc-id embeddings must not
    become web-specific features (the transfer test would then measure
    embedding breakage, not policy transfer). Consider leaving doc features
    OUT of v1 featurization entirely (position-in-record scalars only).
11. **Pile constants are missing.** n/r, parse, χ for real pile text are
    unmeasured (recon = expectation); stage 0 exists precisely to measure
    them; all pile cost numbers above carry that uncertainty.
12. **Fork-set candidate cap.** σ=207 (pile) vs 4 (DNA): the multi-hot
    featurization changes width across the stage-2 swap. Fix: fixed 256-dim
    multi-hot in both domains (sparse in DNA); verify in the test battery.

---

## 6. Evidence ledger

- Kept (load-bearing): `/home/erikg/emender/README.md` (mechanism, releases);
  `ndm/models/e88_fused.py` (per-head recurrence, geometry, fused path);
  `ndm/models/e88_fla_hybrid.py` (E88 design docstring, decay/gating);
  `ndm/models/e97.py` (split-edit identity); `ndm/models/ladder_lm.py`
  (LadderLM protocol: loss_mask/reset_before/valid_mask; create_ladder_model);
  `train.py` (byte-level default, TBPTT, DiLoCo, sched-free, chunked CE);
  `ndm/data/dataset.py` (0x1E document-stream, mmap, byte vocab);
  `paper/main.typ` (§4/§5: 7,492 tok/s, 100% util, 15.7% MFU, 23 days,
  0.973 bpb, dim1664/depth12/H370/N32, RTX 6000 Ada);
  `docs/validation/e97-huggingface-dense-base-and-cli-agent-release.md`
  (513B tokens, Frontier 256n); `/home/erikg/sxgc/lean/Sxgc.lean` (coversAt,
  requirements, suffixient, chi); `lean/LowerBound.lean`
  (CorrectLocateOne, emitted_suffixient, chi_le_of_oracle);
  `bit6/SXI_QUERY.md` (xsa mems/MS/serve/ropebwt3 contracts);
  `RESEARCH.md` (LM-OVER-CHI-INDEX section; pile recon; k10/yeast/466
  χ values; k10.sxi/yeast235.sxi publications; cost tables); `/proc`
  hardware probe (§4.1).
- Rejected/deprioritized: none of the read sources were rejected; the E88
  code-file default geometry (dim 2176/H 98) was deprioritized in favor of the
  paper's measured 1.3B run config (dim 1664/H 370) — cited both to avoid the
  classic default-vs-run confusion.
