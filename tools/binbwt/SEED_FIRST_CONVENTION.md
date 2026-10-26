# Seed-first product convention

The product alphabet is `T[i] = remap[corpus[i]]`. SA contains exactly the n
positions, sorted by finite suffix `T[p..n)`, with a shorter suffix preceding
its proper extensions. There is no added sentinel row in the n-row product.
Its BWT is `BWT[r] = T[SA[r] ? SA[r]-1 : n-1]`: predecessor lookup wraps,
suffix comparison does not. In particular, wrapped predecessors do not imply
cyclic rotation order or an LF-consistent finite suffix array.

The producer's ten padded rows and its SA-zero dollar marker belong to the
transport frame. Normalization drops those rows and substitutes the actual
terminal predecessor for the marker. A seam class emits its finite terminal
seed first, followed by the remaining occurrences in finite suffix order.
Finite prefix exhaustion also breaks equal-rotation ties: shorter suffixes
come first, including periodic chain members. Rotation
happens before run coalescing, in the shared class rebuild, including classes
received incrementally. The m1 replay uses the same rebuilt classes.

LCP uses finite suffixes as well: `col[r] = LCE(SA[r-1], SA[r])` for r>0,
`col[0]=0`. A run's interior minimum is over `col[head+1..tail]`. Without a
column, direct LCE is capped at both suffix ends; seam discovery still uses
cyclic comparisons to discover its occurrence classes.

A rank stamp is an assertion against the independent stream-length cursor.
It cannot replace the cursor. Both endpoints must agree, and the complete
stream must cover exactly n rows. The column arm additionally refuses any
rank hole, disorder, noncontiguous run, or fallback, per band and after the
final tail. There is no top-query walk fallback. Supplying a column enables
these checks unconditionally; an environment switch cannot disable them.

The former cyclic container hashes are historical evidence, not gates for
this convention. Authoritative current baselines and measured limitations
are in the latest RESULTS section of `../../RESULTS_TAPE_PROTOTYPE.md`.

## Exact offline arbiter

Build and test the independent tool:

```sh
g++ -O3 -std=c++17 -Wall -Wextra tools/binbwt/stream_vs_bwt.cpp -o /tmp/stream_vs_bwt
/tmp/stream_vs_bwt --selftest
```

Run with `--corpus C --remap M --sa SA --container PRODUCT` to decode the
actual published m1 Huffman chars and gamma lengths, expand logically in
stream order, and compare every one of the n BWT rows to the verified SA.
`--stream DUMP` checks the actual emitted band's 40-byte run records and
also compares both run endpoints to SA at their derived ranks. Either or
both representations can be checked. Equality is a direct comparison, not
a digest, and wrong bytes return nonzero after counting all mismatched rows.
The standalone `sa_check --dollar 1 --remap M` proves SA permutation and
finite suffix ordering separately.

`--chi --chi-expect N` derives runs from SA and computes every adjacent LCP
by direct finite byte comparison, then uses an independent scalar reference
scan to count chi. It includes no product Sweep/LCP implementation. Optional
`--chi-column COL` uses the supplied LCP column instead and labels that
weaker independence explicitly. No inverse suffix array is needed.

`seed_first_gate.py --help` documents the reproducible A/B/C gate driver.
It uses a packaged Cargo bundle, completes emission with
`CROSS_V2_PIECE_WINDOW=100000000`, checks DONE and both transport artifacts,
and only then starts finish. It executes separate normal and explicit-debug
runs. Both containers must match, both are checked directly against SA, and
the debug band stream is checked over all n rows. Product runs use no dump,
full ISA, bridge, or rank-exception artifact. Scratch sampling records
observed file sizes, not a guaranteed peak or an unmeasured pile attestation.

`--debug-artifacts` is mandatory for band/answer/bridge dumps, full-ISA audits,
and old rank-exception diagnostics. Environment variables alone cannot
activate them. This mode never relaxes stamp or closure refusal. Normal
product dump scratch is exactly **0 bytes**; that is not a claim that the
whole construction pipeline, including its supplied SA/column, fits in 2n.
