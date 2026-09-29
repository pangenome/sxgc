# Streaming byte remap

The measured inputs fit 249 nonseparator codes plus 0x1E. No escape encoding
is needed for these measurements. The full pile is not proven by slices.

PFP's text reader processes 1 MiB chunks. ByteRemap begins with an identity
permutation sigma and its inverse, and a 256-bit seen set with 0x1E pinned.
On the first occurrence of c, if sigma[c] is reserved (0..5), it selects the
highest allowed target whose preimage has not been observed, then swaps the
two unseen source entries. Later occurrences simply use the recorded value.
The displaced source can itself be reassigned on first encounter. Encounter
order determines the mapping, independently of chunk boundaries and threads.

Invariant: sigma remains a permutation; no observed source's mapping changes;
all observed sources map above 5; sigma[0x1E] remains 0x1E. Consequently all
previously parsed bytes agree with the final table. No forbidden input means
no swaps, including for bytes >=128. Memory is constant, no histogram prepass
is used in production, and no transformed text file exists. The measurement
passes are diagnostic only. Parsing, audit, and querying use unsigned bytes.

At successful input EOF the parser writes prefix.remap (exactly 256 bytes).
The pipeline requires and validates it, journals entries and SHA256, maps the
terminal byte, and passes it to the audit and writer. The audit translates
bounded source reads before comparison. The writer adds optional SXI1 member
7 and flag 0x10 for a nonidentity table; identity tables are omitted. Names
member 6 remains optional independently. Both readers validate table size,
permutation, fixed separator and flag/member agreement. Old readers reject
new flags. No header layout/version change is necessary. DNA complementation uses original bytes (reversal commutes with mapping); the shared product engine maps bytes
before exact search, MS, and MEM maximality checks, covering CLI and HTTP.

The endpoint adapter, streamed chi sweep and sample audit previously assumed
7-bit symbols. Their arrays/range checks now accept all 256 byte values.
PFP itself must convert signed char from its input buffer to unsigned char
before mapping; otherwise UTF-8 was incorrectly rejected as a low byte.

All coordinates and lengths remain original byte coordinates. Alphabet order
may change, so nonidentity parses/indexes are not byte-identical to another
permutation. Identity parses and container payloads retain old behavior.

## Exhaustion and escapes

On the 250th distinct nonseparator byte, the parser fails explicitly before
publishing a successful build. A 256-byte alphabet cannot inject into 249
payload codes. Supporting it would require variable-width escapes or a wider
parser alphabet, neither silently introduced here. Escapes change byte
positions, suffix ordering, matching-statistics lengths and MEM maximality.
Naively encoding queries also admits matches beginning inside escape codes.
Correct support needs code-boundary restrictions and a compressed mapping
between encoded and original coordinates, plus updates to endpoints, chi,
query lengths, separator semantics and independent text audit. Merely
recording a 256-entry byte table is insufficient. A full-pile encounter of
exhaustion must stop and trigger that separate design decision.

## Review scope

No commits or staged files; upstream /home/erikg/pfp and shared fragment are
untouched. Vendored fork changes are carried by bit6/patches/pfp_agc.patch,
including the new byte_remap.hpp, and mirrored into the packaged runtime.
Raw `.ri4` artifacts have no permutation metadata and consume indexed bytes;
the public SXI product query, MEM, matching-statistics and HTTP paths consume
original bytes, including SXI files passed through the `--ri4` alias. JSON HTTP supports UTF-8 strings and escaped controls, not
arbitrary invalid UTF-8 sequences; binary FASTA reads cover other byte values
subject to FASTA newline/header framing.
