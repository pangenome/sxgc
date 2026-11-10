# NOTE to the Emender side: your 1024-byte window is deeper than the data, and it leaks

**From:** sxgc · **Re:** the consultation feature at "the spine after the last <=1024 bytes"
**Measurements:** `CONTEXT_DEPTH.md`, 1,000 uniformly sampled positions over the first 100 GB of
pile.txt, in the 100 GB Pile container. Commands and receipts are in that file.

## What we measured

How long a context can be and still occur somewhere else in the corpus:

```
support at depth K          p50      p90      p99
K=2   (one other place)     18 B     38 B    2082 B
K=10                        16 B     28 B     314 B
K=100                       12 B     22 B      96 B
```

At exactly 1024 bytes, the fraction of positions whose context occurs anywhere else is **1.4 %**
(7.5 % if you count the source occurrence itself). So for ~98.6 % of positions a 1024-byte
consultation has nothing to say and must emit an empty histogram - which is the behaviour, and it is
not a bug in the reader, it is the data.

Where contexts do repeat, they stop being informative very quickly:

```
depth   contexts repeating   median entropy   mass on top-1 byte
1        1000/1000           3.89 bits        0.23
8         966/1000           1.29             0.72
16        644/1000           0.50             0.93
32        142/1000           0                1.00   ← deterministic
1024       14/1000           0                1.00
```

Two consequences, one of which matters for training correctness:

1. **The information is in the first ~16 bytes.** By 32 bytes, a context that repeats at all repeats
   with exactly one possible continuation: it is boilerplate, a duplicated document, or a template.
   Conditioning on depth beyond ~16 buys nothing and costs a linear factor (see below).
2. **At the depths where 1024 does fire, you are reading the answer off the source document.** Those
   1.4 % of positions are contexts unique to one place in the corpus, so the "next byte" is simply the
   continuation of that document. If the document is in the training set, that is target leakage
   delivered as a feature. Source-document exclusion must happen BEFORE any maximality or support
   test, not after.

## Cost, measured on the same container

`xsa branches` (one context -> next-byte branch histogram), serial CLI round trip:

```
depth 1      472 us p50          depth 16    7.0 ms p50
depth 8     6.1 ms               depth 1024   51 ms p50
```

Linear in depth, because each byte is a rank step. So a 1024-byte consultation costs ~100x a 16-byte
one and, per the measurements above, is empty ~99 % of the time.

## What we suggest

- Use a **short** spine: 8-16 bytes, with support-weighted `relax` (ascend until the context has
  enough occurrences) rather than a fixed length. A variable-length spine matches the data: common
  text gives you a long context, novel text gives you two.
- Exclude the source document from every count.
- Keep the 1024-byte field if you need it for compatibility, but do not spend the query budget there.

At depth <=16 an in-core descent is ~8 us (16 rank steps on an index in memory), so 150k positions/s
is a design target we can meet - the current 2.3k/s is dominated by depth 1024 and by the CLI round
trip, not by the index.

We will follow up when `extend`/`relax` are exposed over the tree, with a measured descent rate.
