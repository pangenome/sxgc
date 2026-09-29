# Parsed-space front-end implementation and limits

This lane is opt-in. It does not replace the sealed product toolset, edit an
upstream checkout, or modify retained artifacts. No commit/staging is performed.
The requested `contact_supervisor` capability is absent from this session's
tool catalog; there is no callable supervisor channel. Do not treat this file
as an acknowledged handoff of a running gate.

## Three memory changes

1. `external/phrase_store.hpp` specializes PFP's byte dictionary. Exact phrase
   bytes and hash-chain records are stored in unique, owned, sharded files.
   Dedup reads the disk records and verifies every matching hash against exact
   bytes, failing closed on collisions. Bucket heads and 16-byte sorting keys
   with 8-byte record IDs are phrase-count sized. Rehashing rewrites metadata
   through a bounded buffer. The input stream is opened/read only by the
   existing parser. The current phrase has a configurable hard limit
   (`SXI_MAX_PHRASE_BYTES`, default 64 MiB), checked before every append;
   oversize phrases fail rather than silently growing a text-sized buffer. The integer L2 dictionary remains parse-sized.
2. `external/dictionary.hpp` builds generalized suffix arrays of phrase-aligned
   blocks using the pinned 32-bit gSACA-K ABI, writes local 32-bit offsets plus 16-bit clipped LCPs to
   disk, and performs a replayable LCP-aware tournament merge with bounded run buffers.
   Equal generalized suffixes are ordered by global position, matching the
   differential gSACA-K oracle. The final global SA is never materialized.
   Global LCP is carried by the tournament, without dictionary-wide resident ISA or LCP arrays.
   Clipped block LCPs are exact lower bounds and are extended by byte comparison.
   r-pfbwt makes three sequential passes (chunk planning, BWT, endpoint tap).
   One thread/one chunk is enforced because this stream has one cursor/cache.
3. The byte dictionary uses explicit `pread` pages, including reverse/colex
   comparisons and preceding-character accesses during merge. Dense D-bit
   boundaries become phrase-start offsets. There is no mmap or D-sized resident
   SA, ISA, DA, LCP, dictionary, or boundary bitvector in this backend.

The default block budget is 64 MiB, dictionary cache 256 MiB, per-run buffer
1 MiB. `SXI_SA_BLOCK_BYTES`, `SXI_DICT_CACHE_BYTES`, and
`SXI_SA_RUN_BUFFER_BYTES` configure these; the block must be below INT32_MAX.
A phrase larger than the block fails explicitly, without silently growing RAM.
The full-pile block recommendation is 1 GiB, reducing fan-in to about 1,321 runs
and total run buffers to about 1.39 GB. These are bounded block allocations,
not an allocation proportional to D for suffix construction.

`SXI_SCRATCH_DIRS` is a colon-separated list. Duplicate roots implement weighted
striping. Every scratch file uses mkstemp and is unlinked by its owner on normal
destruction. SIGKILL/abort can leave files inside the experiment's owned scratch
directory. Never remove a directory belonging to another experiment.

## Residency audit beyond the new backend

- `rpfbwt_endpoints.cpp` reaches dictionary bytes through `SeamRepair`, whose
  `SlimLCE(..., true, ..., true)` selects paged `SlimDict`.
- `sxi_pipeline.py` already passes `--dict-stream` to `slim_dump`; the gate
  driver preserves this flag. The generic standalone slim CLI also offers a
  resident-dictionary mode; that mode is not used in this lane.
- SlimDict's actual cache is four 64 KiB pages per thread, FIFO replacement
  (not LRU). Its bounded residency is the relevant guarantee.
- The optional PFP properties writer loads a complete dictionary. The final
  external parser patch rejects properties and FASTA/VCF modes before reading
  input; this front end supports the requested text/AGC and integer parse path.

## Full-pile blockers that the three changes do not fix

The existing downstream chain is not wholly parse-granular. In
`chi_rspace_dump.cpp`, `Ri4` owns run characters (R bytes), run lengths (4R),
starts (8R), and packed tail samples (ceil(log2(n))/8 per run). `LfIndex` adds
at least 12R bytes and stores run IDs as uint32_t, with narrowing casts. The
mapped head array can add another 8R resident bytes. These are existing
R-sized structures, outside this lane's three dictionary fixes.

The retained 1 GB w10/p100 measurement has 9,094,817 unique phrases. Constant
ratio extrapolation to 1.31 TB gives 11.914 billion unique IDs, above the
uint32_t parse ABI. This is a capacity scenario, not a proven lower bound on
the full corpus. The new store refuses overflow; it does not silently widen
the on-disk parse format or the downstream integer alphabet.

At the retained 100 MB web R/n=0.38647349, R projects to 505.47 billion for
the actual 1,307,910,802,540-byte pile. The existing slim arrays need at least
(13+41/8+12)R = 15.227 TB before allocator
overhead, dictionaries, fingerprints, and mapped heads. This independently
violates the under-900-GB target. A 10–50 GB slice is not evidence that these
full-scale limits are solved; 32-bit LF run IDs can already overflow near
11.1 GB under this constant-ratio scenario.

These findings require larger-scale R/U measurements or a downstream
representation/width decision before claiming a feasible full-pile build. This patch intentionally does not widen
scope to rewrite the R-sized chain or the parse ABI.
