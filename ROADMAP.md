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

## 6. Sequencing (2026-09-22; superseded by the §14 execution ladder)

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

---

## 7. The extrema theorem: "first appearance" is a build-time choice (2026-09-22, second session)

The suffixient set answers, for every distinct substring, its *extremal*
occurrence in text order. In a multi-string BCR collection, text order IS
the concatenation order — i.e., **the document sort**. Therefore:

- documents ordered **oldest-first** → χ + tag sidecar = first-ever
  publication of every distinct string in the corpus;
- **newest-first** → same binary, same walk, same gates → most recent
  mention of everything;
- by source quality → canonical/most-authoritative occurrence per string;
- by population/phylogeny (pangenome) → novelty attribution along a
  biological ordering.

Two builds (asc, desc) give both endpoints of every string's lifetime and
enable **phrase extinction**: not "when did this sentence first appear"
but "when did it die" — the last snapshot containing it. No existing
system answers that over a whole corpus at r-space cost.

Footnotes: string permutation changes the BWT (BCR sentinel convention is
what makes the order matter); R and χ shift only marginally under
permutation; with PFP-BWT adopted, one more build is ~8 GB scratch at
k=10-class scale, not TB. Anything BETWEEN the extremes (all mentions,
timelines, per-document lists) is interval enumeration = the r-index's
home turf (.ri4 samples) — χ gives O(1) endpoints, r-index the interior.

## 8. Sortable columnar extrema store; the χ-parallel tag sidecar

Framing: one build = one column = one sort order; each column precomputes
the extremal fact for every distinct substring; query = predecessor +
array lookup. With fast construction, per-quarter re-sorting of a whole
corpus becomes an operational routine.

Sidecar taxonomy (tags hang off true positions; the index stores only
what must be true):

- **(a) Boundary sidecar, O(#strings)** — tag per string + sorted
  boundaries; any position resolves by binary search. Already in the
  chain (--sidecar names.tsv). Always shipped.
- **(b) χ-parallel tag sidecar, O(χ)** — for every first-occurrence
  position, the tag of where/when it first appeared: at 466 ≈ 2.36 B
  entries x 8 B ≈ 19 GB (haplotype id / string ordinal); at web scale,
  keyed by (document, snapshot, date) this IS the first-appearance
  product table. χ query + sidecar = "when did this first appear" with
  zero text reads. The walk already holds names.tsv, so this is a
  post-pass, not new machinery. For the pangenome paper: the
  **novelty-attribution map** — which haplotype every novel substring
  first appears in — is a scientific artifact (population hotspots of new
  content), not just plumbing.
- **(c) Row-space tags (which documents contain P, un-located) = the
  document-listing problem** — does not compress for free (per-row
  storage is n-sized; per-run tags conflate documents across a run's
  rows). Real research line; park it. (a)+(b) cover the product promise.

Proposed rungs after the 466 walk lands:
- **chi_tags**: resolve χ positions through the boundary sidecar, emit
  per-novelty (string, haplotype) stamps; gate by independent
  re-derivation of sampled positions.
- **seed_project**: query sequence → MS walk (BWT, text-free) → maximal
  seeds → per-seed leftmost/stamped position (pred_S + verify); the
  mapper story and the web provenance story in one binary, oracle-gated.

## 9. The names are a corpus too: recursive r-indexing of metadata

At web scale the sidecar's *content* is itself TB-scale, wildly repetitive
text: Reddit usernames/subreddits/permalinks over 15.7 B items; Common
Crawl's 2.5 B URLs (~200 GB of domain-structured paths); Wikipedia page
titles shared across a billion revisions; repo/package names in SWH.
Feed the names as a multi-string collection into the SAME chain → a
second r-index sized by the names' novelty (GB-class), buying:
substring search over metadata ("all documents whose URL contains
/wiki/Talk:", prefix/autocomplete, seed-fuzzy match); and a trivial
join: name-index string ordinal ↔ boundary sidecar ↔ position ranges in
the main corpus, both directions O(log k).

The recursion terminates: the tag sidecar of the name index is a few
scalars. Every byte in the system is either raw public archive or an
r-space index proportional to novelty.

## 10. Construction speed as the operating lever

GPU-driven construction: nothing in the chain is fundamentally serial —
PFP/grlBWT phases parallelize; the χ walk's per-run-block structure is a
streaming memory pattern (a port, not a research problem). The research
problem that would trivialize it: O(r)-time χ (per-run interior LCP
minima without row visits). Fast construction makes the column store
operational: new snapshot → incremental parse → new columns per quarter.

## 11. System summary (one sentence)

A corpus of versioned text; per sort-order, an extrema store over every
distinct substring (χ + tags); an r-index for everything between the
extremes; the same pair applied recursively to its own metadata — all
proportional to novelty, never to bytes. The HPRC v2 466 build is this
system's reference implementation in DNA.

## 12. Naming (decided 2026-09-23) and the xsa front door

- Repo name `sxgc` (pangenome/sxgc) stays: historical, 83 MB of git history,
  the paper's artifact repo. GitHub redirects cover any later rename.
  "suffixient genome compressor" is preserved as a backronymic footnote only.
- The science carries **chi**: measure, law, and headline are chi(HPRC v2).
- Users type **`xsa`** — a Rust multiplexer binary (multiplexer precedent:
  git/samtools/bcftools/vg/odgi/impg). XsA = chi * sA ("the chi-stamped
  suffixient array"); artifact family `.XsA` (e.g. `h466.XsA`); tagline
  "xsa -- chi sa" (it/it: the store answers *who knows*).
  Collision audit 2026-09-23: free in Debian (nearest: xsane), crates.io,
  PyPI; "XSA" exists only as Xilinx Support Archive, SAP XS Advanced, and
  Xen Security Advisory numbering — all domain-disjoint. Precedent: `vg`
  collides with LVM on every Linux box and survives.
- Subcommand map (veneer-first; each shell-wraps the gated binary, then
  absorbs a native Rust implementation when warranted):
  `xsa build` (chain: agc2flat -> grlBWT/PFP-BWT -> TeraLCP -> teralcp_chi),
  `xsa stats` (chi/r/n-r + law check), `xsa query` (v4 count/locate),
  `xsa project` (MS seeds + leftmost stamps = seed_project),
  `xsa tags` (first-appearance sidecars, asc/desc extrema),
  `xsa ms` (standalone matching statistics), `xsa graph` (impg fusion,
  later). Short aliases q/p/t.
- Do NOT rewrite the 96-core C++ that is gated and running (teralcp_chi on
  the sdsl substrate, vendored TeraLCP patches, grlBWT, rpfbwt): the veneer
  calls them; native Rust absorbs the format readers first (.XsA/.ri4/sdsl
  deserialization, sidecars), then tags (post-pass), then the LF machinery
  only if a rewrite is ever justified.

## 13. syng = a sparse suffixient array (the impg bridge, made precise)

syng (Durbin; embedded in impg) is structurally the suffixient/decision
structure of the *syncmer-projected* corpus: project each haplotype onto
its (k=63, s=8) syncmer tokens; the dictionary = the projected corpus's
distinct-content set, the edges = its (context, choice) transitions, the
GBWT = its run-compressed BWT. (k,s) is the sparsification kernel; chi is
the full-resolution limit. Three real differences: (1) syng is tunable but
quantization-bound; chi is parameter-free and exact; (2) syng localizes
variation only to +/-k (a SNP destroys every containing syncmer; the walk
breaks but cannot say where within the syncmer); MS/chi anchors are exact,
tightening impg's ends-only BiWFA "trust the interior" pattern; (3) inverted
storage: syng materializes anchor content (khash dictionary), chi stores
positions and demands text access — each pays where the other doesn't.
Testable slogan (gate-sized, cheap, after the 466 walk): build syng + chi
at yeast235 and k=10; verify every syng graph branch projects into a
chi-witnessed fork and measure the fork content that (63,8) quantization
misses. If the projection is clean, the slogan is a lemma — the bridge
section of the impg/xsa paper.

## 14. Execution ladder (2026-09-23; supersedes the §6 ordering)

PFP-BWT is adopted (two-scale gates GREEN, BIT_LADDER 2026-09-22) and the
.ri4 sovereignty is gated (ft30 5/5, s200 inversion, sentinel-identity
erasure law recorded) — so the ladder reshapes around what is now proven:

- **T0 (in flight)**: 466 chi walk -> chain completes (samples, patterns,
  query, AGC oracle verdict) -> record chi(HPRC v2), law check at
  R=2.74B, commit. k=10 accessor gate (30 Gbp inversion, parallel).
  Then: sidecar-based window mode of rl_text_extract (production
  configuration: emitted bytes vs S-coords via names.tsv, no brute SA).
- **T1 (post-466 rungs, dependency order)**:
  1. `xsa` veneer — Rust multiplexer scaffolding; `stats`/`query` over
     the h466 artifacts. Everything else lands as subcommands.
  2. `chi_tags` — first-appearance (haplotype) stamps over chi +
     names.tsv (~19 GB post-pass). Paper figure + product table in one.
  3. **`names` index — the metadata r-index (rung, from §9)**: run the
     same chain over the name/metadata collection; at 466 it is trivial
     (38,790 names) but the rung ships the recursion — every index build
     emits its own names index, so substring search over metadata and
     the ordinal join (names -> boundary sidecar -> corpus ranges) are
     always present. Real target: web scale, where names are TB-scale
     (URLs, titles, permalinks). `xsa names` subcommand.
  4. `seed_project` — MS walk + stamped positions, oracle-gated vs AGC;
     the mapper + provenance engine in one binary.
  5. syng cross-measure — dictionary size vs chi vs r at yeast/k10;
     the impg bridge lemma.
- **T2 (strategic builds)**: PFP-BWT streaming front-end (kill the
  1.44 TB revlines + 2 TB temps); iteration-2 466 PFP-BWT rebuild
  (full-scale differential gate + v3 dress rehearsal + optional
  asc/desc extrema pair).
- **Parked research, merit order**: O(r)-time chi walk -> byte-alphabet
  substrate (web unlock) -> Lean Bit-1 proof completion.

## 15. CLI consolidation, xsa placement, and the r-space construction research program (2026-09-24)

### 15.1 One engine, four readouts: `xsa query` is the CLI

DECISION (supersedes the seed_project-as-separate-tool framing in §14):
all question-asking lives in ONE engine with escalating readouts of a
single backward walk:
    xsa query            exact:   all occurrences of whole strings (done,
                                  byte-identical at k=10)
    xsa query --ms        lengths: per-position longest match (the walk
                                  tracks survival instead of all-or-nothing)
    xsa query --mem       MEMs:    --ms seeds + one left-extension probe
    xsa query --project   stamps:  MEMs + chi/sidecar first-appearance
                                  join (the provenance readout)
`stats` and `tags` remain separate (reports over artifacts, not
questions to the index). Exact is the special case of --ms where
length = m everywhere. Gate order: --ms byte-exact vs the s200 brute
fixtures (tera_ms.len/pos, 10,000 reads), then --mem, then --project
oracle-stamped at k=10 vs the AGC (which also closes the revlines
forward-orientation caveat from chi_tags).

### 15.2 xsa placement: build here, graduate at release

DECISION: `sxgc/xsa` stays embedded during development — it is the
reference front door, and its development pace (several gated modes per
day) argues against repo friction. Graduation plan: when (a) the query
mode set stabilizes post---ms/--mem/--project and (b) the 466 result
is published, xsa moves to its own repo (pangenome/xsa) carrying the
Rust crate + format specs; sxgc remains the construction chain, gates,
and artifacts of record. Until then: users meet xsa through the sxgc
README's tool section. Naming already survives the move (xsa is
namespace-clean: Debian/crates/PyPI free, .XsA artifact family).

### 15.3 r-space construction of the whole thing: the open problem, stated

The remaining O(n)-time piece is the chi walk (per-row visits over the
r-index to find per-run interior LCP minima). If chi could be computed
in O(r) TIME from the rlbwt + phi structure, construction cost becomes
proportional to novelty: the v3+ refresh loop, GPU construction, and
the whole operating-cost story change shape. World-changing is the
correct adjective.

What we know now:
- PLCP along TEXT order is piecewise-LINEAR with O(r) pieces, slope -1
  per piece (the phi-interval machinery: lcp = PLCPsamples[interval] -
  (pos - start) inside each interval). The walk exploits this
  (incremental tracking), but still pays one step per row.
- Per-run interior minima reduce, on this structure, to: for each
  (run, phi-interval) intersection, the EXTREME text position in the
  intersection (min of a decreasing linear piece is at its largest
  covered position). So O(r)-time chi requires deriving, for every
  (run, interval) pair that intersects, that extreme position --
  WITHOUT enumerating the rows of the run (the scatter of a run's rows
  across intervals is where the O(n) hides).
- Unknown: whether the (run, interval) incidence can be compressed. The
  scatter encodes the run/DAWG structure; no known bound either way.
  Research program, in order: (1) prove or refute O(r) enumerability
  of (run, interval) extreme points (lower-bound attempt is honest
  progress: an impossibility result would redirect to O(r log r)
  approximations or sublinear-time chi); (2) if possible, build it;
  (3) Lean Bit-1 carries the definitions either way.
Both outcomes publish: an O(r) algorithm is a landmark; an Ω(n) lower
bound in a stated model closes the question the field will otherwise
keep asking. The current walk (38 h at 466) is the cost of not knowing.
