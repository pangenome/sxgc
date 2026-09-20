# sxgc — suffixient arrays for fast search at HPRC v3 scale

**Objective**: build and query **suffixient-array (χ) indexes** at HPRC v3
scale. **HPRC v2** (466 haplotypes, 1.4 Tbp, one AGC archive on disk) is the
development vehicle; **yeast235** (235 strains, 3.34 Gbp) is the unit gate —
χ = 85,404,240 (2.56% of n), `.sA` artifact = 341 MB (10.2% of text). The
suffixient array itself — the minimal-χ definition (Lean-proved
covering/minimality), χ as a repetitiveness measure, χ_tag for graphs — is
the project's novel contribution; everything else is substrate.

## Architecture at a glance

```
AGC archive (ragc, Rust)            the only sequence source; random access ~26 MiB/s
 │
 ▼  agc2flat --revlines [--samples K.txt]        (sxgc's own)
 │  one reversed contig per line ('\n' sentinel) = BCR collection format;
 │  same-pass sidecar with forward flat offsets; haplotype-subset CLI
 │  preserving archive order, aborting loudly on unknown names
 ▼  grlBWT                                        (adopted, GPL-3, external tool)
 │  semi-external BCR-BWT construction (Díaz-Domínguez & Navarro, CPM 2022);
 │  grammar+run-length compressed intermediates → RLBWT
 ▼  grlbwt2rle                                    (sxgc's own glue)
 │  runs as .syms/.len → renamed .bwt.heads/.bwt.len = TeraLCP rlbwt input format
 ▼  TeraTools: TeraLCP / TeraIndex                (adopted, MIT)
 │  LF, ψ, φ, φ⁻¹, LCP, PLCP and samples from the RLBWT in O(r) space, O(n)
 │  time, parallel; "Phi+samples" phase; --thr-pfp → pfp-thresholds-style
 │  5-byte threshold files; TeraIndex adds LF + inverse-φ (matching-statistics index)
 ▼  sxgc χ layer                                  (the product)
    one-pass suffixient scan over threshold/LCP structures → minimal-χ set;
    queries via the AGC text oracle (`-o agc`, 64 KiB window LRU);
    mappos → sample#contig:offset; anchor→φ-locate hybrid for
    all-occurrence enumeration
```

## Roles

| Layer | What | Provenance / license |
|---|---|---|
| `agc2flat/` | **sxgc's own** — AGC → flat text / `--revlines` BCR stream + `names.tsv` sidecar; `--samples` subset CLI | Rust (`ragc-core`) |
| `grlbwt2rle` | **sxgc's own** — grlBWT runs → TeraLCP rlbwt input (`.bwt.heads`/`.bwt.len`) | glue |
| χ layer (scan, sA builder, AGC oracle, `mappos.py`, Lean proofs) | **sxgc's own** — the product: minimal-χ suffixient arrays over the substrate | C++/Rust/Lean |
| AGC archive | adopted — the only sequence source; random access via ragc FFI | upstream: github.com/ekg/ragc |
| grlBWT | adopted — semi-external BCR-BWT construction; **external tool, not linked**; GPL-3 | Díaz-Domínguez & Navarro, CPM 2022 |
| TeraTools (TeraLCP, TeraIndex, TeraMEM/TeraMS) | adopted — RLBWT → (LCP, φ, samples, thresholds); matching-statistics index; MEMs | UCF S. Zhang Lab; MIT |
| `sA/` | submodule → pangenome/suffixient-array (fork of regindex/suffixient-array); integration patches land here | upstream fork |

The in-house PFP machinery (`pscan -S`, `pfp_suffixient`, `rindex_build`,
`rlbwt_sampler`) remains the **yeast-gated reference constructor** and the
χ-legacy path; it is superseded for production by the substrate above,
pending gates.

## Where to read next

- **`ARCHITECTURE.md`** — the architecture (substrate chain, component
  contracts), the full measured-constants tables, the risk register, the
  decision log, and the active rung-4a plan.
- **`BIT_LADDER.md`** — the gate record: current objective and state at the
  top, followed by the append-only history of every gate run.
- **`RESEARCH.md`** — χ_tag (suffixient sets over (context, tag) pairs) and
  graph-space research notes.
- Subproject specs: `oracle-agc/README.md` (AGC query oracle),
  `r-index-toehold/README.md` (all-occurrence enumeration),
  `tag-array/README.md` (graph-space projection).
