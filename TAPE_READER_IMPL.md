# TAPE_READER implementation notes (feat/tape-reader)

TAPE_READER.md is the spec. This file records only the decisions the spec leaves
to the implementer, and the acceptance amendments from the operator (steering,
mid-run): the tie ladder's big window (level 2) and the level-3 measurement gate.

## The four components, as built (`xsa/runtime/tools/binbwt/tape_sort.cpp`)

1. THE TAPE — one reader thread, a request queue, one matcher per pass. A request
   is `{prefix bytes (symbol space), key_len, kbase, posLo/posHi (point-mass
   slices only), sink}`. The sink is a RAM chunk (P/Kb/Lp/Tp planes). A pass
   serves every outstanding request from ONE sequential windowed sweep
   (SS_SCAN_CHUNK pread windows, remapped in place); a position matching no
   request costs one table lookup. The matcher is a flat trie over the
   outstanding prefixes (they are pairwise disjoint: a follow-up's prefix is a
   proper extension inside one unit's range, and that unit's initial request is
   never outstanding twice). Journal `PASS n= requests= rows= bytes=`, and
   `TAPE_IDLE idle_s= read_bytes_delta=0` whenever the queue is empty (poll +
   sleep, zero I/O). Pass scheduling: a pass starts when requests are pending
   AND every live sorter is parked-or-done (the fixpoint that makes one pass
   serve every follow-up that can exist yet).
2. THE PLAN — the survey (subdiv_survey.h, carried over verbatim, one pass) and
   the unit builder (the wave descent carried over: exact root counts from the
   survey, sample-scaled census deeper, no depth cap, single-child / 99.9%
   shaving classes to the point-mass branch, the tail-run proof, slices in
   descending position). Target = `--unit-target-rows` (default 100M). Every unit
   journals `UNIT id= prefix= rows_est= depth= arena_bytes=rows*(8+K_max+6+work)`
   once, before extraction. Plan-time refusal with numbers if a unit's chunk at
   K_max cannot fit `envelope - working_reserve`. Units ordered by prefix = the
   output order.
3. THE SORTERS — the wavekey leaf sort carried over (8-bit MSD, 257 buckets,
   bucket 0 = virtual END, run-length digit (sigma, family, ±L, t, key), small
   spans by direct order-key compare, run-aware LCP, XSA_WAVEKEY_VERIFY oracle).
   THE TIE LADDER (operator amendment): K=8 captured at sweep time; a tie group
   that survives round 0 issues ONE follow-up request for a big context window
   (XSA_TIE_WINDOW_BYTES, default 4096) — the doubling rounds 16/32/64 are
   in-memory radix levels over that one window, not extra passes. Residual past
   the big window goes to the counted-walk LCE fallback (carried over) and the
   run journals `TIE_GROUPS` (groups/rows/window_bytes_total/p50/p99/max) plus a
   `TIE_TOTALS` receipt at K and corpus scale, which is the level-3 (PFP parse)
   decision gate: build level 3 only if measured necessary. Sorters never read
   the corpus except the LCE residual (mmap walk, budgeted) — every key byte
   arrives through a chunk.
4. THE EMITTER — one thread, in unit order: seam LCP against the previous unit's
   last row, `band_<u>.lcp` slice ('SXLS': rows, escapes, max_lcp, rank_base,
   flags=1) + `.lcp.done`, `band_<u>.sa` + `.opened` when TRIE_TAP is armed,
   then the rows feed a piece cutter that writes the EXISTING piece format
   byte-identically (BPS1 start, PRC1 pieces at 65536-run bands with
   frag.runcols.consumed flow control, BPG1 progress with refs=0 classes=0,
   BPT1 terminal, SXRE runexc) into `<frag>.pieces`. The finish, the trie tap,
   the container, the docstarts are UNCHANGED and consume exactly what they
   consume on main.

## Deleted (vs main's tape route)

scatter_sort's band-part files and consumed-marker handshake, cross_lcp_merge's
emit-tape concat phase (spawned by no route any more; the binary still serves
the bin route unchanged), the position-window machinery (subdiv_pos), and the
runtime budget model that sized waves from knobs (units are sized by
--unit-target-rows; the envelope bounds what is in flight, not the unit).

## Honest consequences, journaled

- Chunks are RAM (the spec's `outstanding_chunk_bytes <= envelope -
  working_reserve`). A corpus whose whole chunk plane (n x (8+K+6)) exceeds the
  envelope is extracted in envelope-fitting batches: passes = ceil(total chunk
  bytes / in-flight bound) + tie rounds — never one per unit. At the P1 scale
  (3 GB, ~66 GB of chunks) one pass covers the whole corpus.
- The trie tap keeps its band stream (band_<u>.sa + .opened + end/end.ready);
  without --trie no band .sa is written at all.
