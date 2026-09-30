# The χ Fork Matrix: Suffixient Arrays as a Genotyping Platform (Paper Two Vision)

## The object

Project every haplotype of the corpus onto the χ witnesses:

- **Rows:** the strings of the collection (466 haplotypes / 38,790 sequences at HPRC scale).
- **Columns:** the χ witnesses — the decision points of the corpus (χ = 2,250,211,129 at HPRC-466).
- **Entry (h, i):** the coverage class haplotype h falls into at witness i — which branch the
  haplotype takes at that fork.

This is the **χ fork matrix**: a sparse, provably complete, provably minimal decision-point
panel for the pangenome. It is the natural substrate for association studies, generation,
imputation, and population structure — taken directly from the index, with no alignment
pipeline and no variant-calling heuristics.

## Why χ specifically (the theory that qualifies it)

1. **Completeness (proven, Rossi et al. + our formalization):** every coverage event in the
   corpus is witnessed by some χ position. No context-fork that distinguishes collection
   members can be missed by a χ panel. SNP/variant panels miss structural context by
   construction; χ misses nothing by theorem.
2. **Minimality (proven here — `fam_floor_chi`, Lean):** the suffixient family carries the
   first space lower bound. The multiple-testing burden — the practical enemy of GWAS — is
   bounded below by the same theorem that certifies the panel is not padded.
3. **Measured:** χ/n = 0.1604% (HPRC-466), χ/R = 0.8214; χ/R constant ≈ 0.77 on web text.
   The witnesses are ~4× fewer than runs and ~600× fewer than positions.

## The free lunch: extraction is a walk, not an alignment

At a fork, the BWT's run structure restricted to the witness's interval **is** the haplotype
partition — strings sitting in the same run share the same branch, by the definition of a run.
Therefore the fork matrix is extractable directly from the published artifact (`hprc.sxi`,
52 GB) with O(witness) run-walks and zero per-haplotype alignment. The index built for search
is already the genotyping platform; it needs only the extractor.

Planned surface: `xsa forks --jsonl` over an SXI2 artifact → sparse (string_id, witness_id,
class_id) records. The EF-χ member of v6 + the move machinery provide the machinery.

## Generation ("PropGen-type"): haplotypes as paths through decisions

A haplotype is a consistent path through fork decisions. Therefore:

- **generation** = sampling consistent paths (LM over the fork alphabet; the
  LM-over-χ backend spec, `docs/LM_CHI_BACKEND_SPEC.md`, transfers from webtext to DNA);
- **imputation** = filling missing branches from fork correlations;
- **population structure** = the same matrix, PCA'd.

The fork table is the substrate; the LM machinery is the same machinery.

## Honest caveats (for the paper, not the brochure)

- χ columns are corpus-complete, not population-curated: effective independence will be far
  below the nominal column count. Local redundancy = haplotype block structure — itself
  informative, but kinship/PCA control is mandatory, and the effective-independence
  accounting must be done, not asserted.
- Association at a fork marks a **context locus**, not a causal variant — the standard
  pangenome-feature caveat, sharpened by completeness.
- Phenotypes must be joined externally (HPRC annotations).

## Concrete next step (when the running gates land)

1. `xsa forks` extractor over `hprc.sxi` → sparse 466 × χ fork matrix.
2. Smoke GWAS: simulated phenotypes with injected association at known loci → power curves
   vs. SNP-panel baseline.
3. Small-scale real test on yeast235 (phenotype labels per collection member).

## Positioning (one line)

*The suffixient array as a genotyping platform: a provably complete, provably minimal
decision-point panel for pangenome association, extracted as a walk over a 52 GB artifact
covering 1.4 Tbp.*
