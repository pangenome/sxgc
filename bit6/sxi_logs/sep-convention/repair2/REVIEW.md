# Self-review; external reviewer gate still required

- Prefix intervals are laminar; their maximal union cannot move across class
  boundaries. Discovery stops when the terminal suffix becomes unique.
- q=0 preserves the old streaming path; no new PFP dependency is exercised
  there. General repair loads only compressed run/PFP artifacts.
- Both Phi directions use padded samples. Class cut neighbors preserve the
  endpoints of untouched run fragments; coalescing uses outer samples.
- Periodic cyclic ties use increasing starting position, including endpoints.
- Class size, total repair rows and discovery depth have explicit r-based
  limits. Fingerprint probe and verification work has a separate polylog n
  limit. All such refusals precede output creation. No fallback exists.
- The sweep derives reverse-coordinate witnesses from the preceding BWT
  character. Legacy newline semantics are preserved. Cyclic mode includes
  separator/newline bytes as ordinary characters and checks SA bounds.
- Tests compare complete BWT runs and both endpoints against direct rotations;
  dense LCP/aggregate/witness checks cover both sweep paths and periodic ties.
  A 2-billion-byte conceptual constant-run frame refuses from 976 RLE bytes.
- The full control rebuild uses fresh parse/producer stages and byte gates;
  publication uses the unchanged writer and validator. Hashes are checked
  for all five core members.

No external review approval has been received or inferred. The runtime lacks
contact_supervisor, and this lane did not spawn a reviewer agent. The gate
must be completed by the independent reviewer using the retained evidence.
