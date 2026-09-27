# Bounded class repair and cyclic witnesses

This lane preserves the q=0 streaming certificate and adds a bounded repair
for uncertified padded frames. Producer, tap, parser, source preparation,
pipeline and SXI writer are unchanged by this lane.

## Classes

Let N=n+10 be the padded frame size. The first ten rows are dollar suffixes;
after removing them, the order is the finite suffix order of T. Comparing
finite suffixes differs from comparing cyclic rotations only if one finite
suffix is a prefix of the other. For every terminal suffix p=T[n-d:n],
backward search in the padded RLBWT gives a contiguous prefix interval I(p).
The shortest suffix n-d is its first row, because dollar is smaller than
any corpus byte. Start with the all-row interval and the known SA=n row 0,
and extend left using its BWT character and LF rank. This is one operation
per terminal-prefix depth, not a walk over all corpus rows. Stop at the
first singleton: extending a singleton cannot create another occurrence.

Prefix intervals are nested or disjoint. Their maximal union partitions all
potentially ambiguous rows into disjoint seam classes. Every rotation in a
class still starts with that class's prefix; therefore it cannot move outside
the class. Every comparison outside the union mismatches before either suffix
ends and retains its order. Sorting each maximal class by exact cyclic LCE
is sufficient. Equal complete rotations tie by increasing position, matching
the independent oracle.

The q counter is only a certificate diagnostic, **not a seam-class size**.
On full yeast235 q=6,893, but discovery finds seven maximal classes with
13,503 total candidate rows (including rows that need not change position).
The largest includes the 9,901 separator-starting rotations. Eight backward
extensions suffice. Raw r=100,905,044; policy limit=100,905.

## Resolving samples and splicing

In a BWT run interior, Phi(x) is a circular piecewise translation. Anchors
(head SA, previous-run tail SA), sorted by head SA, determine Phi by predecessor
search and modular offset. Swapping the pair columns and sorting gives the
inverse, whose anchors are (tail SA, next-run head SA). Thus every class row
is resolved in O(log r), starting from its known first SA=n-d. The padded
frame has no equal rotations: its unique dollar block breaks periodic ties.
The same samples give the unchanged rows immediately before/after a class.
No interior row is resolved by an unbounded LF walk.

The adapter streams untouched run segments, substitutes sorted class segments,
and coalesces adjacent equal characters. Known neighbor samples fix run cuts;
head/tail samples after merging are taken from the first/last surviving rows.
Counts, cumulative frequencies and packed tails are recomputed. Raw producer
.ssa and .ssa_t are never rewritten. The first complete validation/count pass
precedes opening either output.

## Refusal and complexity

B=max(1000,floor(raw_r/1000)) is a fixed policy, not a caller override. A
candidate prefix class larger than B refuses immediately with its size.
Discovery also refuses after B backward extensions; the union refuses if
its total size exceeds B. These latter checks prevent many small classes or
a long periodic prefix chain from hiding a corpus-length computation. The
1000-row floor supports tiny tests and small inputs; above that it admits
at most 0.1% of the compressed run count as repair rows. This intentionally
conservative policy can reject otherwise repairable inputs.

The existing slim LCE is exact but **not inherently polylogarithmic**: it
verifies fingerprint guesses symbol by symbol. Adapter comparisons set a
separate Q=max(1000,bit_width(N)^3) bound on both fingerprint suffix probes
and direct prefix verification. A probe or proposed verification exceeding
Q refuses before reading it. Each admitted raw LCE makes logarithmically
many hash probes, each with O(Q+log N) work, and verifies at most Q symbols.
Cyclic LCE uses at most three raw LCE calls. Thus admitted comparisons have
polylogarithmic work (conservatively O(log^4 N), with the constant floor),
including exact verification. There is no probabilistic acceptance of a
hash collision. The shared slim consumer retains its prior unlimited default;
only the adapter opts into this additional refusal policy.

Index construction is O(r log r) time / O(r) space for run rank and Phi;
class discovery is O(B log r). For s admitted rows, resolution costs
O(s log r) and sorting uses O(s log s) cyclic comparisons. Existing PFP
parse/dictionary structures still require compressed-artifact preprocessing
O(P+D), as they do in slim; no expanded T, SA(T), or n-sized bitmap is built.
Output passes are O(r+s). No O(n) expanded-text fallback exists.

The constant-run A^1024 B A^1024 fixture refuses class_size=2048, limit=1000,
before loading PFP or creating outputs. Smaller members of the same family
are repaired and independently checked, including exact endpoint ties.

## Cyclic witness coordinate

For a suffix starting at p, the extension character is the preceding BWT
byte, at q=(p-1) mod n. Its zero-based position in reverse(T) is
n-1-q = (n-p) mod n. The sweep now uses that coordinate in cyclic mode;
in particular p=0 emits 0, rather than n+1. This is a coordinate derivation,
not a modulo applied to the legacy n+1 witness. No state-machine candidate
or separator character is suppressed. Cyclic mode is detected from the
RLE alphabet exactly as in slim: 0x1e present, or no newline present. In
cyclic mode newline, if present as ordinary input, is also compared/emitted.
Legacy newline-only inputs retain n+1-p and the old sentinel suppression,
which is required for the existing yeast member-byte regression.

Dense test-only cyclic rotations independently supply BWT, head/tail samples,
all adjacent LCPs, per-run aggregate minima and reverse-coordinate witnesses.
Both streaming and in-memory sweep modes are checked against those values.

## Coordination and review

No contact_supervisor tool is exposed in this runtime's tool catalog. No
supervisor approval or independent review is claimed. The required external
review gate remains pending. No agent was spawned; no commits or staging.
