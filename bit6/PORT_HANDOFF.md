# PORT_HANDOFF — PFP-LCP primitive port (lcp_index eliminated)

> **SEE ALSO (2026-09-26):** item 3 below ("Decide on the residual
> O(n)-scaled construction") is DONE — see `bit6/PFP_INDEX_FOLD_REPORT.md`
> and `bit6/PFP_INDEX_FOLD_HANDOFF.md`: the dictionary/parse/pf_parsing
> build is now a persisted index sidecar (`bit6/pfp_index_build.cpp`) that
> the dump loads with `--pfp-index`; construction phase at yeast
> 536.26 s -> 32.29 s, whole dump 819.70 s -> 341.02 s, .agg
> byte-identical, chi gate green.  Item 1's harness update is now
> possible end-to-end (commands in the fold handoff).

State written for the next lane/session. No git commits made (protocol).

## What is DONE

`bit6/chi_rspace_dump.cpp` in this worktree now produces the per-run
`.agg` from **PFP artifacts + `.ri4` only** — no lcp_index, no TeraLCP.
- `topLCP` = `clamp(LCE_sup(resolve_row(a-1), resolve_row(a)))` for
  run-head row `a` (0 if `a==0`), the PLCP = adjacent-row-LCP identity.
- `--lcp-index` is now OPTIONAL (cross-check only; `CHECK_TOP` env, or
  the `G0_ALL=1` all-rows validator).
- Diff: `bit6/chi_rspace_dump_lce.patch` (91 insertions / 16 deletions).

Gates ALL GREEN (see `bit6/PFP_LCP_PORT_REPORT.md`):
- G0 `G0_ALL`: 997k positions over the 7 battery texts, 0 mismatches.
- G1: `.agg` byte-identical on 7/7 battery texts, no lcp_index.
- G2 yeast: `.agg` byte-identical (3,228,956,204 B); `xsa chi-rspace`
  → chi = 85,404,240, sorted-set equality vs oracle = True.
- G3: 867.70 s / 25.3 GB vs baseline 901.03 s / 28.4 GB.

Build: `/tmp/build_dump.sh <src> <out>` (pinned flags; links gsacak64.o).
Binaries: `/tmp/laneI/dump_final` (deliverable), `dump_port`, `dump_g0`,
`dump_base` (old path, for byte-diffing).

## Exact next steps (priority order)

1. **Update the battery chain harness** `bit6/chi_rspace_battery_chain.sh`
   to drop the lcp_index + TeraLCP dependency for the aggregate path:
   remove the `$TLC ... -oindex` step and the `--lcp-index` arg to
   `$DUMP`. Verify the 7-text G1/G2 battery re-runs green end-to-end
   with TeraLCP absent. (The harness currently uses
   `DUMP=/tmp/laneY/dump`, `XSA=/tmp/sxgc-laneY/xsa/...` — repoint at a
   build of this worktree's source.)
2. **466 r-space confirmation** with the lcp_index-free dumper: the
   committed 466 pilot used the old dumper; re-derive
   `h466.agg` without lcp_index and re-run `xsa chi-rspace` → expect
   chi = 2,249,968,075. (Cheap parallel confirmation.)
3. **Decide on the residual O(n)-scaled construction.** If the goal is
   "the only Ω(n) is the single pfp++ text read," the remaining target
   is `pfpds::pf_parsing::build_b_bwt_and_M()` (O(n)-bit `b_bwt` +
   `|M| ≈ 0.1n` entries). Options: (a) accept it as one-off index
   construction (honest and probably correct for the paper — it is not
   a per-query scan); (b) if pfp++ can be made to persist `M`/`b_bwt`
   (or a lighter LCE that avoids them), load them instead of rebuilding.
   Investigate whether `pfp_sa_support`/`pfp_lce_support` can be driven
   without the full `M` (they index `pfp.M[lex_rank_i]`).
4. **Port the identity to the M3 piece-build path.** The Python
   prototype (`tools/teralcp_m3.py`) used an O(LCP-value) phrase walk;
   the same PLCP-via-LCE identity plus `pfp_lce_support` retires it:
   for a candidate piece start `p`, PLCP(p) = LCE(p, prev-in-SA), with
   `prev-in-SA` obtained from a pos→row inverse (the `c_probe_r3`
   resolve_inv machinery is already gated — reuse it). That would make
   the **piece sidecar** build genuinely O(r·polylog).
5. **Skip** the ft30/sat4 fixtures unless their own chain is rebuilt
   (their `.ri4` text differs from any pfp parse I could regenerate —
   mispairing trap; see report).

## Residual risks

- The identity is validated by byte-identity of the full `.agg` at
  yeast and by `G0_ALL` on the battery; it is not yet in Lean. The
  natural statement: `PLCP(SA[a]) = LCP(SA[a-1], SA[a])` (adjacent LCP)
  plus `lcp_at_pos` ambient consistency.
- The clamp `n - max(prev,pos)` matches the lcp_index convention on all
  measured texts; at the text end the pfp LCE is cyclic (see
  `lce_support.hpp` `i = (i + w) % n`), so the clamp is load-bearing.
  No boundary failures observed on 7 battery texts + yeast.
- `spdlog` info spam on stderr is the vendored pfp_ds logger; harmless
  (grep or `2>/dev/null`). It is NOT from the port.
