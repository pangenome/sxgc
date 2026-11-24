# Post-parse navigation — fixture only

`src/overlap_nav.rs` is imported by one integration test, not linked by the producer. The proposal replaces the bounded component's rank-order `u64` aligned leaves and list-order `u64` positions; it does not replace dictionary sorting, produce a packed parse, or change a gate. The fixture builds from slices/iterators and certifies against raw suffixes. Reproduce without a corpus or benchmark:

```
CARGO_TARGET_DIR=$(mktemp -d) cargo test --offline --manifest-path xsa/pfp/Cargo.toml --test overlap_nav
```

### Column bound (inclusive universe U, length m)

For each at-most-64 element block of size t, subtract its first value. For span d choose `w=max(0,floor(log2(d/t)))` (zero for d=0). Write the lows in `ceil(t*w/64)` words. In three bitmap words set position `(delta >> w)+i` for local ordinal i: the quotient is <2t, even when w=0. Select the i-th set bit (skip whole words by population count; at most 63 low-set-bit clears, no global search); subtract i to get the high quotient. Lows cross at most one word boundary. This includes singleton, duplicate and near-`u64::MAX` values; the base is not added to the universe. Reconstruction is bounded by the last original value.

Block spans sum to at most U by monotonicity. `w <= log2(1+d/t)`; weighted concavity of log bounds the sum of `t*w` by `m*log2(1+U/m)`. Use integer `L=bit_length(ceil(U/m))` (zero at U=0), hence at most `ceil(m*L/64)` low words: only the last block can require padding. This covers arbitrarily skewed gaps; it is not a uniform-gap assumption. Use u128 before division/multiplication, fail on unaddressable allocations and too few/many/decreasing/out-of-universe values. Empty columns have zero blocks/low words. Requested capacity in bytes on 64-bit is

`C(m,U) = 48*ceil(m/64) + 8*ceil(m*L/64)`.

One 512-byte block buffer and O(1) scalars on stack; no dense staging, shrink/copy, or growing vector capacity. Vector headers and allocator rounding are not payload bounds. This model charges the reserved capacity even if actual packed lows use less.

### Why the adjusted *byte* PLCP is monotone

The prerequisite is a context-local trigger: except for the forced first start, **all** qualifying complete W-windows start an occurrence. This fails for arbitrary subsampling or position-dependent tie breaking. Let `s[j]` be text starts; compare raw suffixes, with virtual END smaller than every byte and not counted in an LCP. `P[j]` compares the aligned suffix to its immediately preceding aligned suffix, or zero at lexical first (END). For a nonfinal occurrence set `a=s[j+1]-s[j]`. If `P[j]>=a+W`, the earlier-in-order suffix matches the complete next trigger window, so its shift by a is also aligned. Both shifted suffixes preserve their strict order (remove a common prefix), and agree on `P[j]-a` bytes. Every suffix between them shares that prefix, including the immediate aligned predecessor of `j+1`. Even a suffix ending at that prefix is ordered correctly by END. Thus `P[j+1]>=P[j]-a`. Otherwise `P[j]<=a+W-1` and the trivial `P[j+1]>=0` suffices. Consequently `P[j]+s[j]+(W-1)*j` never decreases. Partial-window equality does **not** certify a trigger. The seeded tests require a witnessed decrease without slack, not just a positive test with slack. No comparison crosses the virtual END.

`Aligned::build` streams starts then P in occurrence order. Store n separately as the one-past start, with `U=n` and `U=n+(W-1)*(m-1)` respectively. A nonrepresentable worst-case ceiling refuses before allocation. Checks include strong-case residual, monotonicity, full trigger extent, strictly increasing starts from 0, lex-first PLCP and bounds on both raw suffix lengths; they do not prove a caller's PLCP is the *exact* LCP. For a future adapter: from token PLCP z at j and predecessor i, add `s[j+z]-s[j]` to the dictionary LCP of the two first differing words. The unique final word containing END cannot be equal at different occurrences. Supply text-order values, **not** rank-order leaves; retain/account for any random-access starts, SA/inverse and token PLCP required by that adapter. No dense-to-succinct savings during construction are assumed for the existing harness.

### Successors, positions and LCP

Certify the supplied u32 parse SA permutation and strictly increasing `(lexical word id, rank(j+1))`, with ranks 1..m and successor 0 at final. Backward induction on finite suffixes gives the suffix recurrence; first-word buckets of SA are exactly the occurrence lists, internally sorted by successor rank. Store flat u32 Psi (not one padded column per often-singleton head). List slot i is parse rank i+1. If keeping the compact SA, j is `SA[i]`; alternatively recover j from either that rank or `position(Psi[i])-1` (END position=m). `loc` still delimits head lists. For offset d emit `start(j)+d`, using dictionary predecessor for interior bytes, the existing per-list predecessor byte at word start, and the special BWT code at text start. Global END row is separate. Do not count END as a raw byte in a within-tie LCP.

Optional B=2/4/8: sample positions j divisible by B, in SA order. Bitmap plus u32 prefix population per 64 ranks indexes packed u32 samples; repeated Psi increases occurrence by exactly one until a sample or the final occurrence (implicitly known by successor zero). At most B-1 forward hops; subtract the hop count. None stores no positions; its fallback walks to final in O(m), **not** a suggested large-rung lookup. No need to retain dense SA in sampled query state.

`BlockMin` takes minima over rank blocks and builds a 2q-entry iterative tree (`q=ceil(m/b)`). For x<y reduce leaves of ranks `(x,y]`: up to `2(b-1)` residual decodes plus O(log q) tree loads; x=0 returns zero. A decode uses a sampled position walk and two bounded column lookups. Tie LCP = finite-tail length minus W plus aligned LCP between successor ranks. Independent raw suffix sorting checks every row, BWT, position and adjacent LCP including global END; separate token-prefix and occurrence-SA oracles exercise overlaps and tie order. This does not measure indirect access locality.

### Payload ledger: actual 10 GB dimensions, **10× sensitivity only**

Actual census from `gates/receipts/packed-front-10g/front-end.json`: N=10,000,000,000; m=318,164,488; k=234,472,723; transformed D=11,239,544,612. Right column multiplies N,m,k,D by ten, **not measured 100 GB dimensions**. Numbers below are calculated array bytes, not measured allocations. W=8, B=8, b=16. The current packed loader retains tape capacity T=D+3k-1 even after in-place transformation; do not charge only truncated D.

| Class | 10 GB census model | 10× sensitivity |
|---|---:|---:|
| T dictionary capacity |11,942,962,780|119,429,627,809|
| Dense dictionary offsets `8(k+1)` |1,875,781,792|18,757,817,848|
| Counts + loc `8k+4(k+1)` |2,813,672,680|28,136,726,764|
| Starts C(m,N) |477,246,776|4,772,467,360|
| Adjusted P C(m,N+7(m-1)) |477,246,776|4,772,467,360|
| Psi 4m |1,272,657,952|12,726,579,520|
| Samples `12 ceil(m/64)+4 ceil(m/B)` |218,738,096|2,187,380,864|
| Tree `16 ceil(m/b)` |318,164,496|3,181,644,880|
| List predecessor bytes m |318,164,488|3,181,644,880|
| **Sum before order arena, cursors, stacks, output etc.** |19,714,635,836|197,146,357,285|
| Reduction vs current `21m + tree` emit navigation (same dictionary/arena) |3,917,400,160|39,174,002,496|
| Optional **instead of** dense dictionary offsets C(k+1,D), planned not integrated/allocated |351,709,120|3,517,090,848|
| Sum if replaced (still before arena etc.) |18,190,563,164|181,905,630,285|

Construction is a different ledger. Wide parse sort alone (`20m+32k` four-k / `20m+48k` six-k) = 13,866,416,896 / 17,617,980,464 bytes on the measured dimensions; 138,664,168,960 / 176,179,804,640 at 10×. These include original IDs, not dictionary residency. Token PLCP construction uses 12m for IDs, SA and inverse/PLCP. `Successors::build` concurrently needs borrowed IDs+SA (8m), temporary inverse (4m), Psi and samples: 5,309,369,904 / 53,093,698,944 bytes, **plus** other retained state. Release or charge retained SA, token PLCP, dictionary and any dense adapter inputs at each transition. The column builder adds at most one block buffer at a time; tree construction fills its final array directly. Fixture sort/oracle vectors live outside the proposed representation. Only tiny fixture arrays were allocated here.

The 140 GiB ceiling is 150,323,855,360 bytes for **all live arrays**. Even the optimistic dictionary-offset replacement misses it on this sensitivity before descriptors (8 each), bounded heap cursors, plan and radix stack (`10240+1536+40*passes` per compact-order plan), output buffers, decode tables and phase overlap. Neither allocation order nor retention for an integrated phase has been validated. Account separately for allocator overhead, other stacks, RSS/cache, verifier, scratch and consumer grants. No raw or prepared wall, physical IO, scale admission or production claim. Next fixture work would connect a streaming text-order PLCP source and cost its peak, before considering any measured comparison.
