# PHRASE RADIX — the parse BECOMES the binning

Branch `phrase-radix`, base `pile-week-v1 @ aa1a95c`. Status: DESIGN, written
before any code, in the operator's framing. The measurements this document
predicts are in `tools/binbwt/RESULTS_PHRASE_RADIX.md`; where the built thing
differs from what is described here, the results document says so and why.

---

## 0. The one paragraph

The bin machine builds a byte-radix tree over the corpus and the corpus's own
repetition shows up as DEPTH: at 10 GB the deepest split the census journal
recorded was `depth_max=879`, i.e. there are suffixes in that corpus that share
878+ bytes of prefix - the boilerplate epoch of the web (cookie banners, nav
furniture, licence blocks, the same document body copied a million times). Every
one of those 879 levels is a count pass and a scatter pass over the node's rows,
and every row of the node pays a random byte read per level for the privilege.
A PHRASE PARSE answers the same question once, in one content-addressed lookup:
if the dictionary says "the text at offset `x` is entry (block, offset) of length
`s`", then the 879 levels become ONE level of width `s`. The parse IS the
binning, because a phrase is a pre-computed 879-level-deep radix path.

---

## 1. THE CENSUS MOTIVATION (what was measured, not argued)

The 10 GB probe (`tools/binbwt/RESULTS_AUTO_RADIX.md` §3, `pf-10gb.txt`,
construction only, 96t) journals:

```
new (auto target 1M, dcap 4096)  build_s 300.369  depth_max 854
                                 level_elems 41021242629  symbols 4982269168684
                                 comparisons 214876750387  leaf_rows_max 1048416
```

and the 10 GB ladder's note (§ "Undividable buckets"):

```
at 10 GB exactly two leaves needed the depth cap at 256 (6.05M rows: a
duplicate block needing 854-879 bytes of shared prefix before it can be
divided).
```

Three facts follow, and they are the whole motivation:

1. **`depth_max` is the census of the corpus's boilerplate.** It is an OUTPUT
   of the data (`bin_sort_rec`: depth is not a policy, the target is). 879 is
   not a tuning artefact; it is the length of the longest prefix shared by
   enough suffixes to still be one node when the target stopped subdividing.
2. **The mass is large.** A 6.05M-row leaf that needed 854-879 certified bytes
   is a node that was split, one coded symbol at a time, 879 times. Each of
   those levels cost one byte read per row plus one scatter write per row -
   `O(m x 879)`, on one thread, for one structure.
3. **The finish's verified symbols grow per-GB with exactly those structures.**
   The finish does not see the tree; it sees the ordered run stream and it
   verifies run boundaries by walking bytes (`SlimFingerprint::lce`, the
   `verified_symbols` counter: 1.176e9 at 100 MB, 12.82e9 at 1 GB - it scales
   with the corpus and with its LCE depth). Where the corpus repeats a
   900-byte block a million times, the adjacent rows in the SA order share
   900 bytes and the verification walks all of them. Repetition is a cost in
   BOTH phases, and it is the SAME repetition.

The census is therefore not just a wall-clock story. `depth_max`, the
`structural_leaves`/`structural_rows` pair, and the finish's
`verified_symbols` are three views of one object: the corpus's long repeated
blocks.

---

## 2. THE IDENTITY: a phrase IS a pre-computed radix path

The byte-radix tree's job, at a node with certified depth `d`, is to decide for
each row `p` what the byte at `tail(p)+d` is and to group rows by it. Do that
879 times and you have certified 879 bytes.

Now suppose you already know, before the tree starts, that the text at
`tail(p)+d` continues with a specific 900-byte string `S` that is in the
dictionary. Then you do not need 900 levels: **the group of rows that all start
`S` is one group, and it is at depth `d+900`**. That is the identity:

> binning by the first phrase == binning by the leading bytes up to the phrase
> length, in ONE radix level.

The dictionary lookup is content-addressed and costs one 8-byte load and one
table probe per row, versus 900 random byte loads and 900 scatter writes. The
parse collapses the tree's hot paths into single edges. It does not change the
tree's TOPOLOGY - it changes the tree's COST along the paths the corpus itself
proves are shared.

The identity is exactness-preserving and needs no new theory:

> **Lemma (the skip).** Let a node hold rows `R` and be certified at depth `d`
> (every `p` in `R` has `M[p .. p+d)` equal to the node's prefix). If every row
> of the node resolves to the same dictionary entry `E = (block, offset, len)`,
> and every row's text at `tail(p)+d` compares equal to `E`'s `len` bytes, then
> replacing the node by `(R, d+len)` is EXACT: the order inside `R` depends only
> on the bytes after `d+len`, and no byte before it differs between rows, so no
> discriminating information is discarded.

That is the whole of the mechanism's correctness argument, and the oracle
proves it at tiny scale against a brute suffix array.

---

## 3. THE DEGENERATE DICTIONARY: small by construction, both regimes automatic

The dictionary is deliberately **degenerate as a PFP dictionary**: it is not a
complete parses of the corpus, it is the handful of structures that HURT.

* **Length threshold `L`** (default 64 bytes) and **frequency floor `F`**
  (default 2). Only strings at least `L` bytes long that occur at least `F`
  times are even candidates. A phrase that occurs twice does not need a token;
  the byte radix handles it in a few levels.
* **Small by construction.** The block pool is capped (`--phrase-pool-max`,
  default 16 MB), the entry table is a fixed direct-mapped array
  (`--phrase-entries-max`), the number of blocks is capped
  (`--phrase-block-max`). A direct-mapped table can only LOSE opportunities to
  a collision; it can never produce a wrong answer, because every use is
  byte-verified.
* **Cache-resident.** The pool is the phrase text (16 MB of the corpus's most
  repeated bytes) and the table is 8-24 bytes per slot. Both live in RAM for
  the whole build and are touched once per row-probe, not once per byte.
* **Sorted lexicographically once, so phrase IDs inherit byte order.** Blocks
  are ordered by their content bytes and their offsets ascending, so entry
  index order IS lexicographic order of the phrase content. Because of that,
  bin order = phrase order = lexicographic order, for free: no sort of the
  dictionary's users is ever needed. (The tree takes the order from the byte
  radix it still runs; the dictionary's order is what makes a phrase a
  legitimate symbol rather than a label.)
* **Both regimes automatically.** The dictionary is consulted ONLY where the
  tree is already grinding - at a node whose split made no progress
  (`biggest == m`: every row shares the next byte, the deep-boilerplate
  signature). The diverse majority of the corpus never pays a probe: its nodes
  divide normally, the hook does not fire, and the byte radix is what runs.
  There is no flag, threshold or heuristic deciding which region is which: the
  data decides, node by node.

---

## 4. THE HARD CORE: mid-phrase suffixes

A suffix that starts INSIDE a phrase occurrence is the correctness core, and it
is where a naive phrase radix breaks. Fix the accounting: the dictionary is a
set of blocks, and it registers an entry for EVERY OFFSET of every block, not
only offset 0. Entry `(block, o)` is the string `block[o .. blockLen)`.

Then a mid-phrase position is handled by the ordinary rule: the suffix at
`p+o` (inside an occurrence of `block` at `p`) begins with `block[o..]`, which
is entry `(block, o)`. The tree bins those suffixes as `(phrase, offset)` - and
the members of one `(phrase, offset)` bin are, by construction, byte-identical
through the phrase (they are all occurrences of the same entry), so the node's
shared-prefix certificate extends to the END of the phrase, and the members
order by their continuation. That is PFP's end-occurrence sort, generalised:
PFP derives it from the bucket order of phrase ends; here it falls out of the
same uniform-entry check that handles offset 0.

The consequence to be honest about: without the offset suffixes the mechanism
still works but only reaches a phrase whose boundary happens to coincide with
the node's certified depth, so the byte radix would grind up to one phrase
spacing before it could jump. Registering offsets is what makes the first
probe - at a divergence point whose offset inside the block is essentially
arbitrary - succeed immediately.

---

## 5. THE CONVERGENCE: the bin machine already discovers the dictionary

Two things arrive at the same object from opposite ends:

* the **bin machine's hot paths ARE the dictionary**: `depth_max=879`,
  `structural_leaves/rows`, the 6.05M-row undividable leaf - the tree spends
  its wall discovering, one byte at a time, exactly the set of long repeated
  strings the dictionary names;
* the **census journals it**: `depth_max`, `no_progress_levels`,
  `structural_leaves`, `leaf_rows_max` are a measurements-only description of
  the phrase set. A run of `no_progress_levels` at large `m` and large `depth`
  is a phrase, whether or not anyone has named it.

So the dictionary is not foreign material bolted onto the machine. It is the
machine's own census, promoted from a measurement to an index: the parse is a
sampling of the same structure the tree would have re-derived byte by byte.

---

## 6. WHAT IT BUYS (the claims the results must measure, not argue)

1. **The producer's hot-path level passes (879 levels) become one parse pass.**
   `level_passes`, `level_elems`, `symbols` and `comparisons` are exact
   counters; the thesis predicts they fall, and `depth_max` too (a depth-max
   driven by phrase skips reports the certified depth, which is still 879 but
   reached in one level rather than 879).
2. **The finish's deep byte-walks become token comparisons.** The finish's
   `verified_symbols` is the number this must move. The parse PRE-ANSWERS the
   verification's expensive questions once, at dictionary-build time: the
   length of the shared prefix of two rows is a property of the phrases that
   contain them. Whether the finish can consume that answer is a plumbing
   question (the certified prefix a row carries out of the tree), not a theory
   question - and it is the one place where the built thing may fall short of
   the design; the results document reports what actually moved and what did
   not.
3. **End-to-end wall.** The producer's build term is a measured anchor
   (`build_s` at 1 GB / 10 GB) and the finish term is the pile's limiter
   (`RESULTS_FINISH_DECOMP.md`: finish-dominated). A lever that only moves the
   producer moves the smaller term and must be reported as such.
4. **The parse costs one pass.** It is a single sweep with a rolling minimizer
   and one sort of the sampled hashes; it must be measured, and it must be
   cheap enough that (1) minus (the parse) is still positive.

---

## 7. THE RISKS, honestly

* **The mid-phrase machinery is the correctness core.** Section 4 is where a
  phrase radix silently produces a wrong order if the offset bins are not
  exact through the phrase. The mitigation is structural, not careful: every
  use of an entry is BYTE-VERIFIED (`memcmp` against the pool) before it can
  advance a depth, so a wrong entry is never a wrong order - it is a missed
  skip. Probabilistic shortcuts are forbidden here for the same reason the
  merge's fingerprint checkpoints are only HINTS: a 64-bit hash may pick a
  candidate, and may never certify one.
* **The parse must be deterministic.** Same corpus, same flags ⇒ same blocks,
  same offsets, same table, same skip decisions, same BWT. The dictionary is
  built by a fixed rule (winnow by a fixed hash, sort by (hash, position), rank
  survivors by (count, hash), insert in that order into a fixed table), and it
  is journalled so a second run can be diffed. A non-deterministic parse would
  make every downstream gate unreproducible.
* **The product is χ of the RAW text.** The parse is INTERNAL SCAFFOLDING: it
  chooses where the tree spends its levels, never what the rows are. The
  container bytes are the same bytes, the χ constant is the same constant, and
  the byte-identity banks (100 MB `χ=29716349`, 1 GB `χ=283296933`) must hold
  with `--phrase-radix` ON and OFF. If a phrase run does not bank, the phrase
  code is wrong, full stop - there is no "the parse is an approximation"
  defence, and none is offered.
* **Flag OFF must be bit-identical to today, not merely equivalent.** The
  product path is the byte radix unchanged; the default is OFF; every new code
  path is behind a pointer that is null when the flag is absent. The existing
  36,180-case oracle, the window pass, the refusal pass, the adaptive-depth
  pass and the byte-identity banks are run against the flag-OFF build
  unchanged.
* **RAM.** The parse sorts its sampled hashes; that array is
  `O(n/window)` and it must fit the phase envelope or the parse must be
  externalised. It is bounded and journalled, and the parse runs as a phase of
  its own, before the windows exist.
* **The dictionary can be wrong for the corpus.** A corpus with no repeated
  blocks (`L` above everything) yields an EMPTY dictionary, and then the byte
  radix must run EXACTLY as today - same counters, same wall - which is one of
  the oracle's cases and one of the gates.

---

## 8. What lands where

| part | file |
|---|---|
| the parse, the dictionary, the lookup, the journal | `bit6/phrase_radix.h` (new) |
| the skip hook in the tree, the counters, the journal lines | `bit6/binbwt_slice.h` |
| the flags (`--phrase-radix`, `--phrase-min-len`, `--phrase-min-freq`, ...) | `bit6/cross_lcp_merge.cpp` |
| the new oracle family (determinism, mid-phrase exactness, thresholds, empty dict) | `tools/binbwt/oracle.cpp` |
| measurements | `tools/binbwt/RESULTS_PHRASE_RADIX.md` |
