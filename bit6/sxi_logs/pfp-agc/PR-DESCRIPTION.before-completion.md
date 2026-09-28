Native AGC input for PFP++

A genome archive can now be parsed directly with `-t collection.agc --agc`.
The reader supplies uppercase, per-contig reversed bytes with 0x1e separators
and terminator, preserving archive sample/contig order. It writes optional
names/coordinate metadata without an extracted text file.

The new TextSource boundary preserves the existing file/gzip/FIFO path.
This PFP++ revision's text parser is serial: it does not implement the
parallel text chunk algorithm assumed in the task. AGC mode parallelizes
independent seekable archive fetches under `-j` and feeds chunks to the same
rolling-hash parser, retaining state across chunk seams. It does not claim
parallel phrase construction.

AGC is opt-in with PFP_ENABLE_AGC and AGC_ROOT. The dependency is the
MIT-licensed refresh-bio/agc repository (the requested lh3/agc URL is not
available), pinned/tested at e67e3fc865a459779118d3d4e9fbdf42c70ba75e.
The adapter supports AGC format 3 using the library's internal C++
collection/segment interface. It needs an upstream review of that coupling.

Why compressed length indexing is necessary: the public API sorts samples,
and yeast235 has a segment advertised as 8,429 bytes that actually decodes
to 8,431. A metadata-only sum misaligns subsequent seeks. The reader counts
compressed delta instructions, expanding neither matches nor N runs, and
keeps archive-descriptor metadata. No additional expanded-text pass occurs.
Independent per-thread handles and ZSTD contexts avoid shared decoder state;
delivery buffering is j MiB and decoded data is not persistently cached.

Validation evidence and remaining gates are recorded in ACCEPTANCE.md and
throughput.json in this directory. No commits or staging were performed.
The separate SXI pipeline diff changes default AGC parsing from FUSE to the
native CLI, retains XSA_PFP selection and explicit --fifo fallback, and
labels --materialize forensic-only. It is not part of the upstream PFP PR.
