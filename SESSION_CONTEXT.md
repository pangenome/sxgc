# XSA session context: causal branch reader (prototype)

`xsa session DEPTH` owns one in-memory session. It reads one line and flushes one JSON reply per successful command. Bytes are hex so newline, NUL, 0x1e and 0xff are all data. `DEPTH` is the maximum query length in bytes (1..64). Malformed requests fail closed. This is a branch-only slice, not an implementation of locate, arbitrary intervals, archive lookup, eviction or durable recovery.

```
append 6162
seal
append 6358616263
branch 616263
end
branch 616263
```

Here the branch for `abc` has one `X` (88). Before `end`, its second occurrence has no observed continuation; after `end`, the structural END count becomes one. Both queries use the same interface. An explicit `end` closes the current nonempty document; the next append begins the next document. Completed documents remain visible but matches never cross boundaries. Use a fresh session for each independent training document/window and advance only with bytes already observed before prediction.

`seal_async` copies the current text and document-end list and submits a seal to one background worker. Keep querying and appending; `status` reports `pending`, `visible`, and `cut`. At the next command after the worker finishes the server publishes its warmed generation, retaining all text and END events received since submission. There is at most one pending seal; a second seal request while pending is an error. This prototype pays for the snapshot copy and two generations of resident memory; cold startup and bulk prefill are separate costs.

`seal` synchronously builds a real boundary-aware XSA m20 member (`ctx::Builder`, support threshold 1) in memory; the reader uses `ctx::Trie`. The sorted input needs only depth+1 prefix order. An open prefix's artificial EOF is subtracted from m20 END. After sealing, every new content branch belongs to the tail by its **continuation** offset (including when the pattern starts in the base); a newly confirmed END belongs to the tail by end-event ordinal. Sealing again changes neither meaning nor output. No `.sxi2` file is created. The exact histogram sums *eligible continuation events*, not raw interval width.

Correctness target: `cargo test --manifest-path xsa/Cargo.toml prefix_seal_and_boundary_parity`. This uses deterministic binary documents, overlapping patterns, absent queries, every prefix, five seal schedules, and a document-by-document direct-scan oracle for all 257 counters. It tests the m20 member path rather than an external build of a whole container.

Performance is **not** accepted yet. The reference writer rebuilds synchronously, the tail query scans, and the first query after sealing pays to initialize the edge directory, now retained by the owned m20 generation across queries. For deployment, benchmark decode (append + query), prefill, RSS and worst seal pause separately. Do not use CLI smoke wall-clock to claim acceptable decoder latency.
