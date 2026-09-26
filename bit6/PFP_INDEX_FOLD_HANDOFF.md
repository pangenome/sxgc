# PORT_HANDOFF — PFP-index fold (dump-time O(n) construction retired)

State for the next lane/session. No git commits made (protocol). This
supersedes the "residual O(n)-scaled construction" caveat at the bottom of
`bit6/PFP_LCP_PORT_REPORT.md` and completes item 3 of `bit6/PORT_HANDOFF.md`.

## What is DONE

`bit6/chi_rspace_dump.cpp` accepts `--pfp-index FILE` and, in that mode,
performs **no in-process construction**: it loads the dictionary's `b_d`
(+ `isaD`, `lcpD`), the parse's `p` (from the retained `.parse`), `saP`,
`isaP`, `lcpP`, and `pf_parsing`'s `n`, `b_bwt`, `b_p`, `M`, `w_wt` from the
sidecar written by `bit6/pfp_index_build.cpp`. Only rank/select and RMQ
supports are rebuilt (linear passes over loaded vectors).

Gates ALL GREEN at yeast scale (n = 3,336,986,770, R = 100,904,881):
- G1: load `.agg` byte-identical, 7/7 battery texts (incl. duplicates-600k).
- G1b: all 10 structure digests equal build-vs-load, 7/7 texts.
- G2: yeast load `.agg` **byte-identical** to `/tmp/laneY/yeast_pfp2.agg`
  (3,228,956,204 B); all 10 digests equal vs build mode; `xsa chi-rspace`
  → **chi = 85,404,240**, sorted-set equality vs `chi_yeast_pfp2.sA` = True.
- G3: dump **819.70–913.27 s → 341.02–346.59 s (2.40–2.64×)** (two runs
  each; the machine is shared), RSS 25.3 → 19.5 GB; construction phase
  **536–626 s → ~37 s**; index build (one-off) 568.54 s / 25.8 GB; index
  file **8.460 GB** at yeast.

Artifacts/exe: `/tmp/laneJ/{pfp_index_build,dump_idx}`,
`/tmp/laneJ/yeast/yeast2.idx` (8.46 GB, format version 2).
Build: `/tmp/build_idx.sh <src> <out>` (vendor dir FIRST on the include path).
Full gate log + cost/size table: `bit6/PFP_INDEX_FOLD_REPORT.md`.

## The one decision this lane hands up: RETENTION AT 466

Persisting the index costs **2.60 bytes per input symbol** → projected
**≈ 3.74 TB** at n = 1.44 Tbp (M 1.57 TB, isaD 845 GB, lcpD 423 GB, w_wt
365 GB, b_bwt+b_p 344 GB, saP/isaP/lcpP 169 GB, b_d 28 GB), assuming the
yeast ratios (`|D|/n=0.154`, `|M|/n=0.0951`, `|P|/n=0.01023`) hold. For
scale: 466 revlines text = 1.40 TB, `h466.ri4` = 27.7 GB, dead
`h466rt.lcp_index.lcp_index` = 82.5 GB. Free space: nvme3n1 1.9 TB (too
small), `/` 3.8 TB, nvme2n1 6.5 TB.

Projected time at 466: BUILD-mode dump ≈ 66.4 h → LOAD-mode ≈ 6.1 h, with a
one-off index build ≈ 69.4 h. The index depends only on the parse, so it is
rebuilt only when the parse changes; the win is per re-dump (iterations,
refreshes, extra aggregate variants), not for a single dump.

**Recommendation to weigh:** keep the capability, and do NOT commit to a
3.7 TB artifact for the 466 *first* run — the 466 chain has no PFP parse yet
(iteration 2), so nothing blocks on this decision until then. If the 466
`.agg` is produced once, rebuild in-process (66 h) and skip retention; if
the dump will be re-run (it will, across algorithm iterations), retain it.

## Exact next steps

1. **Update the battery harness** `bit6/chi_rspace_battery_chain.sh` to the
   two-step form (the previous lane's handoff item 1 is now satisfiable
   end-to-end without TeraLCP):
   `PFPB=pfb_index_build; $PFPB --parse ${name}_pfp -o $name.pfpidx` then
   `$DUMP --ri4 $name.ri4 --parse ${name}_pfp --pfp-index $name.pfpidx -o $name.agg ...`
   (`DUMP`/`XSA` must point at builds of this worktree's source; the old
   paths `/tmp/laneY/dump`, `/tmp/sxgc-laneY/...` are stale).
2. **466 r-space confirmation** with the lcp_index-free + index-free dumper:
   needs a 466 PFP parse first (iteration 2) and, if retention is chosen, a
   466 index build (~69 h projected / ~3.7 TB). Then expect
   chi = 2,249,968,075.
3. **Optional size reduction (if retention is chosen):** pack `M` tighter
   (`len` needs ≤ 2 B for any real phrase length; `left`/`right` are colex
   ids < `n_phrases`, currently 4 B) — ~8 B/entry instead of 12 B cuts `M`
   by ~a third (−14% of the whole index). Also consider whether `saP` can be
   dropped (it is read by `pfp_sa_support`; not droppable as-is) and whether
   `lcpD`'s width (2 B here) is already minimal (it is).
4. **Fold the index build into the pfp++/r-pfbwt parse build** so the
   sidecar is produced alongside the parse rather than as a separate pass.
   `pfp_index_build.cpp` is already a standalone "parse → index" tool, so
   this is a driver/plumbing change, not new machinery.
5. **Optional:** port the same identity to the M3 piece-build path
   (`tools/teralcp_m3.py`) — see `PORT_HANDOFF.md` item 4; unchanged by
   this lane.

## Residual risks

- The load path's correctness at scale is gated by (a) byte-identity of the
  full `.agg` and (b) FNV digests over the logical values of all ten
  structures at yeast. It is not in Lean; the natural statement is
  "load(serialize(x)) = x for each structure", i.e. an encoding identity —
  cheap to state, and the digest gives the measured counterpart.
- The two vendored defer ctors are the only header divergences; if the
  upstream pfp_ds is ever updated, the vendor dir must be refreshed and the
  two constructors re-applied (they are commented as such in place).
- Index format is version-tagged (`"XPF1"`, version 2) and the loader
  rejects other versions; an index built before this lane (version 1) will
  be refused, and must be rebuilt.
- `.dict` is no longer read on the load path, so a *mispaired* `.parse`
  (not the matching one) is caught only by the `|P|` cross-check and the
  `.agg`/digest gates — pass the correct `--parse` prefix for the index.
