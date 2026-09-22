# Roadmap: from pangenomes to versioned corpora (byte-alphabet scale-out)

Status note (2026-09-22): this document records the strategic direction that
emerged while the HPRC ladder was mid-flight. It does not change the current
rung (full-466 v2 is in progress; see BIT_LADDER). It exists so the
decisions, numbers, and sequencing below are written down before they fade.

---

## 1. The claim

Search over massive **versioned** corpora should cost *redundancy*, not
bytes. The machinery this project has proven at pangenome scale —
grlBWT/BCR → TeraLCP (O(r) LCP/φ) → χ (smallest suffixient set) + run-bound
SA samples + MEM/MS queries — generalizes from DNA to arbitrary byte
strings. The DNA work is the reference implementation on the hardest
available instance; the destination is versioned text corpora at 10 TB–2 PB
scale, queried by LLMs as first-class search planners.

What substring search over such corpora unlocks (and token-inverted indexes
cannot do at all):

- arbitrary-string lookup: exact phrases, code, CJK, mid-word, error-tolerant
  via MEM seeding — no tokenization in the path
- **provenance with receipts**: "this exact string, first appearing in
  document D, revision R, timestamp T" — leftmost-occurrence is χ's native
  operation (contamination audits, quote-tracing, plagiarism, fact-checking)
- retrieval as a primitive for LLM grounding at sub-millisecond per query
- a notable legal property: an r-index stores no text — runs and positions
  only; search without hosting the content

## 2. Measured ground truth (why we believe the math)

| Collection | n | R (runs) | n/r | χ | index artifacts |
|---|---|---|---|---|---|
| yeast235 | 3.34 Gbp | 100.9 M | 33 | 85.4 M | ~2.5 GB (~0.075%) |
| human k=10 | 30.15 Gbp | 1.86 B | 16 | 1.63 B | ~150 GB (5× text; low n/r) |
| human k=50 | 150.95 Gbp | 2.03 B | 74 | 1.75 B | ~250 GB (~0.17%) |
| human 466 (projected) | 1.44 Tbp | ~2.53 B | ~570 | ~2.2 B | ~150-200 GB (~0.01%) |

Empirical law measured across three independent constructions and an 18×
range in r: **χ ≈ 0.86 · r** (0.846 yeast, 0.874 k=10, 0.863 k=50).

Reading: index cost is a *few tens of bytes per run*; at pangenome-like
repetitiveness (n/r in the hundreds), total index ≈ 10% of text or better.
At low n/r (k=10), artifacts exceed text — the machinery only pays where the
corpus is genuinely versioned/repetitive. Target selection is therefore the
whole game: **versioned corpora are pangenomes with better metadata.**

## 3. Candidate corpora (sizes are sourced, not guessed)

| Corpus | Size | Repetitiveness | Notes |
|---|---|---|---|
| Software Heritage (all of open source) | **~2 PB compressed**, 27–28 B unique source files, 421 M projects, ~50 B-node Merkle DAG | extreme (forks, vendored deps, near-identical releases) | crawling already done, dedup'd, versioned, graph API |
| Common Crawl, one month, extracted text (WET) | **~7 TiB compressed / ~20–25 TiB raw**, 2.3–2.7 B pages; full archive >10 PiB since 2008 | cross-snapshot very high; single-snapshot moderate | free on S3; ships cc-index-tables |
| Wikipedia, all revisions | enwiki full history ≈ **10–20 TB raw** (1 B+ revisions) | extreme (adjacent revisions near-identical) | every revision a separate string = our BCR production convention, with timestamps |
| GDELT (global news) | 100+ TB historical to 1979; **updates every 15 min** | moderate-high | the "active web" firehose |
| Internet Archive Wayback | PB-scale historical page versions | high (snapshots) | the time dimension; pairs with CC |

Crawl providers / what to ask for:
- **Common Crawl** (free): "WET files + cc-index-tables (columnar URL index
  per crawl)". No crawling needed.
- **Internet Archive**: historical versions (Wayback) for provenance queries.
- **Software Heritage**: consume their dataset/API — the code corpus, done.
- Commercial (freshness beyond public feeds): Zyte, Bright Data, Oxylabs —
  ask for "WARC-format crawl, text-extracted, index/redistribution license."

Change/notify feeds (the "what should I index" infrastructure — all exists):
- Common Crawl's own index diff: ~750 M *new* URLs per monthly crawl
- GDELT DOC 2.0: world news as it appears, 15-minute latency, 100+ languages
- Wikipedia EventStreams: live push of every edit (SSE)
- GH Archive (gharchive.org): hourly dumps of all GitHub events (push = repo changed)
- WebSub (PubSubHubbub): push notification for RSS/blog updates

Strategic consequence: the expensive part (accumulating/archiving the
corpora) has already been paid by nonprofits. The missing layer is exactly
ours: an index whose cost is proportional to **what is new** in each
snapshot — which is what the χ machinery does for a living.

## 4. The product shape (recorded for later, not a commitment)

- **Open-core**: free quarterly snapshot indexes over public corpora
  (Wikipedia-history, Common Crawl WET, SWH slices), self-hostable; pay for
  freshness. Free tier alone changes what LLM data teams can do
  (contamination audits, provenance, quote-tracing) at self-host cost.
- **Tool API**: `search(s) → occurrences + (doc, version, date)`,
  `mems(read) → seeds`, `count(s) → n`. Position→(doc, revision, timestamp)
  resolution = the sidecar machinery we already run (names.tsv → provenance).
- **LLM in the loop**: the LLM is the query planner — decomposes questions
  into candidate strings, probes, gets exact provenance, iterates, answers
  with receipts. Per-query cost sub-ms against a resident index; undercuts
  per-query search APIs on the batch/forensics market (the LLM-centric APIs
  bundle search with generation; the search primitive itself is commoditizable).
- First wedge: **Wikipedia all-history MEM search with dated provenance** —
  byte-alphabet, versioned, high-n/r, public, and a community that would use
  "when did this sentence first appear" daily. Alternative first wedge: the
  SWH fork forest (highest repetitiveness on Earth).

## 5. Engineering gaps and the byte-alphabet decision

**Decision: full 256-byte alphabet is the target.** Every interesting
non-DNA corpus is bytes; DNA remains one easy alphabet case. Sequence:

1. **pfp++ reserved bytes** (0x00–0x02 internal): order-preserving remap at
   ingestion (b → b+3). Trivial.
2. **grlBWT / rpfbwt**: byte-agnostic already.
3. **TeraLCP's ropebwt3 substrate**: small-alphabet by design; at 256 symbols
   the per-block counter headers cannot fit. Fix: byte-native ingestion layer
   replacing rld — and we have already written the core of it
   (teralcp_chi's LF/rank machinery runs on 256-entry tables over raw
   bytes). ~weeks of gated work; the rld block-geometry law is documented in
   our vendored patches.
4. **teralcp_chi scan**: SIGMA 128 → 256; trivial. Walks/LF already 256-safe.
5. **χ theory / Lean machinery**: alphabet-parametric by construction.

Honest caveats:
- **Construction time is O(n)** (parse ≈ bandwidth-bound; χ walk ~18.2M
  rows/s at 96 cores → ~2 months at 100 TB single-box). Sharding makes
  construction embarrassingly parallel (BWT per shard; count/locate merge
  trivially; MEM merge standard) — a cluster non-problem.
- **The O(r)-time χ walk is the open problem**: per-run interior LCP minima
  currently cost a row visit. An O(r)-time computation would make
  *construction* cost proportional to novelty — the product's operating-cost
  curve, and the project's natural crown-jewel theory result.
- **Single-snapshot open web is not pangenome-repetitive** (n/r ~10–40):
  index may approach tens of % of text. Versioned corpora (history,
  snapshots, forks) are where 10% holds. This is target selection, not a
  weakness to engineer around.
- **PFP-BWT adoption** (in pilot at k=10) is the construction-side enabler:
  streamed parse from the archive (no flat text materialization), scratch
  ~10% of text instead of ~2×text. s200 gate already GREEN (r-pfbwt ==
  grlBWT run-for-run; see BIT_LADDER).

## 6. Sequencing (does not disturb the current ladder)

1. **Now**: finish the 466 rung (χ(HPRC v2) + oracle verdicts) — the
   reference implementation and the paper's headline.
2. **Next**: PFP-BWT validation at k=10 → adopt as v3 construction (kills the
   1.4 TB revlines + 2 TB temps).
3. **Then**: byte-alphabet substrate port (items above), gated.
4. **Then**: Wikipedia-all-history wedge demo: index + provenance sidecar +
   tool API + one LLM-driven notebook proving "find when this sentence
   first appeared" end-to-end.
5. **Later**: sharded construction, snapshot-delta refresh (the cc-index
   diff as the input feed), open-core packaging.

The 466 result and the χ≈0.86·r law are the credibility for everything in
this document; nothing here should delay them.
