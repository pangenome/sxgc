# Radix-plan revision gates

Baseline pulled with `git pull --ff-only`: 380451dd4fc213ff56d4dcb6f6cdbcfca38152f1,
already up to date. Review branch: `paper/writeup`; no merge requested or performed.

## Q1 — PDF

```
export PATH=/tmp/paper-texlive/.TinyTeX/bin/x86_64-linux:$PATH
latexmk -pdf -interaction=nonstopmode -halt-on-error -outdir=/tmp/sxgc-paper-build paper/main.tex
```

Passed: 8 pages. Final log has no undefined references/citations, warnings,
overfull boxes or underfull boxes. PyMuPDF independently opened all pages.
The initial shell lacked latexmk on PATH; the existing isolated TinyTeX install
provided it. `paper/main.pdf` is the refreshed ignored build artifact.

## Q2 — algorithm and theory

- Algorithm: main.tex lines 53–192, PDF pages 1–3.
- Pseudocode: main.tex lines 104–132, PDF page 3.
- Theory subsection: main.tex lines 134–192, PDF pages 2–3.
- Includes systematic-sample estimator and limitations, local-order proof,
  explicit CPU/I/O bounds, and arena accounting with oversize-admission caveat.

## Q3 — numeric provenance

`python3 paper/audit_numbers.py` passed:
48 sourced claim IDs, 52 occurrences; 2 documented CLI parameters;
50 ledger quantities total; 0 missing or inconsistent values.
NUMBERS.md is unchanged. Formula constants are mathematical definitions,
not added empirical claims. No corpus benchmark was rerun.

## Q4 — thesis and emphasis

The abstract opens verbatim: “Suffix-array construction here is a radix plan
over the distribution of the string, not a monolithic algorithm.”
The abstract has no benchmark numbers. Evaluation retains exact sourced numbers,
negative results and the untested-full-Pile sentence. Algorithmic comparisons
cover SA-IS, DC3, prefix doubling, external and distributed construction.
Primary publications were consulted for the added bibliography entries.

## Q5 — publication

Revision is intended only for `paper/writeup`; commit and push receipts are
reported in the task result. `git diff --check` passed before commit.

## Manual notes and residual risks

- Source inspection contradicts an unconditional position-slice claim: full
  suffix comparisons within slices cannot order interleaving slices. The
  implemented fallback proves a single-symbol tail class and otherwise fails.
- Sampling is systematic, not iid. No probabilistic hard band-size bound is
  claimed; current oversize admission can exceed the arena target.
- Prefix widths are finite. Adaptation is not an unlimited-depth guarantee.
- The Pile/enwiki depth contrast is attributed to source rationale. A numeric
  depth histogram receipt was not available and none is invented.
- Historical build measurements do not freshly validate the revised envelope
  planner. The paper distinguishes them and retains prior receipt limitations.
- Full-Pile search, whole-build constant RSS, one-read input construction,
  and complete pipeline deadlock freedom remain unproved.
- Supervisor-contact tooling was not exposed. Work proceeded under the explicit
  instruction not to stop; no unsupported claims were introduced to resolve
  the mathematical discrepancies.
