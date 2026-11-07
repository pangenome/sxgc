# Paper gates

Baseline: `c46a85217b9b269c91179cbebc396f96453181af`.
Review branch: `paper/writeup`. No mainline merge.

## P1 — PDF

Run from the repository root (pdflatex supplied by an isolated TinyTeX install):

```sh
pdflatex -interaction=nonstopmode -halt-on-error -output-directory=/tmp/paper-texlive paper/main.tex
pdflatex -interaction=nonstopmode -halt-on-error -output-directory=/tmp/paper-texlive paper/main.tex
```

Final result: **5 pages**, 401892 bytes. No undefined references/citations,
LaTeX/package warnings, overfull boxes or underfull boxes in the final log.
Engine: pdfTeX 1.40.29 / TeX Live 2026. Initial compilation exposed a missing
bitmap font and a small overfull box; using Latin Modern and rephrasing the
long compound resolved both. Final two passes succeeded.

The generated PDF is `paper/main.pdf` in the review worktree (ignored build
artifact, not committed). PyMuPDF independently opened all five pages and
extracted their text. The first page was visually inspected. No large corpus
build, benchmark or recovery experiment was rerun.

## P2 — numbers

```sh
python3 paper/audit_numbers.py
```

```text
P2 PASS: 48 sourced claim IDs, 60 occurrences; 2 documented CLI parameters; 50 ledger quantities total; 0 missing or inconsistent values.
P2 scope: structural conventions and non-empirical identifiers are listed separately; source truth and command limitations require manual review.
```

Manual review checked the committed source sections and separated measured
values from the unavailable original command wrappers. NUMBERS.md explicitly
labels those command gaps; a reproduction recipe is not presented as a
recovered historical invocation. Source citations in the manuscript are pinned.

## P3 — novelty wording

```sh
rg -ni '\b(novel|new|first)\b' paper/main.tex
```

```text
18:is measured scale and cost. On the first $\qnum{input}{10^{11}}$ bytes of the Pile, a
237:fanout example is evidence of an implemented application, not a claim of a new ranking principle. We
248:remain serving costs; the requested cold-first-answer time is unverified in the committed evidence
275:\bibitem{sa} U. Manber and G. Myers. \emph{Suffix Arrays: A New Method for On-Line String Searches}.
```

These refer respectively to an input prefix, a disclaimed ranking claim, an
excluded measurement, and the cited Manber–Myers title. The introduction also
explicitly disclaims conceptual novelty. The prior draft's lower-bound and
priority claims were removed.

## Manual findings and residual risks

- No blockers found in the rewritten manuscript or numeric crosswalk.
- The requested one-read corpus claim is contradicted by per-wave source scans;
  the paper states the narrower ordered-stream consumption claim and the
  existing input-file requirement.
- The 7.9% rowcode figure is row-byte savings in a partial Wikipedia probe,
  not completed-run total scratch. The paper uses the correct scope and the
  paired 3.48% scratch result.
- The 22,768 contexts/s figure exists in commit messages, but has no matching
  committed result-file/invocation receipt in this evidence set. The 145 s
  cold-start claim was not found. Both are excluded as UNVERIFIED.
- Whole-driver restart replays sort/emit/finish in the cited kill/restart gate.
  The mixed-unit work summary is not an elapsed-time or finish-reuse fraction.
- There is no full-Pile search certificate. The common-pattern certificate is
  sampled for locations, not an exhaustive index enumeration.
- Historical benchmark wrappers/settings are incomplete for several receipts.
  The paper attests committed reports, not independent fresh replication.
- The inherited bibliography had incorrect given names for the suffixient-set
  paper. The inline bibliography uses the authors on the primary arXiv record.
- Supervisor-contact tooling was not exposed; no decision was needed.
