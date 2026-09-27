# Checked acceptance: ropebwt3 output lane

Implementation and local gates pass. Independent reviewer approval remains
pending. No commits/staging or protected k10 access occurred.

## Deliverables

- `mems --out native|ropebwt3`; native remains the default and byte-compatible.
- SMEM query containment filtering across references and both strands by
  default in ropebwt3 mode; `--mem` retains all per-occurrence maximal matches.
- Authoritative counts with no default positions; `-p N`/`--positions N`
  retains at most N deterministic position tokens per interval.
- `--gap N`, `--gap-seq`, `--cov`, and forward-reference coordinates.

Only product.rs, SXI_QUERY.md, two new tests, and this log directory changed
in this lane. See `product.diff`, `docs.diff`, and `hygiene.json`.

## Evidence

- 25 fixtures / 195 reads / 756 SMEM intervals /
  44583 maximal occurrences independently brute-verified.
- 23 fixtures compared with a freshly compiled real ropebwt3:
  SMEM sets/counts and full normalized position multisets equal; gap/coverage
  rows equal byte-for-byte. Both reference storage orientations, palindromes,
  repeats, duplicates, boundary queries, nested/overlapping matches, no hits,
  position caps and deterministic threading are covered.
- All seven original construction fixtures participate via an order-preserving
  control-alphabet-to-DNA relabeling, necessary because upstream is DNA-only.
  Both tools index the exact same relabeled references. The SXI test conversion
  changes only the RLBWT symbols/C in O(r) and reuses head/tail/anchor samples;
  no production SA/LCP construction or full-text index walk was introduced.
- The original native battery remains unchanged: 23 fixtures,
  44481 direct-text MEM occurrences, exact query/MS,
  gzip/FASTQ, text/DNA and HTTP parity checks all pass.
- Yeast235: 1720 native occurrences match a fresh direct-text oracle
  and the saved binary byte-for-byte. 5 SMEM intervals with
  74 hits match counts and every normalized position.
  All-MEM aggregation also matches. All six container members and n/k/r match
  the retained prior hashes (`yeast-members.json`).
- Committed format, build CLI and Phi-inverse regressions pass; the committed
  LCE primitive test passes 216,000 comparisons. `cargo test --release` passes
  (the crate currently has zero Rust unit tests). Rustfmt and diff checks pass.
- Largest measured gate RSS: 3,260,416 KiB
  (3.11 GiB), below 10 GB. Test runners impose a
  9 GiB address-space ceiling. See `memory.json` for every timed command.

## Intentional compatibility differences

The task's exact sampled layout omits upstream's additional n_pos column.
The comparator removes only that column and compares full position multisets;
cap-selected subsets/order can differ. xsa retains its literal case/IUPAC/text
matching (upstream normalizes nt6), and gap sequence bytes remain original.
`--mem` is an xsa extension with maximal-occurrence counts. Native remains
the default format. Zero-coverage omission and gap-over-cov precedence match
upstream. Full semantics are documented in `bit6/SXI_QUERY.md`.

Large all-MEM queries retain the existing repeated-search/locate cost and may
aggregate quadratically many intervals. Explicit large position caps increase
memory/output. Default SMEM counts do not locate hits.

See `COMMANDS.md` for reproduction, per-command stdout/stderr/time under
`fixtures/` and `yeast/`, `ropebwt3-provenance.json` for exact source identity,
and `REVIEW.md` for scope and review notes. The contact_supervisor tool was
not available; no unresolved decision required coordination.
