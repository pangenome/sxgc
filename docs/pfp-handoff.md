# Production PFP columns (private scratch)

`pfp_parse RAW EMPTY_DIR WORKERS SORT_BYTES SCRATCH_BYTES` is a native bundle stage, not a new `xsa` command. RAW is the exact immutable byte stream. The byte-level trigger is [`xsa_pfp::trigger`](../xsa/runtime/rolling/pfp_trigger.hpp). Positive starts include every matching complete 8-byte window, including the last complete window. There is always a start at 0. No length cap, suppression, or document split. `[p,q)` owns `q-p` bytes; a nonterminal phrase references `[p,q+8)`. The terminal phrase has a virtual END after its last raw byte.

| File | LE wire | Meaning |
|---|---|---|
| `parse.u64` | `m` × u64 | Dense, zero-based lexical phrase IDs **in original text order**, accepted directly by `stream_rank inverse` |
| `phrases.ref` | `k` × `(u64 start, u64 stride, u64 count)` | In lexical ID order. Start = first occurrence, stride = owned length, count = exact frequency. `start+stride==N` identifies the unique terminal word, not `start+stride+8==N`. |
| `histogram` | 256 × u64 | Raw byte counts for the v10 encoder |

There is one sequential trigger scan, not necessarily only one disk read of each byte. A bounded batch of `(position,stride,ordinal)` records is ordered by the **finite** referenced phrase bytes and terminal convention, with ordinal as a deterministic last tiebreak. Hashes choose anchors, never equality. Lexically merge sorted *phrase-reference* runs when the batch fills. Adjacent equal words assign one ID; scatter that ID by ordinal, count its occurrences and write one reference. No copied phrase bytes, raw-suffix run files, or byte suffix array. Long comparisons use bounded caches into RAW.

The comparator caches, batch, merge fan-in, scatter pages and I/O buffers are sized before sorting. An exact logical scratch ledger covers output columns and temporary runs (24 bytes per live occurrence per run generation); it refuses before exceeding the caller's grant. A dense u64 tape can itself exceed 2N on very frequent triggers: this is a refusal, **not** permission to change the parse. Scratch length is not an XFS quota or cgroup RAM measurement. The stage works inside a caller-owned private directory; failure of any stage must prevent publication and the caller removes private files.
