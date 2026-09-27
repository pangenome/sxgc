# Review notes

Self-review found no unresolved implementation blockers. Independent reviewer
approval remains required and has not been obtained.

- Scope: only `xsa/src/product.rs`, query documentation, two new test runners,
  and this evidence directory changed in this lane. The inherited tracked
  diff is unchanged, so there are no new index-format, parser, tap, front-end,
  or seam edits. No protected k10 path/process was accessed.
- Native serialization/order uses the same occurrence visitor and remains
  byte-identical to the saved executable. The HTTP batch still calls native.
- SMEM containment sorts query starts ascending, ends descending, then keeps
  strictly increasing right ends. Equal intervals and cross-strand containment
  are handled globally. Candidate storage is bounded by twice the read length.
- Counts use both oriented FM intervals, independently of position caps. The
  existing separator boundary contract prevents cross-record matches.
- The reverse-complement query is located in the forward reference; annotation
  handles reversed storage. Reapplying the reference reverse-strand formula
  would be an erroneous second mirror. Both storage orientations are tested.
- Gap and coverage use interval unions; zero coverage omission and gap-over-cov
  precedence match upstream. All-MEM union parity is also tested.
- Upstream's sampled-position-count column is intentionally omitted per task.
  Alphabet normalization and all-MEM extensions are documented in SXI_QUERY.md.
- Position sampling is a deterministic capped prefix, not random and not the
  same subset chosen by ropebwt3. Counts always include unsampled hits.
- Large `--mem` queries retain existing repeated-search/locate runtime costs;
  aggregation may hold O(read_length²) intervals. Explicit large position caps
  increase output and memory. Default SMEM count-only output avoids locate.

No `contact_supervisor` capability was available in the exposed tool catalog.
There was no unresolved decision requiring a substitute coordination path.
