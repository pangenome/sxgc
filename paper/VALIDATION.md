# Final complexity and cache-coherence pass

Baseline: `6703ed511567e6687ab4be2f8a2fa74e55d31aff`; `git pull --ff-only` was already up to date.
Branch: `paper/writeup`; no merge.

## R1: build

`PATH=/tmp/paper-texlive/.TinyTeX/bin/x86_64-linux:$PATH latexmk -pdf -interaction=nonstopmode -halt-on-error -outdir=/tmp/sxgc-paper-build paper/main.tex`

Passed: 9 pages; no undefined references/citations, LaTeX warnings, overfull or underfull boxes.

## R2: verbatim abstract and complexity/coherence paragraphs

The following are exact TeX source quotations, including evidence footnotes.

```tex
Suffix-array construction here is a radix plan over the distribution of the string, not a
monolithic algorithm. A positional stride sample estimates prefix populations; quantile fences
and a memory-derived row budget determine bands, while a recursive census determines wave split
depth. The radix suffix sort scatters positions into ordered key ranges, sorts full suffixes
locally, and emits BWT runs, LCP, and suffixient witnesses through one ordered-stream pipeline,
without a materialised global suffix array or an additional text copy. The algorithmic property
is adaptation: prefix depth and work-unit size follow the corpus distribution and resource
envelope. Sampling supplies estimates, not worst-case capacity guarantees; position slicing
requires an order proof, and the implementation has finite prefix limits. The Pile evaluation
validates this design on a skewed corpus and reports resource costs, negative results, and
recovery limitations; a full-Pile deployment remains untested.
The local sorting comparison count is $O(n\log B)$ with $B$ bounded by the memory
budget, so for fixed memory $O(n)$, conditional on actual bands meeting that
memory-derived row bound; planning, repeated scans and suffix-symbol inspection are
additional costs.
```

```tex
\paragraph{Resource-parameterised work.}
Let $B\geq 2$ bound the actual rows per band and let $K$ be the band count.
When admission enforces the memory-derived target $R$, take $B=\max(2,R)$.
Then $\sum_i n_i\log(n_i+1)=O(N\log B)$: $O(n\log B)$ with $B$ bounded by the memory
budget, so for fixed memory $O(n)$. The comparison-sort logarithm applies to
$B$, never to $n$; its constant-size factor is set by the resource envelope.
This is not a new asymptotic bound for suffix sorting in general. Adversarial
strings and underestimated populations can produce large bands in the current
implementation. The architectural claim is that the target bound's parameters
come from the resource envelope rather than from $n$, conditional on successful
refinement and admission, not that sampling proves a hard bound.

To state total cost rather than just comparison count, let $m=|S|$, $d$ be the
maximum inspected planning depth, $P$ the number of full corpus scans, $C$ the
corpus-symbol inspections for suffix comparisons and adjacent LCP, and $H$ the
work of wave predicates and band-fence searches. A conservative bound is
\[
 O(md\log(m+1)+Pn+H+K+n\log B+C),
 \qquad H=O(Pnd+Nd\log(K+1)).
\]
Final-container encoding adds its own work. Thus the construction's local sorting
component is linear in $n$ at fixed memory, but total elapsed work is linear only
if these other terms are linear too. In particular, repeated full scans cannot
be absorbed into a constant when the number of waves grows with $n$.

\paragraph{Depth, passes, and I/O.}
Depth is determined by how much key prefix the corpus distribution requires to
separate a class under the row target; it is a separate parameter from input
length, not a prescribed $\log n$ recursion. It need not remain constant as a
corpus grows. Each position is scattered once per wave it belongs to (exactly
one in a disjoint completed plan); each band is sorted once and its output is
streamed once. The implementation nevertheless scans the entire corpus for each
wave, plus preparation and explicit point-mass proofs. If $V_s$ is total scatter
payload bytes and $V_o$ total sorted-band payload bytes, writing and reading each
payload gives sequential volume $O(Pn+V_s+V_o)$, plus final-output bytes.
For disk blocks of $L$ bytes this is $O((Pn+V_s+V_o)/L+K)$ transfers allowing
partial band blocks. Corpus probes add cache misses, conservatively at most
$O(C+H)$ further transfers.

\paragraph{Cache coherence and its boundary.}
The bulk data path streams the corpus forward, appends band payloads through a
bounded number of sequential output streams, sorts each admitted band in RAM,
and emits in key order. Its ordering state is bucket-local: no global pointer
chasing, tree walk, or random access over an $n$-row suffix-order structure is
required. This locality is a first-class systems advantage of choosing bands
from the cache and memory envelope. The text need not be wholly resident and
has no additional materialised copy; the existing corpus is file-backed.
This does not mean every text access is sequential: full suffix comparisons and
LCP computation can probe distant mapped corpus pages, whose residency and
misses remain real costs. In comparison-based suffix sorting a comparison may
walk a long common prefix, so its effective cost is an LCP-length scan, not a
unit-cost compare; banding confines row workspace but does not eliminate this
cost. Usual monolithic RAM forms of SA-IS, DC3 and prefix doubling maintain
whole-text arrays occupying multiples of $n$ bytes~\cite{sais,dc3,sa}.
At the measured $n=\qnum{input}{10^{11}}$ bytes, such workspace can exceed a fixed
machine budget even when the input fits on disk. This is a capacity argument,
not a measured failure of those competing implementations.

\paragraph{Point masses and the Pile evidence.}
Long common prefixes still make sorting within a band expensive. A point mass
at the inspected prefix therefore calls for deeper refinement, not simply a
larger band; if deeper keys cannot split it, a proved analytic case or an
explicit failure is required. The Pile case at $\qnum{input}{10^{11}}$ bytes
illustrates the pressure: the source records that the space class and its
largest byte pair exceed wave capacity, requiring deeper waves than the shallow
encyclopedic-text rule. The diagnostic recording this shape is
\texttt{SS\_WAVE\_DEPTH depth\_hist=...}, emitted in
\texttt{xsa/runtime/tools/binbwt/scatter\_sort.cpp}.
\ev{xsa/runtime/tools/binbwt/scatter_sort.cpp}{wave-list rationale at lines
1160--1171 and depth diagnostic at lines 2524--2527}
The headline receipt quantifies the completed build as
$\qnum{waves}{102}$ waves; it does not provide a numeric depth histogram.
\ev{tools/binbwt/RESULTS_PILE_100GB.md}{opening build receipt}
We therefore attribute the whitespace-depth explanation to the source and do
not turn the diagnostic format into an invented measured depth.

```

## R3: P2 ledger

`python3 paper/audit_numbers.py`

48 sourced claim IDs, 55 occurrences; 2 documented CLI parameters;
50 ledger quantities total; 0 missing or inconsistent values.
No new empirical quantity was introduced. Reused input size and wave count
are supported by the pinned headline receipt. Formula parameters are definitions.
Manual source review distinguishes whitespace rationale from a numeric depth receipt.

## R4: claim-language grep

`grep -nE 'novel|first ever|new asymptotic' paper/main.tex`

```text
170: This is not a new asymptotic bound for suffix sorting in general. Adversarial
```

The sole hit explicitly disclaims a new general asymptotic bound.

## R5: publication

Commit and push to `paper/writeup` only; final task result records the commit.
`git diff --check` passed.

## Manual notes and residual risks

The requested unconditional total O(n) and universal sequential-access claims
are not supported by the implementation. The paper states the conditional
comparison bound and keeps repeated scans, symbol inspection, and random mapped
text accesses explicit. Fixed-memory B is conditional on successful refinement
and actual admission; current oversize admission is not a hard memory guarantee.
Depth is a distribution parameter, not guaranteed independent of growing n.
No numeric SS_WAVE_DEPTH histogram for the headline Pile run was found in the
committed receipts searched. The paper cites the source rationale and diagnostic
location and separately reports the measured 102 waves, without fabricating depth.
Full-Pile deployment remains untested. No benchmarks were rerun.
Supervisor-contact tooling was not exposed; the task explicitly instructed
continuation without waiting for a supervisor.
