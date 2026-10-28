Partial implementation, not full acceptance

Base: 0d85475. Branch: virtual-end-fix.
The standalone scatter producer now has exactly one virtual END path. It stores
only corpus bytes, includes position n at rank zero, and sorts the unkeyed tail
with the finite comparator. The fixture writes the bare corpus. The SA checker
no longer requires a reserved byte. The stream checker excludes END from byte
reconstruction and reports the actual convention.

producer-gates.log contains exact oracle, SA, inverse-BWT, and local run-delta
checks. All-256-byte coverage is standalone producer coverage, NOT container
coverage. G2 compares finite SA plus its legacy wrapping predecessor baseline:
insert rank-zero END suffix, then replace position-zero predecessor by virtual
END. It asserts every other row is unchanged and reports each run delta.
It does not validate chi.

Remaining blockers:
- bit6/xsa_finish.cpp still reserves code zero and requires 0xf5 absent.
- The producer refuses tape output when data maps to cell zero, rather than
  allowing the inherited finisher to confuse data with END. This refusal is
  covered by negative tests on both all-256-byte corpora.
- Container serialization still represents END as an ordinary byte code. The
  requested alphabet k and ends_in_sentinel flag are not implemented.
- stable/emit_remap.py remains unchanged. Its 256-byte bijection cannot encode
  literal values 1..256 when all 256 input bytes are present. A wider logical
  remap (with an agreed wire encoding) is required to satisfy that requirement;
  silently wrapping 256 to zero would violate byte order in current consumers.
- Full-container G1/G2/G3/G5/G7 have not passed. No 150 MB fixture was run.

No Rust source or xsa/build_tools.py was edited. Packaged native sources were
refreshed with tools/package_xsa_sources.py.

Validation: test_end_scatter.py PASS; stream_vs_bwt --selftest PASS;
git diff --check PASS. Release build passed before the final tape safety guard;
the final producer source was compiled and exercised by the Python gate.
