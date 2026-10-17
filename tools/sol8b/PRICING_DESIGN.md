# Bucketed-front pricing

The measurement extends sol8a's keys-only RAM radix census. A record holds an
8-byte prefix and a chunk bit, never a suffix position. Stable sorting moves
both together; reduction yields exact prefix mass and contributor count.
Chunk lengths come from the existing sanctioned compact-position headers.
Finite suffixes shorter than d remain separate from NUL-padded full prefixes.
The measurement is limited to at most 64 input chunks and uses 32n bytes for
two record banks. It is a measurement instrument, not a pile directory design.

For each d=4,6,8, report the exact mass-weighted log2(contributors). The original
whole-merge mean, 60485597061/3674629435, is NOT the conditional in-bucket LCE
mean. Output fields named `proxy` explicitly apply that unqualified global mean
for sensitivity only; they cannot establish the requested hybrid symbol cost.
Break-even weighted tail cost is baseline / sum(mass*log2(contributors)).

The optional generated instrumented build counts all same-prefix comparator
calls in the inherited serial HEAP stage, independently for d=4,6,8. It retains
per-prefix comparison counts and tail-symbol sums, never positions. Generation
asserts each instrumentation insertion matches once; production sources remain
untouched. A discard sink checks full row coverage without producing a product,
and the inherited anchor repair and compressed-position readers are used.
The per-bucket measured baseline conditional mean is multiplied by the requested
mass*log2(contributors). Missing in-bucket observations are explicitly reported;
they must not silently acquire a global mean. Even complete observation is a
projection: the hybrid changes the heap operands and their frequency. No model
is an exact measurement of the unimplemented hybrid.

Pile projection assumes 1310 independent one-GB chunks with per-bucket occurrence
rate equal to the observed full-fixture bucket mass. It uses log2(E[C]), with
E[C]=1310*(1-exp(-mass)); this is not E[log2(C)]. The fixture's eight-way observed
contributors are not scaled linearly to obtain pile contributors. Distribution
shift and unseen prefixes are unresolved. Finite end effects are negligible at
pile but remain a limitation of this occupancy model.

A literal independent binary search for every (chunk,bucket) is also priced as
a count of searches. Prefix decoding, boundary-search time and repair work do
not disappear from total CPU work merely because they are excluded from tail
comparison-symbol counters. No implementation or launch decision follows from
the global-mean proxies alone. Chunk cyclic orders must undergo inherited global
anchor repair before global-prefix binary searches are valid.

Reproduce:

    c++ -O3 -std=c++17 -pthread tools/sol8a/measure.cpp -latomic -o SCRATCH/measure
    python3 tools/sol8b/oracle.py SCRATCH/measure
    SCRATCH/measure --hybrid CORPUS REMAP CHUNKS
    python3 tools/sol8b/build_pricing.py SCRATCH
    SCRATCH/measure-lce --hybrid CORPUS REMAP CHUNKS FRESH_SCRATCH_PREFIX

The requested contact_supervisor capability was searched first and is absent
from the callable tool catalog. No substitute external messaging is performed.
